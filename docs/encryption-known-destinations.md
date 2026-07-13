# Known-Destinations Encryption

Module: [`src/storage/KnownDestEncryption.{h,cpp}`](../src/storage/KnownDestEncryption.h)
Wired into: [`src/reticulum/ReticulumManager.cpp`](../src/reticulum/ReticulumManager.cpp)
Library-side hook: [`cryptspeak/microReticulum`](https://github.com/cryptspeak/microReticulum)'s
`RNS::Identity::known_destinations_crypto()`

Domain: magic `RKD1`, HKDF info `rscardputer.knowndest.v1`.

## What this protects

microReticulum keeps its own record of every destination whose announce
has been validated, independent of this firmware's contacts/name-cache
files (see [encryption-contacts-settings.md](encryption-contacts-settings.md)).
Whenever `RNS::Transport` validates an announce, it calls
`RNS::Identity::remember()` with that announce's `app_data` — for an LXMF
peer, the same msgpack-encoded display name `AnnounceManager::extractDisplayName()`
parses into `_nameCache`. The library keeps this table in RAM and
periodically flushes it to `<storage>/known_destinations` on its own,
via `Identity::save_known_destinations()`/`load_known_destinations()`.

This table is populated for every validated announce, not just peers
saved as contacts — `AnnounceManager::lookupName()` uses
`RNS::Identity::recall_app_data()` as a fallback name source specifically
because it covers that broader set. In practice, having any message
history with a peer means their announce was validated at some point, so
this file alone is enough to reconstruct "who this device has talked to"
from a raw flash dump, independent of whether contacts were saved or
encrypted.

## How it's protected

The destination hash, packet hash, and public key in each entry are
already public by Reticulum's own design (see [threat-model.md](threat-model.md));
only `app_data` needs protecting. The library serializes each entry as
part of one packed binary record, so the whole serialized table is
encrypted as a single blob rather than as individual fields.

Since this file is owned by the vendored library, the hook is exposed
there rather than reimplemented in firmware code:

```cpp
using StorageCryptoFn = std::function<bool(const Bytes& in, Bytes& out)>;
static void known_destinations_crypto(StorageCryptoFn encrypt, StorageCryptoFn decrypt);
```

`save_known_destinations()` runs the encrypt hook on the serialized
buffer right before `OS::write_file()`; `load_known_destinations()` runs
the decrypt hook on the raw buffer right after `OS::read_file()`, before
parsing entries out of it. With no hook registered, both are no-ops —
any other consumer of this fork that doesn't wire the hook sees the same
plaintext behavior as before this feature existed.

`ReticulumManager::begin()` wires the hook right after
`loadOrCreateIdentity()` returns (decrypting requires the identity's
private key, so this can't happen earlier), using `AtRestCrypto` through
the `KnownDestEncryption` domain wrapper. Unlike `ContactsEncryption`/
`SettingsEncryption`, this wrapper operates on raw `RNS::Bytes` rather
than `String`, since the known-destinations format is packed binary
(hash lengths, timestamps, keys) that can legitimately contain `0x00`
bytes throughout — the same binary-safety concern documented for the
settings NVS backup in [encryption-contacts-settings.md](encryption-contacts-settings.md).

`RNS::Identity::load_known_destinations()` runs after
`loadOrCreateIdentity()`, once the hook is wired. Immediately after
loading, `save_known_destinations()` runs once unconditionally, the same
migrate-on-load pattern used for contacts and settings — this upgrades a
pre-encryption plaintext file on the first boot after updating.
Decrypting a plaintext file (no `RKD1` magic) passes the bytes through
unchanged, so the migration is non-breaking for existing devices.

## Domain separation

This uses its own magic and HKDF info string rather than reusing
`ContactsEncryption`'s domain, for the same reason messages, contacts,
and settings each have their own: a key derived for one domain can't
decrypt another, so recovering one doesn't hand over the others.
`ContactsEncryption`'s API is also `String`-shaped (JSON in, JSON out),
which wouldn't fit `known_destinations`' packed-binary format — this
domain uses `AtRestCrypto::encrypt()`/`decrypt()` directly instead.
