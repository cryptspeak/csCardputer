# Identity Encryption

Module: [`src/reticulum/IdentityCrypto.{h,cpp}`](../src/reticulum/IdentityCrypto.h)

The Reticulum identity is the device's private key — everything else
(message encryption, contacts encryption, settings encryption) ultimately
derives its keys from this one secret, so it gets its own KDF straight
from the user's password rather than depending on anything else being
unlocked first.

## Password → key

```
password ──PBKDF2-HMAC-SHA256(salt, iterations)──▶ KEK (32 bytes)
```

- **Salt**: 16 random bytes, generated fresh per wrap (i.e. per save) and
  stored alongside the ciphertext.
- **Iterations**: `1 << iter_log`, with `iter_log` defaulting to 16
  (65,536 iterations). This takes roughly 2-3 seconds on the ESP32-S3 at
  240 MHz — that cost is also what an attacker pays per guess if they try
  to brute-force the password offline.
- `iter_log` is stored in the file header, so the cost can be raised in a
  future firmware version without breaking the ability to read
  already-encrypted files written under the old cost.

The KEK is then split with HKDF-SHA256 into:

```
enc_key (32 bytes) → AES-256-CTR
mac_key (32 bytes) → HMAC-SHA256
```

using the context string `"rscardputer.identity.v1"`.

## On-disk format

```
┌─────────┬──────┬─────────┬───────┬──────┬──────────────┬─────────────┐
│ magic   │ ver  │ iter_log│ salt  │ IV   │ ciphertext   │ HMAC-SHA256 │
│ 4 bytes │ 1 B  │ 1 B     │ 16 B  │ 16 B │ N bytes      │ 32 bytes    │
└─────────┴──────┴─────────┴───────┴──────┴──────────────┴─────────────┘
magic = "RID1"        OVERHEAD = 70 bytes
```

Encrypt-then-MAC: the HMAC covers
`magic | version | iter_log | salt | IV | ciphertext`. On unlock, the MAC
is verified with a **constant-time comparison before any decryption
happens** — a wrong password, a corrupted file, and a tampered file all
fail identically, and none of them ever produce plaintext.

## Storage tiers

The encrypted identity blob is written to all three storage tiers so the
device can recover from any single tier failing:

| Tier | Path / key |
|---|---|
| Flash (LittleFS) | `/identity/identity.key` |
| NVS (internal flash, no SPI bus) | namespace `ratcom_id`, key `encid` (via `Preferences::putBytes`/`getBytes`, not `putString` — the blob is binary) |
| SD card | `/ratcom/identity/identity.key` |

`ReticulumManager::saveIdentityToAll()` (`src/reticulum/ReticulumManager.cpp`)
writes all three. It refuses to write a *plaintext* identity if no
password has been set — encryption is mandatory, there's no skip path.

## Legacy plaintext → encrypted migration

If a device was provisioned before the Crypto Edition (or its identity
file predates this scheme), `ReticulumManager::probeIdentityState()`
detects the absence of the `RID1` magic and classifies the identity as
`LEGACY_PLAINTEXT`. The boot flow then:

1. Loads the plaintext key as-is.
2. Forces the password-setup flow (there's no "unlock" for a plaintext
   identity, since there's nothing to unlock yet).
3. Immediately re-wraps and rewrites the identity to all three tiers,
   encrypted.

See [boot-sequence.md](boot-sequence.md) for where this sits in the full
boot order, and how it interacts with message migration.

## Lockout

Wrong-password attempts are capped at 10 per power cycle, with a backoff
delay between attempts. No data is wiped on lockout — losing access to
the password means losing access to the data, not the data itself being
destroyed. Power-cycling resets the attempt counter (there's no
persistent lockout state, intentionally — see
[threat-model.md](threat-model.md) for why this is an acceptable
trade-off for this device class).
