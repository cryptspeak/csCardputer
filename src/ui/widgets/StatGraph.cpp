#include "StatGraph.h"

void StatGraph::pushSample(float a, float b) {
    _a[_head] = a;
    _b[_head] = b;
    _head = (_head + 1) % MAX_SAMPLES;
}

void StatGraph::clear() {
    for (int i = 0; i < MAX_SAMPLES; i++) { _a[i] = 0; _b[i] = 0; }
    _head = 0;
}

void StatGraph::render(M5Canvas& canvas, int x, int y, int w, int h,
                        uint16_t colorA, uint16_t colorB, uint16_t axisColor) const {
    float maxVal = 1.0f;  // floor avoids div-by-zero and keeps a flat line at idle
    for (int i = 0; i < MAX_SAMPLES; i++) {
        if (_a[i] > maxVal) maxVal = _a[i];
        if (_b[i] > maxVal) maxVal = _b[i];
    }

    int baseY = y + h - 1;
    canvas.drawFastHLine(x, baseY, w, axisColor);

    auto plot = [&](const float* series, uint16_t color) {
        int prevX = x, prevY = baseY;
        for (int i = 0; i < MAX_SAMPLES; i++) {
            int idx = (_head + i) % MAX_SAMPLES;
            int px = x + (i * (w - 1)) / (MAX_SAMPLES - 1);
            float frac = series[idx] / maxVal;
            if (frac > 1.0f) frac = 1.0f;
            int py = baseY - (int)(frac * (h - 1));
            if (i > 0) canvas.drawLine(prevX, prevY, px, py, color);
            prevX = px;
            prevY = py;
        }
    };

    // B (RX) drawn first so A (TX) renders on top where they overlap.
    plot(_b, colorB);
    plot(_a, colorA);
}
