#pragma once

#include <Reticulum.h>
#include <Identity.h>
#include <Destination.h>
#include <Transport.h>
#include <Interface.h>
#include <FileSystem.h>
#include <Utilities/OS.h>

#include "transport/LoRaInterface.h"
#include "storage/FlashStore.h"
#include "storage/SDStore.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// LittleFS-backed filesystem for microReticulum
class LittleFSFileSystem : public RNS::FileSystemImpl {
public:
    virtual bool init() override;
    virtual bool file_exists(const char* file_path) override;
    virtual size_t read_file(const char* file_path, RNS::Bytes& data) override;
    virtual size_t write_file(const char* file_path, const RNS::Bytes& data) override;
    virtual RNS::FileStream open_file(const char* file_path, RNS::FileStream::MODE file_mode) override;
    virtual bool remove_file(const char* file_path) override;
    virtual bool rename_file(const char* from, const char* to) override;
    virtual bool directory_exists(const char* directory_path) override;
    virtual bool create_directory(const char* directory_path) override;
    virtual bool remove_directory(const char* directory_path) override;
    virtual std::list<std::string> list_directory(const char* directory_path, Callbacks::DirectoryListing callback = nullptr) override;
    virtual size_t storage_size() override;
    virtual size_t storage_available() override;
};

class ReticulumManager {
public:
    // Identity-at-rest state, probed before begin() so the UI can decide
    // whether to show password setup, unlock, or migration screens.
    enum class IdentityState {
        UNKNOWN,            // probe not yet run
        NONE,               // no identity at any tier — fresh device
        ENCRYPTED,          // at least one tier holds an encrypted blob (needs password)
        LEGACY_PLAINTEXT    // tier(s) hold an unencrypted identity (needs migration)
    };

    ReticulumManager()
        : _reticulum({RNS::Type::NONE}),
          _identity({RNS::Type::NONE}),
          _destination({RNS::Type::NONE}),
          _loraIface({RNS::Type::NONE}) {}

    bool begin(SX1262* radio, FlashStore* flash);
    void setSDStore(SDStore* sd) { _sd = sd; }
    void loop();
    void persistData();

    // ── Identity-at-rest (password-protected) ──────────────────────────────
    // Pass the user-supplied password before begin(). Required.
    void setPassword(const String& pw);
    // Clear cached password (e.g. after lock). After this, save/migrate
    // operations will fail until setPassword() is called again.
    void clearPassword();

    // Look at all storage tiers without modifying anything; classify the
    // identity-at-rest state. Cheap — only reads file headers.
    IdentityState probeIdentityState();

    // Was the loaded identity stored in plaintext (pre-encryption upgrade)?
    bool legacyIdentityLoaded() const { return _legacyLoaded; }

    // Pre-supply a decrypted private-key blob (used when main.cpp has
    // already verified the password externally). When set, begin() uses
    // this instead of reading from disk. Cleared after begin().
    void preloadIdentityKey(const RNS::Bytes& key);

    // Wrap the currently-loaded identity with the stored password and
    // write the encrypted form to every storage tier. Removes any
    // legacy-plaintext residue. Returns true on success.
    bool migrateLegacyIdentity();

    // Identity
    const RNS::Identity& identity() const { return _identity; }
    String identityHash() const;
    String destinationHashHex() const;
    String destinationHashStr() const;

    // Transport status
    bool isTransportActive() const { return _transportActive; }
    size_t pathCount() const;
    size_t linkCount() const;

    // Announce
    void announce(const RNS::Bytes& appData = {});
    unsigned long lastAnnounceTime() const { return _lastAnnounceTime; }

    // Access
    RNS::Destination& destination() { return _destination; }
    LoRaInterface* loraInterface() { return _loraImpl; }

private:
    bool loadOrCreateIdentity();
    // Writes the encrypted (wrapped) identity blob to flash + NVS + SD.
    // `keyData` is the raw private-key bytes; wrapping happens here using
    // the cached _password. Caller must have already setPassword().
    bool saveIdentityToAll(const RNS::Bytes& keyData);
    void startPersistTask();
    static void persistTaskFunc(void* param);

    RNS::Reticulum _reticulum;
    RNS::Identity _identity;
    RNS::Destination _destination;
    RNS::Interface _loraIface;
    LoRaInterface* _loraImpl = nullptr;
    FlashStore* _flash = nullptr;
    SDStore* _sd = nullptr;
    bool _transportActive = false;
    unsigned long _lastPersist = 0;
    unsigned long _lastAnnounceTime = 0;
    uint8_t _persistCycle = 0;  // Rotating: 0=Transport, 1=Identity

    // Background persist task (core 0) — flash writes don't block main loop
    QueueHandle_t _persistQueue = nullptr;
    TaskHandle_t _persistTask = nullptr;

    // Identity-at-rest state
    String _password;                // held in RAM only — cleared on clearPassword()
    RNS::Bytes _preloadKey;          // when non-empty, begin() uses this directly
    bool _legacyLoaded = false;      // true if loaded identity was found in plaintext
};
