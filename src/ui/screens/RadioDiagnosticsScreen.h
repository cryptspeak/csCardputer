#pragma once

#include "ui/Screen.h"
#include "ui/UIManager.h"
#include "ui/widgets/StatGraph.h"
#include "radio/SX1262.h"
#include "reticulum/ReticulumManager.h"
#include "reticulum/LXMFManager.h"
#include "reticulum/AnnounceManager.h"
#include "power/PowerManager.h"
#include <functional>

// Read-only live view of LoRa PHY health and Reticulum/LXMF traffic —
// reachable from Settings > Radio > Diagnostics. Three pages (;/. to
// cycle): Live (throughput graph + signal quality), Counters (packet/error
// tallies), Network (live PHY config + Reticulum/LXMF mesh stats + system
// health). Pushed/returned the same way MessageView is from MessagesScreen
// — see main.cpp for the open/back callback wiring.
class RadioDiagnosticsScreen : public Screen {
public:
    void render(M5Canvas& canvas) override;
    bool handleKey(const KeyEvent& event) override;
    void onEnter() override;
    void tick() override;
    const char* title() const override { return "Diagnostics"; }

    void setRadio(SX1262* radio) { _radio = radio; }
    void setRNS(ReticulumManager* rns) { _rns = rns; }
    void setLXMF(LXMFManager* lxmf) { _lxmf = lxmf; }
    void setAnnounceManager(AnnounceManager* am) { _announce = am; }
    void setUIManager(UIManager* ui) { _ui = ui; }
    // Calling power->activity() while this screen is open keeps the device
    // from auto-dimming (which also halts rendering — see UIManager/main.cpp)
    // purely because the user hasn't pressed a key, even though they're
    // actively watching a live graph. Same idea as a video screen staying
    // awake while playing.
    void setPower(PowerManager* power) { _power = power; }

    // Snapshot from main.cpp's heartbeat (max loop time + min free heap
    // since the last heartbeat) — reuses instrumentation that already
    // exists there instead of duplicating a timer in this screen.
    void setSystemStats(unsigned long maxLoopMs, uint32_t minFreeHeap) {
        _maxLoopMs = maxLoopMs;
        _minFreeHeap = minFreeHeap;
    }

    using BackCallback = std::function<void()>;
    void setBackCallback(BackCallback cb) { _backCb = cb; }

private:
    enum Page { PAGE_LIVE = 0, PAGE_COUNTERS, PAGE_NETWORK, PAGE_COUNT };

    void renderLive(M5Canvas& canvas, int y);
    void renderCounters(M5Canvas& canvas, int y);
    void renderNetwork(M5Canvas& canvas, int y);

    SX1262* _radio = nullptr;
    ReticulumManager* _rns = nullptr;
    LXMFManager* _lxmf = nullptr;
    AnnounceManager* _announce = nullptr;
    UIManager* _ui = nullptr;
    PowerManager* _power = nullptr;
    BackCallback _backCb;

    int _page = PAGE_LIVE;
    StatGraph _graph;

    // Throughput sampling (byte-counter deltas -> bytes/sec)
    static constexpr unsigned long SAMPLE_INTERVAL_MS = 400;
    unsigned long _lastSampleMs = 0;
    size_t _prevTxBytes = 0;
    size_t _prevRxBytes = 0;
    float _txRate = 0.0f;
    float _rxRate = 0.0f;

    // Polled at the same cadence as the throughput sample, only while this
    // screen is on-screen (tick() only fires for the active Screen) — keeps
    // the extra SPI register reads off the hot path the rest of the time.
    int _noiseFloor = 0;
    uint16_t _deviceErrors = 0;

    unsigned long _maxLoopMs = 0;
    uint32_t _minFreeHeap = 0;
};
