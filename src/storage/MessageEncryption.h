#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <Bytes.h>
#include <Identity.h>

// =============================================================================
// MessageEncryption — at-rest encryption for stored message JSON blobs
// =============================================================================
//
// In-transit messages keep using Reticulum/LXMF's own protocol-level
// encryption (Ed25519 + Reticulum's ratchet). This module ONLY protects the
// message JSON blob once it has been written to flash or SD. The decision
// to add this layer is to make raw-flash dumps useless to an attacker.
//
// Keying:
//   keys = HKDF-SHA256(64 bytes, ikm = identity.encryptionPrivateKey(),
//                                salt = identity.hash(),
//                                info = "rscardputer.msg.v1")
//   keys[0:32]  → AES-256-CTR encryption key
//   keys[32:64] → HMAC-SHA256 authentication key
//
//   The identity hash as salt binds derived keys to this specific identity,
//   not just the raw key bytes — flashing a different identity onto the
//   device cannot decrypt old messages.
//
//   The HKDF info string is namespaced so the same identity can derive
//   separate keys for other purposes (e.g. identity-vault) without collision.
//
// File layout (per encrypted message, 53+ bytes):
//   ┌─────────┬──────┬──────┬──────────────┬─────────────┐
//   │ magic   │ ver  │ IV   │ ciphertext   │ HMAC-SHA256 │
//   │ 4 bytes │ 1 B  │ 16 B │ N bytes      │ 32 bytes    │
//   └─────────┴──────┴──────┴──────────────┴─────────────┘
//   magic = "RMS1"
//
// Notes:
//   - Encrypt-then-MAC: HMAC covers magic|ver|IV|ciphertext. Verified
//     constant-time BEFORE decrypt — wrong identity / tampered file fail.
//   - Random 16-byte IV per write: AES-256-CTR with same key is safe so long
//     as no IV repeats. With 2^128 IV space the birthday bound is enormous;
//     for a device storing <2^32 messages collision risk is negligible.
//   - Files smaller than OVERHEAD or without the magic header are treated as
//     plaintext legacy files (transparent fallback for un-migrated data).
// =============================================================================

namespace MessageEncryption {

extern const uint8_t MAGIC[4];          // "RMS1"
constexpr uint8_t  VERSION    = 0x01;
constexpr size_t   MAGIC_LEN  = 4;
constexpr size_t   IV_LEN     = 16;
constexpr size_t   MAC_LEN    = 32;
constexpr size_t   HEADER_LEN = MAGIC_LEN + 1 + IV_LEN;   // 21
constexpr size_t   OVERHEAD   = HEADER_LEN + MAC_LEN;     // 53

// True if buffer starts with the encrypted-message magic and is long enough
// to be a valid envelope.
bool isEncrypted(const uint8_t* data, size_t len);
inline bool isEncrypted(const RNS::Bytes& b) { return isEncrypted(b.data(), b.size()); }
inline bool isEncrypted(const String& s) {
    return isEncrypted((const uint8_t*)s.c_str(), s.length());
}

// Encrypt the bytes in `plaintext` using keys derived from `identity`.
// On success returns true and writes the envelope to `out`.
// `identity` must have its private key loaded.
bool encrypt(const RNS::Identity& identity,
             const uint8_t* plaintext, size_t pt_len,
             RNS::Bytes& out);

// Convenience: encrypts a JSON String and returns a String of raw envelope
// bytes (suitable for writing through the existing FlashStore/SDStore
// writeString path — they treat the String as an opaque byte buffer).
bool encryptString(const RNS::Identity& identity,
                   const String& plaintext,
                   String& out);

// Verify MAC and decrypt envelope into `out`. Returns false on bad magic,
// wrong identity, or tampered data. No plaintext is produced on failure.
bool decrypt(const RNS::Identity& identity,
             const uint8_t* blob, size_t blob_len,
             RNS::Bytes& out_plaintext);

// Decrypts envelope bytes into a String. If `blob` does NOT carry the magic
// header (legacy plaintext file), returns it unchanged so callers can treat
// the storage tier as transparently mixed during migration.
bool decryptOrPassthrough(const RNS::Identity& identity,
                          const String& blob,
                          String& out);

}  // namespace MessageEncryption
