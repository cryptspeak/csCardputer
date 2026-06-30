#include "SettingsScreen.h"
#include "ui/Theme.h"
#include "config/Config.h"
#include "security/Duress.h"
#include "storage/FactoryWipe.h"
#include "reticulum/IdentityCrypto.h"
#include <algorithm>
#include <cctype>
#include <WiFi.h>
#include <WiFiClient.h>

// Strict: exactly 6 hex digits, nothing else. Used to drive the live color
// swatch while editing a Theme > Custom field — only renders once the typed
// text is a complete, valid hex value.
static bool parseHex6(const std::string& s, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (s.size() != 6) return false;
    for (char c : s) {
        if (!isxdigit((unsigned char)c)) return false;
    }
    long v = strtol(s.c_str(), nullptr, 16);
    r = (uint8_t)((v >> 16) & 0xFF);
    g = (uint8_t)((v >> 8) & 0xFF);
    b = (uint8_t)(v & 0xFF);
    return true;
}

// Single source of truth for the Theme > Custom field list: display order,
// label, and which Theme::BaseColors member each row actually edits.
// `index` matches the field argument used throughout this file (startMixer,
// commitEdit, getCurrentValue, commitMixer) — those stay in BaseColors'
// declaration order (bg, text, primary, secondary, accent, error, warning);
// only the on-screen order/labels are reshuffled here, in one place, so menu
// order and field plumbing can never drift out of sync with each other.
// Grouped by what a user is actually looking at: surface/text colors first,
// then the two "emphasis" colors (selection vs. screen-header chrome), then
// status colors in ascending severity (warn before error).
struct ThemeFieldInfo { int index; const char* label; };
static const ThemeFieldInfo kThemeFields[7] = {
    {0, "Background"},  // bg
    {1, "Text"},         // text
    {3, "Subtext"},      // secondary
    {2, "Highlight"},    // primary
    {4, "Header"},       // accent
    {6, "Warning"},      // warning
    {5, "Error"},        // error
};

static std::string hexForBaseField(const Theme::BaseColors& c, int fieldIndex) {
    switch (fieldIndex) {
        case 0: return std::string(c.bg.c_str());
        case 1: return std::string(c.text.c_str());
        case 2: return std::string(c.primary.c_str());
        case 3: return std::string(c.secondary.c_str());
        case 4: return std::string(c.accent.c_str());
        case 5: return std::string(c.error.c_str());
        case 6: return std::string(c.warning.c_str());
        default: return std::string();
    }
}

static const char* labelForBaseField(int fieldIndex) {
    for (const auto& f : kThemeFields) {
        if (f.index == fieldIndex) return f.label;
    }
    return "Color";
}

void SettingsScreen::onEnter() {
    _subMenu = MENU_MAIN;
    _editing = false;
    _editField = -1;
    buildMainMenu();
}

void SettingsScreen::buildMainMenu() {
    _list.clear();
    _list.addItem("Radio");
    _list.addItem("Wi-Fi");
    _list.addItem("Display");
    _list.addItem("Audio");
    _list.addItem("Time");
    _list.addItem("Security");
    _list.addItem("Messaging");
    _list.addItem("SD Card");
    _list.addItem("About");
    _list.addItem("Factory Reset", Theme::ERROR);
}

// LXMF anti-spam stamp settings — see docs/lxmf-stamps.md.
void SettingsScreen::buildMessagingMenu() {
    _list.clear();
    if (!_config) return;
    auto& s = _config->settings();
    char buf[48];

    snprintf(buf, sizeof(buf), "Stamp Cost Ceiling: %d", s.stampCostCeiling);
    _list.addItem(buf);

    _list.addItem("< Back");
}

void SettingsScreen::buildTimeMenu() {
    _list.clear();
    if (!_config) return;
    auto& s = _config->settings();

    _list.addItem(s.manualTimezoneEnabled ? "Time Source: Manual" : "Time Source: Auto (GPS)");
    if (s.manualTimezoneEnabled) {
        char buf[24];
        snprintf(buf, sizeof(buf), "UTC Offset: %+d", s.manualUtcOffsetHours);
        _list.addItem(buf);
    }
    _list.addItem("< Back");
}

// Auto-lock preset list — fixed minute values selectable via the
// POPUP_AUTOLOCK popup (see onPopupConfirmed()). 0 = disabled. Kept as a
// single table so the status label (buildSecurityMenu) and the popup's
// options can never drift out of sync with each other.
struct AutoLockOption { uint16_t minutes; const char* label; };
static const AutoLockOption kAutoLockOptions[] = {
    {30,  "30 min"},
    {60,  "1 hour"},
    {240, "4 hours"},
    {480, "8 hours"},
    {720, "12 hours"},
    {0,   "Disabled"},
};
static constexpr int kAutoLockOptionCount = sizeof(kAutoLockOptions) / sizeof(kAutoLockOptions[0]);

static const char* autoLockLabel(uint16_t minutes) {
    for (auto& o : kAutoLockOptions) {
        if (o.minutes == minutes) return o.label;
    }
    return "Disabled";
}

// Security menu: each row is a status summary. Both Duress Password and
// Auto-Lock open a small OptionPopup instead of a dedicated page — neither
// has enough options to need one (see openPopup/onPopupConfirmed).
void SettingsScreen::buildSecurityMenu() {
    _list.clear();
    bool configured = Duress::isConfigured();
    _list.addItem(configured ? "Duress Password: Enabled"
                              : "Duress Password: Disabled");
    if (_config) {
        std::string label = "Auto-Lock: " + std::string(autoLockLabel(_config->settings().autoLockMinutes));
        _list.addItem(label);
    }
    _list.addItem("< Back");
}

void SettingsScreen::openPopup(PopupPurpose purpose, const std::string& title,
                                std::vector<std::string> options, int selectedIndex) {
    _popupPurpose = purpose;
    _optionPopup.open(title, std::move(options), selectedIndex);
}

// Dispatches on the purpose set by openPopup() — one shared confirm path
// for every small fixed-choice setting instead of one bespoke handler per
// popup. Each case does exactly what its old inline toggle/page-select code
// did: write the field, applyAndSave(), rebuild the menu it was opened from.
void SettingsScreen::onPopupConfirmed(int result) {
    PopupPurpose purpose = _popupPurpose;
    _popupPurpose = POPUP_NONE;
    if (!_config) return;
    auto& s = _config->settings();

    switch (purpose) {
        case POPUP_WIFI_MODE:
            s.wifiMode = (RatWiFiMode)result;
            applyAndSave();
            showToast("Reboot to apply");
            buildWiFiMenu();
            break;

        case POPUP_DURESS_ACTION: {
            bool configured = Duress::isConfigured();
            if (result == 0) {
                if (configured) {
                    _confirmPending = true;
                    _confirmAction = 2;
                } else {
                    startDuressSetup();
                }
            } else if (result == 1 && configured) {
                startDuressSetup();
            }
            break;
        }

        case POPUP_AUDIO:
            s.audioEnabled = (result == 0);
            if (_audio) _audio->setEnabled(s.audioEnabled);
            applyAndSave();
            buildAudioMenu();
            break;

        case POPUP_TIME_SOURCE:
            s.manualTimezoneEnabled = (result == 1);
            applyAndSave();
            buildTimeMenu();
            break;

        case POPUP_AUTO_LAN:
            // IPv6 enable is one-shot per WiFi init, so changes take effect
            // on next reboot — same as before this was a popup.
            s.autoIfaceEnabled = (result == 0);
            applyAndSave();
            showToast("Reboot to apply");
            buildWiFiMenu();
            break;

        case POPUP_THEME_INPUT:
            _useMixer = (result == 1);
            buildThemeCustomMenu();
            break;

        case POPUP_AUTOLOCK:
            if (result >= 0 && result < kAutoLockOptionCount) {
                s.autoLockMinutes = kAutoLockOptions[result].minutes;
                applyAndSave();
                buildSecurityMenu();
            }
            break;

        default:
            break;
    }
}

// Opens the small inline duress-password popup (see render()'s
// _duressEntryActive block and the handleKey block guarding it) — drawn on
// top of this screen rather than swapping to a different one, and
// cancelable with Esc at any point, same as any other Settings field edit.
// Closes the action popup first if that's what led here (Enable/Reset).
void SettingsScreen::startDuressSetup() {
    _duressStage = 0;
    _duressFirstPw = "";
    _duressInput.clear();
    _duressInput.setMaxLength(PasswordScreen::MAX_LEN);
    _duressInput.setMasked(true);
    _duressInput.setActive(true);
    _duressInput.setSubmitCallback([this](const std::string& v) { onDuressInputSubmit(v); });
    _duressEntryActive = true;
}

void SettingsScreen::cancelDuressSetup() {
    _duressEntryActive = false;
    _duressInput.setActive(false);
    _duressInput.clear();
    IdentityCrypto::secureZero((void*)_duressFirstPw.c_str(), _duressFirstPw.length());
    _duressFirstPw = "";
    showToast("Cancelled");
}

// Stage 0: validate length + difference from the real at-rest password,
// then advance to confirm. Stage 1: must match stage 0's entry, then
// persist via Duress::setup(). Mirrors the validation PasswordScreen does
// for the boot-time setup flow, just driven locally instead of through a
// pushed screen.
void SettingsScreen::onDuressInputSubmit(const std::string& value) {
    String pw = value.c_str();

    if (_duressStage == 0) {
        if ((int)pw.length() < PasswordScreen::MIN_LEN) {
            showToast("Min 6 characters");
            _duressInput.clear();
            return;
        }
        if (_rns && _rns->isCurrentPassword(pw)) {
            showToast("Must differ from main password");
            _duressInput.clear();
            return;
        }
        _duressFirstPw = pw;
        _duressStage = 1;
        _duressInput.clear();
        return;
    }

    // Stage 1: confirm.
    if (pw != _duressFirstPw) {
        showToast("Passwords do not match");
        IdentityCrypto::secureZero((void*)_duressFirstPw.c_str(), _duressFirstPw.length());
        _duressFirstPw = "";
        _duressStage = 0;
        _duressInput.clear();
        return;
    }

    bool ok = Duress::setup(pw);
    IdentityCrypto::secureZero((void*)pw.c_str(), pw.length());
    IdentityCrypto::secureZero((void*)_duressFirstPw.c_str(), _duressFirstPw.length());
    _duressFirstPw = "";

    _duressEntryActive = false;
    _duressInput.setActive(false);
    _duressInput.clear();
    buildSecurityMenu();
    showToast(ok ? "Duress password set" : "Failed to set");
}

void SettingsScreen::buildRadioMenu() {
    _list.clear();
    if (!_config) return;
    _list.addItem("Config");
    _list.addItem("Diagnostics");
    _list.addItem("< Back");
}

void SettingsScreen::buildRadioConfigMenu() {
    _list.clear();
    if (!_config) return;
    auto& s = _config->settings();
    char buf[40];

    snprintf(buf, sizeof(buf), "Frequency: %lu Hz", (unsigned long)s.loraFrequency);
    _list.addItem(buf);
    snprintf(buf, sizeof(buf), "Spreading Factor: %d", s.loraSF);
    _list.addItem(buf);
    snprintf(buf, sizeof(buf), "Bandwidth: %lu Hz", (unsigned long)s.loraBW);
    _list.addItem(buf);
    snprintf(buf, sizeof(buf), "Coding Rate: %d", s.loraCR);
    _list.addItem(buf);
    snprintf(buf, sizeof(buf), "TX Power: %d dBm", s.loraTxPower);
    _list.addItem(buf);

    _list.addItem("< Back");
}

void SettingsScreen::buildWiFiMenu() {
    _list.clear();
    if (!_config) return;
    auto& s = _config->settings();

    // Item 0: Mode selector
    const char* modeNames[] = {"OFF", "AP", "STA"};
    char modeBuf[24];
    snprintf(modeBuf, sizeof(modeBuf), "Mode: %s", modeNames[s.wifiMode]);
    _list.addItem(modeBuf);

    if (s.wifiMode == RAT_WIFI_AP) {
        // Items 1-2: AP fields
        String apSSID = s.wifiAPSSID.isEmpty() ? "(auto)" : s.wifiAPSSID;
        _list.addItem(("AP SSID: " + std::string(apSSID.c_str())));
        _list.addItem(("AP Pass: " + std::string(s.wifiAPPassword.c_str())));
    } else if (s.wifiMode == RAT_WIFI_STA) {
        // Item 1: Connection status
        if (WiFi.status() == WL_CONNECTED) {
            char statusBuf[48];
            snprintf(statusBuf, sizeof(statusBuf), "Connected: %s", WiFi.SSID().c_str());
            _list.addItem(statusBuf);
            _list.addItem("[Disconnect]");          // Item 2
        } else if (!s.wifiSTASSID.isEmpty()) {
            char statusBuf[48];
            snprintf(statusBuf, sizeof(statusBuf), "Saved: %s (offline)", s.wifiSTASSID.c_str());
            _list.addItem(statusBuf);
            _list.addItem("[Connect]");             // Item 2
        } else {
            _list.addItem("No network configured");
            _list.addItem("");                      // Item 2 placeholder
        }
        _list.addItem("Scan Networks");             // Item 3
        _list.addItem("TCP Connections");            // Item 4
        // Item 5: AutoInterface (LAN auto-discovery via IPv6 multicast)
        char autoBuf[40];
        snprintf(autoBuf, sizeof(autoBuf), "Auto-discover LAN: %s",
                 s.autoIfaceEnabled ? "ON" : "OFF");
        _list.addItem(autoBuf);
    }
    // OFF mode: only mode selector + back

    _list.addItem("< Back");
}

void SettingsScreen::buildTCPMenu() {
    _list.clear();
    if (!_config) return;
    auto& s = _config->settings();

    _list.addItem("Add Connection");

    for (size_t i = 0; i < s.tcpConnections.size(); i++) {
        auto& ep = s.tcpConnections[i];
        char buf[64];
        snprintf(buf, sizeof(buf), "%s:%d [%s]",
                 ep.host.c_str(), ep.port,
                 ep.autoConnect ? "ON" : "OFF");
        _list.addItem(buf);
    }

    _list.addItem("< Back");
}

void SettingsScreen::addTCPConnection(const std::string& host, uint16_t port) {
    if (!_config) return;
    auto& s = _config->settings();
    if (s.tcpConnections.size() >= MAX_TCP_CONNECTIONS) {
        showToast("Max 4 connections");
        return;
    }

    TCPEndpoint ep;
    ep.host = host.c_str();
    ep.port = port;
    if (ep.host.isEmpty()) return;

    // Test connection before adding (only if WiFi is connected)
    if (WiFi.status() == WL_CONNECTED) {
        showToast("Testing connection...");
        WiFiClient testClient;
        if (!testClient.connect(ep.host.c_str(), ep.port, 3000)) {
            testClient.stop();
            showToast("Connection failed!", 2500);
            Serial.printf("[TCP] Test failed: %s:%d\n", ep.host.c_str(), ep.port);
            buildTCPMenu();
            return;
        }
        testClient.stop();
    }

    s.tcpConnections.push_back(ep);
    applyAndSave();
    if (_tcpReloadCb) {
        _tcpReloadCb();
        showToast("Added & connecting");
    } else {
        showToast("Added! Reboot to connect");
    }
    buildTCPMenu();
}

void SettingsScreen::toggleTCPConnection(int index) {
    if (!_config) return;
    auto& s = _config->settings();
    if (index < 0 || index >= (int)s.tcpConnections.size()) return;

    s.tcpConnections[index].autoConnect = !s.tcpConnections[index].autoConnect;
    applyAndSave();
    // Apply immediately -- previously this only took effect after a reboot,
    // since the live TCPClientInterface set is owned by main.cpp and nothing
    // here ever told it to rebuild. Disabling a connection without this left
    // its interface registered with Transport (still possibly the "best"
    // path for some destinations) until a reboot finally tore it down.
    if (_tcpReloadCb) _tcpReloadCb();
    buildTCPMenu();
}

void SettingsScreen::removeTCPConnection(int index) {
    if (!_config) return;
    auto& s = _config->settings();
    if (index < 0 || index >= (int)s.tcpConnections.size()) return;

    s.tcpConnections.erase(s.tcpConnections.begin() + index);
    applyAndSave();
    if (_tcpReloadCb) _tcpReloadCb();
    showToast("Removed");
    buildTCPMenu();
}

// =============================================================================
// SD Card submenu
// =============================================================================

void SettingsScreen::buildSDCardMenu() {
    _list.clear();

    if (_sdStore && _sdStore->isReady()) {
        _list.addItem("Status: INSERTED");

        char buf[32];
        uint64_t total = _sdStore->totalBytes();
        uint64_t used = _sdStore->usedBytes();
        uint64_t free = total > used ? total - used : 0;

        snprintf(buf, sizeof(buf), "Total: %llu MB", total / (1024 * 1024));
        _list.addItem(buf);
        snprintf(buf, sizeof(buf), "Used: %llu MB", used / (1024 * 1024));
        _list.addItem(buf);
        snprintf(buf, sizeof(buf), "Free: %llu MB", free / (1024 * 1024));
        _list.addItem(buf);

        _list.addItem("Initialize SD Storage");
        _list.addItem("Wipe All Data", Theme::ERROR);
    } else {
        _list.addItem("Status: NOT INSERTED");
    }

    _list.addItem("< Back");
}

void SettingsScreen::sdCardFormat() {
    if (!_sdStore || !_sdStore->isReady()) {
        showToast("No SD card");
        return;
    }

    if (_sdStore->formatForStandalone()) {
        showToast("SD initialized!");
    } else {
        showToast("Init failed");
    }
    buildSDCardMenu();
}

// =============================================================================
// WiFi Scanner
// =============================================================================

void SettingsScreen::startWiFiScan() {
    _list.clear();
    _list.addItem("Scanning...");

    Serial.println("[WIFI] Starting network scan...");

    // Disconnect from current network to free the radio for scanning
    WiFi.disconnect(false);  // disconnect but don't erase credentials
    delay(100);

    // Ensure WiFi is on in STA mode for scanning
    if (WiFi.getMode() == WIFI_OFF) {
        WiFi.mode(WIFI_STA);
    }

    int n = WiFi.scanNetworks(false, false);
    _scanResults.clear();

    if (n > 0) {
        for (int i = 0; i < n; i++) {
            WiFiNetwork net;
            net.ssid = WiFi.SSID(i);
            net.rssi = WiFi.RSSI(i);
            net.encType = WiFi.encryptionType(i);
            if (!net.ssid.isEmpty()) {
                _scanResults.push_back(net);
            }
        }
        // Sort by signal strength (strongest first)
        std::sort(_scanResults.begin(), _scanResults.end(),
            [](const WiFiNetwork& a, const WiFiNetwork& b) {
                return a.rssi > b.rssi;
            });
    }

    WiFi.scanDelete();
    Serial.printf("[WIFI] Found %d networks\n", (int)_scanResults.size());

    _subMenu = MENU_WIFI_SCAN;
    buildScanResultsMenu();
}

void SettingsScreen::buildScanResultsMenu() {
    _list.clear();

    if (_scanResults.empty()) {
        _list.addItem("No networks found");
    } else {
        for (auto& net : _scanResults) {
            char buf[48];
            const char* lock = (net.encType == WIFI_AUTH_OPEN) ? "" : "*";
            snprintf(buf, sizeof(buf), "%s%s (%d dBm)",
                     lock, net.ssid.c_str(), net.rssi);
            _list.addItem(buf);
        }
    }

    _list.addItem("Rescan");
    _list.addItem("< Back");
}

void SettingsScreen::disconnectWiFi() {
    WiFi.disconnect(false);
    showToast("Disconnected");
    buildWiFiMenu();
}

void SettingsScreen::connectWiFi() {
    auto& s = _config->settings();
    if (s.wifiSTASSID.isEmpty()) {
        showToast("No SSID set");
        return;
    }
    WiFi.disconnect(false);
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.begin(s.wifiSTASSID.c_str(), s.wifiSTAPassword.c_str());
    showToast("Connecting...");
    buildWiFiMenu();
}

void SettingsScreen::selectNetwork(int index) {
    if (index < 0 || index >= (int)_scanResults.size()) return;
    if (!_config) return;

    auto& s = _config->settings();
    s.wifiSTASSID = _scanResults[index].ssid;
    Serial.printf("[WIFI] Selected: %s\n", s.wifiSTASSID.c_str());

    // Open password editor
    _subMenu = MENU_WIFI;
    // field 1 = STA password in WiFi STA mode
    startEditing(1, s.wifiSTAPassword.c_str());
}

// =============================================================================
// Display / Audio menus
// =============================================================================

void SettingsScreen::buildDisplayMenu() {
    _list.clear();
    if (!_config) return;
    auto& s = _config->settings();

    _list.addItem("Screen");
    _list.addItem("Theme");
    String name = s.displayName.isEmpty() ? "(none)" : s.displayName;
    _list.addItem("Device Name: " + std::string(name.c_str()));
    _list.addItem("< Back");
}

void SettingsScreen::buildDisplayScreenMenu() {
    _list.clear();
    if (!_config) return;
    auto& s = _config->settings();
    char buf[40];

    snprintf(buf, sizeof(buf), "Brightness: %d", s.brightness);
    _list.addItem(buf);
    snprintf(buf, sizeof(buf), "Dim timeout: %ds", s.screenDimTimeout);
    _list.addItem(buf);
    snprintf(buf, sizeof(buf), "Off timeout: %ds", s.screenOffTimeout);
    _list.addItem(buf);
    _list.addItem("< Back");
}

void SettingsScreen::buildAudioMenu() {
    _list.clear();
    if (!_config) return;
    auto& s = _config->settings();
    char buf[40];

    _list.addItem(s.audioEnabled ? "Audio: ON" : "Audio: OFF");
    snprintf(buf, sizeof(buf), "Volume: %d%%", s.audioVolume);
    _list.addItem(buf);
    _list.addItem("< Back");
}

// =============================================================================
// Theme submenu — presets, plus per-hue hex editing for a custom theme.
// Colors live in their own plaintext theme.json (see docs/theme-config.md),
// not in the encrypted UserConfig blob — they aren't sensitive, and the
// boot/password screens need a theme before any identity is unlocked.
// =============================================================================

void SettingsScreen::buildThemeMenu() {
    _list.clear();
    char buf[40];

    Theme::BaseColors c = Theme::current();
    snprintf(buf, sizeof(buf), "Active: %s", c.name.c_str());
    _list.addItem(buf);

    int activeIdx = Theme::activePresetIndex();
    for (int i = 0; i < Theme::PRESET_COUNT; i++) {
        char label[40];
        snprintf(label, sizeof(label), "%s[%s]",
                 (i == activeIdx) ? ">" : " ", Theme::PRESETS[i].name);
        _list.addItem(label, (i == activeIdx) ? Theme::PRIMARY : 0);
    }

    // "Custom" is a selectable row in the same list as the presets above —
    // not a separate menu link — so it reads as one more option to pick,
    // not a different kind of action. Selecting it opens the per-hue editor.
    char customLabel[40];
    snprintf(customLabel, sizeof(customLabel), "%s[Custom]", (activeIdx < 0) ? ">" : " ");
    _list.addItem(customLabel, (activeIdx < 0) ? Theme::PRIMARY : 0);
    _list.addItem("< Back");
}

void SettingsScreen::applyThemePreset(int index) {
    if (index < 0 || index >= Theme::PRESET_COUNT) return;
    const Theme::Preset& p = Theme::PRESETS[index];

    Theme::BaseColors c;
    c.name = p.name;
    c.bg = p.bg; c.text = p.text; c.primary = p.primary;
    c.secondary = p.secondary; c.accent = p.accent;
    c.error = p.error; c.warning = p.warning;

    Theme::apply(c);
    Theme::save(_flash, _sdStore);
    if (_ui) _ui->refreshPalette();
    buildThemeMenu();
    showToast("Theme applied!");
}

void SettingsScreen::buildThemeCustomMenu() {
    _list.clear();
    Theme::BaseColors c = Theme::current();

    // Optional alternative to typing hex directly — toggled here, applies to
    // whichever hue you pick below. Defaults to Hex (fastest); Mix opens an
    // RGB slider screen instead.
    _list.addItem(_useMixer ? "Input: Mix" : "Input: Hex");

    for (const auto& f : kThemeFields) {
        _list.addItem(std::string(f.label) + ": " + hexForBaseField(c, f.index));
    }
    _list.addItem("< Back");
}

// Seed the mixer's working R/G/B from the field's current hex value.
void SettingsScreen::startMixer(int field) {
    Theme::BaseColors c = Theme::current();
    std::string hex = hexForBaseField(c, field);
    uint8_t r, g, b;
    if (!parseHex6(hex, r, g, b)) { r = g = b = 0; }
    _mixerField = field;
    _mixerChannel = 0;
    _mixerR = r; _mixerG = g; _mixerB = b;
    _mixerHeldKey = 0;
    _mixerHeldSince = 0;
    _mixerLastStepAt = 0;
    _subMenu = MENU_THEME_MIXER;
}

// How far a single nudge moves the selected channel, given how long the key
// has been held. The UI canvas renders at 8bpp truecolor (3 red bits, 3
// green bits, 2 blue bits — not an indexed palette, despite
// setPaletteColor() calls elsewhere suggesting otherwise), so a fixed step
// of 1 can sit inside the same quantization bucket for up to 31 (R/G) or 63
// (B) consecutive presses with no visible color change at all. The step
// only needs to be big enough that a few presses' worth of *accumulated*
// movement crosses a bucket within a fraction of a second — it doesn't
// need to guarantee a new bucket on every single press, which is what made
// the first version of this ramp feel like it was teleporting. Kept
// deliberately gentle: a tap (or the very start of a hold) still moves by
// exactly 1 for precise, ungapped adjustment, and even at full cruise the
// step is a fraction of a bucket width, not a whole one.
static int mixerStepSize(int channel, unsigned long held) {
    if (held < 500) return 1;
    int unit = (channel == 2) ? 2 : 1;  // blue's bucket is 2x wider than red/green's
    if (held < 1500) return 2 * unit;
    if (held < 3000) return 4 * unit;
    return 8 * unit;
}

// Nudge whichever channel is currently selected, by `amount` (signed).
void SettingsScreen::stepMixerChannel(int amount) {
    uint8_t* chan = (_mixerChannel == 0) ? &_mixerR : (_mixerChannel == 1) ? &_mixerG : &_mixerB;
    int v = (int)*chan + amount;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    *chan = (uint8_t)v;
    if (_ui) _ui->markContentDirty();
}

void SettingsScreen::commitMixer() {
    char hexBuf[7];
    snprintf(hexBuf, sizeof(hexBuf), "%02X%02X%02X", _mixerR, _mixerG, _mixerB);
    Theme::BaseColors c = Theme::current();
    c.name = "Custom";
    switch (_mixerField) {
        case 0: c.bg = hexBuf; break;
        case 1: c.text = hexBuf; break;
        case 2: c.primary = hexBuf; break;
        case 3: c.secondary = hexBuf; break;
        case 4: c.accent = hexBuf; break;
        case 5: c.error = hexBuf; break;
        case 6: c.warning = hexBuf; break;
    }
    Theme::apply(c);
    Theme::save(_flash, _sdStore);
    if (_ui) _ui->refreshPalette();
    showToast("Saved!");
    _subMenu = MENU_THEME_CUSTOM;
    buildThemeCustomMenu();
}

// Start editing a field — show TextInput with current value. `label`
// replaces the generic "Edit value:" prompt; always set explicitly (even to
// "") so a label from a previous edit never leaks into the next one.
void SettingsScreen::startEditing(int field, const std::string& currentValue, const std::string& label) {
    _editField = field;
    _editing = true;
    _editLabel = label;
    _editInput.clear();
    _editInput.setText(currentValue);
    _editInput.setActive(true);
    _editInput.setMaxLength(_subMenu == MENU_THEME_CUSTOM ? 6 : 64);
    _editInput.setSubmitCallback([this](const std::string& value) {
        commitEdit(value);
    });
}

// Apply edited value to settings
void SettingsScreen::commitEdit(const std::string& value) {
    if (!_config) return;
    auto& s = _config->settings();

    if (_subMenu == MENU_RADIO_CONFIG) {
        long v = atol(value.c_str());
        switch (_editField) {
            case 0:  // Frequency: 150MHz-960MHz
                if (v >= 150000000L && v <= 960000000L) s.loraFrequency = (uint32_t)v;
                break;
            case 1:  // SF: 5-12
                if (v >= 5 && v <= 12) s.loraSF = (uint8_t)v;
                break;
            case 2:  // BW: 7800-500000
                if (v >= 7800 && v <= 500000) s.loraBW = (uint32_t)v;
                break;
            case 3:  // CR: 5-8
                if (v >= 5 && v <= 8) s.loraCR = (uint8_t)v;
                break;
            case 4:  // TX Power
                if (v >= -9 && v <= LORA_MAX_TX_POWER) s.loraTxPower = (int8_t)v;
                break;
        }
        applyAndSave();
        buildRadioConfigMenu();
    } else if (_subMenu == MENU_TCP) {
        if (_editField == 99 && !value.empty()) {
            // Host submitted — stash and open port input
            _tcpPendingHost = value;
            _editField = 100;
            _editing = true;
            _editLabel = "Port (1-9999):";
            _editInput.clear();
            _editInput.setText("4242");
            _editInput.setActive(true);
            _editInput.setMaxLength(5);
            _editInput.setNumericOnly(true);
            _editInput.setSubmitCallback([this](const std::string& v) {
                commitEdit(v);
            });
            return;  // Stay in editing mode
        }
        if (_editField == 100 && !value.empty()) {
            // Port submitted — validate and add
            int port = atoi(value.c_str());
            if (port < 1 || port > 9999) {
                showToast("Port 1-9999");
                buildTCPMenu();
            } else {
                addTCPConnection(_tcpPendingHost, (uint16_t)port);
            }
            _tcpPendingHost.clear();
        }
        buildTCPMenu();
    } else if (_subMenu == MENU_WIFI) {
        // Fields: 0=SSID, 1=Password (AP or STA depending on mode)
        if (_config->settings().wifiMode == RAT_WIFI_AP) {
            switch (_editField) {
                case 0: s.wifiAPSSID = value.c_str(); break;
                case 1: s.wifiAPPassword = value.c_str(); break;
            }
        } else if (_config->settings().wifiMode == RAT_WIFI_STA) {
            switch (_editField) {
                case 0: s.wifiSTASSID = value.c_str(); break;
                case 1: s.wifiSTAPassword = value.c_str(); break;
            }
        }
        applyAndSave();
        if (_config->settings().wifiMode == RAT_WIFI_STA && _editField == 1) {
            connectWiFi();  // Live reconnect with new credentials
        } else {
            showToast("Saved!");
        }
        buildWiFiMenu();
    } else if (_subMenu == MENU_DISPLAY) {
        if (_editField == 0) s.displayName = value.c_str();
        applyAndSave();
        buildDisplayMenu();
    } else if (_subMenu == MENU_DISPLAY_SCREEN) {
        switch (_editField) {
            case 0: {
                int v = atoi(value.c_str());
                if (v < 1) v = 1;
                if (v > 255) v = 255;
                s.brightness = (uint8_t)v;
                if (_power) _power->setBrightness(s.brightness);
                break;
            }
            case 1: s.screenDimTimeout = (uint16_t)atoi(value.c_str()); break;
            case 2: s.screenOffTimeout = (uint16_t)atoi(value.c_str()); break;
        }
        applyAndSave();
        buildDisplayScreenMenu();
    } else if (_subMenu == MENU_AUDIO) {
        if (_editField == 1) {
            int v = atoi(value.c_str());
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            s.audioVolume = (uint8_t)v;
            if (_audio) _audio->setVolume(s.audioVolume);
        }
        applyAndSave();
        buildAudioMenu();
    } else if (_subMenu == MENU_MESSAGING) {
        if (_editField == 0) {
            int v = constrain(atoi(value.c_str()), 1, 24);
            s.stampCostCeiling = (uint8_t)v;
        }
        applyAndSave();
        buildMessagingMenu();
    } else if (_subMenu == MENU_TIME) {
        if (_editField == 1) {
            int v = atoi(value.c_str());
            if (v >= -12 && v <= 14) s.manualUtcOffsetHours = (int8_t)v;
        }
        applyAndSave();
        buildTimeMenu();
    } else if (_subMenu == MENU_THEME_CUSTOM) {
        // Exactly 6 hex digits or reject outright (previous color kept) —
        // no silent coercion of a malformed/truncated value.
        bool valid = value.size() == 6;
        for (size_t i = 0; valid && i < value.size(); i++) {
            valid = isxdigit((unsigned char)value[i]);
        }
        if (!valid) {
            showToast("Use 6 hex digits (RRGGBB)");
            _editing = false;
            _editField = -1;
            return;
        }
        std::string hex = value;
        for (auto& ch : hex) ch = (char)toupper((unsigned char)ch);

        Theme::BaseColors c = Theme::current();
        c.name = "Custom";
        switch (_editField) {
            case 0: c.bg = hex.c_str(); break;
            case 1: c.text = hex.c_str(); break;
            case 2: c.primary = hex.c_str(); break;
            case 3: c.secondary = hex.c_str(); break;
            case 4: c.accent = hex.c_str(); break;
            case 5: c.error = hex.c_str(); break;
            case 6: c.warning = hex.c_str(); break;
        }
        Theme::apply(c);
        Theme::save(_flash, _sdStore);
        if (_ui) _ui->refreshPalette();
        showToast("Saved!");
        buildThemeCustomMenu();
    }

    _editing = false;
    _editField = -1;
}

// Get current value of a field as string for editing
std::string SettingsScreen::getCurrentValue(SubMenu menu, int field) {
    if (!_config) return "";
    auto& s = _config->settings();
    char buf[32];

    if (menu == MENU_RADIO_CONFIG) {
        switch (field) {
            case 0: snprintf(buf, sizeof(buf), "%lu", (unsigned long)s.loraFrequency); return buf;
            case 1: snprintf(buf, sizeof(buf), "%d", s.loraSF); return buf;
            case 2: snprintf(buf, sizeof(buf), "%lu", (unsigned long)s.loraBW); return buf;
            case 3: snprintf(buf, sizeof(buf), "%d", s.loraCR); return buf;
            case 4: snprintf(buf, sizeof(buf), "%d", s.loraTxPower); return buf;
        }
    } else if (menu == MENU_WIFI) {
        if (s.wifiMode == RAT_WIFI_AP) {
            switch (field) {
                case 0: return s.wifiAPSSID.c_str();
                case 1: return s.wifiAPPassword.c_str();
            }
        } else if (s.wifiMode == RAT_WIFI_STA) {
            switch (field) {
                case 0: return s.wifiSTASSID.c_str();
                case 1: return s.wifiSTAPassword.c_str();
            }
        }
    } else if (menu == MENU_DISPLAY) {
        if (field == 0) return s.displayName.c_str();
    } else if (menu == MENU_DISPLAY_SCREEN) {
        switch (field) {
            case 0: snprintf(buf, sizeof(buf), "%d", s.brightness); return buf;
            case 1: snprintf(buf, sizeof(buf), "%d", s.screenDimTimeout); return buf;
            case 2: snprintf(buf, sizeof(buf), "%d", s.screenOffTimeout); return buf;
        }
    } else if (menu == MENU_AUDIO) {
        if (field == 1) { snprintf(buf, sizeof(buf), "%d", s.audioVolume); return buf; }
    } else if (menu == MENU_MESSAGING) {
        if (field == 0) { snprintf(buf, sizeof(buf), "%d", s.stampCostCeiling); return buf; }
    } else if (menu == MENU_THEME_CUSTOM) {
        return hexForBaseField(Theme::current(), field);
    }
    return "";
}

void SettingsScreen::render(M5Canvas& canvas) {
    if (_subMenu == MENU_ABOUT) {
        renderAbout(canvas);
        return;
    }

    int y0 = Theme::CONTENT_Y;

    // Header with accent bar
    const char* headers[] = {"SETTINGS", "RADIO", "WIFI", "TCP CONNECTIONS",
                             "SD CARD", "DISPLAY", "AUDIO", "ABOUT", "WIFI SCAN", "THEME",
                             "SCREEN", "CUSTOM THEME", "MIX COLOR", "TIME", "SECURITY",
                             "RADIO CONFIG", "MESSAGING"};
    const int headerH = Theme::SECTION_HEADER_H;
    canvas.fillRect(0, y0, Theme::CONTENT_W, headerH, Theme::BG_SURFACE);
    canvas.fillRect(0, y0 + 2, 3, headerH - 4, Theme::ACCENT);
    canvas.setTextColor(Theme::ACCENT);
    Theme::useUiFont(canvas);
    canvas.drawString(headers[_subMenu], 8, y0 + 2);
    canvas.drawFastHLine(0, y0 + headerH, Theme::CONTENT_W, Theme::DIVIDER);
    Theme::useSmallFont(canvas);

    if (_subMenu == MENU_THEME_MIXER) {
        renderMixer(canvas, y0, headerH);
    } else if (_editing) {
        // Show field name
        canvas.setTextColor(Theme::MUTED);
        std::string label = _editLabel.empty() ? "Edit value:" : _editLabel;
        canvas.drawString(label.c_str(), 4, y0 + headerH + 5);

        // Show text input
        _editInput.render(canvas, 0, y0 + headerH + 19, Theme::CONTENT_W);

        if (_subMenu == MENU_THEME_CUSTOM) {
            // Live swatch — fills in the moment a valid 6-digit hex value is
            // typed, updates on every further keystroke. No instructional
            // text needed: typing a hex code and pressing Enter is the same
            // interaction as every other field in Settings.
            uint8_t r, g, b;
            if (parseHex6(_editInput.getText(), r, g, b)) {
                int sy = y0 + headerH + 35;
                uint16_t swatch = Theme::rgb565(r, g, b);
                canvas.fillRect(4, sy, 28, 16, swatch);
                canvas.drawRect(4, sy, 28, 16, Theme::BORDER);
            }
        } else {
            canvas.setTextColor(Theme::MUTED);
            canvas.drawString("Enter=save  Esc=cancel", 4, y0 + headerH + 36);
        }
    } else {
        _list.render(canvas, 0, y0 + headerH + 2, Theme::CONTENT_W,
                     Theme::CONTENT_H - headerH - 3);
    }

    // Confirmation dialog overlay
    if (_confirmPending) {
        const char* prompt = _confirmAction == 0 ? "Factory Reset? Y/N" :
                             _confirmAction == 1 ? "Wipe SD Data? Y/N" :
                                                    "Disable Duress PW? Y/N";
        int tw = strlen(prompt) * Theme::CHAR_W + 16;
        int th = Theme::CHAR_H + 10;
        int tx = (Theme::CONTENT_W - tw) / 2;
        int ty = Theme::CONTENT_Y + Theme::CONTENT_H / 2 - th / 2;
        canvas.fillRoundRect(tx, ty, tw, th, 3, Theme::BG);
        canvas.drawRoundRect(tx, ty, tw, th, 3, Theme::ERROR);
        canvas.setTextColor(Theme::ERROR);
        canvas.setCursor(tx + 8, ty + 5);
        canvas.print(prompt);
    }

    // Shared popup for every small fixed-choice setting (WiFi Mode,
    // Auto-Lock, Duress action, on/off toggles...) — see PopupPurpose.
    _optionPopup.render(canvas);

    // Duress password popup overlay — small modal, same family as the
    // confirm dialog above, with a masked input field in place of Y/N.
    if (_duressEntryActive) {
        const char* title = _duressStage == 0 ? "Duress Password" : "Confirm Password";
        const char* hint  = "Enter=next  Esc=cancel";
        int bw = 150;
        int bh = 48;
        int bx = (Theme::CONTENT_W - bw) / 2;
        int by = Theme::CONTENT_Y + (Theme::CONTENT_H - bh) / 2;
        canvas.fillRoundRect(bx, by, bw, bh, 3, Theme::BG);
        canvas.drawRoundRect(bx, by, bw, bh, 3, Theme::PRIMARY);
        canvas.setTextColor(Theme::PRIMARY);
        canvas.setCursor(bx + 6, by + 5);
        canvas.print(title);
        _duressInput.render(canvas, bx + 6, by + 16, bw - 12);
        canvas.setTextColor(Theme::MUTED);
        canvas.setCursor(bx + 6, by + 37);
        canvas.print(hint);
    }

    // Toast overlay (drawn on top of everything)
    if (_toastMessage && millis() < _toastUntil) {
        int tw = strlen(_toastMessage) * Theme::CHAR_W + 12;
        int th = Theme::CHAR_H + 8;
        int tx = (Theme::CONTENT_W - tw) / 2;
        int ty = Theme::CONTENT_Y + Theme::CONTENT_H - th - 4;
        canvas.fillRoundRect(tx, ty, tw, th, 3, Theme::SELECTION_BG);
        canvas.drawRoundRect(tx, ty, tw, th, 3, Theme::PRIMARY);
        canvas.setTextColor(Theme::PRIMARY);
        canvas.setCursor(tx + 6, ty + 4);
        canvas.print(_toastMessage);
    } else {
        _toastMessage = nullptr;
    }
}

void SettingsScreen::renderMixer(M5Canvas& canvas, int y0, int headerH) {
    const char* fieldName = labelForBaseField(_mixerField);

    int y = y0 + headerH + 4;
    canvas.setTextColor(Theme::ACCENT);
    canvas.drawString(fieldName, 4, y);
    y += 13;

    const char* chLabels[3] = {"R", "G", "B"};
    uint8_t chVals[3] = {_mixerR, _mixerG, _mixerB};
    const int barX = 16, barW = 130, barH = 9;

    for (int i = 0; i < 3; i++) {
        bool sel = (_mixerChannel == i);
        canvas.setTextColor(sel ? Theme::PRIMARY : Theme::TEXT_SECONDARY);
        canvas.drawString(chLabels[i], 4, y + 1);

        canvas.drawRect(barX, y, barW, barH, sel ? Theme::PRIMARY : Theme::BORDER);
        int fillW = (barW - 2) * chVals[i] / 255;
        if (fillW > 0) {
            canvas.fillRect(barX + 1, y + 1, fillW, barH - 2, sel ? Theme::PRIMARY : Theme::TEXT_MUTED);
        }

        char valBuf[8];
        snprintf(valBuf, sizeof(valBuf), "%3d", chVals[i]);
        canvas.setTextColor(sel ? Theme::PRIMARY : Theme::TEXT_SECONDARY);
        canvas.drawString(valBuf, barX + barW + 6, y + 1);

        y += barH + 4;
    }

    y += 3;
    uint16_t swatch = Theme::rgb565(_mixerR, _mixerG, _mixerB);
    canvas.fillRect(4, y, 28, 16, swatch);
    canvas.drawRect(4, y, 28, 16, Theme::BORDER);
    char hexBuf[8];
    snprintf(hexBuf, sizeof(hexBuf), "#%02X%02X%02X", _mixerR, _mixerG, _mixerB);
    canvas.setTextColor(Theme::TEXT_SECONDARY);
    canvas.drawString(hexBuf, 38, y + 4);
}

void SettingsScreen::renderAbout(M5Canvas& canvas) {
    int y0 = Theme::CONTENT_Y;
    const int headerH = Theme::SECTION_HEADER_H;
    canvas.fillRect(0, y0, Theme::CONTENT_W, headerH, Theme::BG_SURFACE);
    canvas.fillRect(0, y0 + 2, 3, headerH - 4, Theme::ACCENT);
    Theme::useUiFont(canvas);
    canvas.setTextColor(Theme::ACCENT);
    canvas.drawString("About", 8, y0 + 2);
    canvas.drawFastHLine(0, y0 + headerH, Theme::CONTENT_W, Theme::DIVIDER);

    Theme::useSmallFont(canvas);
    int y = y0 + headerH + 5;
    canvas.setTextColor(Theme::PRIMARY);
    canvas.drawString("Cryptspeak v" RSCARDPUTER_VERSION_STRING, 4, y); y += 10;

    canvas.setTextColor(Theme::SECONDARY);
    canvas.drawString("M5Stack Cardputer Adv", 4, y); y += 10;
    canvas.drawString("Cap LoRa-1262 (SX1262)", 4, y); y += 10;

    canvas.setTextColor(Theme::MUTED);
    canvas.drawString("rsCardputer-CE", 4, y); y += 12;

    canvas.setTextColor(Theme::SECONDARY);
    String idLine = "ID: " + _identityHash;
    canvas.drawString(idLine.c_str(), 4, y); y += 10;

    char heap[32];
    snprintf(heap, sizeof(heap), "Heap: %lu bytes", (unsigned long)ESP.getFreeHeap());
    canvas.drawString(heap, 4, y); y += 10;

    char uptime[32];
    snprintf(uptime, sizeof(uptime), "Up: %lus", millis() / 1000);
    canvas.drawString(uptime, 4, y); y += 12;

    canvas.setTextColor(Theme::MUTED);
    canvas.drawString("[Esc: back]", 4, y);
}

bool SettingsScreen::handleKey(const KeyEvent& event) {
    // Handle confirmation dialog
    if (_confirmPending) {
        if (event.character == 'y' || event.character == 'Y') {
            _confirmPending = false;
            if (_confirmAction == 0) {
                factoryReset();
            } else if (_confirmAction == 1) {
                if (_sdStore && _sdStore->isReady()) {
                    if (_sdStore->wipeStandalone()) {
                        showToast("SD wiped!");
                    } else {
                        showToast("Wipe failed");
                    }
                    buildSDCardMenu();
                }
            } else if (_confirmAction == 2) {
                Duress::disable();
                buildSecurityMenu();
                showToast("Duress password disabled");
            }
        } else {
            _confirmPending = false;
            showToast("Cancelled");
        }
        return true;
    }

    // Shared popup for every small fixed-choice setting — Esc cancels
    // (handled inside OptionPopup), Enter confirms and dispatches through
    // onPopupConfirmed() based on the purpose set by openPopup().
    if (_optionPopup.isActive()) {
        bool confirmed = false;
        int result = -1;
        _optionPopup.handleKey(event, &confirmed, &result);
        if (confirmed) onPopupConfirmed(result);
        return true;
    }

    // Duress password popup — small inline modal (see render()). Esc cancels
    // at any point and discards anything typed so far; everything else goes
    // to the masked text field.
    if (_duressEntryActive) {
        if (event.character == 27) {
            cancelDuressSetup();
            return true;
        }
        _duressInput.handleKey(event);
        return true;
    }

    // RGB mixer (Theme > Custom > Input: Mix > <field>) — owns input while
    // active. ';'/'.' switch channel, Enter saves, Esc/Del cancels. ','/'/'
    // apply one immediate step here (tap = single nudge); a sustained hold
    // is picked up by tick() polling continuous key state, which is what
    // actually accelerates — see Keyboard::isKeyPressed().
    if (_subMenu == MENU_THEME_MIXER) {
        if (event.character == 27 || event.del) {
            _mixerHeldKey = 0;
            _subMenu = MENU_THEME_CUSTOM;
            buildThemeCustomMenu();
            return true;
        }
        if (event.enter) {
            _mixerHeldKey = 0;
            commitMixer();
            return true;
        }
        if (event.character == ';') { _mixerChannel = (_mixerChannel + 2) % 3; return true; }
        if (event.character == '.') { _mixerChannel = (_mixerChannel + 1) % 3; return true; }
        if (event.character == ',' || event.character == '/') {
            stepMixerChannel(event.character == ',' ? -1 : 1);  // tap = exact, ungapped single step
            _mixerHeldKey = event.character;
            _mixerHeldSince = millis();
            _mixerLastStepAt = _mixerHeldSince;
            return true;
        }
        return true;  // swallow anything else while the mixer is active
    }

    // ESC or Delete goes back
    if (event.character == 27 || (event.del && !_editing)) {
        if (_editing) {
            _editing = false;
            _editField = -1;
            _tcpPendingHost.clear();
            _editLabel.clear();
            return true;
        }
        if (_subMenu == MENU_TCP) {
            _subMenu = MENU_WIFI;
            buildWiFiMenu();
            return true;
        }
        if (_subMenu == MENU_WIFI_SCAN) {
            _subMenu = MENU_WIFI;
            buildWiFiMenu();
            return true;
        }
        if (_subMenu == MENU_DISPLAY_SCREEN || _subMenu == MENU_THEME) {
            _subMenu = MENU_DISPLAY;
            buildDisplayMenu();
            return true;
        }
        if (_subMenu == MENU_THEME_CUSTOM) {
            _subMenu = MENU_THEME;
            buildThemeMenu();
            return true;
        }
        if (_subMenu == MENU_RADIO_CONFIG) {
            _subMenu = MENU_RADIO;
            buildRadioMenu();
            return true;
        }
        if (_subMenu != MENU_MAIN) {
            _subMenu = MENU_MAIN;
            buildMainMenu();
            return true;
        }
        return false;
    }

    if (_editing) {
        if (_editInput.handleKey(event)) return true;
        return false;
    }

    // Delete key in TCP submenu removes selected connection
    if (event.del && _subMenu == MENU_TCP) {
        int sel = _list.getSelectedIndex();
        int tcpIdx = sel - 1;  // item 0 is "Add", items 1..N are connections
        if (tcpIdx >= 0 && tcpIdx < (int)_config->settings().tcpConnections.size()) {
            removeTCPConnection(tcpIdx);
            return true;
        }
    }

    // Navigation: ; = up, . = down
    if (event.character == ';') {
        _list.scrollUp();
        return true;
    }
    if (event.character == '.') {
        _list.scrollDown();
        return true;
    }

    // Enter — select item
    if (event.enter) {
        int sel = _list.getSelectedIndex();

        if (_subMenu == MENU_MAIN) {
            switch (sel) {
                case 0: _subMenu = MENU_RADIO; buildRadioMenu(); break;
                case 1: _subMenu = MENU_WIFI; buildWiFiMenu(); break;
                case 2: _subMenu = MENU_DISPLAY; buildDisplayMenu(); break;
                case 3: _subMenu = MENU_AUDIO; buildAudioMenu(); break;
                case 4: _subMenu = MENU_TIME; buildTimeMenu(); break;
                case 5: _subMenu = MENU_SECURITY; buildSecurityMenu(); break;
                case 6: _subMenu = MENU_MESSAGING; buildMessagingMenu(); break;
                case 7: _subMenu = MENU_SDCARD; buildSDCardMenu(); break;
                case 8: _subMenu = MENU_ABOUT; break;
                case 9: _confirmPending = true; _confirmAction = 0; break;
            }
            return true;
        }

        // WiFi scan results handling
        if (_subMenu == MENU_WIFI_SCAN) {
            int lastItem = _list.itemCount() - 1;
            int rescanItem = lastItem - 1;

            if (sel == lastItem) {
                // "< Back"
                _subMenu = MENU_WIFI;
                buildWiFiMenu();
            } else if (sel == rescanItem) {
                // "[Rescan]"
                startWiFiScan();
            } else if (sel < (int)_scanResults.size()) {
                selectNetwork(sel);
            }
            return true;
        }

        // "Back" is always last item
        if (sel == _list.itemCount() - 1) {
            if (_subMenu == MENU_TCP) {
                _subMenu = MENU_WIFI;
                buildWiFiMenu();
            } else if (_subMenu == MENU_DISPLAY_SCREEN || _subMenu == MENU_THEME) {
                _subMenu = MENU_DISPLAY;
                buildDisplayMenu();
            } else if (_subMenu == MENU_THEME_CUSTOM) {
                _subMenu = MENU_THEME;
                buildThemeMenu();
            } else if (_subMenu == MENU_RADIO_CONFIG) {
                _subMenu = MENU_RADIO;
                buildRadioMenu();
            } else {
                _subMenu = MENU_MAIN;
                buildMainMenu();
            }
            return true;
        }

        // Radio menu: "Config" opens the editable PHY settings page (the
        // values that used to sit directly in this menu); "Diagnostics"
        // opens a full pushed screen instead (see setOpenDiagnosticsCallback).
        if (_subMenu == MENU_RADIO) {
            if (sel == 0) {
                _subMenu = MENU_RADIO_CONFIG;
                buildRadioConfigMenu();
            } else if (sel == 1) {
                if (_openDiagnosticsCb) _openDiagnosticsCb();
            }
            return true;
        }

        // Display menu navigation (Screen / Theme / Name)
        if (_subMenu == MENU_DISPLAY) {
            switch (sel) {
                case 0: _subMenu = MENU_DISPLAY_SCREEN; buildDisplayScreenMenu(); break;
                case 1: _subMenu = MENU_THEME; buildThemeMenu(); break;
                case 2: startEditing(0, getCurrentValue(MENU_DISPLAY, 0)); break;
            }
            return true;
        }

        // Theme menu: item 0 is "Active:" (non-interactive), items 1..PRESET_COUNT
        // are presets, and the final row is "[Custom]" — selectable the same
        // way as any preset, just opening the per-hue editor instead of
        // applying a fixed palette.
        if (_subMenu == MENU_THEME) {
            if (sel == 0) return true;
            if (sel >= 1 && sel <= Theme::PRESET_COUNT) {
                applyThemePreset(sel - 1);
                return true;
            }
            if (sel == Theme::PRESET_COUNT + 1) {
                _subMenu = MENU_THEME_CUSTOM;
                buildThemeCustomMenu();
                return true;
            }
            return true;
        }

        // Theme > Custom: item 0 opens the Hex/Mix input mode popup, items
        // 1..7 are the base hues — opening either the hex field or the RGB
        // mixer depending on that mode.
        if (_subMenu == MENU_THEME_CUSTOM) {
            if (sel == 0) {
                openPopup(POPUP_THEME_INPUT, "Input Mode", {"Hex", "Mix"}, _useMixer ? 1 : 0);
                return true;
            }
            int row = sel - 1;
            if (row >= 0 && row < 7) {
                int fieldIdx = kThemeFields[row].index;
                if (_useMixer) {
                    startMixer(fieldIdx);
                } else {
                    startEditing(fieldIdx, getCurrentValue(MENU_THEME_CUSTOM, fieldIdx),
                                 std::string(kThemeFields[row].label) + " (hex RRGGBB):");
                }
            }
            return true;
        }

        // Audio on/off (item 0 in Audio menu)
        if (_subMenu == MENU_AUDIO && sel == 0) {
            auto& s = _config->settings();
            openPopup(POPUP_AUDIO, "Audio", {"On", "Off"}, s.audioEnabled ? 0 : 1);
            return true;
        }

        // Time menu: item 0 opens the Time Source popup; item 1 (only
        // present while manual is on) edits the UTC offset.
        if (_subMenu == MENU_TIME) {
            auto& s = _config->settings();
            if (sel == 0) {
                openPopup(POPUP_TIME_SOURCE, "Time Source", {"Auto (GPS)", "Manual"},
                          s.manualTimezoneEnabled ? 1 : 0);
                return true;
            }
            if (sel == 1 && s.manualTimezoneEnabled) {
                char buf[8];
                snprintf(buf, sizeof(buf), "%d", s.manualUtcOffsetHours);
                startEditing(1, buf, "UTC Offset (-12 to 14):");
                return true;
            }
            return true;  // Back (last item) handled above
        }

        // Security menu: Duress Password and Auto-Lock both open a popup
        // (see openPopup/onPopupConfirmed) instead of a dedicated page.
        if (_subMenu == MENU_SECURITY) {
            if (sel == 0) {
                bool configured = Duress::isConfigured();
                std::vector<std::string> options = {configured ? "Disable" : "Enable"};
                if (configured) options.push_back("Reset Password");
                openPopup(POPUP_DURESS_ACTION, "Duress Password", options, 0);
                return true;
            }
            if (sel == 1) {
                std::vector<std::string> options;
                int current = 0;
                uint16_t minutes = _config ? _config->settings().autoLockMinutes : 0;
                for (int i = 0; i < kAutoLockOptionCount; i++) {
                    options.push_back(kAutoLockOptions[i].label);
                    if (kAutoLockOptions[i].minutes == minutes) current = i;
                }
                openPopup(POPUP_AUTOLOCK, "Auto-Lock", options, current);
                return true;
            }
            return true;  // Back (last item) handled above
        }

        // WiFi mode (item 0 in WiFi menu) — opens the mode popup instead of
        // blindly cycling on every Enter press.
        if (_subMenu == MENU_WIFI && sel == 0) {
            openPopup(POPUP_WIFI_MODE, "WiFi Mode", {"OFF", "AP", "STA"},
                      (int)_config->settings().wifiMode);
            return true;
        }

        // STA mode WiFi menu actions
        if (_subMenu == MENU_WIFI && _config->settings().wifiMode == RAT_WIFI_STA) {
            if (sel == 1) {
                // Status line (non-interactive)
                return true;
            }
            if (sel == 2) {
                // [Disconnect] or [Connect]
                if (WiFi.status() == WL_CONNECTED) {
                    disconnectWiFi();
                } else {
                    connectWiFi();
                }
                return true;
            }
            if (sel == 3) {
                // [Scan Networks]
                startWiFiScan();
                return true;
            }
            if (sel == 4) {
                // > TCP Connections
                _subMenu = MENU_TCP;
                buildTCPMenu();
                return true;
            }
            if (sel == 5) {
                // Auto-discover LAN (AutoInterface) on/off popup.
                auto& s = _config->settings();
                openPopup(POPUP_AUTO_LAN, "Auto-discover LAN", {"On", "Off"},
                          s.autoIfaceEnabled ? 0 : 1);
                return true;
            }
        }

        // SD Card menu actions
        if (_subMenu == MENU_SDCARD) {
            if (_sdStore && _sdStore->isReady()) {
                // Item 4 = Initialize, Item 5 = Wipe All Data
                if (sel == 4) {
                    sdCardFormat();
                    return true;
                }
                if (sel == 5) {
                    _confirmPending = true;
                    _confirmAction = 1;
                    return true;
                }
            }
            // Info items are non-interactive
            return true;
        }

        // TCP submenu actions
        if (_subMenu == MENU_TCP) {
            if (sel == 0) {
                // Add new connection — open host input first
                _editField = 99;
                _editing = true;
                _editLabel = "Host:";
                _editInput.clear();
                _editInput.setActive(true);
                _editInput.setMaxLength(64);
                _editInput.setNumericOnly(false);
                _editInput.setSubmitCallback([this](const std::string& value) {
                    commitEdit(value);
                });
                return true;
            }
            int tcpIdx = sel - 1;
            if (tcpIdx >= 0 && tcpIdx < (int)_config->settings().tcpConnections.size()) {
                toggleTCPConnection(tcpIdx);
                return true;
            }
            return true;  // Back handled above
        }

        // Edit the selected field (offset by 1 for WiFi mode; everything else
        // reached here — Radio, Display > Screen — has no header row, so the
        // list index is the field index directly)
        int fieldIdx = sel;
        if (_subMenu == MENU_WIFI) fieldIdx -= 1;
        std::string currentVal = getCurrentValue(_subMenu, fieldIdx);
        startEditing(fieldIdx, currentVal);
        return true;
    }

    return false;
}

// Continuous polling for the mixer's accelerating hold-to-repeat: the first
// step comes from handleKey() on the press edge (instant feedback on a
// tap, always exactly 1 — see mixerStepSize()). From here on, while the
// same key stays physically down, both the repeat rate AND the step size
// ramp up together the longer it's held — starts as deliberate, ungapped
// single nudges, ends at a fast cruise that jumps a full RGB332 bucket per
// step. Resets the moment the key is released or swapped.
void SettingsScreen::tick() {
    if (_subMenu != MENU_THEME_MIXER || !_keyboard || _mixerHeldKey == 0) {
        return;
    }

    if (!_keyboard->isKeyPressed(_mixerHeldKey)) {
        _mixerHeldKey = 0;
        return;
    }

    unsigned long now = millis();
    unsigned long held = now - _mixerHeldSince;
    unsigned long interval;
    if (held < 500)        interval = 200;  // still feels like deliberate taps
    else if (held < 1500)  interval = 130;
    else if (held < 3000)  interval = 80;
    else                   interval = 50;   // fast cruise — same tiers as mixerStepSize()

    if (now - _mixerLastStepAt >= interval) {
        _mixerLastStepAt = now;
        int direction = (_mixerHeldKey == ',') ? -1 : 1;
        stepMixerChannel(direction * mixerStepSize(_mixerChannel, held));
    }
}

void SettingsScreen::showToast(const char* msg, unsigned long durationMs) {
    _toastMessage = msg;
    _toastUntil = millis() + durationMs;
}

void SettingsScreen::applyAndSave() {
    if (!_config || !_flash) return;

    // Save to both SD + flash when SD is available
    if (_sdStore && _sdStore->isReady()) {
        _config->save(*_sdStore, *_flash);
    } else {
        _config->save(*_flash);
    }

    auto& s = _config->settings();

    // Apply radio settings to hardware
    if (_radio) {
        _radio->setFrequency(s.loraFrequency);
        _radio->setSpreadingFactor(s.loraSF);
        _radio->setSignalBandwidth(s.loraBW);
        _radio->setCodingRate4(s.loraCR);
        _radio->setTxPower(s.loraTxPower);
        _radio->receive();  // Re-enter RX after reconfiguration
    }

    // Apply power settings
    if (_power) {
        _power->setDimTimeout(s.screenDimTimeout);
        _power->setOffTimeout(s.screenOffTimeout);
        _power->setBrightness(s.brightness);
        _power->setAutoLockTimeout(s.autoLockMinutes);
    }

    // Apply audio settings
    if (_audio) {
        _audio->setEnabled(s.audioEnabled);
        _audio->setVolume(s.audioVolume);
    }

    // Apply LXMF stamp cost ceiling — cached inside LXMFManager (not read
    // from UserConfig live), so edits here need an explicit push or they'd
    // only take effect after reboot.
    if (_lxmf) {
        _lxmf->setStampCostCeiling(s.stampCostCeiling);
    }

#if HAS_GPS
    // Apply timezone override (no-op if manual mode is off — GPS keeps
    // estimating on its own)
    if (_gps) {
        _gps->setManualOffset(s.manualTimezoneEnabled, s.manualUtcOffsetHours);
    }
#endif

    Serial.println("[SETTINGS] Saved and applied");
    if (!_toastMessage) showToast("Saved!");
}

void SettingsScreen::factoryReset() {
    Serial.println("[SETTINGS] Factory reset — wiping ALL data");
    FactoryWipe::wipeAll(_flash, _sdStore);
    Serial.println("[RESET] Factory reset complete — rebooting");
    delay(500);
    ESP.restart();
}
