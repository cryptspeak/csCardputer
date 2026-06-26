#pragma once

#include <M5GFX.h>
#include "input/Keyboard.h"

class Screen {
public:
    virtual ~Screen() = default;

    // Called when this screen becomes active
    virtual void onEnter() {}

    // Called when this screen is deactivated
    virtual void onExit() {}

    // Render content area to the canvas
    virtual void render(M5Canvas& canvas) = 0;

    // Handle key event. Return true if consumed.
    virtual bool handleKey(const KeyEvent& event) { return false; }

    // Called once per main-loop iteration regardless of render/dirty state —
    // for logic that needs continuous polling rather than discrete key
    // events (e.g. an accelerating hold-to-repeat control). No-op by default;
    // cheap to call unconditionally on whichever screen is active.
    virtual void tick() {}

    // Screen title for tab/title bar
    virtual const char* title() const = 0;
};
