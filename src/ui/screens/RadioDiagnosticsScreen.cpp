#include "RadioDiagnosticsScreen.h"
#include "ui/Theme.h"
#include "transport/LoRaInterface.h"
#include <Arduino.h>
#include <cstdarg>

namespace {

// SX126x GetDeviceErrors bit names (Semtech DS_SX1261-2 ch13.3.5). Decoded
// rather than shown as a raw hex blob — "PLL_LOCK" tells you something
// actionable about radio health, a bitmask doesn't.
const char* const kDeviceErrorNames[] = {
    "RC64K", "RC13M", "PLL_CAL", "ADC_CAL", "IMG_CAL", "XOSC", "PLL_LOCK"
};

void formatDeviceErrors(uint16_t bits, char* out, size_t outLen) {
    if (bits == 0) {
        snprintf(out, outLen, "OK");
        return;
    }
    out[0] = '\0';
    bool first = true;
    for (int i = 0; i < 7; i++) {
        if (bits & (1 << i)) {
            size_t len = strlen(out);
            snprintf(out + len, outLen - len, "%s%s", first ? "" : ",", kDeviceErrorNames[i]);
            first = false;
        }
    }
    if (bits & 0x100) {
        size_t len = strlen(out);
        snprintf(out + len, outLen - len, "%s%s", first ? "" : ",", "PA_RAMP");
        first = false;
    }
    // Bits outside the known set are reserved and shouldn't ever be set —
    // but if a glitched SPI read produces one, fall back to the raw value
    // rather than silently showing an empty (yet still error-colored) string.
    if (first) {
        snprintf(out, outLen, "0x%04X", bits);
    }
}

// Bytes/sec -> short human string ("0 B/s", "1.2 KB/s")
void formatRate(float bytesPerSec, char* out, size_t outLen) {
    if (bytesPerSec >= 1024.0f) {
        snprintf(out, outLen, "%.1fKB/s", bytesPerSec / 1024.0f);
    } else {
        snprintf(out, outLen, "%dB/s", (int)(bytesPerSec + 0.5f));
    }
}

}  // namespace

void RadioDiagnosticsScreen::onEnter() {
    _page = PAGE_LIVE;
    _graph.clear();
    _lastSampleMs = 0;
    LoRaInterface* lif = _rns ? _rns->loraInterface() : nullptr;
    _prevTxBytes = lif ? lif->txBytesTotal() : 0;
    _prevRxBytes = lif ? lif->rxBytesTotal() : 0;
}

void RadioDiagnosticsScreen::tick() {
    unsigned long now = millis();
    if (_lastSampleMs != 0 && now - _lastSampleMs < SAMPLE_INTERVAL_MS) return;

    LoRaInterface* lif = _rns ? _rns->loraInterface() : nullptr;
    unsigned long dt = _lastSampleMs == 0 ? SAMPLE_INTERVAL_MS : (now - _lastSampleMs);
    _lastSampleMs = now;

    if (lif) {
        size_t txb = lif->txBytesTotal();
        size_t rxb = lif->rxBytesTotal();
        _txRate = (float)(txb - _prevTxBytes) * 1000.0f / (float)dt;
        _rxRate = (float)(rxb - _prevRxBytes) * 1000.0f / (float)dt;
        _prevTxBytes = txb;
        _prevRxBytes = rxb;
        _graph.pushSample(_txRate, _rxRate);
    }

    if (_radio && _radio->isRadioOnline()) {
        _noiseFloor = _radio->currentRssi();
        _deviceErrors = _radio->getDeviceErrors();
    }

    // The user is actively watching a live graph, not pressing keys — don't
    // let the normal inactivity dim/freeze the display out from under them
    // (see UIManager::render()/main.cpp, which skips rendering entirely once
    // PowerManager reaches DIMMED). Leaving this screen lets normal dimming
    // resume.
    if (_power) _power->activity();

    if (_ui) _ui->markContentDirty();
}

bool RadioDiagnosticsScreen::handleKey(const KeyEvent& event) {
    if (event.character == 27) {  // Esc
        if (_backCb) _backCb();
        return true;
    }
    if (event.character == ';') {
        _page = (_page - 1 + PAGE_COUNT) % PAGE_COUNT;
        return true;
    }
    if (event.character == '.') {
        _page = (_page + 1) % PAGE_COUNT;
        return true;
    }
    if ((event.character == 'c' || event.character == 'C') && _page == PAGE_COUNTERS) {
        if (_radio) {
            _radio->clearDeviceErrors();
            _deviceErrors = 0;
        }
        return true;
    }
    return false;
}

void RadioDiagnosticsScreen::render(M5Canvas& canvas) {
    int y0 = Theme::CONTENT_Y;
    const int headerH = Theme::SECTION_HEADER_H;
    canvas.fillRect(0, y0, Theme::CONTENT_W, headerH, Theme::BG_SURFACE);
    canvas.fillRect(0, y0 + 2, 3, headerH - 4, Theme::ACCENT);
    Theme::useUiFont(canvas);
    canvas.setTextColor(Theme::ACCENT);

    static const char* const kPageTitles[PAGE_COUNT] = {"Live", "Counters", "Network"};
    char titleBuf[40];
    snprintf(titleBuf, sizeof(titleBuf), "Diagnostics: %s (%d/%d)", kPageTitles[_page], _page + 1, (int)PAGE_COUNT);
    canvas.drawString(titleBuf, 8, y0 + 2);
    canvas.drawFastHLine(0, y0 + headerH, Theme::CONTENT_W, Theme::DIVIDER);

    Theme::useSmallFont(canvas);
    int y = y0 + headerH + 3;

    switch (_page) {
        case PAGE_LIVE:     renderLive(canvas, y); break;
        case PAGE_COUNTERS: renderCounters(canvas, y); break;
        case PAGE_NETWORK:  renderNetwork(canvas, y); break;
    }
}

void RadioDiagnosticsScreen::renderLive(M5Canvas& canvas, int y) {
    LoRaInterface* lif = _rns ? _rns->loraInterface() : nullptr;

    // Throughput graph
    int gx = 4, gw = Theme::CONTENT_W - 8, gh = 30;
    _graph.render(canvas, gx, y, gw, gh, Theme::PRIMARY, Theme::ACCENT, Theme::DIVIDER);
    y += gh + 3;

    char txStr[16], rxStr[16];
    formatRate(_txRate, txStr, sizeof(txStr));
    formatRate(_rxRate, rxStr, sizeof(rxStr));
    canvas.fillRect(4, y + 1, 5, 5, Theme::PRIMARY);
    canvas.setTextColor(Theme::TEXT_PRIMARY);
    canvas.setCursor(11, y);
    canvas.printf("TX %s", txStr);
    canvas.fillRect(110, y + 1, 5, 5, Theme::ACCENT);
    canvas.setCursor(117, y);
    canvas.printf("RX %s", rxStr);
    y += 10;

    canvas.setTextColor(Theme::SECONDARY);
    canvas.setCursor(4, y);
    if (lif) {
        canvas.printf("RSSI %ddBm SNR %.1fdB Noise %ddBm", lif->lastRxRssi(), lif->lastRxSnr(), _noiseFloor);
    } else {
        canvas.print("RSSI --  SNR --  Noise --");
    }
    y += 10;

    float airtime = lif ? lif->airtimeUtilization() : 0.0f;
    uint16_t airtimeColor = Theme::SUCCESS;
    if (airtime >= LoRaInterface::AIRTIME_THROTTLE) airtimeColor = Theme::ERROR;
    else if (airtime >= LoRaInterface::AIRTIME_THROTTLE * 0.6f) airtimeColor = Theme::WARNING;
    canvas.setTextColor(airtimeColor);
    canvas.setCursor(4, y);
    canvas.printf("Airtime %.0f%% (throttle %.0f%%)", airtime * 100.0f, LoRaInterface::AIRTIME_THROTTLE * 100.0f);
    y += 10;

    // "Chip" = the SX1262 silicon itself (isRadioOnline(): did it init and
    // respond on SPI). "Link" = the Reticulum-side LoRaInterface wrapping
    // it (isOnline(): has Transport actually started/attached it). They're
    // almost always in lockstep on this firmware, but they CAN diverge —
    // e.g. chip present and healthy while the interface hasn't been started
    // yet — so they're tracked and shown separately rather than collapsed
    // into one "online" flag.
    bool chipOn = _radio && _radio->isRadioOnline();
    bool linkOn = lif && lif->isOnline();
    canvas.fillCircle(8, y + 4, 3, chipOn ? Theme::SUCCESS : Theme::ERROR);
    canvas.setTextColor(Theme::TEXT_PRIMARY);
    canvas.setCursor(14, y);
    canvas.printf("Chip %s", chipOn ? "on" : "OFF");
    canvas.fillCircle(80, y + 4, 3, linkOn ? Theme::SUCCESS : Theme::ERROR);
    canvas.setCursor(86, y);
    canvas.printf("Link %s", linkOn ? "on" : "OFF");
    y += 10;

    canvas.setTextColor(Theme::MUTED);
    canvas.setCursor(4, y);
    if (lif) {
        long rxAge = lif->msSinceLastRx(), txAge = lif->msSinceLastTx();
        char rxAgeStr[16], txAgeStr[16];
        if (rxAge < 0) snprintf(rxAgeStr, sizeof(rxAgeStr), "never");
        else snprintf(rxAgeStr, sizeof(rxAgeStr), "%lds ago", rxAge / 1000);
        if (txAge < 0) snprintf(txAgeStr, sizeof(txAgeStr), "never");
        else snprintf(txAgeStr, sizeof(txAgeStr), "%lds ago", txAge / 1000);
        canvas.printf("Last RX %s   Last TX %s", rxAgeStr, txAgeStr);
    }
}

void RadioDiagnosticsScreen::renderCounters(M5Canvas& canvas, int y) {
    LoRaInterface* lif = _rns ? _rns->loraInterface() : nullptr;
    canvas.setTextColor(Theme::TEXT_PRIMARY);

    auto row = [&](const char* fmt, ...) {
        canvas.setCursor(4, y);
        char buf[64];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        canvas.print(buf);
        y += 10;
    };

    if (!lif) {
        row("No LoRa interface");
        return;
    }

    row("TX pkts %lu   RX pkts %lu", (unsigned long)lif->txPacketCount(), (unsigned long)lif->rxPacketCount());
    canvas.setTextColor(lif->txFailureCount() > 0 ? Theme::WARNING : Theme::TEXT_PRIMARY);
    row("TX fail %lu   RX runt %lu", (unsigned long)lif->txFailureCount(), (unsigned long)lif->rxDroppedCount());

    // CRC failures = preamble+header WERE detected but the payload didn't
    // check out — i.e. something is reaching the radio, just not cleanly.
    // High count here with low RX pkts points at a weak/marginal link
    // (distance, antenna, interference), not a missing/blocked signal.
    uint32_t crcFail = lif->rxCrcFailCount();
    uint32_t rxAttempts = crcFail + lif->rxPacketCount() + lif->rxDroppedCount();
    float crcFailPct = rxAttempts > 0 ? (100.0f * crcFail / rxAttempts) : 0.0f;
    canvas.setTextColor(crcFail > 0 ? Theme::WARNING : Theme::TEXT_PRIMARY);
    row("RX CRC fail %lu (%.0f%%)", (unsigned long)crcFail, crcFailPct);
    canvas.setTextColor(Theme::TEXT_PRIMARY);
    row("Split TX %lu   Split RX %lu", (unsigned long)lif->splitTxCount(), (unsigned long)lif->splitRxCount());
    canvas.setTextColor(lif->txQueueDropCount() > 0 ? Theme::WARNING : Theme::TEXT_PRIMARY);
    row("TX queue %d/4   drops %lu", lif->txQueueDepth(), (unsigned long)lif->txQueueDropCount());

    char errStr[40];
    formatDeviceErrors(_deviceErrors, errStr, sizeof(errStr));
    canvas.setTextColor(_deviceErrors == 0 ? Theme::SUCCESS : Theme::ERROR);
    row("Device errors: %s", errStr);

    canvas.setTextColor(Theme::MUTED);
    row("[c] clear device errors");
}

void RadioDiagnosticsScreen::renderNetwork(M5Canvas& canvas, int y) {
    canvas.setTextColor(Theme::TEXT_PRIMARY);

    auto row = [&](const char* fmt, ...) {
        canvas.setCursor(4, y);
        char buf[64];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        canvas.print(buf);
        y += 10;
    };

    if (_radio) {
        row("%.3fMHz SF%d BW%lu CR4/%d", _radio->getFrequency() / 1e6,
            _radio->getSpreadingFactor(), (unsigned long)_radio->getSignalBandwidth(), _radio->getCodingRate4());
        row("TX power %ddBm   Preamble %ld", _radio->getTxPower(), _radio->getPreambleLength());
    }

    if (_rns) {
        row("Paths %u   Links %u", (unsigned)_rns->pathCount(), (unsigned)_rns->linkCount());
        unsigned long lastAnn = _rns->lastAnnounceTime();
        if (lastAnn > 0) {
            row("Last announce %lus ago", (millis() - lastAnn) / 1000);
        } else {
            row("Last announce: never");
        }
    }

    if (_announce) {
        row("Known peers %d", _announce->nodeCount());
    }

    if (_lxmf) {
        row("LXMF outbox %d", _lxmf->queuedCount());
    }

    // Min-free-heap (since last heartbeat) catches slow leaks/fragmentation
    // that current-free-heap alone would hide between two healthy-looking
    // snapshots. Max loop time matters because a starved main loop is a
    // real, separate cause of radio symptoms (missed IRQs, late TX/RX
    // servicing) that's easy to misdiagnose as a radio/antenna problem.
    canvas.setTextColor(Theme::MUTED);
    row("Heap %luKB  min %luKB", (unsigned long)(ESP.getFreeHeap() / 1024), (unsigned long)(_minFreeHeap / 1024));
    row("Loop max %lums   Up %lus", _maxLoopMs, millis() / 1000);
}
