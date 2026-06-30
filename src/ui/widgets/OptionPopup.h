#pragma once

#include <M5GFX.h>
#include <vector>
#include <string>
#include "input/Keyboard.h"
#include "ui/Theme.h"

// Small centered modal listing a fixed set of options as marked rows
// (">  [Label]"), for picking one value out of a short, known list —
// WiFi Mode, Auto-Lock duration, on/off toggles, etc. Esc cancels without
// changing the selection; Enter confirms. Reserved for option counts that
// fit on a 240x135 screen (roughly <=6) — longer/open-ended lists belong in
// ScrollList instead.
class OptionPopup {
public:
    void open(const std::string& title, std::vector<std::string> options, int selectedIndex);
    void close();
    bool isActive() const { return _active; }

    void render(M5Canvas& canvas) const;

    // Returns true if the event was consumed (always true while active).
    // On Enter, sets *confirmed=true and *result=selected index.
    // On Esc, sets *confirmed=false.
    bool handleKey(const KeyEvent& event, bool* confirmed, int* result);

private:
    bool _active = false;
    std::string _title;
    std::vector<std::string> _options;
    int _selected = 0;
};
