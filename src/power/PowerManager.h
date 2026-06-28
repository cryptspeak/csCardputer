#pragma once

#include <Arduino.h>
#include <M5Unified.h>

class PowerManager {
public:
    enum State { ACTIVE, DIMMED, SCREEN_OFF };

    void begin();
    void loop();

    // Call on any user activity (keypress, etc.)
    void activity();

    // Configuration (seconds)
    void setDimTimeout(uint16_t seconds) { _dimTimeout = seconds * 1000UL; }
    void setOffTimeout(uint16_t seconds) { _offTimeout = seconds * 1000UL; }
    void setBrightness(uint8_t brightness) { _fullBrightness = brightness; }

    // Auto-lock: reboot back to the at-rest password screen after this many
    // minutes of inactivity (0 = disabled). Unlike dim/off above — which
    // just blank the display — this discards the already-decrypted identity
    // from RAM, so the real at-rest password must be re-entered on the next
    // boot before the device does anything again. Optional, off by default.
    void setAutoLockTimeout(uint16_t minutes) { _autoLockTimeoutMs = (unsigned long)minutes * 60000UL; }

    State state() const { return _state; }
    bool isScreenOn() const { return _state != SCREEN_OFF; }

private:
    void setState(State newState);

    State _state = ACTIVE;
    unsigned long _lastActivity = 0;
    unsigned long _dimTimeout = 30000;   // 30s
    unsigned long _offTimeout = 60000;   // 60s
    unsigned long _autoLockTimeoutMs = 0; // 0 = disabled
    uint8_t _fullBrightness = 255;
    static constexpr uint8_t DIM_BRIGHTNESS = 64;
};
