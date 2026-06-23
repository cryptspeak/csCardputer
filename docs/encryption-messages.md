# Message Encryption

Module: [`src/storage/MessageEncryption.{h,cpp}`](../src/storage/MessageEncryption.h)
Wired into: [`src/storage/MessageStore.cpp`](../src/storage/MessageStore.cpp)

This module is unchanged by the contacts/settings work in this branch —
documented here for completeness since it's the design template the new
domains followed. See [encryption-overview.md](encryption-overview.md)
for how it now shares its core envelope logic with
`ContactsEncryption`/`SettingsEncryption` via `AtRestCrypto`, without its
own behavior changing at all.

## Scope

Protects the JSON blob of each stored LXMF message (content + metadata)
once it's written to flash or SD. In-transit messages are unaffected —
they keep using Reticulum/LXMF's own protocol-level encryption
(Ed25519 + Reticulum's ratchet). This is purely an at-rest layer on top.

## Keying

```
keys = HKDF-SHA256(64 bytes,
                    ikm  = identity.encryptionPrivateKey(),
                    salt = identity.hash(),
                    info = "rscardputer.msg.v1")
keys[0:32]  → AES-256-CTR encryption key
keys[32:64] → HMAC-SHA256 authentication key
```

Salting with the identity hash binds the derived keys to *this* identity
— flashing a different identity onto the device cannot decrypt old
messages, even though the cipher and mode are the same.

## On-disk format

```
┌─────────┬──────┬──────┬──────────────┬─────────────┐
│ magic   │ ver  │ IV   │ ciphertext   │ HMAC-SHA256 │
│ 4 bytes │ 1 B  │ 16 B │ N bytes      │ 32 bytes    │
└─────────┴──────┴──────┴──────────────┴─────────────┘
magic = "RMS1"        OVERHEAD = 53 bytes
```

Same encrypt-then-MAC, constant-time-verify-before-decrypt construction as
identity encryption (see [encryption-identity.md](encryption-identity.md)).

## Where it's wired in

`MessageStore` holds a `const RNS::Identity*` (set via
`setIdentity()` once the identity is unlocked at boot — see
[boot-sequence.md](boot-sequence.md)). Two free functions in
`MessageStore.cpp` wrap every disk access:

- `maybeEncrypt(identity, json)` — called before every write
  (`saveMessage()`, status updates). If no identity is set, falls back to
  writing plaintext (so the store still works before boot reaches the
  point where the identity is available, e.g. very early init).
- `maybeDecrypt(identity, raw)` — called after every read
  (`loadConversation()`, pending-outgoing reload, etc.). Transparently
  passes through legacy files that don't carry the `RMS1` magic, so
  reading is never broken by mixed encrypted/plaintext history.

## Migration

`MessageStore::migratePlaintextMessages()` walks every stored conversation
and re-encrypts any plaintext file it finds in place, skipping files
already encrypted. Because a device can accumulate a large message
history, this is **not** run unconditionally at boot — the user is asked
(`MigrationWarningScreen`) whether to migrate identity only (fast) or
identity *and* messages (slower, with a progress bar), the first time a
legacy plaintext identity is detected. See
[boot-sequence.md](boot-sequence.md) for the exact trigger condition.

This is in contrast to the contacts and settings migrations added in this
branch, which run unconditionally and immediately because both datasets
are small (≤50 contacts, one settings file) — see
[encryption-contacts-settings.md](encryption-contacts-settings.md).
