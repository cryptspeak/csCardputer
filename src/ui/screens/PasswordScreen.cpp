#include "PasswordScreen.h"
#include "ui/Theme.h"
#include "config/Config.h"

// ─────────────────────────────────────────────────────────────────────────────
// PasswordScreen
// ─────────────────────────────────────────────────────────────────────────────

void PasswordScreen::setMode(Mode m) {
    _mode = m;
    resetState();
}

void PasswordScreen::resetState() {
    _pw = "";
    _confirm = "";
    _editingConfirm = false;
    _statusMsg = "";
    _lockedOut = false;
}

void PasswordScreen::setUnlockFailed(int attemptsRemaining) {
    if (attemptsRemaining <= 0) {
        setLockedOut();
        return;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "Wrong password — %d left", attemptsRemaining);
    _statusMsg = buf;
    _statusColor = Theme::ERROR;
    _pw = "";
}

void PasswordScreen::setLockedOut() {
    _statusMsg = "LOCKED — power cycle device";
    _statusColor = Theme::ERROR;
    _lockedOut = true;
    _pw = "";
}

void PasswordScreen::setValidationError(const String& msg) {
    _statusMsg = msg;
    _statusColor = Theme::ERROR;
}

bool PasswordScreen::handleKey(const KeyEvent& event) {
    if (_lockedOut) return false;

    // Ctrl+S toggles plaintext display (handy on a tiny keyboard where
    // typos are common; clearly opt-in via a modifier).
    if (event.ctrl && (event.character == 's' || event.character == 'S')) {
        _showChars = !_showChars;
        return true;
    }

    String& target = (_mode == Mode::SETUP && _editingConfirm) ? _confirm : _pw;

    if (event.enter) {
        if (_mode == Mode::UNLOCK) {
            if (_pw.length() == 0) return true;
            if (_submit) _submit(_pw);
            return true;
        }
        // SETUP mode
        if (!_editingConfirm) {
            if ((int)_pw.length() < MIN_LEN) {
                char buf[48];
                snprintf(buf, sizeof(buf), "Min %d characters", MIN_LEN);
                setValidationError(buf);
                return true;
            }
            _editingConfirm = true;
            _statusMsg = "";
            return true;
        }
        // Confirming
        if (_pw != _confirm) {
            setValidationError("Passwords do not match");
            _confirm = "";
            return true;
        }
        if (_submit) _submit(_pw);
        return true;
    }

    // Clear any stale status on next keystroke
    if (!_statusMsg.isEmpty()) _statusMsg = "";

    if (event.del) {
        // Backwards jump in setup mode: backspace on an empty confirm field
        // goes back to editing the first field. Must be checked before the
        // generic delete below, which would otherwise always return first
        // and make this unreachable.
        if (_mode == Mode::SETUP && _editingConfirm && _confirm.length() == 0) {
            _editingConfirm = false;
            return true;
        }
        if (target.length() > 0) target.remove(target.length() - 1);
        return true;
    }

    char c = event.character;
    if (c >= 32 && c < 127 && (int)target.length() < MAX_LEN) {
        target += c;
        return true;
    }
    return false;
}

void PasswordScreen::renderTitle(M5Canvas& canvas, const char* t) {
    int cx = Theme::SCREEN_W / 2;
    canvas.setTextSize(2);
    canvas.setTextColor(Theme::PRIMARY);
    int tw = strlen(t) * 12;
    canvas.setCursor(cx - tw / 2, Theme::CONTENT_Y + 4);
    canvas.print(t);
    canvas.setTextSize(1);
}

void PasswordScreen::renderField(M5Canvas& canvas, int y,
                                 const String& value, bool active,
                                 const char* label) {
    int cx = Theme::SCREEN_W / 2;

    int fieldW = MAX_LEN > 24 ? 24 * Theme::CHAR_W + 8 : MAX_LEN * Theme::CHAR_W + 8;
    int fieldX = cx - fieldW / 2;
    int fieldY = y + 10;
    int fieldH = Theme::CHAR_H + 6;

    // Left-aligned to the field's own edge rather than centered above it —
    // a label floating in the middle of empty space over a wide box reads
    // as disconnected from it; lining the two up reads as one unit.
    if (label && label[0]) {
        canvas.setTextColor(active ? Theme::PRIMARY : Theme::MUTED);
        canvas.setCursor(fieldX, y);
        canvas.print(label);
    }

    canvas.fillRoundRect(fieldX, fieldY, fieldW, fieldH, 3, Theme::BG_ELEVATED);
    canvas.drawRoundRect(fieldX, fieldY, fieldW, fieldH, 3,
                         active ? Theme::PRIMARY : Theme::BORDER);
    canvas.setTextColor(Theme::TEXT_PRIMARY);
    canvas.setCursor(fieldX + 4, fieldY + 3);

    // Render masked or plaintext. Cap shown characters to field width so
    // long passwords don't overflow.
    int maxShow = (fieldW - 8) / Theme::CHAR_W;
    int len = (int)value.length();
    int shown = len > maxShow ? maxShow : len;
    int hiddenLeft = len - shown;
    if (_showChars) {
        canvas.print(value.substring(hiddenLeft).c_str());
    } else {
        for (int i = 0; i < shown; i++) canvas.print('*');
    }

    if (active && (millis() / 500) % 2 == 0) {
        int curX = fieldX + 4 + shown * Theme::CHAR_W;
        canvas.drawFastVLine(curX, fieldY + 2, Theme::CHAR_H, Theme::PRIMARY);
    }
}

void PasswordScreen::render(M5Canvas& canvas) {
    int cx = Theme::SCREEN_W / 2;

    if (_mode == Mode::SETUP) {
        renderTitle(canvas, "SET PASSWORD");
    } else {
        renderTitle(canvas, "UNLOCK");
    }

    // Subtitle
    canvas.setTextColor(Theme::SECONDARY);
    const char* sub = _mode == Mode::SETUP
        ? "Encrypts your identity at rest"
        : "Enter password to unlock identity";
    int sw = strlen(sub) * Theme::CHAR_W;
    canvas.setCursor(cx - sw / 2, Theme::CONTENT_Y + 24);
    canvas.print(sub);

    if (_lockedOut) {
        canvas.setTextColor(Theme::ERROR);
        canvas.setTextSize(1);
        const char* m1 = "TOO MANY ATTEMPTS";
        int m1w = strlen(m1) * Theme::CHAR_W;
        canvas.setCursor(cx - m1w / 2, Theme::CONTENT_Y + 50);
        canvas.print(m1);
        const char* m2 = "Power cycle device to retry";
        int m2w = strlen(m2) * Theme::CHAR_W;
        canvas.setCursor(cx - m2w / 2, Theme::CONTENT_Y + 64);
        canvas.print(m2);
        return;
    }

    // Status/hint position depends on mode: SETUP has two fields stacked
    // above and needs more room, so it can't reuse the bottom-anchored
    // offsets below without the confirm field colliding with them.
    int statusY, hintY;
    if (_mode == Mode::SETUP) {
        // Spread across the full screen height instead of clumping near
        // the top with dead space left unused at the bottom.
        renderField(canvas, Theme::CONTENT_Y + 38, _pw,      !_editingConfirm, "Password");
        renderField(canvas, Theme::CONTENT_Y + 68, _confirm,  _editingConfirm, "Confirm");
        statusY = Theme::CONTENT_Y + 99;
        hintY   = Theme::CONTENT_Y + 111;
    } else {
        // Only one field here — give it generous room on both sides rather
        // than crowding it against the header or the footer.
        renderField(canvas, Theme::CONTENT_Y + 50, _pw, true, nullptr);

        statusY = Theme::CONTENT_Y + Theme::CONTENT_H - 20;
        hintY   = Theme::CONTENT_Y + Theme::CONTENT_H - 10;
    }

    // Status line
    if (!_statusMsg.isEmpty()) {
        canvas.setTextColor(_statusColor);
        int mw = _statusMsg.length() * Theme::CHAR_W;
        canvas.setCursor(cx - mw / 2, statusY);
        canvas.print(_statusMsg.c_str());
    }

    // Hint: Ctrl+S to toggle visibility
    canvas.setTextColor(Theme::BORDER);
    const char* hint = _showChars ? "Ctrl+S hide  Enter submit" : "Ctrl+S show  Enter submit";
    int hw = strlen(hint) * Theme::CHAR_W;
    canvas.setCursor(cx - hw / 2, hintY);
    canvas.print(hint);
}

// ─────────────────────────────────────────────────────────────────────────────
// MigrationWarningScreen
// ─────────────────────────────────────────────────────────────────────────────

void MigrationWarningScreen::setProgress(const String& msg) {
    _progress = msg;
}

bool MigrationWarningScreen::handleKey(const KeyEvent& event) {
    if (!_progress.isEmpty()) return false;  // mid-migration: ignore input

    if (event.character == ',' || event.character == ';') { _selected = 0; return true; }
    if (event.character == '/' || event.character == '.') { _selected = 1; return true; }
    if (event.enter) {
        if (_cb) _cb(_selected == 0 ? Choice::IDENTITY_ONLY : Choice::ALL_MESSAGES);
        return true;
    }
    return false;
}

void MigrationWarningScreen::render(M5Canvas& canvas) {
    int cx = Theme::SCREEN_W / 2;

    canvas.setTextSize(2);
    canvas.setTextColor(Theme::WARNING);
    const char* title = "WARNING";
    int tw = strlen(title) * 12;
    canvas.setCursor(cx - tw / 2, Theme::CONTENT_Y + 4);
    canvas.print(title);

    canvas.setTextSize(1);
    canvas.setTextColor(Theme::TEXT_PRIMARY);
    const char* l1 = "Unencrypted identity";
    int l1w = strlen(l1) * Theme::CHAR_W;
    canvas.setCursor(cx - l1w / 2, Theme::CONTENT_Y + 24);
    canvas.print(l1);

    const char* l2 = "detected on this device.";
    int l2w = strlen(l2) * Theme::CHAR_W;
    canvas.setCursor(cx - l2w / 2, Theme::CONTENT_Y + 34);
    canvas.print(l2);

    if (!_progress.isEmpty()) {
        canvas.setTextColor(Theme::ACCENT);
        int pw = _progress.length() * Theme::CHAR_W;
        canvas.setCursor(cx - pw / 2, Theme::CONTENT_Y + 56);
        canvas.print(_progress.c_str());
        return;
    }

    canvas.setTextColor(Theme::SECONDARY);
    const char* l3 = "Choose migration scope:";
    int l3w = strlen(l3) * Theme::CHAR_W;
    canvas.setCursor(cx - l3w / 2, Theme::CONTENT_Y + 50);
    canvas.print(l3);

    // Two-option selector
    auto drawOpt = [&](int idx, int y, const char* short_, const char* hint) {
        bool active = (_selected == idx);
        int boxW = Theme::SCREEN_W - 16;
        int boxX = 8;
        int boxH = 14;
        canvas.drawRect(boxX, y, boxW, boxH, active ? Theme::PRIMARY : Theme::BORDER);
        if (active) canvas.fillRect(boxX + 1, y + 1, boxW - 2, boxH - 2, Theme::PRIMARY_SUBTLE);
        canvas.setTextColor(active ? Theme::PRIMARY : Theme::TEXT_PRIMARY);
        canvas.setCursor(boxX + 4, y + 3);
        canvas.print(short_);
        canvas.setTextColor(Theme::MUTED);
        int hintW = strlen(hint) * Theme::CHAR_W;
        canvas.setCursor(boxX + boxW - hintW - 4, y + 3);
        canvas.print(hint);
    };

    drawOpt(0, Theme::CONTENT_Y + 62, "Identity only",   "fast");
    drawOpt(1, Theme::CONTENT_Y + 78, "Identity + msgs", "slow");

    canvas.setTextColor(Theme::BORDER);
    const char* hint = ",  /  switch    Enter confirm";
    int hw = strlen(hint) * Theme::CHAR_W;
    canvas.setCursor(cx - hw / 2, Theme::CONTENT_Y + Theme::CONTENT_H - 10);
    canvas.print(hint);
}
