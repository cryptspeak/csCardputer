# Contacts & Settings Encryption

Contacts and device settings are encrypted at rest the same way identity
and messages are — see [encryption-overview.md](encryption-overview.md)
for the shared `AtRestCrypto` envelope. Each has its own magic bytes and
HKDF info string so a key derived for one can't decrypt another.

## Upgrade paths

Two populations of existing devices reach the current, fully-encrypted
state differently:

1. **Upstream (non-Cryptspeak) devices** — nothing is encrypted yet.
   `ReticulumManager::probeIdentityState()` returns `LEGACY_PLAINTEXT`,
   `legacyIdentityLoaded()` becomes `true`, and the identity/message
   migration flow runs (opt-in, with a progress screen for messages —
   see [boot-sequence.md](boot-sequence.md)).
2. **Existing Cryptspeak devices** — identity and messages are already
   encrypted, but contacts and settings predate this feature and are
   still plaintext. `probeIdentityState()` returns `ENCRYPTED`, the
   normal unlock path runs, and `legacyIdentityLoaded()` stays `false`,
   so the message-migration prompt does not fire for these users.

The contacts/settings re-encryption in `main.cpp` is gated only on
`encryptionEnabled()` ("is a valid identity loaded right now"), which is
true in both cases above and a no-op once everything is already
encrypted. One mechanism covers both upgrade paths.

## Contacts

Modules: [`src/storage/ContactsEncryption.{h,cpp}`](../src/storage/ContactsEncryption.h)
Wired into: [`src/reticulum/AnnounceManager.cpp`](../src/reticulum/AnnounceManager.cpp)

Domain: magic `RCN1`, HKDF info `rscardputer.contacts.v1`.

`AnnounceManager` persists two kinds of data, both encrypted the same way:

- **Per-contact files** — one JSON file per saved contact, under
  `/contacts` (flash) and `/ratcom/contacts` (SD). Content is
  `{"hash": "...", "name": "..."}`. The filename is **not** the hash —
  see below.
- **Name cache** (`names.json`) — a hash→display-name map so names
  survive reboots even for peers that haven't been explicitly saved as
  contacts.

`AnnounceManager` exposes:

```cpp
void setIdentity(const RNS::Identity* identity);
bool encryptionEnabled() const;
```

mirroring `MessageStore`'s pattern. Two private free functions in
`AnnounceManager.cpp`, `maybeEncryptContact()` / `maybeDecryptContact()`,
wrap every read/write of both contact files and the name cache — the
same shape as `MessageStore`'s `maybeEncrypt()`/`maybeDecrypt()`.

### Filenames are not the hash either

Contact files and conversation directories are named with an
identity-keyed blind index, not the peer's destination hash or a
truncation of it. A directory listing alone — no decryption required —
would otherwise hand an attacker with disk access the exact set of
hashes saved as contacts, which is a stronger signal than "a hash was
announced": destination hashes are broadcast over the air as part of
Reticulum's own discovery (see [threat-model.md](threat-model.md)), but
which of those hashes *this device* specifically saved is not.

`saveContact()`/`removeContact()` name each file with
`contactFileToken()` (`AnnounceManager.cpp`): HMAC-SHA256 of the hash
under its own HKDF info string (`rscardputer.contactfile.v1`),
hex-encoded and truncated to 16 characters. Same width and lookup cost
as a truncated hash, same determinism (a given peer always lands on the
same filename across reboots), but only this device's identity can
compute or invert the mapping. `loadContacts()` never relies on the
filename for anything besides iterating the directory — every field it
needs comes from the decrypted content.

Existing installs migrate automatically: `loadContacts()` flags any file
whose name doesn't match its contact's recomputed token, and the next
`saveContacts()` (which already runs on every boot when encryption is
enabled — see "Migration" below) writes the correctly-named copy and
removes the stale one.

`MessageStore` conversation directories (`/messages/<hash16>/`) had the
same property and use the same fix, `dirTokenForPeer()`, sharing
`AtRestCrypto::blindIndexHex()` with its own info string
(`rscardputer.msgdir.v1`). `MessageStore::migrateConversationDirTokens()`
renames each old-named directory once, on the first boot after
upgrading — a directory rename is a single directory-entry update on
both LittleFS and FAT, not a per-message-file operation.

### Migration

Contacts and the name cache are capped at a small size
(`RSCARDPUTER_MAX_NODES = 50`), so unlike message migration there's no
user-facing opt-in/progress flow. `main.cpp` calls `saveContacts()` and
`saveNameCache()` once, immediately after `setIdentity()` +
`loadContacts()`/`loadNameCache()`, whenever encryption is enabled:

```cpp
announceManager->setIdentity(&rns.identity());
announceManager->loadContacts();
announceManager->loadNameCache();
...
if (announceManager->encryptionEnabled()) {
    announceManager->saveContacts();
    announceManager->saveNameCache();
}
```

Existing plaintext files load transparently (the decrypt path passes
them through unchanged when the magic header is absent), then get
rewritten encrypted on this forced save. From the next boot onward
they're already encrypted, so this is a no-op cost-wise.

## Settings

Modules: [`src/storage/SettingsEncryption.{h,cpp}`](../src/storage/SettingsEncryption.h)
Wired into: [`src/config/UserConfig.cpp`](../src/config/UserConfig.cpp)

Domain: magic `RUC1`, HKDF info `rscardputer.settings.v1`.

`UserConfig` persists one JSON blob (`UserSettings`) across three tiers:
SD (`/ratcom/config/user.json`), flash (`/config/user.json`), and NVS
(namespace `ratcom_cfg`, key `json`, as a backup that survives flash
corruption). That blob includes the WiFi AP/STA SSID+password, TCP hub
host/port list, LoRa radio parameters, display name, and display/audio
preferences.

`UserConfig` exposes the same `setIdentity()` / `encryptionEnabled()`
pair, plus two private helpers, `maybeEncrypt()` / `maybeDecrypt()`,
used by every load/save tier (`load(FlashStore&)`, `save(FlashStore&)`,
`load(SDStore&, FlashStore&)`, `save(SDStore&, FlashStore&)`).

### NVS binary safety

The NVS backup is stored with `Preferences::putBytes()`/`getBytes()`,
not `putString()`/`getString()`. An encrypted envelope is binary and can
legitimately contain `0x00` bytes anywhere in the ciphertext or MAC — a
`0x00` byte would silently truncate a `putString()`-backed entry.
`IdentityCrypto`'s NVS path uses the same byte-exact approach for the
identity blob.

`loadFromNVS()` tries `getBytes()` first and falls back to `getString()`
if no byte-typed entry is found, so a pre-upgrade NVS backup written
under the old string format still loads on the first boot after
upgrading:

```cpp
static String loadFromNVS() {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, true)) return "";
    String out;
    size_t len = prefs.getBytesLength(NVS_KEY);
    if (len > 0) {
        // current format: raw bytes (handles binary ciphertext safely)
        std::vector<uint8_t> buf(len);
        prefs.getBytes(NVS_KEY, buf.data(), len);
        out.reserve(len);
        for (size_t i = 0; i < len; i++) out.concat((char)buf[i]);
    } else {
        // legacy string-typed entry from a pre-encryption build
        out = prefs.getString(NVS_KEY, "");
    }
    prefs.end();
    return out;
}
```

The next successful save rewrites the NVS entry as bytes, so the
fallback only matters for the one boot right after upgrading.

> NVS key names are capped at 15 characters
> (`NVS_KEY_NAME_MAX_SIZE - 1`) — `Preferences::putBool()`/`putBytes()`
> silently refuse anything longer, so a migration-complete flag under an
> over-length key never persists and that migration quietly re-runs
> every boot. Keep new NVS keys at 15 characters or under. See
> [storage-layer.md](storage-layer.md#nvs-key-length) for the specific
> keys this bit twice.

### Migration

Settings is a single small file, so — same reasoning as contacts — there's
no opt-in flow. `main.cpp` forces one re-save immediately after load when
encryption is enabled:

```cpp
userConfig.setIdentity(&rns.identity());
userConfig.load(sdStore, flash);
if (userConfig.encryptionEnabled()) {
    userConfig.save(sdStore, flash);
}
```

## Why identity is guaranteed to be available by this point

Both `AnnounceManager::loadContacts()` and `UserConfig::load()` are called
in `main.cpp`'s `setup()` *after* the password gate has run and
`rns.begin()` has returned successfully — the same point at which
`MessageStore::setIdentity()` is already called for message encryption.
There's no boot path that reads contacts or settings before the identity
is unlocked (settings aren't needed to *show* the password screen — the
boot screen, keyboard, radio, and SD card are all initialized with
hardcoded defaults before the identity gate runs). See
[boot-sequence.md](boot-sequence.md) for the full ordering.
