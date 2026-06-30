#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include <Identity.h>
#include "storage/FlashStore.h"
#include "storage/SDStore.h"
#include "config/Config.h"
#include "config/BoardConfig.h"

enum RatWiFiMode : uint8_t { RAT_WIFI_OFF = 0, RAT_WIFI_AP = 1, RAT_WIFI_STA = 2 };

struct TCPEndpoint {
    String host;
    uint16_t port = TCP_DEFAULT_PORT;
    bool autoConnect = true;
};

struct UserSettings {
    // Radio
    uint32_t loraFrequency = LORA_DEFAULT_FREQ;
    uint8_t loraSF = LORA_DEFAULT_SF;
    uint32_t loraBW = LORA_DEFAULT_BW;
    uint8_t loraCR = LORA_DEFAULT_CR;
    int8_t loraTxPower = LORA_DEFAULT_TX_POWER;
    uint8_t radioRegion = REGION_AMERICAS;

    // WiFi
    RatWiFiMode wifiMode = RAT_WIFI_OFF;
    String wifiAPSSID;
    String wifiAPPassword = WIFI_AP_PASSWORD;
    String wifiSTASSID;
    String wifiSTAPassword;

    // TCP outbound connections (STA mode only)
    std::vector<TCPEndpoint> tcpConnections;

    // Display
    uint16_t screenDimTimeout = 30;   // seconds
    uint16_t screenOffTimeout = 60;   // seconds
    uint8_t brightness = 255;

    // Audio
    bool audioEnabled = true;
    uint8_t audioVolume = 80;  // 0-100

    // GPS — primary time source; gives UTC directly, plus a TimeZoneDB/
    // longitude-estimated local offset (see GPSManager)
    bool gpsTimeEnabled = true;      // Sync system clock from GPS
    bool gpsLocationEnabled = false; // Track position (user must opt in)

    // Manual timezone override — off by default (GPS estimates the local
    // offset automatically). When on, manualUtcOffsetHours is used as-is
    // instead, with no automatic DST adjustment.
    bool manualTimezoneEnabled = false;
    int8_t manualUtcOffsetHours = 0;

    // Security — auto-lock reboots the device back to the at-rest password
    // screen after this many minutes of inactivity (0 = disabled). Unlike
    // screenOffTimeout (which just blanks the display), this discards the
    // already-decrypted identity from RAM, so the real password must be
    // re-entered before the device functions again.
    uint16_t autoLockMinutes = 0;

    // Setup
    bool radioConfigured = false; // false = show LoRa setup wizard at boot

    // AutoInterface (IPv6 multicast LAN auto-discovery; opt-in)
    bool   autoIfaceEnabled  = false;
    String autoIfaceGroupId  = "reticulum";
    uint8_t autoIfaceMaxPeers = 4;

    // Identity
    String displayName;

    // LXMF anti-spam stamps — costs at or below this are generated
    // automatically in the background; above it, the user is asked first.
    // See LXStamper.h for what "cost" means on this hardware.
    uint8_t stampCostCeiling = 12;
};

class UserConfig {
public:
    // Flash-only (original API, kept for compatibility)
    bool load(FlashStore& flash);
    bool save(FlashStore& flash);

    // Dual-backend: SD primary, flash fallback
    bool load(SDStore& sd, FlashStore& flash);
    bool save(SDStore& sd, FlashStore& flash);

    UserSettings& settings() { return _settings; }
    const UserSettings& settings() const { return _settings; }

    // ── At-rest encryption ──────────────────────────────────────────────
    // Provide the loaded RNS::Identity (with private key) so settings
    // (including WiFi passwords and TCP endpoints) are encrypted before
    // they hit disk/NVS and decrypted on load. Identity is held by
    // pointer; lifetime must outlive this object. Calling with nullptr
    // disables encryption (legacy/test path).
    void setIdentity(const RNS::Identity* identity) { _identity = identity; }
    bool encryptionEnabled() const { return _identity != nullptr && *_identity; }

private:
    bool parseJson(const String& json);
    String serializeToJson();
    void sanitizeSettings();
    String maybeEncrypt(const String& json) const;
    bool maybeDecrypt(const String& raw, String& out) const;

    UserSettings _settings;
    const RNS::Identity* _identity = nullptr;
};
