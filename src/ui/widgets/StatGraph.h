#pragma once

#include <M5GFX.h>

// Fixed-size scrolling dual-series line graph — e.g. LoRa TX/RX throughput
// over time. Self-scaling: the Y axis always stretches to whichever series
// has the higher current sample, so it stays readable whether traffic is
// bytes/sec or kilobytes/sec.
class StatGraph {
public:
    static constexpr int MAX_SAMPLES = 60;

    void pushSample(float a, float b);
    void clear();

    void render(M5Canvas& canvas, int x, int y, int w, int h,
                uint16_t colorA, uint16_t colorB, uint16_t axisColor) const;

private:
    float _a[MAX_SAMPLES] = {};
    float _b[MAX_SAMPLES] = {};
    int _head = 0;  // index of the next sample to be written (= oldest sample)
};
