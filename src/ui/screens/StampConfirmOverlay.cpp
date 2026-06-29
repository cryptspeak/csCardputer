#include "StampConfirmOverlay.h"
#include "ui/Theme.h"
#include <cstdio>

namespace {
// Rough, conservative order-of-magnitude framing -- not a measured number.
// See LXStamper.h / docs/lxmf-stamps.md for why cost scales this way.
const char* roughEstimate(int cost) {
    if (cost <= 16) return "likely a few seconds to ~1 minute";
    if (cost <= 20) return "likely several minutes";
    return "could take a long time on this hardware";
}
}

void StampConfirmOverlay::show(const std::string& displayName, const std::string& peerHex,
                                int cost, ResponseCallback cb) {
    _displayName = displayName;
    _peerHex = peerHex;
    _cost = cost;
    _cb = cb;
    _visible = true;
}

void StampConfirmOverlay::render(M5Canvas& canvas) {
    if (!_visible) return;

    int oy = Theme::CONTENT_Y;
    canvas.fillRect(10, oy + 2, Theme::CONTENT_W - 20, Theme::CONTENT_H - 4, Theme::BG);
    canvas.drawRect(10, oy + 2, Theme::CONTENT_W - 20, Theme::CONTENT_H - 4, Theme::BORDER);

    int x = 16;
    int y = oy + 6;

    canvas.setTextColor(Theme::PRIMARY);
    canvas.setTextSize(Theme::FONT_SIZE);
    canvas.drawString("STAMP REQUIRED", x, y); y += 10;
    canvas.drawFastHLine(x, y, Theme::CONTENT_W - 40, Theme::BORDER); y += 5;

    canvas.setTextColor(Theme::SECONDARY);
    std::string who = _displayName.empty() ? (_peerHex.substr(0, 12) + "...") : _displayName;
    char line[64];
    snprintf(line, sizeof(line), "%s requires proof-of-work", who.c_str());
    canvas.drawString(line, x, y); y += 9;
    snprintf(line, sizeof(line), "to accept this message (cost %d).", _cost);
    canvas.drawString(line, x, y); y += 9;

    canvas.setTextColor(Theme::MUTED);
    canvas.drawString(roughEstimate(_cost), x, y); y += 9;
    canvas.drawString("Device will be busy but usable.", x, y); y += 12;

    canvas.setTextColor(Theme::PRIMARY);
    canvas.drawString("Enter  Generate stamp & send", x, y); y += 9;
    canvas.drawString("Esc    Cancel this message", x, y);
}

bool StampConfirmOverlay::handleKey(const KeyEvent& event) {
    if (!_visible) return false;

    if (event.enter) {
        _visible = false;
        if (_cb) _cb(true);
        return true;
    }
    if (event.character == 27) {  // Esc (Fn+` on Cardputer Adv -- see Keyboard.cpp)
        _visible = false;
        if (_cb) _cb(false);
        return true;
    }
    // Swallow everything else while visible -- this is a modal prompt.
    return true;
}
