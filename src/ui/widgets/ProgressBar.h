#pragma once

#include <M5GFX.h>
#include "ui/Theme.h"

class ProgressBar {
public:
    // fade: 0 = invisible (blended fully to Theme::BG), 1 = normal colors.
    // Lets callers (e.g. the boot screen's reveal) fade the bar in over
    // time instead of having it pop in at full brightness.
    void render(M5Canvas& canvas, int x, int y, int w, int h, float fade = 1.0f);

    void setProgress(float progress);  // 0.0 to 1.0
    float getProgress() const { return _progress; }

private:
    float _progress = 0.0f;
};
