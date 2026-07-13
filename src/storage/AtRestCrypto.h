#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <Bytes.h>
#include <Identity.h>

// =============================================================================
// AtRestCrypto — shared envelope format for identity-keyed at-rest encryption
// =============================================================================
//
// This is the generic engine behind MessageEncryption, ContactsEncryption and
// SettingsEncryption. All three protect a different category of data with
// the exact same scheme; only the on-disk magic and the HKDF "info" string
// differ (so a key derived for one category can never decrypt another).
//
// Keying:
//   keys = HKDF-SHA256(64 bytes, ikm = identity.encryptionPrivateKey(),
//                                salt = identity.hash(),
//                                info = domain.hkdfInfo)
//   keys[0:32]  → AES-256-CTR encryption key
//   keys[32:64] → HMAC-SHA256 authentication key
//
//   Salting with the identity hash binds derived keys to this specific
//   identity — flashing a different identity onto the device cannot decrypt
//   old data. Namespacing by `info` means the same identity yields unrelated
//   keys per domain, so a message key can't be reused to open a settings
//   blob even though both come from the same private key material.
//
// File layout (per encrypted blob, OVERHEAD = 53+ bytes):
//   ┌─────────┬──────┬──────┬──────────────┬─────────────┐
//   │ magic   │ ver  │ IV   │ ciphertext   │ HMAC-SHA256 │
//   │ 4 bytes │ 1 B  │ 16 B │ N bytes      │ 32 bytes    │
//   └─────────┴──────┴──────┴──────────────┴─────────────┘
//
// Notes:
//   - Encrypt-then-MAC: HMAC covers magic|ver|IV|ciphertext. Verified
//     constant-time BEFORE decrypt — wrong identity / tampered file fail
//     without ever producing plaintext.
//   - Random 16-byte IV per write: AES-256-CTR with the same key is safe so
//     long as no IV repeats; with 2^128 IV space the birthday bound is
//     enormous for any data volume this device could ever hold.
//   - Buffers shorter than OVERHEAD or without a matching magic are treated
//     as legacy plaintext (transparent fallback during migration).
// =============================================================================

namespace AtRestCrypto {

constexpr uint8_t  VERSION    = 0x01;
constexpr size_t   MAGIC_LEN  = 4;
constexpr size_t   IV_LEN     = 16;
constexpr size_t   MAC_LEN    = 32;
constexpr size_t   HEADER_LEN = MAGIC_LEN + 1 + IV_LEN;   // 21
constexpr size_t   OVERHEAD   = HEADER_LEN + MAC_LEN;     // 53

// Identifies one at-rest encryption domain: a 4-byte on-disk magic plus the
// HKDF info string used to derive that domain's keys. Each data category
// (messages, contacts, settings, ...) gets its own static Domain instance.
struct Domain {
    uint8_t magic[4];
    const char* hkdfInfo;
};

// True if buffer starts with `domain`'s magic and is long enough to be a
// valid envelope.
bool isEncrypted(const Domain& domain, const uint8_t* data, size_t len);
inline bool isEncrypted(const Domain& domain, const String& s) {
    return isEncrypted(domain, (const uint8_t*)s.c_str(), s.length());
}

// Encrypt `plaintext` using keys derived from `identity` for `domain`.
// `identity` must have its private key loaded.
bool encrypt(const Domain& domain, const RNS::Identity& identity,
             const uint8_t* plaintext, size_t pt_len,
             RNS::Bytes& out);

// Convenience: encrypts a String and returns a String of raw envelope bytes
// (suitable for writing through FlashStore/SDStore's writeString, which
// treats the String as an opaque byte buffer).
bool encryptString(const Domain& domain, const RNS::Identity& identity,
                    const String& plaintext, String& out);

// Verify MAC and decrypt envelope into `out`. Returns false on bad magic,
// wrong identity, or tampered data. No plaintext is produced on failure.
bool decrypt(const Domain& domain, const RNS::Identity& identity,
             const uint8_t* blob, size_t blob_len,
             RNS::Bytes& out_plaintext);

// Decrypts envelope bytes into a String. If `blob` does NOT carry the
// domain's magic header (legacy plaintext file), returns it unchanged so
// callers can treat the storage tier as transparently mixed during
// migration.
bool decryptOrPassthrough(const Domain& domain, const RNS::Identity& identity,
                           const String& blob, String& out);

// Securely overwrite a buffer (defeats compiler dead-store elimination).
void secureZero(void* p, size_t n);

// Deterministic, identity-keyed "blind index": HMAC-SHA256(key, input),
// hex-encoded and truncated to `hexChars` characters. `key` is derived the
// same way as encrypt()/decrypt() (HKDF over the identity's private key,
// salted with its hash) but namespaced by its own `hkdfInfo` string, so an
// index token can't be correlated with, or used to derive, any domain's
// encryption key.
//
// Used where a value must stay addressable as a filename/directory name
// (so it can be looked up without decrypting everything first) without
// that name itself revealing the plaintext it stands for — e.g. a peer's
// destination hash used as a conversation directory name. Same identity
// always produces the same token for the same input (so existing files
// stay reachable across reboots), but a different identity — or no
// identity at all — cannot reproduce or invert it.
bool blindIndexHex(const RNS::Identity& identity, const char* hkdfInfo,
                    const uint8_t* input, size_t input_len,
                    std::string& out_hex, size_t hexChars = 16);

}  // namespace AtRestCrypto
