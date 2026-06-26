#pragma once

#include <stdint.h>
#include "ui/Screen.h"
#include "ui/widgets/ProgressBar.h"

class BootScreen : public Screen {
public:
    void render(M5Canvas& canvas) override;
    bool handleKey(const KeyEvent& event) override { return false; }
    const char* title() const override { return "Boot"; }

    void setProgress(float progress, const char* status = nullptr);
    bool isComplete() const { return _progress >= 1.0f; }

    // One-time intro animation (mark wipe-in + title decrypt/scramble),
    // timed from the first render() call. main.cpp spins a short render
    // loop for this long right at boot start so the animation actually
    // gets enough frames to read as motion, before any real init work
    // begins — see setup() in main.cpp.
    static constexpr uint32_t INTRO_MS = 950;

private:
    ProgressBar _progressBar;
    float _progress = 0.0f;
    const char* _statusText = "Initializing...";
    uint32_t _introStartMs = 0;
};
