#pragma once

#include "ui/Screen.h"
#include <functional>
#include <string>

// A yes/no prompt for the one case LXMF stamps need user input: a peer or
// propagation node demanding a proof-of-work cost above the configured
// ceiling (see LXMFManager::setStampConfirmCallback, LXStamper.h). Modeled
// on HelpOverlay -- same single-overlay-slot mechanism (UIManager::setOverlay),
// since this can fire while any screen is active, not just Settings/Messages.
class StampConfirmOverlay : public Screen {
public:
    using ResponseCallback = std::function<void(bool proceed)>;

    void render(M5Canvas& canvas) override;
    bool handleKey(const KeyEvent& event) override;
    const char* title() const override { return "Confirm Stamp"; }

    bool isVisible() const { return _visible; }
    void hide() { _visible = false; }

    // `displayName` may be empty (falls back to showing the hex prefix).
    void show(const std::string& displayName, const std::string& peerHex, int cost, ResponseCallback cb);

private:
    bool _visible = false;
    std::string _displayName;
    std::string _peerHex;
    int _cost = 0;
    ResponseCallback _cb;
};
