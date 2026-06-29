#include "LoRaInterface.h"
#include "config/BoardConfig.h"
#include <algorithm>

// RNode on-air framing constants (from RNode_Firmware Framing.h / Config.h)
// Every LoRa packet has a 1-byte header: upper nibble = random sequence, lower nibble = flags
#define RNODE_HEADER_L      1
#define RNODE_FLAG_SPLIT    0x01
#define RNODE_NIBBLE_SEQ    0xF0
#define RNODE_SINGLE_MTU    (MAX_PACKET_SIZE - RNODE_HEADER_L)  // 254 bytes payload per frame

LoRaInterface::LoRaInterface(SX1262* radio, const char* name)
    : RNS::InterfaceImpl(name), _radio(radio)
{
    _IN = true;
    _OUT = true;
    _bitrate = 2000;
    // Reticulum MTU (500 bytes) — split-packet framing allows up to 2x254 = 508 bytes
    _HW_MTU = RNS::Type::Reticulum::MTU;
}

LoRaInterface::~LoRaInterface() {
    stop();
}

bool LoRaInterface::start() {
    if (!_radio || !_radio->isRadioOnline()) {
        Serial.println("[LORA_IF] Radio not available");
        _online = false;
        return false;
    }
    _online = true;
    _radio->receive();
    Serial.println("[LORA_IF] Interface started (split-packet enabled, MTU=500)");
    return true;
}

void LoRaInterface::stop() {
    _online = false;
    Serial.println("[LORA_IF] Interface stopped");
}

void LoRaInterface::send_outgoing(const RNS::Bytes& data) {
    if (!_online || !_radio) return;

    // Reject packets exceeding Reticulum MTU (500 bytes)
    if (data.size() > RNS::Type::Reticulum::MTU) {
        Serial.printf("[LORA_IF] TX DROPPED: exceeds Reticulum MTU (%d > %d)\n",
            (int)data.size(), (int)RNS::Type::Reticulum::MTU);
        return;
    }

    // Listen-before-talk: dcd() reports a LoRa preamble/header currently
    // latched on the channel, i.e. some other node is mid-transmission.
    // Transmitting into that is a guaranteed on-air collision -- this
    // single-radio half-duplex link previously had no notion of "someone
    // else is using the channel right now" at all (see SX1262.h's "no
    // CSMA/CA" comment) and would key up regardless. Treat it exactly like
    // the radio being locally busy: queue it, and let loop()'s contention-
    // window drain (below) send it once the channel actually clears.
    if (_txPending || _splitTxPending || _splitRxPending || _radio->dcd()) {
        if ((int)_txQueue.size() < TX_QUEUE_MAX) {
            _txQueue.push_back(data);
            if (_splitRxPending) {
                Serial.printf("[LORA_IF] TX deferred (split RX pending, %d in queue)\n", (int)_txQueue.size());
            } else {
                Serial.printf("[LORA_IF] TX queued (%d in queue)\n", (int)_txQueue.size());
            }
        } else {
            Serial.println("[LORA_IF] TX queue full, dropping oldest");
            _txQueue.pop_front();
            _txQueue.push_back(data);
            _txQueueDropCount++;
        }
        return;
    }

    transmitNow(data);
}

// A handful of random symbol-time slots, bounded the same way RNode's CSMA
// implementation bounds its slot time (CSMA_SLOT_MIN_MS/CSMA_SLOT_MAX_MS) --
// short enough not to add noticeable latency at typical SF/BW settings,
// long enough that two nodes which both deferred for the same busy channel
// don't pick the same instant to retry once it clears.
unsigned long LoRaInterface::csmaContentionWindowMs() const {
    float symbolTimeMs = 1000.0f * (float)(1UL << _radio->getSpreadingFactor())
                          / (float)_radio->getSignalBandwidth();
    float slotMs = symbolTimeMs * 12.0f;  // CSMA_SLOT_SYMBOLS, RNode convention
    if (slotMs < 24.0f) slotMs = 24.0f;    // CSMA_SLOT_MIN_MS
    if (slotMs > 100.0f) slotMs = 100.0f;  // CSMA_SLOT_MAX_MS
    return (unsigned long)(slotMs * (float)random(1, 8));
}

void LoRaInterface::transmitNow(const RNS::Bytes& data) {
    uint8_t header = (uint8_t)(random(256)) & RNODE_NIBBLE_SEQ;
    bool needsSplit = (data.size() > RNODE_SINGLE_MTU);

    if (needsSplit) {
        header |= RNODE_FLAG_SPLIT;
        size_t firstLen = RNODE_SINGLE_MTU;
        _splitTxCount++;

        Serial.printf("[LORA_IF] TX SPLIT: %d bytes in 2 frames (seq=0x%02X)\n",
            (int)data.size(), header & RNODE_NIBBLE_SEQ);

        _radio->beginPacket();
        _radio->write(header);
        _radio->write(data.data(), firstLen);
        _radio->endPacket(true);

        // Save remaining data for second frame
        _splitTxPending = true;
        _splitTxRemaining = RNS::Bytes(data.data() + firstLen, data.size() - firstLen);
        _splitTxHeader = header;

        Serial.printf("[LORA_IF] TX SPLIT frame 1: %d+1 bytes (remaining: %d)\n",
            (int)firstLen, (int)_splitTxRemaining.size());
    } else {
        _radio->beginPacket();
        _radio->write(header);
        _radio->write(data.data(), data.size());
        _radio->endPacket(true);

        Serial.printf("[LORA_IF] TX %d+1 bytes (hdr=0x%02X)\n", (int)data.size(), header);
    }

    _txPending = true;
    _txData = data;
    _txPacketCount++;
    _everTx = true;
    _lastTxMs = millis();
    InterfaceImpl::handle_outgoing(data);

    // Track airtime
    size_t airBytes = needsSplit ? (RNODE_SINGLE_MTU + RNODE_HEADER_L) : (data.size() + RNODE_HEADER_L);
    float airtimeMs = _radio->getAirtime(airBytes);
    unsigned long txNow = millis();
    if (txNow - _airtimeWindowStart >= AIRTIME_WINDOW_MS) {
        _airtimeAccumMs = 0;
        _airtimeWindowStart = txNow;
    } else {
        float elapsed = (float)(txNow - _airtimeWindowStart);
        float remaining = 1.0f - (elapsed / AIRTIME_WINDOW_MS);
        if (remaining < 0) remaining = 0;
        _airtimeAccumMs *= remaining;
        _airtimeWindowStart = txNow;
    }
    _airtimeAccumMs += airtimeMs;
}

void LoRaInterface::loop() {
    if (!_online || !_radio) return;

    // Handle async TX completion
    if (_txPending) {
        if (!_radio->isTxBusy()) {
            _txPending = false;

            // Async TX timed out in the radio (not just a delayed loop() poll —
            // isTxBusy() already accounts for that). Retry the whole original
            // packet once before giving up; _txData holds it unsplit regardless
            // of whether this was frame 1 or frame 2 of a split send.
            if (_radio->lastTxFailed()) {
                _splitTxPending = false;
                _splitTxRemaining = RNS::Bytes();

                if (!_txRetried && !_txData.empty()) {
                    _txRetried = true;
                    Serial.println("[LORA_IF] TX timed out, retrying once");
                    RNS::Bytes retryData = _txData;
                    transmitNow(retryData);
                    return;
                }

                Serial.println("[LORA_IF] TX timed out after retry, dropping packet");
                _txFailureCount++;
                _txRetried = false;
                _txData = RNS::Bytes();

                // Re-arm RX unconditionally -- a queued packet goes out via
                // the DCD/contention-window drain further down in loop(),
                // on this same or the very next tick, rather than straight
                // out of this retry path -- so it gets the same listen-
                // before-talk treatment as any other queued send instead of
                // skipping it.
                _radio->receive();
                return;
            }
            _txRetried = false;

            // If split TX pending, send the second frame immediately
            if (_splitTxPending) {
                _splitTxPending = false;

                size_t frame2Size = _splitTxRemaining.size();
                Serial.printf("[LORA_IF] TX SPLIT frame 2: %d+1 bytes\n",
                    (int)frame2Size);

                _radio->beginPacket();
                _radio->write(_splitTxHeader);
                _radio->write(_splitTxRemaining.data(), frame2Size);
                _radio->endPacket(true);

                _txPending = true;
                _splitTxRemaining = RNS::Bytes();

                float airtimeMs = _radio->getAirtime(frame2Size + RNODE_HEADER_L);
                _airtimeAccumMs += airtimeMs;
                return;
            }

            _txData = RNS::Bytes();

            // Re-arm RX unconditionally -- see the comment on the same
            // pattern in the TX-timeout branch above; any queued packet
            // goes out via the DCD/contention-window drain below, not
            // immediately here.
            _radio->receive();
        }
        return;
    }

    // Split RX timeout: discard stale partial packets. Any queued TX picks
    // itself up via the DCD/contention-window drain below now that
    // _splitRxPending is clear, rather than going out immediately here.
    if (_splitRxPending && (millis() - _splitRxTimestamp > SPLIT_RX_TIMEOUT_MS)) {
        Serial.println("[LORA_IF] RX SPLIT timeout, discarding partial");
        _splitRxPending = false;
        _splitRxBuffer = RNS::Bytes();
    }

    // Drain the TX queue once the channel clears. Items can land here
    // purely because dcd() reported the channel busy at send_outgoing()
    // time (see there) -- unlike a TX-done or split-RX-done event, there's
    // no IRQ for "carrier no longer detected", so this poll (run every
    // loop() tick, i.e. _txPending is already known false at this point)
    // is what notices the channel clearing and, after a randomized
    // contention window so that every node which deferred for the same
    // busy channel doesn't retry at the same instant, actually sends.
    // Skipped while a packet is sitting unread in the radio so a queued
    // send can never race a receive that's still in progress this tick.
    if (!_txQueue.empty() && !_radio->packetAvailable) {
        if (_radio->dcd()) {
            _txBackoffPending = false;  // busy again -- wait for clear before re-arming a window
        } else if (!_txBackoffPending) {
            _txBackoffPending = true;
            _txBackoffUntilMs = millis() + csmaContentionWindowMs();
        } else if (millis() >= _txBackoffUntilMs) {
            _txBackoffPending = false;
            if (!_radio->dcd()) {  // re-check -- still clear after the wait
                RNS::Bytes next = _txQueue.front();
                _txQueue.pop_front();
                transmitNow(next);
                return;
            }
            // else: someone else took the channel during the wait -- the
            // dcd()-busy branch above picks this back up next tick.
        }
    }

    // Periodic RX debug
    static unsigned long lastRxDebug = 0;
    if (millis() - lastRxDebug > 30000) {
        lastRxDebug = millis();
        int rssi = _radio->currentRssi();
        uint8_t status = _radio->getStatus();
        uint8_t chipMode = (status >> 4) & 0x07;
        Serial.printf("[LORA_IF] RX: RSSI=%d dBm, status=0x%02X(mode=%d)\n",
            rssi, status, chipMode);
    }

    if (!_radio->packetAvailable) return;
    _radio->packetAvailable = false;

    int packetSize = _radio->parsePacket();
    if (packetSize <= RNODE_HEADER_L) {
        if (_radio->lastRxCrcFailed()) {
            // Heard *something* — preamble + header detected — but the CRC
            // didn't check out. Distinct from total silence: a high rate of
            // these alongside low rxPacketCount means weak/distant senders
            // are reaching the radio but not cleanly, not that nothing is
            // arriving at all. See RadioDiagnosticsScreen's Counters page.
            _rxCrcFailCount++;
        } else if (packetSize > 0) {
            Serial.printf("[LORA_IF] RX runt packet (%d bytes), discarding\n", packetSize);
            _rxDroppedCount++;
        }
        _radio->receive();
        return;
    }

    uint8_t raw[MAX_PACKET_SIZE];
    memcpy(raw, _radio->packetBuffer(), packetSize);

    // Capture signal quality before any further processing
    _lastRxRssi = _radio->packetRssi();
    _lastRxSnr = _radio->packetSnr();

    uint8_t header = raw[0];
    int payloadSize = packetSize - RNODE_HEADER_L;
    uint8_t seq = header & RNODE_NIBBLE_SEQ;
    bool isSplit = (header & RNODE_FLAG_SPLIT) != 0;

    if (isSplit) {
        if (!_splitRxPending) {
            _splitRxPending = true;
            _splitRxSeq = seq;
            _splitRxBuffer = RNS::Bytes(raw + RNODE_HEADER_L, payloadSize);
            _splitRxTimestamp = millis();

            Serial.printf("[LORA_IF] RX SPLIT frame 1: %d bytes (seq=0x%02X), RSSI=%d, SNR=%.1f\n",
                payloadSize, seq, _lastRxRssi, _lastRxSnr);
            _radio->receive();
            return;
        } else if (seq == _splitRxSeq) {
            Serial.printf("[LORA_IF] RX SPLIT frame 2: %d bytes (seq=0x%02X), RSSI=%d, SNR=%.1f\n",
                payloadSize, seq, _lastRxRssi, _lastRxSnr);

            _splitRxBuffer.append(raw + RNODE_HEADER_L, payloadSize);
            int totalSize = _splitRxBuffer.size();
            _splitRxPending = false;

            Serial.printf("[LORA_IF] RX SPLIT reassembled: %d bytes total\n", totalSize);

            _splitRxCount++;
            _rxPacketCount++;
            _everRx = true;
            _lastRxMs = millis();
            InterfaceImpl::handle_incoming(_splitRxBuffer);
            _splitRxBuffer = RNS::Bytes();

            // Re-arm RX unconditionally -- as elsewhere, a queued packet
            // goes out via the DCD/contention-window drain further down in
            // loop(), not immediately here.
            if (!_txPending) _radio->receive();
            return;
        } else {
            Serial.printf("[LORA_IF] RX SPLIT new seq (had 0x%02X, got 0x%02X), previous frame 2 lost\n",
                _splitRxSeq, seq);
            _splitRxSeq = seq;
            _splitRxBuffer = RNS::Bytes(raw + RNODE_HEADER_L, payloadSize);
            _splitRxTimestamp = millis();
            _radio->receive();
            return;
        }
    }

    if (_splitRxPending) {
        Serial.printf("[LORA_IF] RX non-split %d bytes while awaiting split frame 2 (kept)\n", payloadSize);
    }

    Serial.printf("[LORA_IF] RX %d bytes (hdr=0x%02X, payload=%d), RSSI=%d, SNR=%.1f\n",
                  packetSize, header, payloadSize,
                  _lastRxRssi, _lastRxSnr);

    RNS::Bytes buf(payloadSize);
    memcpy(buf.writable(payloadSize), raw + RNODE_HEADER_L, payloadSize);
    _rxPacketCount++;
    _everRx = true;
    _lastRxMs = millis();
    InterfaceImpl::handle_incoming(buf);

    if (!_txPending) {
        _radio->receive();
    }
}

float LoRaInterface::airtimeUtilization() const {
    if (_airtimeAccumMs <= 0) return 0;
    unsigned long elapsed = millis() - _airtimeWindowStart;
    if (elapsed == 0) elapsed = 1;
    float windowMs = std::min((float)elapsed, (float)AIRTIME_WINDOW_MS);
    return _airtimeAccumMs / windowMs;
}
