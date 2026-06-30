#pragma once

#include "ui/Screen.h"
#include "ui/UIManager.h"
#include "ui/widgets/ScrollList.h"
#include "ui/widgets/TextInput.h"
#include "ui/widgets/OptionPopup.h"
#include "config/UserConfig.h"
#include "storage/FlashStore.h"
#include "storage/SDStore.h"
#include "radio/SX1262.h"
#include <WiFi.h>
#include "audio/AudioNotify.h"
#include "power/PowerManager.h"
#include "transport/WiFiInterface.h"
#include "transport/TCPClientInterface.h"
#include "reticulum/ReticulumManager.h"
#include "input/Keyboard.h"
#include "config/Config.h"
#include "ui/screens/PasswordScreen.h"
#include "reticulum/LXMFManager.h"
#if HAS_GPS
#include "hal/GPSManager.h"
#endif
#include <vector>

class SettingsScreen : public Screen {
public:
    void render(M5Canvas& canvas) override;
    bool handleKey(const KeyEvent& event) override;
    void tick() override;
    const char* title() const override { return "Settings"; }
    void onEnter() override;

    void setUserConfig(UserConfig* cfg) { _config = cfg; }
    void setFlashStore(FlashStore* flash) { _flash = flash; }
    void setSDStore(SDStore* sd) { _sdStore = sd; }
    void setUIManager(UIManager* ui) { _ui = ui; }
    void setKeyboard(Keyboard* kb) { _keyboard = kb; }
    void setRadio(SX1262* radio) { _radio = radio; }
    void setAudio(AudioNotify* audio) { _audio = audio; }
    void setPower(PowerManager* power) { _power = power; }
    void setWiFi(WiFiInterface* wifi) { _wifi = wifi; }
    void setTCPClients(std::vector<TCPClientInterface*>* clients) { _tcpClients = clients; }
    void setRNS(ReticulumManager* rns) { _rns = rns; }
    void setIdentityHash(const String& hash) { _identityHash = hash; }
    void setLXMF(LXMFManager* lxmf) { _lxmf = lxmf; }
#if HAS_GPS
    void setGPS(GPSManager* gps) { _gps = gps; }
#endif

    // Callback for back navigation
    using BackCallback = std::function<void()>;
    void setBackCallback(BackCallback cb) { _backCb = cb; }

    // Radio > Diagnostics opens a full pushed screen (RadioDiagnosticsScreen)
    // rather than a submenu page here — same family as MessagesScreen's
    // setOpenCallback for MessageView.
    using OpenDiagnosticsCallback = std::function<void()>;
    void setOpenDiagnosticsCallback(OpenDiagnosticsCallback cb) { _openDiagnosticsCb = cb; }

    // Fired after a TCP Connections edit (toggle/add/remove) so main.cpp can
    // tear down and recreate the live TCPClientInterface set from the new
    // config immediately -- without this, edits here only took effect after
    // a manual reboot (the interfaces are owned/registered with Transport in
    // main.cpp, not here, so this screen can't do it directly).
    using TCPReloadCallback = std::function<void()>;
    void setTCPReloadCallback(TCPReloadCallback cb) { _tcpReloadCb = cb; }

private:
    enum SubMenu { MENU_MAIN, MENU_RADIO, MENU_WIFI, MENU_TCP, MENU_SDCARD,
                   MENU_DISPLAY, MENU_AUDIO, MENU_ABOUT, MENU_WIFI_SCAN, MENU_THEME,
                   MENU_DISPLAY_SCREEN, MENU_THEME_CUSTOM, MENU_THEME_MIXER, MENU_TIME,
                   MENU_SECURITY, MENU_RADIO_CONFIG, MENU_MESSAGING };

    // What an OptionPopup confirmation should do once Enter is pressed —
    // set on open(), read back in the shared confirm handler so every
    // small fixed-choice setting (WiFi Mode, Auto-Lock, on/off toggles...)
    // can share one popup instance instead of one bespoke modal each.
    enum PopupPurpose { POPUP_NONE, POPUP_WIFI_MODE, POPUP_DURESS_ACTION,
                         POPUP_AUDIO, POPUP_TIME_SOURCE, POPUP_AUTO_LAN,
                         POPUP_THEME_INPUT, POPUP_AUTOLOCK };

    void buildMainMenu();
    void buildMessagingMenu();
    void buildRadioMenu();
    void buildRadioConfigMenu();
    void buildWiFiMenu();
    void buildTCPMenu();
    void buildSDCardMenu();
    void sdCardFormat();
    void buildDisplayMenu();
    void buildDisplayScreenMenu();
    void buildAudioMenu();
    void buildTimeMenu();
    void buildSecurityMenu();
    void openPopup(PopupPurpose purpose, const std::string& title,
                    std::vector<std::string> options, int selectedIndex);
    void onPopupConfirmed(int result);
    void startDuressSetup();
    void cancelDuressSetup();
    void onDuressInputSubmit(const std::string& value);
    void buildThemeMenu();
    void buildThemeCustomMenu();
    void applyThemePreset(int index);
    void startMixer(int field);
    void commitMixer();
    void stepMixerChannel(int amount);
    void renderMixer(M5Canvas& canvas, int y0, int headerH);
    void renderAbout(M5Canvas& canvas);

    // WiFi scanner
    void startWiFiScan();
    void buildScanResultsMenu();
    void selectNetwork(int index);
    void disconnectWiFi();
    void connectWiFi();

    void addTCPConnection(const std::string& host, uint16_t port);
    void toggleTCPConnection(int index);
    void removeTCPConnection(int index);

    void startEditing(int field, const std::string& currentValue, const std::string& label = "");
    void commitEdit(const std::string& value);
    std::string getCurrentValue(SubMenu menu, int field);
    void applyAndSave();
    void factoryReset();
    void showToast(const char* msg, unsigned long durationMs = 1500);

    UserConfig* _config = nullptr;
    FlashStore* _flash = nullptr;
    SDStore* _sdStore = nullptr;
    UIManager* _ui = nullptr;
    Keyboard* _keyboard = nullptr;
    SX1262* _radio = nullptr;
    AudioNotify* _audio = nullptr;
    PowerManager* _power = nullptr;
    WiFiInterface* _wifi = nullptr;
    std::vector<TCPClientInterface*>* _tcpClients = nullptr;
    ReticulumManager* _rns = nullptr;
    LXMFManager* _lxmf = nullptr;
    String _identityHash;
#if HAS_GPS
    GPSManager* _gps = nullptr;
#endif

    SubMenu _subMenu = MENU_MAIN;
    ScrollList _list;
    bool _editing = false;
    TextInput _editInput;
    int _editField = -1;
    std::string _tcpPendingHost;
    std::string _editLabel;
    BackCallback _backCb;
    OpenDiagnosticsCallback _openDiagnosticsCb;
    TCPReloadCallback _tcpReloadCb;

    // Shared popup for every small fixed-choice setting (WiFi Mode,
    // Auto-Lock, Duress action, on/off toggles...) — see PopupPurpose.
    // openPopup() sets _popupPurpose and opens _optionPopup; the confirm
    // branch in onPopupConfirmed() dispatches on _popupPurpose.
    OptionPopup _optionPopup;
    PopupPurpose _popupPurpose = POPUP_NONE;

    // Duress password — picking Enable/Reset from the action popup above
    // opens _duressEntryActive: the masked password entry popup (two
    // stages: enter, then confirm). This part isn't an OptionPopup since
    // it's free-text input, not a choice among fixed options.
    bool _duressEntryActive = false;
    int _duressStage = 0;          // 0 = enter password, 1 = confirm password
    String _duressFirstPw;
    TextInput _duressInput;

    // WiFi scan state
    struct WiFiNetwork { String ssid; int32_t rssi; uint8_t encType; };
    std::vector<WiFiNetwork> _scanResults;

    // Toast overlay
    unsigned long _toastUntil = 0;
    const char* _toastMessage = nullptr;

    // Confirmation dialog state
    bool _confirmPending = false;
    int _confirmAction = 0;  // 0=factory reset, 1=SD wipe, 2=disable duress password

    // Theme > Custom: optional RGB mixer (alternative to typing hex directly)
    bool _useMixer = false;        // toggled in buildThemeCustomMenu(); session-only
    int _mixerField = -1;          // which BaseColors field (0=bg..6=warning)
    int _mixerChannel = 0;         // 0=R, 1=G, 2=B
    uint8_t _mixerR = 0, _mixerG = 0, _mixerB = 0;
    // Accelerating hold-to-repeat state for ','/'/' (decrement/increment),
    // driven by tick() polling continuous key state rather than discrete
    // KeyEvents — see Keyboard::isKeyPressed().
    char _mixerHeldKey = 0;
    unsigned long _mixerHeldSince = 0;
    unsigned long _mixerLastStepAt = 0;
};
