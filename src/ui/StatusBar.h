#pragma once

#include <M5GFX.h>

class StatusBar {
public:
    void render(M5Canvas& canvas);

    void setDirtyFlag(bool* flag) { _dirty = flag; }
    void setLoRaOnline(bool online) { _loraOnline = online; if (_dirty) *_dirty = true; }
    void setTCPConnected(bool connected) { _tcpConnected = connected; if (_dirty) *_dirty = true; }
    void setGPSTimeFix(bool fix) { _gpsTimeFix = fix; if (_dirty) *_dirty = true; }
    // -1 = disabled (hide); 0 = idle; >0 = N peers reachable
    void setAutoIfacePeers(int n) { if (_autoIfacePeers != n) { _autoIfacePeers = n; if (_dirty) *_dirty = true; } }

private:
    bool _loraOnline = false;
    bool _tcpConnected = false;
    bool _gpsTimeFix = false;
    int _autoIfacePeers = -1;
    float _smoothedBattery = -1.0f;
    unsigned long _lastBatteryRead = 0;
    bool* _dirty = nullptr;
};
