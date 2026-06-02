#pragma once

#include "ui/Screen.h"
#include <Arduino.h>
#include <functional>

// Password entry screen — used for both first-time setup (with confirm
// field) and unlock-on-boot (single field, lockout-aware). The same screen
// implements both modes so we don't duplicate the masked-input rendering.
class PasswordScreen : public Screen {
public:
    enum class Mode {
        SETUP,   // Create new password — two fields, must match
        UNLOCK   // Decrypt existing identity — one field
    };

    void render(M5Canvas& canvas) override;
    bool handleKey(const KeyEvent& event) override;
    const char* title() const override { return "Security"; }

    void setMode(Mode m);

    // Unlock-mode: show failure feedback + remaining attempts. Once
    // attemptsRemaining hits 0 the screen renders a hard-lockout state
    // and stops accepting input.
    void setUnlockFailed(int attemptsRemaining);
    void setLockedOut();

    // Setup-mode: show validation error (e.g. "must match", "too short").
    // Cleared on next keystroke.
    void setValidationError(const String& msg);

    // Fired when the user submits a valid input.
    // - In SETUP mode, called only after both fields match and pass length
    //   validation. `password` is the chosen password.
    // - In UNLOCK mode, called on every Enter press with the entered text;
    //   the caller validates and either sets unlock-failed or advances.
    using SubmitCallback = std::function<void(const String& password)>;
    void setSubmitCallback(SubmitCallback cb) { _submit = cb; }

    static constexpr int MIN_LEN = 6;
    static constexpr int MAX_LEN = 64;

private:
    Mode _mode = Mode::UNLOCK;
    String _pw;
    String _confirm;
    bool _showChars = false;
    bool _editingConfirm = false;  // setup-mode only
    String _statusMsg;
    uint16_t _statusColor = 0;
    bool _lockedOut = false;
    SubmitCallback _submit;

    void resetState();
    void renderTitle(M5Canvas& canvas, const char* title);
    void renderField(M5Canvas& canvas, int y, const String& value,
                     bool active, const char* label);
};

// Warning shown when a legacy (unencrypted) identity is detected. Two
// options: migrate identity only (fast) or migrate identity + encrypt all
// existing messages (slower). User cannot bypass — encryption is mandatory.
class MigrationWarningScreen : public Screen {
public:
    enum class Choice {
        IDENTITY_ONLY,
        ALL_MESSAGES,
    };

    void render(M5Canvas& canvas) override;
    bool handleKey(const KeyEvent& event) override;
    const char* title() const override { return "Security"; }

    // Status text shown while migration is running.
    void setProgress(const String& msg);

    using ChoiceCallback = std::function<void(Choice)>;
    void setChoiceCallback(ChoiceCallback cb) { _cb = cb; }

private:
    int _selected = 0;            // 0 = identity only, 1 = all messages
    String _progress;
    ChoiceCallback _cb;
};
