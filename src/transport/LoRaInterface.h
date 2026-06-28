#pragma once

#include <Interface.h>
#include "radio/SX1262.h"
#include <deque>

class LoRaInterface : public RNS::InterfaceImpl {
public:
    LoRaInterface(SX1262* radio, const char* name = "LoRaInterface");
    virtual ~LoRaInterface();

    virtual bool start() override;
    virtual void stop() override;
    virtual void loop() override;

    virtual inline std::string toString() const override {
        return "LoRaInterface[" + _name + "]";
    }

    float airtimeUtilization() const;

    int lastRxRssi() const { return _lastRxRssi; }
    float lastRxSnr() const { return _lastRxSnr; }
    bool isOnline() const { return _online; }

    // --- Diagnostics counters (see ui/screens/RadioDiagnosticsScreen) ---
    size_t rxBytesTotal() const { return _rxb; }
    size_t txBytesTotal() const { return _txb; }
    uint32_t txPacketCount() const { return _txPacketCount; }
    uint32_t rxPacketCount() const { return _rxPacketCount; }
    uint32_t txFailureCount() const { return _txFailureCount; }
    uint32_t rxDroppedCount() const { return _rxDroppedCount; }
    uint32_t rxCrcFailCount() const { return _rxCrcFailCount; }
    uint32_t txQueueDropCount() const { return _txQueueDropCount; }
    uint32_t splitTxCount() const { return _splitTxCount; }
    uint32_t splitRxCount() const { return _splitRxCount; }
    int txQueueDepth() const { return (int)_txQueue.size(); }

    // ms since the last successful TX/RX, or -1 if none yet this session —
    // "is the link still alive" is a more useful signal at a glance than
    // raw counters, especially for spotting a link that just went quiet.
    long msSinceLastTx() const { return _everTx ? (long)(millis() - _lastTxMs) : -1; }
    long msSinceLastRx() const { return _everRx ? (long)(millis() - _lastRxMs) : -1; }

protected:
    virtual void send_outgoing(const RNS::Bytes& data) override;

private:
    void transmitNow(const RNS::Bytes& data);

    SX1262* _radio;
    bool _txPending = false;
    RNS::Bytes _txData;
    bool _txRetried = false;  // one-shot retry on async TX timeout

    // TX queue: buffer packets when radio is busy instead of dropping
    static constexpr int TX_QUEUE_MAX = 4;
    std::deque<RNS::Bytes> _txQueue;

    // Split-packet TX state: when a packet > 254 bytes, send in two LoRa frames
    bool _splitTxPending = false;
    RNS::Bytes _splitTxRemaining;
    uint8_t _splitTxHeader = 0;

    // Split-packet RX state: reassemble two LoRa frames into one Reticulum packet
    static constexpr unsigned long SPLIT_RX_TIMEOUT_MS = 5000;
    bool _splitRxPending = false;
    uint8_t _splitRxSeq = 0;
    RNS::Bytes _splitRxBuffer;
    unsigned long _splitRxTimestamp = 0;

    int _lastRxRssi = 0;
    float _lastRxSnr = 0;

    unsigned long _airtimeWindowStart = 0;
    float _airtimeAccumMs = 0;
    static constexpr unsigned long AIRTIME_WINDOW_MS = 60000;

    // Diagnostics counters — cumulative since boot, never reset
    uint32_t _txPacketCount = 0;
    uint32_t _rxPacketCount = 0;
    uint32_t _txFailureCount = 0;   // TX timed out even after the one retry
    uint32_t _rxDroppedCount = 0;   // runt packets discarded
    uint32_t _rxCrcFailCount = 0;   // RX_DONE fired but CRC check failed
    uint32_t _txQueueDropCount = 0; // queue was full, oldest entry evicted
    uint32_t _splitTxCount = 0;     // split (2-frame) sends completed
    uint32_t _splitRxCount = 0;     // split (2-frame) receives reassembled

    bool _everTx = false, _everRx = false;
    unsigned long _lastTxMs = 0, _lastRxMs = 0;
public:
    static constexpr float AIRTIME_THROTTLE = 0.25f;
};
