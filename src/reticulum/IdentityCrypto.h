#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <Bytes.h>

// =============================================================================
// IdentityCrypto — at-rest encryption of the Reticulum identity private key
// =============================================================================
//
// Threat model (offline LoRa device):
//   - Attacker has physical access to flash / NVS / SD card after device is off.
//   - Attacker can dump raw bytes from any storage tier.
//   - In-flight Reticulum packets are NOT in scope here — those keep their
//     existing on-wire encryption (Reticulum's own per-packet ratchet/Ed25519).
//   - We only protect the private key BLOB on disk and message blobs on disk.
//
// Scheme:
//   password ──PBKDF2-HMAC-SHA256(salt, iter)──▶ KEK (32 bytes)
//   KEK is split with HKDF-SHA256 into:
//     enc_key (32B) — AES-256-CTR
//     mac_key (32B) — HMAC-SHA256 (encrypt-then-MAC)
//   identity private-key bytes are encrypted with enc_key+random IV, then
//   MACed. MAC is verified BEFORE decrypt (constant-time) → wrong password,
//   bit-flip, or wrong file all fail without ever producing plaintext.
//
// File layout (encrypted identity, 70+ bytes):
//   ┌─────────┬──────┬─────────┬───────┬──────┬──────────────┬─────────────┐
//   │ magic   │ ver  │ iter_log│ salt  │ IV   │ ciphertext   │ HMAC-SHA256 │
//   │ 4 bytes │ 1 B  │ 1 B     │ 16 B  │ 16 B │ N bytes      │ 32 bytes    │
//   └─────────┴──────┴─────────┴───────┴──────┴──────────────┴─────────────┘
//   magic = "RID1"  iter_log = log2(iterations)  (e.g. 16 → 65536 iterations)
//
// Notes:
//   - CTR mode is malleable on its own. The HMAC over (magic|ver|iter_log|salt|
//     IV|ciphertext) provides authenticated encryption (encrypt-then-MAC).
//   - Random IV each write → same plaintext under same key yields different
//     ciphertext, so no ECB-style structure leaks.
//   - No global key reuse: the identity is a one-shot blob, IV is fresh.
// =============================================================================

namespace IdentityCrypto {

extern const uint8_t MAGIC[4];        // "RID1"
constexpr uint8_t  VERSION      = 0x01;
constexpr size_t   MAGIC_LEN    = 4;
constexpr size_t   SALT_LEN     = 16;
constexpr size_t   IV_LEN       = 16;
constexpr size_t   MAC_LEN      = 32;
constexpr size_t   HEADER_LEN   = MAGIC_LEN + 1 + 1 + SALT_LEN + IV_LEN; // 38
constexpr size_t   OVERHEAD     = HEADER_LEN + MAC_LEN;                   // 70

// PBKDF2 iteration count on ESP32-S3 — chosen to balance security vs unlock
// latency. 2^16 = 65536 iterations is ~2-3s on a 240MHz ESP32-S3 with the
// software SHA256 in lib/Crypto. Bumping by 1 doubles the cost.
constexpr uint8_t  DEFAULT_ITER_LOG = 16;

// True if data starts with the encrypted-identity magic.
bool isEncrypted(const uint8_t* data, size_t len);

// Derive 32-byte KEK from password + salt using PBKDF2-HMAC-SHA256.
// iterations = 1 << iter_log.
bool deriveKEK(const char* password, size_t pw_len,
               const uint8_t salt[SALT_LEN], uint8_t iter_log,
               uint8_t out_kek[32]);

// Encrypt identity_key (raw private-key bytes from RNS::Identity::get_private_key())
// using `password`. Produces a self-describing blob suitable for writing to disk.
// Returns true on success; `out` is overwritten only on success.
bool wrap(const char* password, size_t pw_len,
          const uint8_t* identity_key, size_t identity_key_len,
          RNS::Bytes& out);

// Convenience overload — defaults iter_log.
bool wrap(const String& password,
          const RNS::Bytes& identity_key, RNS::Bytes& out);

// Verify HMAC and decrypt. Returns true and writes plaintext on success.
// Returns false on wrong password, corrupted data, bad magic, or unsupported
// version. No plaintext is ever produced from an unauthenticated blob.
bool unwrap(const char* password, size_t pw_len,
            const uint8_t* blob, size_t blob_len,
            RNS::Bytes& out_identity_key);

bool unwrap(const String& password,
            const RNS::Bytes& blob, RNS::Bytes& out_identity_key);

// Securely overwrite a buffer (defeats compiler dead-store elimination).
void secureZero(void* p, size_t n);

}  // namespace IdentityCrypto
