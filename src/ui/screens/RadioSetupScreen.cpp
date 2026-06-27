#include "RadioSetupScreen.h"
#include "ui/Theme.h"
#include <cstdlib>

void RadioSetupScreen::onEnter() {
    _editing = false;
    _editField = -1;
    buildList();
}

void RadioSetupScreen::buildList() {
    if (!_config) return;
    auto& s = _config->settings();
    char buf[48];

    _list.clear();
    snprintf(buf, sizeof(buf), "Region: %s", REGION_LABELS[s.radioRegion]);
    _list.addItem(buf);
    snprintf(buf, sizeof(buf), "Frequency: %lu Hz", (unsigned long)s.loraFrequency);
    _list.addItem(buf);
    snprintf(buf, sizeof(buf), "Spreading Factor: %d", s.loraSF);
    _list.addItem(buf);
    snprintf(buf, sizeof(buf), "Bandwidth: %lu Hz", (unsigned long)s.loraBW);
    _list.addItem(buf);
    snprintf(buf, sizeof(buf), "Coding Rate: 4/%d", s.loraCR);
    _list.addItem(buf);
    snprintf(buf, sizeof(buf), "TX Power: %d dBm", s.loraTxPower);
    _list.addItem(buf);
    _list.addItem("Continue ->", Theme::ACCENT);
}

void RadioSetupScreen::render(M5Canvas& canvas) {
    int y = Theme::CONTENT_Y;

    const int headerH = Theme::SECTION_HEADER_H;
    canvas.fillRect(0, y, Theme::CONTENT_W, headerH, Theme::BG_SURFACE);
    canvas.fillRect(0, y + 2, 3, headerH - 4, Theme::ACCENT);
    Theme::useUiFont(canvas);
    canvas.setTextColor(Theme::ACCENT);
    canvas.drawString("LoRa Radio Setup", 8, y + 2);
    canvas.drawFastHLine(0, y + headerH, Theme::CONTENT_W, Theme::DIVIDER);
    y += headerH + 2;

    if (_editing) {
        Theme::useSmallFont(canvas);
        canvas.setTextColor(Theme::MUTED);
        canvas.drawString(_editLabel.c_str(), 8, y);
        y += Theme::CHAR_H + 4;
        _editInput.render(canvas, 0, y, Theme::CONTENT_W);
        y += Theme::CHAR_H + 8;
        canvas.setTextColor(Theme::MUTED);
        canvas.drawString("Enter=save  Esc=cancel", 8, y);
        return;
    }

    Theme::useSmallFont(canvas);
    canvas.setTextColor(Theme::MUTED);
    canvas.drawString("Set up your LoRa parameters", 8, y);
    y += Theme::CHAR_H + 4;

    _list.render(canvas, 0, y, Theme::CONTENT_W, Theme::CONTENT_H - (y - Theme::CONTENT_Y));
}

void RadioSetupScreen::applyToRadio() {
    if (!_config || !_radio) return;
    auto& s = _config->settings();
    _radio->setFrequency(s.loraFrequency);
    _radio->setSpreadingFactor(s.loraSF);
    _radio->setSignalBandwidth(s.loraBW);
    _radio->setCodingRate4(s.loraCR);
    _radio->setTxPower(s.loraTxPower);
    _radio->receive();
}

void RadioSetupScreen::startEditing(int field) {
    if (!_config) return;
    auto& s = _config->settings();
    char buf[16];

    switch (field) {
        case FIELD_FREQ:
            snprintf(buf, sizeof(buf), "%lu", (unsigned long)s.loraFrequency);
            _editLabel = "Frequency (Hz):";
            break;
        case FIELD_SF:
            snprintf(buf, sizeof(buf), "%d", s.loraSF);
            _editLabel = "Spreading Factor (5-12):";
            break;
        case FIELD_BW:
            snprintf(buf, sizeof(buf), "%lu", (unsigned long)s.loraBW);
            _editLabel = "Bandwidth (Hz):";
            break;
        case FIELD_CR:
            snprintf(buf, sizeof(buf), "%d", s.loraCR);
            _editLabel = "Coding Rate (5-8):";
            break;
        case FIELD_TXPOWER:
            snprintf(buf, sizeof(buf), "%d", s.loraTxPower);
            _editLabel = "TX Power (dBm):";
            break;
        default:
            return;
    }

    _editField = field;
    _editing = true;
    _editInput.clear();
    _editInput.setText(buf);
    _editInput.setActive(true);
    _editInput.setMaxLength(10);  // not numeric-only: TX power needs a leading '-'
    _editInput.setSubmitCallback([this](const std::string& value) {
        commitEdit(value);
    });
}

void RadioSetupScreen::commitEdit(const std::string& value) {
    if (!_config) return;
    auto& s = _config->settings();
    long v = atol(value.c_str());

    switch (_editField) {
        case FIELD_FREQ:
            if (v >= 150000000L && v <= 960000000L) s.loraFrequency = (uint32_t)v;
            break;
        case FIELD_SF:
            if (v >= 5 && v <= 12) s.loraSF = (uint8_t)v;
            break;
        case FIELD_BW:
            if (v >= 7800 && v <= 500000) s.loraBW = (uint32_t)v;
            break;
        case FIELD_CR:
            if (v >= 5 && v <= 8) s.loraCR = (uint8_t)v;
            break;
        case FIELD_TXPOWER:
            if (v >= -9 && v <= LORA_MAX_TX_POWER) s.loraTxPower = (int8_t)v;
            break;
    }

    applyToRadio();
    _editing = false;
    _editField = -1;
    buildList();
}

bool RadioSetupScreen::handleKey(const KeyEvent& event) {
    if (_editing) {
        if (event.character == 27) {  // Esc cancels without saving
            _editing = false;
            _editField = -1;
            return true;
        }
        return _editInput.handleKey(event);
    }

    if (event.character == ';') {
        _list.scrollUp();
        return true;
    }
    if (event.character == '.') {
        _list.scrollDown();
        return true;
    }

    if (event.enter) {
        int sel = _list.getSelectedIndex();
        if (!_config) return true;
        auto& s = _config->settings();

        if (sel == FIELD_REGION) {
            s.radioRegion = (s.radioRegion + 1) % REGION_COUNT;
            s.loraFrequency = REGION_FREQ[s.radioRegion];
            applyToRadio();
            buildList();
        } else if (sel == FIELD_CONTINUE) {
            if (_doneCb) _doneCb();
        } else {
            startEditing(sel);
        }
        return true;
    }

    return false;
}
