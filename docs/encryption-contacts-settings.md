# Contacts & Settings Encryption

This is the gap closed in this branch. Before this change, the audit in
this repo's history found that identity and messages were properly
encrypted and wired end-to-end, but **contacts** and **device settings**
were stored as plain JSON — including saved contact names and, in
settings, WiFi AP/STA passwords and TCP hub addresses.

Both new domains reuse the shared `AtRestCrypto` envelope engine described
in [encryption-overview.md](encryption-overview.md), with their own magic
bytes and HKDF info string so their keys can't cross-decrypt each other
or the message/identity domains.

## This covers two distinct upgrade paths, not just one

It's worth being explicit about this because it's easy to assume the
migration only matters for the upstream/non-encrypted → Cryptspeak jump.
It actually covers two separate populations of existing devices:

1. **Upstream (non-Cryptspeak) devices** — nothing is encrypted yet.
   `ReticulumManager::probeIdentityState()` returns `LEGACY_PLAINTEXT`,
   `legacyIdentityLoaded()` becomes `true`, and the existing
   identity/message migration flow (opt-in, with a progress screen for
   messages — see [boot-sequence.md](boot-sequence.md)) runs.
2. **Existing Cryptspeak devices** — identity and messages are
   *already* encrypted from a prior version of this firmware, but
   contacts and settings were still plaintext (the gap this branch
   closes). `probeIdentityState()` returns `ENCRYPTED`, the normal
   unlock path runs, and `legacyIdentityLoaded()` stays `false` — so the
   message-migration prompt correctly does **not** fire again for these
   users.

The contacts/settings re-encryption in `main.cpp` is deliberately **not**
gated on `legacyIdentityLoaded()`. It's gated only on
`encryptionEnabled()` — "is a valid identity loaded right now" — which is
true in both cases above (and is a no-op the moment everything is already
encrypted, every boot after the first one). One mechanism, both upgrade
paths, no separate code path needed for "upgrading from an
already-partially-encrypted install" versus "upgrading from plaintext."

## Contacts

Modules: [`src/storage/ContactsEncryption.{h,cpp}`](../src/storage/ContactsEncryption.h)
Wired into: [`src/reticulum/AnnounceManager.cpp`](../src/reticulum/AnnounceManager.cpp)

Domain: magic `RCN1`, HKDF info `rscardputer.contacts.v1`.

`AnnounceManager` persists two kinds of data, both now encrypted the same
way:

- **Per-contact files** — one JSON file per saved contact, named by the
  first 16 hex characters of the destination hash (e.g.
  `a1b2c3d4e5f6a7b8.json`), under `/contacts` (flash) and `/ratcom/contacts`
  (SD). Content is `{"hash": "...", "name": "..."}`.
- **Name cache** (`names.json`) — a hash→display-name map used so names
  survive reboots even for peers that haven't been explicitly saved as
  contacts.

`AnnounceManager` gained:

```cpp
void setIdentity(const RNS::Identity* identity);
bool encryptionEnabled() const;
```

mirroring `MessageStore`'s pattern exactly. Two private free functions in
`AnnounceManager.cpp`, `maybeEncryptContact()` / `maybeDecryptContact()`,
wrap every read/write of both contact files and the name cache — same
shape as `MessageStore`'s `maybeEncrypt()`/`maybeDecrypt()`.

### What's *not* encrypted, and why that's fine

The **filename** of a contact file is still the hash prefix in plaintext
— an attacker with disk access can see *that* you've saved some hash as
a contact, just not what name you gave it. This is an intentional
non-issue: destination hashes are derived from identities that get
announced over the air, so they're already public information by
Reticulum's own design (see [threat-model.md](threat-model.md)). The
thing actually worth protecting — the human-readable name you privately
attached to that hash — is what the encrypted file content protects.

### Migration

Contacts and the name cache are capped at a small size
(`RSCARDPUTER_MAX_NODES = 50`), so unlike message migration there's no
need for a user-facing opt-in/progress flow. `main.cpp` simply calls
`saveContacts()` and `saveNameCache()` once, immediately after
`setIdentity()` + `loadContacts()`/`loadNameCache()`, whenever encryption
is enabled:

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
(namespace `ratcom_cfg`, key `json`, as a bulletproof backup that survives
flash corruption). That blob includes the WiFi AP/STA SSID+password, TCP
hub host/port list, LoRa radio parameters, display name, and display/audio
preferences.

`UserConfig` gained the same `setIdentity()` / `encryptionEnabled()` pair,
plus two private helper methods, `maybeEncrypt()` / `maybeDecrypt()`, used
by every load/save tier (`load(FlashStore&)`, `save(FlashStore&)`,
`load(SDStore&, FlashStore&)`, `save(SDStore&, FlashStore&)`).

### The NVS binary-safety fix

This is the one part of this change that isn't a pure copy of the
message-encryption pattern. The NVS backup was previously stored with
`Preferences::putString()`/`getString()` — fine for plaintext JSON (which
is null-terminated, printable text), but an encrypted envelope is binary
and can legitimately contain `0x00` bytes anywhere in the ciphertext or
MAC. A `0x00` byte would silently truncate a `putString()`-backed NVS
entry.

`saveToNVS()`/`loadFromNVS()` were switched to `putBytes()`/`getBytes()`,
which store/retrieve an exact byte length instead of relying on null
termination — the same approach `IdentityCrypto`'s NVS path already used
for the identity blob. To avoid losing a pre-upgrade NVS backup written
under the old `putString()` format, `loadFromNVS()` tries `getBytes()`
first and falls back to `getString()` if no `PT_BLOB` entry is found:

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
        // legacy PT_STR entry from a pre-encryption build
        out = prefs.getString(NVS_KEY, "");
    }
    prefs.end();
    return out;
}
```

The next successful save rewrites the NVS entry as bytes, so this
fallback only matters for the one boot right after upgrading.

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
