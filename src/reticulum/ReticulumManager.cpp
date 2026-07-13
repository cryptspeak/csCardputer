#include "ReticulumManager.h"
#include "config/Config.h"
#include "storage/FlashStore.h"
#include "storage/KnownDestEncryption.h"
#include "reticulum/IdentityCrypto.h"
#include <Log.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <map>
#include <unordered_map>
#include <string>
#include <utility>

namespace {
// NVS key for the encrypted identity blob. Kept distinct from the legacy
// "privkey" key so we can detect un-migrated installs.
constexpr const char* NVS_NAMESPACE     = "ratcom_id";
constexpr const char* NVS_KEY_ENC       = "encid";
constexpr const char* NVS_KEY_LEGACY    = "privkey";
}

// RAII lock guard for the shared LittleFS mutex
struct FSLock {
    FSLock() : _m(FlashStore::mutex()) {
        if (_m) xSemaphoreTake(_m, portMAX_DELAY);
    }
    ~FSLock() {
        if (_m) xSemaphoreGive(_m);
    }
    SemaphoreHandle_t _m;
};

// =============================================================================
// LittleFS Filesystem Implementation for microReticulum
// =============================================================================

bool LittleFSFileSystem::init() {
    return true;  // LittleFS already initialized by FlashStore
}

bool LittleFSFileSystem::file_exists(const char* file_path) {
    FSLock lock;
    return LittleFS.exists(file_path);
}

size_t LittleFSFileSystem::read_file(const char* file_path, RNS::Bytes& data) {
    FSLock lock;
    File f = LittleFS.open(file_path, "r");
    if (!f) return 0;
    size_t size = f.size();
    data = RNS::Bytes(size);
    f.readBytes((char*)data.writable(size), size);
    f.close();
    return size;
}

size_t LittleFSFileSystem::write_file(const char* file_path, const RNS::Bytes& data) {
    FSLock lock;
    // Ensure parent directory exists
    String path = String(file_path);
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash > 0) {
        String dir = path.substring(0, lastSlash);
        if (!LittleFS.exists(dir.c_str())) {
            LittleFS.mkdir(dir.c_str());
        }
    }

    File f = LittleFS.open(file_path, "w");
    if (!f) return 0;
    size_t written = f.write(data.data(), data.size());
    f.close();
    return written;
}

namespace {
bool ensureLittleFSDirLocked(const String& dir) {
    if (dir.length() == 0 || dir == "/") return true;
    if (LittleFS.exists(dir.c_str())) return true;
    int lastSlash = dir.lastIndexOf('/');
    if (lastSlash > 0) {
        if (!ensureLittleFSDirLocked(dir.substring(0, lastSlash))) return false;
    }
    return LittleFS.mkdir(dir.c_str()) || LittleFS.exists(dir.c_str());
}

class LittleFSStreamImpl : public RNS::FileStreamImpl {
public:
    explicit LittleFSStreamImpl(File&& f) : _f(std::move(f)) {}
    ~LittleFSStreamImpl() override { close(); }

protected:
    const char* name() override {
        FSLock lock;
        return _f ? _f.name() : "";
    }

    size_t size() override {
        FSLock lock;
        return _f ? _f.size() : 0;
    }

    void close() override {
        FSLock lock;
        if (_f) _f.close();
    }

    size_t write(uint8_t byte) override {
        FSLock lock;
        return _f ? _f.write(byte) : 0;
    }

    size_t write(const uint8_t* buffer, size_t len) override {
        FSLock lock;
        return _f ? _f.write(buffer, len) : 0;
    }

    int available() override {
        FSLock lock;
        return _f ? _f.available() : 0;
    }

    int read() override {
        FSLock lock;
        return _f ? _f.read() : -1;
    }

    int peek() override {
        FSLock lock;
        return _f ? _f.peek() : -1;
    }

    void flush() override {
        FSLock lock;
        if (_f) _f.flush();
    }

private:
    File _f;
};
}  // namespace

RNS::FileStream LittleFSFileSystem::open_file(const char* file_path, RNS::FileStream::MODE file_mode) {
    const char* mode = "r";
    if (file_mode == RNS::FileStream::MODE_WRITE) {
        mode = "w";
    } else if (file_mode == RNS::FileStream::MODE_APPEND) {
        mode = "a";
    } else if (file_mode != RNS::FileStream::MODE_READ) {
        return {RNS::Type::NONE};
    }

    FSLock lock;
    if (file_mode != RNS::FileStream::MODE_READ) {
        String path(file_path);
        int lastSlash = path.lastIndexOf('/');
        if (lastSlash > 0) {
            ensureLittleFSDirLocked(path.substring(0, lastSlash));
        }
    }

    File f = LittleFS.open(file_path, mode);
    if (!f) return {RNS::Type::NONE};
    return RNS::FileStream(new LittleFSStreamImpl(std::move(f)));
}

bool LittleFSFileSystem::remove_file(const char* file_path) {
    FSLock lock;
    return LittleFS.remove(file_path);
}

bool LittleFSFileSystem::rename_file(const char* from, const char* to) {
    FSLock lock;
    return LittleFS.rename(from, to);
}

bool LittleFSFileSystem::directory_exists(const char* directory_path) {
    FSLock lock;
    return LittleFS.exists(directory_path);
}

bool LittleFSFileSystem::create_directory(const char* directory_path) {
    FSLock lock;
    return LittleFS.mkdir(directory_path);
}

bool LittleFSFileSystem::remove_directory(const char* directory_path) {
    FSLock lock;
    return LittleFS.rmdir(directory_path);
}

std::list<std::string> LittleFSFileSystem::list_directory(const char* directory_path, Callbacks::DirectoryListing callback) {
    FSLock lock;
    std::list<std::string> entries;
    File dir = LittleFS.open(directory_path);
    if (!dir || !dir.isDirectory()) return entries;
    File f = dir.openNextFile();
    while (f) {
        const char* name = f.name();
        entries.push_back(name);
        if (callback) callback(name);
        f.close();
        f = dir.openNextFile();
    }
    dir.close();
    return entries;
}

size_t LittleFSFileSystem::storage_size() {
    FSLock lock;
    return LittleFS.totalBytes();
}

size_t LittleFSFileSystem::storage_available() {
    FSLock lock;
    return LittleFS.totalBytes() - LittleFS.usedBytes();
}

// =============================================================================
// ReticulumManager
// =============================================================================

bool ReticulumManager::begin(SX1262* radio, FlashStore* flash) {
    _flash = flash;

    // Register filesystem with microReticulum (required — library throws if missing)
    LittleFSFileSystem* fsImpl = new LittleFSFileSystem();
    RNS::FileSystem fs(fsImpl);
    fs.init();
    RNS::Utilities::OS::register_filesystem(fs);
    Serial.println("[RNS] Filesystem registered");

    // Endpoint node: routing tables are rebuilt from announces on each boot.
    // No need to restore transport tables from SD — saves ~12KB heap + boot time.

    // Create and register LoRa interface. This is a mobile endpoint, so LoRa
    // uses roaming mode while Reticulum transport remains disabled below.
    _loraImpl = new LoRaInterface(radio, "LoRa.915");
    _loraIface = _loraImpl;
    _loraIface.mode(RNS::Type::Interface::MODE_ROAMING);
    RNS::Transport::register_interface(_loraIface);
    if (!_loraImpl->start()) {
        Serial.println("[RNS] WARNING: LoRa interface failed to start — radio offline");
    }
    Serial.println("[RNS] LoRa interface registered");

    // Create Reticulum instance (endpoint only — no transport/rebroadcast)
    _reticulum = RNS::Reticulum();
    // Suppress verbose microReticulum logging — LOG_TRACE floods serial at 115200 baud,
    // blocking the CPU for hundreds of ms. Change to LOG_TRACE or LOG_DEBUG for protocol debugging.
    RNS::loglevel(RNS::LOG_WARNING);
    RNS::Reticulum::transport_enabled(false);
    RNS::Reticulum::probe_destination_enabled(true);
    // Endpoint tables — 8-bit canvas frees ~32KB for these
    RNS::Transport::path_table_maxsize(48);
    RNS::Transport::announce_table_maxsize(48);
    _reticulum.start();
    Serial.println("[RNS] Reticulum started (Endpoint)");

    // Layer 1: Transport-level announce filter — runs BEFORE Ed25519 verify
    RNS::Transport::set_filter_packet_callback([](const RNS::Packet& packet) -> bool {
        if (packet.packet_type() != RNS::Type::Packet::ANNOUNCE) return true;

        unsigned long now = millis();

        // Rate limit window (per-second)
        static unsigned long windowStart = 0;
        static unsigned int count = 0;
        if (now - windowStart >= 1000) { windowStart = now; count = 0; }

        // Adaptive rate: tight during boot flood, then normal, emergency if low heap
        unsigned int maxRate;
        if (ESP.getFreeHeap() < 15000) {
            maxRate = 1;  // Emergency: almost no processing
        } else if (now < 60000) {
            maxRate = 2;  // Boot flood
        } else {
            maxRate = RSCARDPUTER_MAX_ANNOUNCES_PER_SEC;
        }
        if (++count > maxRate) return false;

        // Skip re-validation of known paths (saves ~100ms Ed25519 per announce)
        // Allow through once per 5 min for name/ratchet updates.
        // Uses raw hash bytes as key (avoids expensive toHex + hops_to per packet).
        if (RNS::Transport::has_path(packet.destination_hash())) {
            static std::unordered_map<std::string, unsigned long> lastRevalidate;
            std::string key((const char*)packet.destination_hash().data(),
                            packet.destination_hash().size());

            auto it = lastRevalidate.find(key);
            if (it != lastRevalidate.end() && (now - it->second) < 300000) return false;
            lastRevalidate[key] = now;

            // Cap map to save heap
            if (lastRevalidate.size() > 40) lastRevalidate.clear();
        }

        return true;
    });

    // Load or create identity
    if (!loadOrCreateIdentity()) {
        Serial.println("[RNS] ERROR: Identity creation failed!");
        return false;
    }

    // Wire at-rest encryption for RNS::Identity's own known_destinations
    // file before loading it. Each entry carries the last-heard announce
    // app_data for a destination -- for an LXMF peer, that's their
    // announced display name -- so this file leaks exactly the same
    // "who you've talked to" information ContactsEncryption protects,
    // just via a path the library manages itself. Must happen after
    // loadOrCreateIdentity() since decrypting needs the identity's private
    // key. See KnownDestEncryption.h for the full rationale.
    RNS::Identity::known_destinations_crypto(
        [this](const RNS::Bytes& in, RNS::Bytes& out) {
            return KnownDestEncryption::encrypt(_identity, in, out);
        },
        [this](const RNS::Bytes& in, RNS::Bytes& out) {
            return KnownDestEncryption::decryptOrPassthrough(_identity, in, out);
        });

    // Load persisted known destinations so Identity::recall() works
    // immediately after reboot for previously-seen nodes.
    RNS::Identity::load_known_destinations();

    // One-time re-save so a pre-encryption plaintext file (or one written
    // before this device ever had a password) gets upgraded immediately --
    // same migrate-on-load intent as contacts/settings, but gated by its
    // own flag rather than run unconditionally: unlike contacts/settings,
    // save_known_destinations() always re-serializes and re-encrypts the
    // whole table (up to RNS_KNOWN_DESTINATIONS_MAX entries) rather than
    // no-op'ing once nothing changed, so doing this on every boot forever
    // is pure wasted flash-write and crypto cost after the first boot.
    {
        Preferences kp;
        kp.begin(NVS_NAMESPACE, true);
        bool knownDestMigrated = kp.getBool("kd_migrated", false);
        kp.end();
        if (!knownDestMigrated) {
            RNS::Identity::save_known_destinations();
            Preferences kp2;
            kp2.begin(NVS_NAMESPACE, false);
            kp2.putBool("kd_migrated", true);
            kp2.end();
        }
    }

    // Create LXMF delivery destination
    _destination = RNS::Destination(
        _identity,
        RNS::Type::Destination::IN,
        RNS::Type::Destination::SINGLE,
        "lxmf",
        "delivery"
    );
    _destination.set_proof_strategy(RNS::Type::Destination::PROVE_ALL);
    _destination.accepts_links(true);
    Serial.printf("[RNS] Destination: %s\n", _destination.hash().toHex().c_str());

    _transportActive = true;
    startPersistTask();
    Serial.println("[RNS] Endpoint active");
    return true;
}

// ─── Identity-at-rest helpers ────────────────────────────────────────────────

void ReticulumManager::setPassword(const String& pw) {
    _password = pw;
}

void ReticulumManager::clearPassword() {
    // Overwrite the heap buffer before releasing it. String's destructor
    // would free without zeroing.
    if (_password.length() > 0) {
        for (size_t i = 0; i < _password.length(); i++) {
            ((char*)_password.c_str())[i] = 0;
        }
    }
    _password = "";
}

void ReticulumManager::preloadIdentityKey(const RNS::Bytes& key) {
    _preloadKey = key;
}

// Best-effort probe: look at each tier and figure out what kind of identity
// material is present. Reads at most one buffer per tier.
ReticulumManager::IdentityState ReticulumManager::probeIdentityState() {
    bool anyEncrypted = false;
    bool anyPlaintext = false;

    auto classify = [&](const uint8_t* buf, size_t len) {
        if (len == 0) return;
        if (IdentityCrypto::isEncrypted(buf, len)) anyEncrypted = true;
        else                                       anyPlaintext = true;
    };

    if (_flash && _flash->exists(PATH_IDENTITY)) {
        uint8_t buf[256];
        size_t len = 0;
        if (_flash->readFile(PATH_IDENTITY, buf, sizeof(buf), len)) {
            classify(buf, len);
        }
    }

    {
        Preferences prefs;
        if (prefs.begin(NVS_NAMESPACE, true)) {
            size_t encLen = prefs.getBytesLength(NVS_KEY_ENC);
            if (encLen > 0 && encLen <= 512) {
                uint8_t buf[512];
                prefs.getBytes(NVS_KEY_ENC, buf, encLen);
                classify(buf, encLen);
            } else if (prefs.getBytesLength(NVS_KEY_LEGACY) > 0) {
                anyPlaintext = true;
            }
            prefs.end();
        }
    }

    if (_sd && _sd->isReady() && _sd->exists(SD_PATH_IDENTITY)) {
        uint8_t buf[256];
        size_t len = 0;
        if (_sd->readFile(SD_PATH_IDENTITY, buf, sizeof(buf), len)) {
            classify(buf, len);
        }
    }

    // Encryption-present wins over plaintext: any encrypted tier means a
    // password is required to make sense of the device. Plaintext-only
    // means a legacy install needing migration.
    if (anyEncrypted) return IdentityState::ENCRYPTED;
    if (anyPlaintext) return IdentityState::LEGACY_PLAINTEXT;
    return IdentityState::NONE;
}

bool ReticulumManager::migrateLegacyIdentity() {
    if (_password.length() == 0) {
        Serial.println("[RNS] migrate: no password set");
        return false;
    }
    if (!_identity) {
        Serial.println("[RNS] migrate: no identity loaded");
        return false;
    }
    RNS::Bytes privKey = _identity.get_private_key();
    if (privKey.size() == 0) {
        Serial.println("[RNS] migrate: identity has no private key");
        return false;
    }
    if (!saveIdentityToAll(privKey)) {
        Serial.println("[RNS] migrate: write failed");
        return false;
    }
    _legacyLoaded = false;
    Serial.println("[RNS] Identity migrated to encrypted form");
    return true;
}

bool ReticulumManager::loadOrCreateIdentity() {
    // Fast path: caller (main.cpp) already verified the password and
    // unwrapped the encrypted blob. We just adopt the plaintext bytes.
    if (_preloadKey.size() > 0) {
        _identity = RNS::Identity(false);
        if (_identity.load_private_key(_preloadKey)) {
            Serial.printf("[RNS] Identity adopted from preloaded key: %s\n",
                          _identity.hexhash().c_str());
            // Re-encrypt to all tiers (heals out-of-sync state). Errors
            // here are non-fatal — identity is already in RAM.
            saveIdentityToAll(_preloadKey);
            // Release our reference. We can't securely-zero the buffer
            // because RNS::Bytes is copy-on-write — the Identity now
            // shares the same backing store, and zeroing it would
            // corrupt the live identity.
            _preloadKey = RNS::Bytes();
            return true;
        }
        _preloadKey = RNS::Bytes();
        Serial.println("[RNS] preloaded key invalid");
        return false;
    }

    // No preload: this happens only when probe returned LEGACY_PLAINTEXT
    // (caller already set the password but did not unwrap externally) or
    // NONE (fresh device). Encrypted-blob handling is the caller's job.

    auto tryLoadPlaintext = [&](const uint8_t* buf, size_t len, const char* src) -> bool {
        if (len == 0) return false;
        if (IdentityCrypto::isEncrypted(buf, len)) {
            // Encrypted but no preload → caller forgot to unwrap. Refuse.
            Serial.printf("[RNS] %s: encrypted identity but no preload — refusing\n", src);
            return false;
        }
        RNS::Bytes keyData(buf, len);
        _identity = RNS::Identity(false);
        if (!_identity.load_private_key(keyData)) return false;
        Serial.printf("[RNS] Identity loaded from %s (LEGACY plaintext): %s\n",
                      src, _identity.hexhash().c_str());
        _legacyLoaded = true;
        return true;
    };

    // Tier 1: Flash
    if (_flash && _flash->exists(PATH_IDENTITY)) {
        uint8_t buf[256];
        size_t len = 0;
        if (_flash->readFile(PATH_IDENTITY, buf, sizeof(buf), len)) {
            if (tryLoadPlaintext(buf, len, "flash")) return true;
        }
    }

    // Tier 2: NVS (check legacy key)
    {
        Preferences prefs;
        if (prefs.begin(NVS_NAMESPACE, true)) {
            size_t len = prefs.getBytesLength(NVS_KEY_LEGACY);
            if (len > 0 && len <= 256) {
                uint8_t buf[256];
                prefs.getBytes(NVS_KEY_LEGACY, buf, len);
                prefs.end();
                if (tryLoadPlaintext(buf, len, "NVS-legacy")) return true;
            } else {
                prefs.end();
            }
        }
    }

    // Tier 3: SD
    if (_sd && _sd->isReady() && _sd->exists(SD_PATH_IDENTITY)) {
        uint8_t buf[256];
        size_t len = 0;
        if (_sd->readFile(SD_PATH_IDENTITY, buf, sizeof(buf), len)) {
            if (tryLoadPlaintext(buf, len, "SD")) return true;
        }
    }

    // No identity anywhere — create new and wrap if a password is set.
    _identity = RNS::Identity();
    Serial.printf("[RNS] New identity created: %s\n", _identity.hexhash().c_str());
    RNS::Bytes privKey = _identity.get_private_key();
    if (privKey.size() > 0) {
        saveIdentityToAll(privKey);
    }
    return true;
}

bool ReticulumManager::saveIdentityToAll(const RNS::Bytes& keyData) {
    if (keyData.size() == 0) return false;
    if (_password.length() == 0) {
        // Mandatory-encryption posture: without a password we have no key
        // to wrap with. Caller is expected to set it before begin() saves
        // anything. Returning false is safer than writing plaintext.
        Serial.println("[RNS] saveIdentityToAll: no password — refusing plaintext write");
        return false;
    }

    RNS::Bytes wrapped;
    if (!IdentityCrypto::wrap(_password, keyData, wrapped)) {
        Serial.println("[RNS] saveIdentityToAll: wrap failed");
        return false;
    }

    // Flash
    bool ok = _flash && _flash->writeAtomic(PATH_IDENTITY, wrapped.data(), wrapped.size());

    // SD (best-effort — SD missing is not a hard failure)
    if (_sd && _sd->isReady()) {
        _sd->ensureDir("/ratcom/identity");
        _sd->writeAtomic(SD_PATH_IDENTITY, wrapped.data(), wrapped.size());
    }

    // NVS — write encrypted blob under new key, then PURGE the legacy
    // plaintext key so an attacker dumping NVS gets nothing.
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE, false)) {
        prefs.putBytes(NVS_KEY_ENC, wrapped.data(), wrapped.size());
        if (prefs.isKey(NVS_KEY_LEGACY)) {
            prefs.remove(NVS_KEY_LEGACY);
        }
        prefs.end();
        Serial.println("[RNS] Encrypted identity saved to NVS (legacy key removed)");
    }
    return ok;
}

void ReticulumManager::loop() {
    if (!_transportActive) return;

    _reticulum.loop();
    if (_loraImpl) {
        _loraImpl->loop();
    }

    unsigned long now = millis();
    if (now - _lastPersist >= PATH_PERSIST_INTERVAL_MS) {
        _lastPersist = now;
        persistData();
    }
}

// --- Background persist task (runs on core 0) ---
// Flash writes take 0.5-4+ seconds on LittleFS. Running them on core 0
// keeps the main loop (core 1) responsive for UI and radio.

void ReticulumManager::startPersistTask() {
    _persistQueue = xQueueCreate(1, sizeof(uint8_t));
    // Pin to core 1 (same as main loop) — core 0 is WiFi/lwIP, sharing LittleFS across cores corrupts FS
    xTaskCreatePinnedToCore(persistTaskFunc, "persist", 8192, this, 1, &_persistTask, 1);
    Serial.println("[RNS] Persist task started on core 1");
}

void ReticulumManager::persistTaskFunc(void* param) {
    ReticulumManager* self = (ReticulumManager*)param;
    uint8_t cycle;
    for (;;) {
        if (xQueueReceive(self->_persistQueue, &cycle, portMAX_DELAY) == pdTRUE) {
            unsigned long start = millis();
            switch (cycle) {
                case 0:
                    RNS::Transport::persist_data();
                    break;
                case 1:
                    RNS::Identity::persist_data();
                    break;
                // Cycle 2 removed — endpoint doesn't need SD transport backup
            }
            Serial.printf("[PERSIST] Cycle %d done (%lums, core %d)\n",
                          cycle, millis() - start, xPortGetCoreID());
        }
    }
}

void ReticulumManager::persistData() {
    if (_persistQueue) {
        // Queue the cycle for background execution — non-blocking
        xQueueOverwrite(_persistQueue, &_persistCycle);
    } else {
        // Fallback: synchronous (before task is started)
        switch (_persistCycle) {
            case 0: RNS::Transport::persist_data(); break;
            case 1: RNS::Identity::persist_data(); break;
        }
    }
    _persistCycle = (_persistCycle + 1) % 2;  // Only 2 cycles now (transport + identity)
}

String ReticulumManager::identityHash() const {
    if (!_identity) return "unknown";
    std::string hex = _identity.hexhash();
    if (hex.length() >= 12) {
        return String((hex.substr(0, 4) + ":" + hex.substr(4, 4) + ":" + hex.substr(8, 4)).c_str());
    }
    return String(hex.c_str());
}

String ReticulumManager::destinationHashStr() const {
    if (!_destination) return "unknown";
    std::string hex = _destination.hash().toHex();
    if (hex.length() >= 12) {
        return String((hex.substr(0, 6) + "::" + hex.substr(hex.length() - 6, 6)).c_str());
    }
    return String(hex.c_str());
}

String ReticulumManager::destinationHashHex() const {
    if (!_destination) return "unknown";
    return String(_destination.hash().toHex().c_str());
}

size_t ReticulumManager::pathCount() const {
    return _reticulum.get_path_table().size();
}

size_t ReticulumManager::linkCount() const {
    return _reticulum.get_link_count();
}

void ReticulumManager::announce(const RNS::Bytes& appData) {
    if (!_transportActive) return;
    _destination.announce(appData);
    _lastAnnounceTime = millis();
    Serial.println("[RNS] Announce sent");
}
