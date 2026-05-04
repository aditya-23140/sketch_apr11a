// session_machine.cpp — Part 5.3: Local timer state machine
// States: IDLE → FOCUS → HYPERFOCUS → BREAK → DISENGAGED → IDLE
// This is the authoritative on-device timer. It does NOT poll the backend
// every second. State transitions POST to backend via api_client.
#include "session_machine.h"
#include "app_state.h"
#include "../api/api_client.h"
#include "../hardware/motor_bridge.h"   // ← NEW: UART bridge to Arduino motor controller

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
void SessionMachine::_reset() {
    _state        = SessionState::IDLE;
    _plannedSec   = 0;
    _elapsedSec   = 0;
    _breakIdleSec = 0;
    _disengagedSec= 0;
    _lastTickMs   = 0;
    _toastActive  = false;
    _toastShownAt = 0;
}

void SessionMachine::_transitionTo(SessionState next) {
    _state = next;
    _postStateToBackend(next);

    // --- ARDUINO HARDWARE BRIDGE ---
    if (next == SessionState::FOCUS || next == SessionState::HYPERFOCUS) {
        MotorBridge::getInstance().sendStart(_plannedSec);  // Send duration so Arduino calculates speed
        Serial.println("[Hardware] Sent START with duration to Arduino");
    }
    else if (next == SessionState::COMPLETED || next == SessionState::IDLE) {
        MotorBridge::getInstance().sendStop();     // Motor REVERSES to start
        Serial.println("[Hardware] Sent STOP (return to start) to Arduino");
    }
    else {
        // BREAK or DISENGAGED — pause in place
        MotorBridge::getInstance().sendPause();    // Motor holds position
        Serial.println("[Hardware] Sent PAUSE (hold position) to Arduino");
    }
    // -------------------------------

    // State-specific reset actions
    if (next == SessionState::BREAK) {
        _breakIdleSec = 0;
    }
    if (next == SessionState::DISENGAGED) {
        _disengagedSec = 0;
    }
    if (next == SessionState::IDLE || next == SessionState::COMPLETED) {
        AppState::getInstance().isSessionRunning = false;
    }
}

void SessionMachine::_postStateToBackend(SessionState s) {
    // Map state to a string and fire a WS update
    const char* stateStr = "idle";
    switch (s) {
        case SessionState::FOCUS:       stateStr = "FOCUS";       break;
        case SessionState::HYPERFOCUS:  stateStr = "HYPERFOCUS";  break;
        case SessionState::BREAK:       stateStr = "BREAK";       break;
        case SessionState::DISENGAGED:  stateStr = "DISENGAGED";  break;
        case SessionState::COMPLETED:   stateStr = "COMPLETE";    break;
        default: break;
    }
    // Session state patch is sent via existing api_client stopSession/startSession
    // (full PATCH /api/sessions/:id wiring belongs to Part 7)
    Serial.printf("SM: → %s (elapsed=%ds)\n", stateStr, _elapsedSec);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void SessionMachine::start(int plannedMinutes) {
    _reset();
    _plannedSec   = plannedMinutes * 60;
    _elapsedSec   = 0;
    _lastTickMs   = millis();

    AppState& app = AppState::getInstance();
    app.isSessionRunning = true;
    app.sessionRemainingSeconds = _plannedSec;

    _transitionTo(SessionState::FOCUS);
}

void SessionMachine::takeBreak() {
    if (_state == SessionState::FOCUS || _state == SessionState::HYPERFOCUS) {
        _toastActive = false;

        AppState& app = AppState::getInstance();
        int breakMin = app.preferredBreakDuration;

        // Pomodoro Rule: Long break after 4 sessions
        if (app.cycleCount > 0 && (app.cycleCount % 4 == 0)) {
            breakMin = 15;
            Serial.println("SM: 4th cycle reached → 15m Long Break");
        }

        _plannedSec = breakMin * 60;
        _breakIdleSec = 0; // repurposed as break elapsed
        _transitionTo(SessionState::BREAK);
    }
}

void SessionMachine::pause() {
    // "Pause" from the UI maps to DISENGAGED (user stepped away mid-session)
    if (_state == SessionState::FOCUS || _state == SessionState::HYPERFOCUS) {
        _transitionTo(SessionState::DISENGAGED);
    }
}

void SessionMachine::resume() {
    if (_state == SessionState::DISENGAGED || _state == SessionState::BREAK) {
        // Resume: decide FOCUS vs HYPERFOCUS based on elapsed
        if (_elapsedSec < _plannedSec) {
            _transitionTo(SessionState::FOCUS);
        } else {
            _transitionTo(SessionState::HYPERFOCUS);
        }
        _lastTickMs = millis();
    }
}

void SessionMachine::stop() {
    if (_state != SessionState::IDLE) {
        AppState& app = AppState::getInstance();

        // Award spoon cost and XP on session completion
        if (_state != SessionState::DISENGAGED) {
            if (app.spoonsUsed > 0) app.spoonsUsed--;
            app.xp += 50 + (_elapsedSec / 60) * 5; // base + per-minute bonus
        }

        _transitionTo(SessionState::COMPLETED);
        _reset();
    }
}

// ---------------------------------------------------------------------------
// tick() — called every 100ms from session_machine_task (Core 0)

// Only seconds-level state advances happen here; arc animation runs in LVGL.
// ---------------------------------------------------------------------------
void SessionMachine::tick() {
    if (_state == SessionState::IDLE || _state == SessionState::COMPLETED) return;

    unsigned long now = millis();
    unsigned long deltaMs = now - _lastTickMs;

    // ---- State-specific logic (only one advances per tick) --------------
    if (_state == SessionState::FOCUS || _state == SessionState::HYPERFOCUS) {
        if (deltaMs >= 1000) {
            uint32_t deltaSec = deltaMs / 1000;
            _elapsedSec += (int)deltaSec;
            _lastTickMs += deltaSec * 1000;

            AppState::getInstance().sessionRemainingSeconds = getRemainingSeconds();

            // FOCUS → HYPERFOCUS when elapsed >= planned
            if (_state == SessionState::FOCUS && _elapsedSec >= _plannedSec) {
                _transitionTo(SessionState::HYPERFOCUS);
                // Show the non-blocking toast (§5.5)
                _toastActive   = true;
                _toastShownAt  = millis();
                Serial.println("SM: Planned duration ended → HYPERFOCUS toast shown");
            }
            // HYPERFOCUS auto-cap → BREAK
            else if (_state == SessionState::HYPERFOCUS) {
                int capMin = AppState::getInstance().hyperFocusDuration;
                int overflowSec = _elapsedSec - _plannedSec;
                if (overflowSec >= capMin * 60) {
                    _toastActive = false;
                    _transitionTo(SessionState::BREAK);
                    Serial.printf("SM: HyperFocus cap reached (%dm) → BREAK\n", capMin);
                }

                // Auto-dismiss toast after 30s if no user action
                if (_toastActive && (millis() - _toastShownAt) >= 30000) {
                    _toastActive = false; // stays in HYPERFOCUS (Continue implied)
                    Serial.println("SM: Toast auto-dismissed after 30s → continue HYPERFOCUS");
                }
            }
        }
    }
    else if (_state == SessionState::BREAK) {
        if (deltaMs >= 1000) {
            uint32_t deltaSec = deltaMs / 1000;
            _breakIdleSec += (int)deltaSec;
            _lastTickMs   += deltaSec * 1000;

            // BREAK → DISENGAGED only after the full break duration + 30s idle
            if (_breakIdleSec >= (_plannedSec + 30)) {
                _transitionTo(SessionState::DISENGAGED);
                Serial.println("SM: Break duration + 30s idle → DISENGAGED");
            }
        }
    }
    else if (_state == SessionState::DISENGAGED) {
        if (deltaMs >= 1000) {
            uint32_t deltaSec = deltaMs / 1000;
            _disengagedSec += (int)deltaSec;
            _lastTickMs    += deltaSec * 1000;
        }
    }
}

// ---------------------------------------------------------------------------
// Computed getters
// ---------------------------------------------------------------------------
int SessionMachine::getRemainingSeconds() const {
    int rem = _plannedSec - _elapsedSec;
    return (rem < 0) ? 0 : rem;
}

int SessionMachine::getOverflowSeconds() const {
    int ov = _elapsedSec - _plannedSec;
    return (ov < 0) ? 0 : ov;
}

// Base arc: 0–360 representing elapsed/planned progress
// In DISENGAGED, the arc drains backward at 1°/s
int SessionMachine::getBaseArcAngle() const {
    if (_plannedSec == 0) return 0;

    if (_state == SessionState::DISENGAGED) {
        // Drain: start from how far we were when DISENGAGED began
        int baseAtDisengage = (_elapsedSec <= _plannedSec)
            ? (_elapsedSec * 360 / _plannedSec)
            : 360;
        int drained = _disengagedSec * 2; // drain at 2°/s
        int angle = baseAtDisengage - drained;
        return (angle < 0) ? 0 : angle;
    }

    if (_state == SessionState::FOCUS) {
        return (_elapsedSec * 360) / _plannedSec;
    }

    // HYPERFOCUS / BREAK / COMPLETED: base stays at 360
    return 360;
}

// Overflow arc: 0–360 covering the HyperFocus cap window
int SessionMachine::getOverflowArcAngle() const {
    if (_state != SessionState::HYPERFOCUS) return 0;
    int capSec = HYPERFOCUS_CAP_MINUTES * 60;
    int ov = getOverflowSeconds();
    if (capSec == 0) return 0;
    int angle = (ov * 360) / capSec;
    return (angle > 360) ? 360 : angle;
}

// Timer label string per spec §5.2
String SessionMachine::getTimeLabelString() const {
    char buf[16];
    switch (_state) {

case SessionState::FOCUS: {
            int rem = getRemainingSeconds();
            snprintf(buf, sizeof(buf), "%02d:%02d", rem / 60, rem % 60);
            return String(buf);
        }
        case SessionState::HYPERFOCUS: {
            int ov = getOverflowSeconds();
            snprintf(buf, sizeof(buf), "+%02d:%02d", ov / 60, ov % 60);
            return String(buf);
        }
        case SessionState::BREAK: {
            int rem = _plannedSec - _breakIdleSec;
            if (rem < 0) rem = 0;
            snprintf(buf, sizeof(buf), "%02d:%02d break", rem / 60, rem % 60);
            return String(buf);
        }
        case SessionState::DISENGAGED:
            return String("Resume");
        default:
            return String("--:--");
    }
}
