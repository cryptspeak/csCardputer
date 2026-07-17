#pragma once

#include "ui/Screen.h"
#include "ui/widgets/ScrollList.h"
#include "ui/widgets/TextInput.h"
#include "config/Config.h"
#include "config/UserConfig.h"
#include "radio/SX1262.h"
#include <functional>
#include <string>

// First-boot LoRa setup wizard — replaces the old timezone picker. GPS now
// handles time entirely on its own (UTC sync + a longitude/DST-estimated
// local offset, see GPSManager), so the boot wizard's only job is letting
// the user pick/confirm LoRa radio parameters instead of having them
// silently derived from a timezone selection.
class RadioSetupScreen : public Screen {
public:
    void render(M5Canvas& canvas) override;
    bool handleKey(const KeyEvent& event) override;
    void onEnter() override;
    const char* title() const override { return "Radio Setup"; }

    void setUserConfig(UserConfig* cfg) { _config = cfg; }
    void setRadio(SX1262* radio) { _radio = radio; }

    using DoneCb = std::function<void()>;
    void setDoneCallback(DoneCb cb) { _doneCb = cb; }

private:
    enum Field { FIELD_REGION = 0, FIELD_FREQ, FIELD_SF, FIELD_BW, FIELD_CR, FIELD_TXPOWER, FIELD_CONTINUE };

    void buildList();
    void startEditing(int field);
    void commitEdit(const std::string& value);
    void applyToRadio();
    void showToast(const char* msg, unsigned long durationMs = 1500);

    UserConfig* _config = nullptr;
    SX1262* _radio = nullptr;
    DoneCb _doneCb;

    ScrollList _list;
    bool _editing = false;
    TextInput _editInput;
    int _editField = -1;
    std::string _editLabel;

    unsigned long _toastUntil = 0;
    const char* _toastMessage = nullptr;
};
