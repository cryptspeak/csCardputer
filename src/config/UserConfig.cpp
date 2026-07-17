#include "UserConfig.h"
#include "config/BoardConfig.h"
#include "storage/SettingsEncryption.h"
#include <Preferences.h>

// Legacy NVS namespace and key for full config backup.
// TODO: Rename only with explicit NVS migration handling.
static constexpr const char* NVS_NS   = "ratcom_cfg";
static constexpr const char* NVS_KEY  = "json";

// ---------------------------------------------------------------------------
// NVS helpers — bulletproof config storage (internal flash, no SPI bus)
// ---------------------------------------------------------------------------
//
// Stored via putBytes/getBytes (not putString/getString) because an
// encrypted blob is binary and may contain embedded NUL bytes, which would
// truncate a null-terminated PT_STR entry. Pre-encryption devices wrote
// this key as a string (PT_STR); loadFromNVS() falls back to getString()
// so those entries still come back intact on the first boot after upgrade,
// at which point the next saveToNVS() call rewrites it as bytes.

static bool saveToNVS(const String& raw) {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, false)) return false;
    bool ok = prefs.putBytes(NVS_KEY, raw.c_str(), raw.length()) == raw.length();
    prefs.end();
    if (ok) Serial.println("[CONFIG] Saved to NVS");
    return ok;
}

static String loadFromNVS() {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, true)) return "";
    String out;
    size_t len = prefs.getBytesLength(NVS_KEY);
    if (len > 0) {
        std::vector<uint8_t> buf(len);
        prefs.getBytes(NVS_KEY, buf.data(), len);
        out.reserve(len);
        for (size_t i = 0; i < len; i++) out.concat((char)buf[i]);
    } else {
        // Legacy PT_STR entry from a pre-encryption build.
        out = prefs.getString(NVS_KEY, "");
    }
    prefs.end();
    return out;
}

static bool validLoRaFrequency(uint32_t freq) {
    return (freq >= 863000000UL && freq <= 870000000UL) ||
           (freq >= 902000000UL && freq <= 928000000UL) ||
           (freq >= 920000000UL && freq <= 925000000UL);
}

void UserConfig::sanitizeSettings() {
    if (_settings.radioRegion >= REGION_COUNT) _settings.radioRegion = REGION_AMERICAS;
    if (!validLoRaFrequency(_settings.loraFrequency)) {
        _settings.loraFrequency = REGION_FREQ[_settings.radioRegion];
    }
    _settings.loraSF = constrain(_settings.loraSF, 5, 12);
    _settings.loraBW = constrain(_settings.loraBW, 7800UL, 500000UL);
    _settings.loraCR = constrain(_settings.loraCR, 5, 8);
    _settings.loraTxPower = constrain(_settings.loraTxPower, -3, LORA_MAX_TX_POWER);

    _settings.screenDimTimeout = constrain(_settings.screenDimTimeout, 5, 3600);
    _settings.screenOffTimeout = constrain(_settings.screenOffTimeout, 10, 7200);
    if (_settings.screenOffTimeout < _settings.screenDimTimeout) {
        _settings.screenOffTimeout = _settings.screenDimTimeout;
    }
    _settings.brightness = constrain(_settings.brightness, 1, 255);
    _settings.audioVolume = constrain(_settings.audioVolume, 0, 100);

    _settings.manualUtcOffsetHours = constrain(_settings.manualUtcOffsetHours, -12, 14);
    _settings.autoIfaceMaxPeers = constrain(_settings.autoIfaceMaxPeers, 1, 16);

    // 1 (trivial) .. 16 (STAMP_HARD_MAX, see LXMFManager.h) -- clamped to the
    // hardware feasibility ceiling.  0 would mean "never auto-generate," which
    // isn't a real option here since cost>0 from a peer always means *some*
    // stamp is required.
    _settings.stampCostCeiling = constrain(_settings.stampCostCeiling, 1, 16);

    // Auto-lock is a fixed preset list (see SettingsScreen's kAutoLockOptions)
    // — snap anything else (corrupted config, future-removed preset) to off
    // rather than rebooting on some arbitrary leftover value.
    switch (_settings.autoLockMinutes) {
        case 0: case 30: case 60: case 240: case 480: case 720: break;
        default: _settings.autoLockMinutes = 0; break;
    }

    std::vector<TCPEndpoint> cleanTcp;
    cleanTcp.reserve(std::min((size_t)MAX_TCP_CONNECTIONS, _settings.tcpConnections.size()));
    for (auto& ep : _settings.tcpConnections) {
        ep.host.trim();
        if (ep.host.isEmpty() || ep.port == 0) continue;
        cleanTcp.push_back(ep);
        if (cleanTcp.size() >= MAX_TCP_CONNECTIONS) break;
    }
    _settings.tcpConnections = cleanTcp;
}

// ---------------------------------------------------------------------------
// JSON serialization helpers
// ---------------------------------------------------------------------------

bool UserConfig::parseJson(const String& json) {
    Serial.printf("[CONFIG] Parsing config (%d bytes)\n", json.length());

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("[CONFIG] Parse error: %s\n", err.c_str());
        return false;
    }

    _settings.loraFrequency = doc["lora_freq"] | (long)LORA_DEFAULT_FREQ;
    _settings.loraSF        = doc["lora_sf"]   | (int)LORA_DEFAULT_SF;
    _settings.loraBW        = doc["lora_bw"]   | (long)LORA_DEFAULT_BW;
    _settings.loraCR        = doc["lora_cr"]   | (int)LORA_DEFAULT_CR;
    _settings.loraTxPower   = doc["lora_txp"]  | (int)LORA_DEFAULT_TX_POWER;
    _settings.radioRegion   = doc["radio_region"] | (int)REGION_AMERICAS;

    // WiFi mode — migrate from legacy wifi_enabled bool
    int mode = doc["wifi_mode"] | -1;
    if (mode >= 0) {
        _settings.wifiMode = (RatWiFiMode)constrain(mode, 0, 2);
    } else {
        _settings.wifiMode = (doc["wifi_enabled"] | true) ? RAT_WIFI_AP : RAT_WIFI_OFF;
    }
    _settings.wifiAPSSID     = doc["wifi_ap_ssid"]     | "";
    _settings.wifiAPPassword = doc["wifi_ap_pass"]     | WIFI_AP_PASSWORD;
    _settings.wifiSTASSID    = doc["wifi_sta_ssid"]    | "";
    _settings.wifiSTAPassword = doc["wifi_sta_pass"]   | "";

    // TCP outbound connections
    _settings.tcpConnections.clear();
    JsonArray tcpArr = doc["tcp_connections"];
    if (tcpArr) {
        for (JsonObject obj : tcpArr) {
            if (_settings.tcpConnections.size() >= MAX_TCP_CONNECTIONS) break;
            TCPEndpoint ep;
            ep.host = obj["host"] | "";
            ep.port = obj["port"] | TCP_DEFAULT_PORT;
            ep.autoConnect = obj["auto"] | true;
            if (!ep.host.isEmpty()) _settings.tcpConnections.push_back(ep);
        }
    }

    _settings.screenDimTimeout = doc["screen_dim"] | 30;
    _settings.screenOffTimeout = doc["screen_off"] | 60;
    _settings.brightness       = doc["brightness"] | 255;

    _settings.audioEnabled = doc["audio_on"]  | true;
    _settings.audioVolume  = doc["audio_vol"] | 80;

    _settings.gpsTimeEnabled = doc["gps_time"] | true;
    _settings.gpsLocationEnabled = doc["gps_loc"] | false;
    _settings.radioConfigured = doc["radio_configured"] | false;

    _settings.manualTimezoneEnabled = doc["tz_manual"] | false;
    _settings.manualUtcOffsetHours  = doc["tz_manual_offset"] | 0;

    _settings.autoIfaceEnabled  = doc["autoiface_en"]    | false;
    _settings.autoIfaceGroupId  = doc["autoiface_group"] | "reticulum";
    _settings.autoIfaceMaxPeers = doc["autoiface_max"]   | 4;

    _settings.displayName = doc["display_name"] | "";

    _settings.autoLockMinutes = doc["auto_lock_min"] | 0;

    _settings.stampCostCeiling = doc["stamp_cost_ceiling"] | 12;

    Serial.printf("[CONFIG] Loaded: wifi_mode=%d ssid='%s' name='%s'\n",
                  (int)_settings.wifiMode,
                  _settings.wifiSTASSID.c_str(),
                  _settings.displayName.c_str());
    sanitizeSettings();
    return true;
}

String UserConfig::serializeToJson() {
    sanitizeSettings();
    JsonDocument doc;

    doc["lora_freq"] = _settings.loraFrequency;
    doc["lora_sf"]   = _settings.loraSF;
    doc["lora_bw"]   = _settings.loraBW;
    doc["lora_cr"]   = _settings.loraCR;
    doc["lora_txp"]  = _settings.loraTxPower;
    doc["radio_region"] = _settings.radioRegion;

    doc["wifi_mode"] = (int)_settings.wifiMode;
    doc["wifi_ap_ssid"] = _settings.wifiAPSSID;
    doc["wifi_ap_pass"] = _settings.wifiAPPassword;
    doc["wifi_sta_ssid"] = _settings.wifiSTASSID;
    doc["wifi_sta_pass"] = _settings.wifiSTAPassword;

    JsonArray tcpArr = doc["tcp_connections"].to<JsonArray>();
    for (auto& ep : _settings.tcpConnections) {
        JsonObject obj = tcpArr.add<JsonObject>();
        obj["host"] = ep.host;
        obj["port"] = ep.port;
        obj["auto"] = ep.autoConnect;
    }

    doc["screen_dim"] = _settings.screenDimTimeout;
    doc["screen_off"] = _settings.screenOffTimeout;
    doc["brightness"] = _settings.brightness;

    doc["audio_on"]  = _settings.audioEnabled;
    doc["audio_vol"] = _settings.audioVolume;

    doc["gps_time"]     = _settings.gpsTimeEnabled;
    doc["gps_loc"]      = _settings.gpsLocationEnabled;
    doc["radio_configured"] = _settings.radioConfigured;

    doc["tz_manual"]        = _settings.manualTimezoneEnabled;
    doc["tz_manual_offset"] = _settings.manualUtcOffsetHours;

    doc["autoiface_en"]    = _settings.autoIfaceEnabled;
    doc["autoiface_group"] = _settings.autoIfaceGroupId;
    doc["autoiface_max"]   = _settings.autoIfaceMaxPeers;

    doc["display_name"] = _settings.displayName;

    doc["auto_lock_min"] = _settings.autoLockMinutes;

    doc["stamp_cost_ceiling"] = _settings.stampCostCeiling;

    String json;
    serializeJson(doc, json);
    return json;
}

// ---------------------------------------------------------------------------
// At-rest encryption helpers
// ---------------------------------------------------------------------------

// Wraps a JSON String in the at-rest envelope when an identity is
// available. Mirrors MessageStore's maybeEncrypt/maybeDecrypt pattern.
String UserConfig::maybeEncrypt(const String& json) const {
    if (!_identity || !*_identity) return json;
    String envelope;
    if (SettingsEncryption::encryptString(*_identity, json, envelope)) {
        return envelope;
    }
    Serial.println("[CONFIG] encrypt failed — writing plaintext fallback");
    return json;
}

// Inverse of maybeEncrypt. Transparently passes through legacy plaintext
// files (no magic header) so migration can be incremental. Returns false
// if the blob is encrypted but no identity is available, or decryption
// fails (wrong identity / tampered file).
bool UserConfig::maybeDecrypt(const String& raw, String& out) const {
    if (raw.length() == 0) { out = ""; return true; }
    if (!SettingsEncryption::isEncrypted(raw)) { out = raw; return true; }  // legacy
    if (!_identity || !*_identity) return false;
    return SettingsEncryption::decryptOrPassthrough(*_identity, raw, out);
}

// ---------------------------------------------------------------------------
// Flash-only (original API)
// ---------------------------------------------------------------------------

bool UserConfig::load(FlashStore& flash) {
    // Try flash file
    String raw = flash.readString(PATH_USER_CONFIG);
    String json;
    if (!raw.isEmpty() && maybeDecrypt(raw, json) && parseJson(json)) return true;

    // Fall back to NVS
    raw = loadFromNVS();
    if (!raw.isEmpty() && maybeDecrypt(raw, json)) {
        Serial.println("[CONFIG] Recovered from NVS (flash file missing)");
        return parseJson(json);
    }

    Serial.println("[CONFIG] No saved config anywhere, using defaults");
    return false;
}

bool UserConfig::save(FlashStore& flash) {
    String json = serializeToJson();
    String envelope = maybeEncrypt(json);
    bool ok = false;

    if (flash.writeString(PATH_USER_CONFIG, envelope)) {
        Serial.println("[CONFIG] Saved to flash");
        ok = true;
    }

    // Always save full config to NVS — bulletproof backup
    saveToNVS(envelope);

    return ok;
}

// ---------------------------------------------------------------------------
// Dual-backend: SD primary, flash fallback, NVS bulletproof
// ---------------------------------------------------------------------------

bool UserConfig::load(SDStore& sd, FlashStore& flash) {
    // Tier 1: SD card
    if (sd.isReady()) {
        String raw = sd.readString(SD_PATH_USER_CONFIG);
        String json;
        if (!raw.isEmpty() && maybeDecrypt(raw, json)) {
            Serial.println("[CONFIG] Loading from SD card");
            if (parseJson(json)) return true;
        }
    }

    // Tier 2: Flash (LittleFS)
    {
        String raw = flash.readString(PATH_USER_CONFIG);
        String json;
        if (!raw.isEmpty() && maybeDecrypt(raw, json)) {
            Serial.println("[CONFIG] Loading from flash");
            if (parseJson(json)) {
                // Auto-migrate to SD if available
                if (sd.isReady()) {
                    sd.ensureDir("/ratcom");
                    sd.ensureDir("/ratcom/config");
                    String migrateJson = maybeEncrypt(serializeToJson());
                    sd.writeString(SD_PATH_USER_CONFIG, migrateJson);
                    Serial.println("[CONFIG] Migrated to SD");
                }
                return true;
            }
        }
    }

    // Tier 3: NVS (bulletproof — survives flash corruption)
    {
        String raw = loadFromNVS();
        String json;
        if (!raw.isEmpty() && maybeDecrypt(raw, json)) {
            Serial.println("[CONFIG] Recovered full config from NVS");
            if (parseJson(json)) {
                // Re-save to SD/flash to heal the corruption
                save(sd, flash);
                return true;
            }
        }
    }

    Serial.println("[CONFIG] No saved config anywhere, using defaults");
    return false;
}

bool UserConfig::save(SDStore& sd, FlashStore& flash) {
    String json = serializeToJson();
    String envelope = maybeEncrypt(json);
    bool ok = false;

    // Write to SD (primary)
    if (sd.isReady()) {
        sd.ensureDir("/ratcom");
        sd.ensureDir("/ratcom/config");
        if (sd.writeString(SD_PATH_USER_CONFIG, envelope)) {
            Serial.println("[CONFIG] Saved to SD");
            ok = true;
        } else {
            Serial.println("[CONFIG] SD write failed");
        }
    }

    // Write to flash (backup)
    if (flash.writeString(PATH_USER_CONFIG, envelope)) {
        Serial.println("[CONFIG] Saved to flash");
        ok = true;
    }

    // Always save full config to NVS — bulletproof backup
    // NVS is on internal flash, no SPI bus, wear-leveled, checksum-protected
    if (saveToNVS(envelope)) ok = true;

    return ok;
}
