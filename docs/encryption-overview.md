# Encryption Overview

Cryptspeak, csCardputer's at-rest encryption layer, adds **mandatory at-rest encryption** for
everything the firmware writes to flash, NVS, and SD card. It does not
change anything about the Reticulum/LXMF wire protocol — packets on LoRa,
WiFi, or TCP keep using their own protocol-level crypto, unmodified. This
is purely about making a raw dump of the device's storage (flash chip
desoldered, SD card pulled, NVS partition extracted) useless to an
attacker who doesn't know the password.

The overall approach (wrap the identity with a password-derived key,
derive everything else from the identity) builds on the idea and
proof-of-concept in [konsumer](https://github.com/konsumer)'s
[arduino-rns-encrypted-store](https://github.com/konsumer/arduino-rns-encrypted-store)
and [arduino-rns-password](https://github.com/konsumer/arduino-rns-password).

## What's protected

| Data category | Storage | Encrypted? | Module |
|---|---|---|---|
| Identity private key | Flash, NVS, SD | Yes | [`IdentityCrypto`](encryption-identity.md) |
| LXMF message bodies | Flash, SD | Yes | [`MessageEncryption`](encryption-messages.md) |
| Saved contacts + name cache | Flash, SD | Yes | [`ContactsEncryption`](encryption-contacts-settings.md) |
| Device settings (WiFi passwords, TCP hubs, radio config, display name) | Flash, NVS, SD | Yes | [`SettingsEncryption`](encryption-contacts-settings.md) |
| microReticulum's `known_destinations` table (announced peer names, cached independently of the app's own contacts/name-cache files) | Flash | Yes | [`KnownDestEncryption`](encryption-known-destinations.md) |
| Announces / destination hashes on the air | — | Not applicable — public by Reticulum's own design | — |
| Reticulum/LXMF packets in flight | — | Unchanged — protocol's own crypto | — |

Every category above went through the same envelope; there is no
unencrypted category left except the color theme (see
[theme-config.md](theme-config.md) for why that one is deliberately
excluded). See [threat-model.md](threat-model.md) for the full
adversarial reasoning.

## One engine, five domains

All five protected categories — identity, messages, contacts, settings,
known-destinations — use the **same authenticated-encryption envelope**
(AES-256-CTR + HMAC-SHA256, encrypt-then-MAC). Identity has its own
implementation (`IdentityCrypto`) because it alone derives its key from
the user's *password* via PBKDF2 (it has to — the identity key is the
thing that makes every other domain's key possible). The other four
domains derive their keys from the *identity's private key* via HKDF, so
they share one generic implementation:

```
src/storage/AtRestCrypto.{h,cpp}       ← shared envelope engine
src/storage/MessageEncryption.{h,cpp}  ← domain: messages           (magic "RMS1")
src/storage/ContactsEncryption.{h,cpp} ← domain: contacts           (magic "RCN1")
src/storage/SettingsEncryption.{h,cpp} ← domain: settings           (magic "RUC1")
src/storage/KnownDestEncryption.{h,cpp}← domain: known-destinations (magic "RKD1")
```

`AtRestCrypto` is the shared envelope engine: the same encrypt/decrypt/MAC
logic used by every domain, parameterized by a 4-byte magic and an HKDF
"info" string per domain rather than duplicating the ~150 lines of crypto
per file.

### Why separate domains instead of one shared key?

Each domain's encryption key is derived as:

```
keys = HKDF-SHA256(64 bytes,
                    ikm  = identity.encryptionPrivateKey(),
                    salt = identity.hash(),
                    info = "<domain-specific string>")
keys[0:32]  → AES-256-CTR key
keys[32:64] → HMAC-SHA256 key
```

The `info` string namespaces the derivation per domain:

| Domain | Magic | HKDF info |
|---|---|---|
| Messages | `RMS1` | `rscardputer.msg.v1` |
| Contacts | `RCN1` | `rscardputer.contacts.v1` |
| Settings | `RUC1` | `rscardputer.settings.v1` |
| Known-destinations | `RKD1` | `rscardputer.knowndest.v1` |

A key derived for one domain cannot decrypt another, even though all
four ultimately come from the same identity private key. This means a
bug or future attack that recovers the message key, say, doesn't also
hand over your WiFi password.

## Envelope format (shared by messages, contacts, settings)

```
┌─────────┬──────┬──────┬──────────────┬─────────────┐
│ magic   │ ver  │ IV   │ ciphertext   │ HMAC-SHA256 │
│ 4 bytes │ 1 B  │ 16 B │ N bytes      │ 32 bytes    │
└─────────┴──────┴──────┴──────────────┴─────────────┘
```

- **Encrypt-then-MAC**: the HMAC covers `magic | version | IV | ciphertext`.
  On decrypt, the MAC is checked with a constant-time comparison **before**
  any decryption happens. Wrong identity, bit-rot, or deliberate tampering
  all fail the same way — no plaintext is ever produced from data that
  doesn't authenticate.
- **Fresh random 16-byte IV per write.** AES-256-CTR reuses a key safely as
  long as the IV never repeats; with a 128-bit IV space the birthday bound
  is astronomically larger than anything this device could ever write.
- **Legacy passthrough.** A buffer that doesn't start with the domain's
  magic (or is shorter than the 53-byte minimum envelope size) is treated
  as a pre-encryption plaintext file and returned unchanged. This is what
  makes the rollout non-breaking — see [boot-sequence.md](boot-sequence.md)
  for how existing plaintext data gets upgraded.

## Crypto primitives in use

All from the vendored [rweather Crypto library](../lib/Crypto/) (no
external dependency added):

| Purpose | Primitive |
|---|---|
| Bulk cipher | AES-256, CTR mode |
| Authentication | HMAC-SHA256 |
| Key derivation (identity → domain keys) | HKDF-SHA256 |
| Key derivation (password → KEK) | PBKDF2-HMAC-SHA256 (hand-rolled in `IdentityCrypto`, see [encryption-identity.md](encryption-identity.md)) |
| Randomness (IVs, salts) | microReticulum's `RNS::Cryptography::random()` |

The library also vendors ChaCha20/Poly1305, GCM, EAX, SHA-3/Keccak,
BLAKE2, and Ed25519/Curve25519 (used by Reticulum's own identity/transport
crypto, not by this at-rest layer). They're available if a future domain
needs them, but AES-256-CTR+HMAC was kept for the new domains specifically
to match the existing, already-reviewed identity/message design rather
than introduce a second authenticated-encryption construction.

## Read next

- New to this codebase? Start with [boot-sequence.md](boot-sequence.md) to
  see where all of this gets wired together at runtime.
- Reviewing the new contacts/settings work specifically?
  [encryption-contacts-settings.md](encryption-contacts-settings.md) has
  the call-site-level detail.
- Curious how microReticulum's own `known_destinations` table got closed
  off? [encryption-known-destinations.md](encryption-known-destinations.md).
- Want the adversarial framing? [threat-model.md](threat-model.md).
- Curious about the optional duress password? It reuses `IdentityCrypto`'s
  wrap/unwrap envelope for an unrelated purpose (a verifier, not a key) —
  see [duress-password.md](duress-password.md).
