#pragma once

#include <Arduino.h>
#include "config/Config.h"

#if HAS_GPS

#include <HardwareSerial.h>
#include <Preferences.h>
#include "hal/NMEAParser.h"
#include "config/BoardConfig.h"

class GPSManager {
public:
    void begin();
    void loop();       // Non-blocking: reads available UART bytes, feeds parser
    void stop();       // Disable UART, clear state

    // Time
    bool hasTimeFix() const { return _timeValid; }

    // Local time, derived either from TimeZoneDB (real POSIX DST rules, if
    // the GPS fix falls inside a covered region) or a coarse longitude +
    // hemisphere/month DST guess otherwise — see updateLocalTimeZone().
    // UTC until the first fix arrives or a prior estimate is restored from
    // NVS. Overridden entirely by setManualOffset() when manual mode is on.
    int8_t localOffsetHours() const { return _localOffsetHours; }
    const char* posixTZ() const { return _posixTZ; }

    // Manual override — when enabled, ignores GPS position entirely and
    // applies this fixed UTC offset (no DST auto-adjustment) instead.
    void setManualOffset(bool enabled, int8_t offsetHours);

    // Location
    bool hasLocationFix() const { return _locationValid; }
    double latitude() const { return _parser.data().latitude; }
    double longitude() const { return _parser.data().longitude; }
    double altitude() const { return _parser.data().altitude; }
    int satellites() const { return _parser.data().satellites; }
    double hdop() const { return _parser.data().hdop; }
    uint8_t fixQuality() const { return _parser.data().fixQuality; }

    // Diagnostics
    uint32_t charsProcessed() const { return _parser.charsProcessed(); }
    uint32_t sentencesParsed() const { return _parser.sentencesParsed(); }
    uint32_t fixAgeMs() const;
    uint32_t timeSyncCount() const { return _timeSyncCount; }
    bool isRunning() const { return _running; }

    // Configuration (set from outside before or after begin())
    void setLocationEnabled(bool enable) { _parser.setParseLocation(enable); }

private:
    void syncSystemTime();
    void updateLocalTimeZone(double latitude, double longitude, uint8_t month);
    void applyPosixTZ(const char* tz);
    void restoreTimeFromNVS();
    void persistToNVS();
    bool tryBaudRate(uint32_t baud);

    NMEAParser _parser;
    HardwareSerial _serial{2};   // UART2 on ESP32-S3
    bool _running = false;
    bool _timeValid = false;
    bool _locationValid = false;
    unsigned long _lastTimeSyncMs = 0;
    unsigned long _lastFixMs = 0;
    unsigned long _lastPersistMs = 0;
    uint32_t _timeSyncCount = 0;
    char _posixTZ[48] = "UTC0";  // Active POSIX TZ string (may carry real DST rules)
    int8_t _localOffsetHours = 0;
    bool _manualOverride = false;
    uint32_t _detectedBaud = 0;

    // Baud auto-detect state
    bool _baudDetected = false;
    int _baudAttemptIdx = 0;
    unsigned long _baudAttemptStart = 0;
    static constexpr uint32_t BAUD_RATES[] = {38400, 115200, 9600};
    static constexpr int BAUD_RATE_COUNT = 3;
    static constexpr unsigned long BAUD_DETECT_TIMEOUT_MS = 3000;

    static constexpr unsigned long TIME_SYNC_INTERVAL_MS = 60000;   // Re-sync every 60s
    static constexpr unsigned long PERSIST_INTERVAL_MS = 300000;     // Persist to NVS every 5 min
    static constexpr unsigned long FIX_TIMEOUT_MS = 10000;           // Fix stale after 10s

    static constexpr const char* NVS_NAMESPACE = "gps";
};

#endif // HAS_GPS
