#ifndef MOTOR_BRIDGE_H
#define MOTOR_BRIDGE_H

#include <Arduino.h>

// ---------------------------------------------------------------------------
// MotorBridge — UART bridge to Arduino UNO motor controller
// Uses Serial2 on GPIO 10 (TX) and GPIO 11 (RX) via the Extended IO header.
// TX line: direct (3.3V is valid HIGH for Arduino)
// RX line: through 10k/20k voltage divider (5V → 3.33V safe for ESP32)
// Protocol: <CMD> framed messages at 9600 baud.
// ---------------------------------------------------------------------------

// Extended IO Header pin mapping (WT32-SC01 Plus Tab. 2)
#define BRIDGE_TX_PIN 10   // EXT_IO1 → GPIO 10
#define BRIDGE_RX_PIN 11   // EXT_IO2 → GPIO 11
#define BRIDGE_BAUD   9600

class MotorBridge {
public:
    static MotorBridge& getInstance() {
        static MotorBridge instance;
        return instance;
    }

    /**
     * Initialize Serial2 on the Extended IO pins.
     * Call once in setup() after Serial.begin().
     */
    void init() {
        Serial2.begin(BRIDGE_BAUD, SERIAL_8N1, BRIDGE_RX_PIN, BRIDGE_TX_PIN);
        Serial.println("[Bridge] Motor bridge initialized on GPIO 10 (TX) / GPIO 11 (RX)");
        
        // Send initial PING to check if Arduino is online
        delay(100);
        sendPing();
    }

    /**
     * Non-blocking update — reads and logs any ACK/response from Arduino.
     * Call every loop() iteration.
     */
    void update() {
        while (Serial2.available()) {
            char c = Serial2.read();
            if (c == '<') {
                _rxIndex = 0;
                _rxActive = true;
            } else if (c == '>' && _rxActive) {
                _rxBuf[_rxIndex] = '\0';
                _rxActive = false;
                _handleResponse(_rxBuf);
            } else if (_rxActive && _rxIndex < sizeof(_rxBuf) - 1) {
                _rxBuf[_rxIndex++] = c;
            }
        }

        // Periodic heartbeat every 5 seconds
        unsigned long now = millis();
        if (now - _lastPing >= 5000) {
            _lastPing = now;
            sendPing();
        }
    }

    /** Send <START:seconds> — motor begins forward rotation, timed to session */
    void sendStart(int durationSec) {
        Serial2.print("<START:");
        Serial2.print(durationSec);
        Serial2.print(">");
        Serial.printf("[Bridge] Sent <START:%d>\n", durationSec);
    }

    /** Send <PAUSE> — motor stops in place, holds position */
    void sendPause() {
        Serial2.print("<PAUSE>");
        Serial.println("[Bridge] Sent <PAUSE>");
    }

    /** Send <STOP> — motor reverses back to starting position */
    void sendStop() {
        Serial2.print("<STOP>");
        Serial.println("[Bridge] Sent <STOP>");
    }

    /** Send <PING> — heartbeat check */
    void sendPing() {
        Serial2.print("<PING>");
    }

    /** True if Arduino responded to at least one PING */
    bool isConnected() const { return _connected; }

private:
    MotorBridge() = default;
    MotorBridge(const MotorBridge&) = delete;
    MotorBridge& operator=(const MotorBridge&) = delete;

    void _handleResponse(const char* msg) {
        if (strcmp(msg, "OK:PONG") == 0) {
            _connected = true;
            // Don't spam the monitor with pong logs
        } else if (strcmp(msg, "OK:START") == 0) {
            Serial.println("[Bridge] Arduino ACK: Motor started");
        } else if (strcmp(msg, "OK:STOP") == 0) {
            Serial.println("[Bridge] Arduino ACK: Motor stopped");
        } else if (strcmp(msg, "ERR:UNKNOWN") == 0) {
            Serial.printf("[Bridge] Arduino error: unknown command\n");
        } else {
            Serial.printf("[Bridge] Arduino says: %s\n", msg);
        }
    }

    char          _rxBuf[32]   = {0};
    uint8_t       _rxIndex     = 0;
    bool          _rxActive    = false;
    bool          _connected   = false;
    unsigned long _lastPing    = 0;
};

#endif // MOTOR_BRIDGE_H
