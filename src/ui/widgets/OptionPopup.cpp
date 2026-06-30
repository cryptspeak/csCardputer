#include "OptionPopup.h"
#include <algorithm>

void OptionPopup::open(const std::string& title, std::vector<std::string> options, int selectedIndex) {
    _title = title;
    _options = std::move(options);
    _selected = (selectedIndex >= 0 && selectedIndex < (int)_options.size()) ? selectedIndex : 0;
    _active = true;
}

void OptionPopup::close() {
    _active = false;
}

void OptionPopup::render(M5Canvas& canvas) const {
    if (!_active) return;

    // Width follows the longest row (title or "> [Label]") so short option
    // sets (On/Off) don't get a needlessly wide box and long ones (Reset
    // Password) aren't clipped — same box-drawing as the original WiFi
    // Mode/Duress popups, just sized to content instead of hard-coded.
    size_t widestChars = _title.size();
    for (auto& opt : _options) {
        widestChars = std::max(widestChars, opt.size() + 4);  // "> []"
    }
    int bw = std::min((int)widestChars * Theme::CHAR_W + 16, Theme::CONTENT_W - 8);
    int bh = 22 + (int)_options.size() * 11;
    int bx = (Theme::CONTENT_W - bw) / 2;
    int by = Theme::CONTENT_Y + (Theme::CONTENT_H - bh) / 2;

    canvas.fillRoundRect(bx, by, bw, bh, 3, Theme::BG);
    canvas.drawRoundRect(bx, by, bw, bh, 3, Theme::PRIMARY);
    canvas.setTextColor(Theme::PRIMARY);
    canvas.setCursor(bx + 6, by + 5);
    canvas.print(_title.c_str());

    int ry = by + 16;
    for (size_t i = 0; i < _options.size(); i++) {
        bool sel = ((int)i == _selected);
        canvas.setTextColor(sel ? Theme::PRIMARY : Theme::TEXT_SECONDARY);
        canvas.setCursor(bx + 8, ry);
        canvas.print(sel ? "> [" : "  [");
        canvas.print(_options[i].c_str());
        canvas.print("]");
        ry += 11;
    }
}

bool OptionPopup::handleKey(const KeyEvent& event, bool* confirmed, int* result) {
    if (!_active) return false;

    int rowCount = (int)_options.size();
    if (event.character == 27) {
        _active = false;
        *confirmed = false;
        return true;
    }
    if (event.character == ';') {
        _selected = (_selected + rowCount - 1) % rowCount;
        return true;
    }
    if (event.character == '.') {
        _selected = (_selected + 1) % rowCount;
        return true;
    }
    if (event.enter) {
        _active = false;
        *confirmed = true;
        *result = _selected;
        return true;
    }
    return true;  // swallow anything else while the popup is active
}
