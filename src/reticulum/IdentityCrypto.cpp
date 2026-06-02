#include "IdentityCrypto.h"

#include <CTR.h>
#include <AES.h>
#include <SHA256.h>
#include <Cryptography/HKDF.h>
#include <Cryptography/HMAC.h>
#include <Cryptography/Random.h>
#include <string.h>

namespace IdentityCrypto {

const uint8_t MAGIC[4] = {'R', 'I', 'D', '1'};

void secureZero(void* p, size_t n) {
    if (!p || n == 0) return;
    volatile uint8_t* v = (volatile uint8_t*)p;
    while (n--) *v++ = 0;
}

bool isEncrypted(const uint8_t* data, size_t len) {
    if (!data || len < HEADER_LEN + MAC_LEN) return false;
    return memcmp(data, MAGIC, MAGIC_LEN) == 0;
}

// Constant-time memcmp: returns 0 iff equal. Does not short-circuit, so
// timing leaks the comparison length, not the matching prefix length.
static int ct_memcmp(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= a[i] ^ b[i];
    return diff;  // 0 iff equal
}

// PBKDF2-HMAC-SHA256 — RFC 8018. Produces a single 32-byte output block (so
// we don't need the multi-block T_1..T_n loop). Stretching is the inner
// iteration loop.
bool deriveKEK(const char* password, size_t pw_len,
               const uint8_t salt[SALT_LEN], uint8_t iter_log,
               uint8_t out_kek[32]) {
    if (!password || pw_len == 0 || !salt || !out_kek) return false;
    if (iter_log == 0 || iter_log > 24) return false;  // sanity: 1..16.7M
    const uint32_t iterations = 1UL << iter_log;

    // U_1 = HMAC(password, salt || INT(1))
    uint8_t block_input[SALT_LEN + 4];
    memcpy(block_input, salt, SALT_LEN);
    block_input[SALT_LEN + 0] = 0;
    block_input[SALT_LEN + 1] = 0;
    block_input[SALT_LEN + 2] = 0;
    block_input[SALT_LEN + 3] = 1;

    SHA256 mac;
    mac.resetHMAC(password, pw_len);
    mac.update(block_input, sizeof(block_input));
    uint8_t U[32];
    mac.finalizeHMAC(password, pw_len, U, 32);

    uint8_t T[32];
    memcpy(T, U, 32);

    // U_i = HMAC(password, U_{i-1});  T = U_1 ^ U_2 ^ ... ^ U_c
    for (uint32_t i = 1; i < iterations; i++) {
        SHA256 m2;
        m2.resetHMAC(password, pw_len);
        m2.update(U, 32);
        m2.finalizeHMAC(password, pw_len, U, 32);
        for (int j = 0; j < 32; j++) T[j] ^= U[j];
        // Yield occasionally so WDT doesn't reset us during ~2-3s derivation.
        if ((i & 0x3FF) == 0) yield();
    }

    memcpy(out_kek, T, 32);
    secureZero(U, sizeof(U));
    secureZero(T, sizeof(T));
    return true;
}

// Split a 32-byte KEK into (enc_key || mac_key) = 64 bytes via HKDF-Expand.
// Use HKDF (already imported by microReticulum) so we don't reimplement.
// The KEK is used as input keying material; we use a fixed context string
// to bind the split to this purpose.
static bool splitKEK(const uint8_t kek[32], uint8_t out64[64]) {
    RNS::Bytes ikm(kek, 32);
    RNS::Bytes salt;            // empty
    // HKDF context binds the derived keys to this specific use, so the same
    // KEK could safely be used for other purposes with a different context.
    static const char* CTX = "rscardputer.identity.v1";
    RNS::Bytes info((const uint8_t*)CTX, strlen(CTX));
    RNS::Bytes derived = RNS::Cryptography::hkdf(64, ikm, salt, info);
    if (derived.size() != 64) return false;
    memcpy(out64, derived.data(), 64);
    return true;
}

static RNS::Bytes compute_mac(const uint8_t mac_key[32],
                              const uint8_t* header, size_t header_len,
                              const uint8_t* ct, size_t ct_len) {
    RNS::Bytes key(mac_key, 32);
    RNS::Cryptography::HMAC hmac(key);
    hmac.update(RNS::Bytes(header, header_len));
    hmac.update(RNS::Bytes(ct, ct_len));
    return hmac.digest();
}

bool wrap(const char* password, size_t pw_len,
          const uint8_t* identity_key, size_t identity_key_len,
          RNS::Bytes& out) {
    if (!password || pw_len == 0) return false;
    if (!identity_key || identity_key_len == 0) return false;

    // Random salt + IV from the CSPRNG microReticulum already uses.
    RNS::Bytes salt_bytes = RNS::Cryptography::random(SALT_LEN);
    RNS::Bytes iv_bytes   = RNS::Cryptography::random(IV_LEN);
    if (salt_bytes.size() != SALT_LEN || iv_bytes.size() != IV_LEN) return false;

    uint8_t kek[32];
    if (!deriveKEK(password, pw_len, salt_bytes.data(),
                   DEFAULT_ITER_LOG, kek)) return false;

    uint8_t keys[64];
    if (!splitKEK(kek, keys)) {
        secureZero(kek, sizeof(kek));
        return false;
    }
    secureZero(kek, sizeof(kek));

    // Encrypt identity_key
    uint8_t* ct = (uint8_t*)malloc(identity_key_len);
    if (!ct) {
        secureZero(keys, sizeof(keys));
        return false;
    }
    {
        CTR<AES256> ctr;
        ctr.setKey(keys, 32);
        ctr.setIV(iv_bytes.data(), IV_LEN);
        ctr.encrypt(ct, identity_key, identity_key_len);
    }

    // Build header bytes
    size_t blob_len = HEADER_LEN + identity_key_len + MAC_LEN;
    uint8_t* buf = out.writable(blob_len);
    if (!buf) {
        free(ct);
        secureZero(keys, sizeof(keys));
        return false;
    }
    size_t off = 0;
    memcpy(buf + off, MAGIC, MAGIC_LEN);            off += MAGIC_LEN;
    buf[off++] = VERSION;
    buf[off++] = DEFAULT_ITER_LOG;
    memcpy(buf + off, salt_bytes.data(), SALT_LEN); off += SALT_LEN;
    memcpy(buf + off, iv_bytes.data(),   IV_LEN);   off += IV_LEN;
    memcpy(buf + off, ct, identity_key_len);        off += identity_key_len;

    RNS::Bytes mac = compute_mac(keys + 32, buf, HEADER_LEN, ct, identity_key_len);
    if (mac.size() != MAC_LEN) {
        free(ct);
        secureZero(keys, sizeof(keys));
        out = RNS::Bytes();
        return false;
    }
    memcpy(buf + off, mac.data(), MAC_LEN);         off += MAC_LEN;
    out.resize(blob_len);

    free(ct);
    secureZero(keys, sizeof(keys));
    return true;
}

bool wrap(const String& password, const RNS::Bytes& identity_key, RNS::Bytes& out) {
    return wrap(password.c_str(), password.length(),
                identity_key.data(), identity_key.size(), out);
}

bool unwrap(const char* password, size_t pw_len,
            const uint8_t* blob, size_t blob_len,
            RNS::Bytes& out_identity_key) {
    if (!password || pw_len == 0) return false;
    if (!blob || blob_len < OVERHEAD) return false;
    if (memcmp(blob, MAGIC, MAGIC_LEN) != 0) return false;

    uint8_t version  = blob[MAGIC_LEN];
    uint8_t iter_log = blob[MAGIC_LEN + 1];
    if (version != VERSION) return false;
    if (iter_log == 0 || iter_log > 24) return false;

    const uint8_t* salt = blob + MAGIC_LEN + 2;
    const uint8_t* iv   = salt + SALT_LEN;
    size_t ct_len       = blob_len - OVERHEAD;
    const uint8_t* ct   = iv + IV_LEN;
    const uint8_t* mac_stored = ct + ct_len;

    uint8_t kek[32];
    if (!deriveKEK(password, pw_len, salt, iter_log, kek)) return false;

    uint8_t keys[64];
    if (!splitKEK(kek, keys)) {
        secureZero(kek, sizeof(kek));
        return false;
    }
    secureZero(kek, sizeof(kek));

    // Verify MAC over header || ciphertext BEFORE decrypting.
    RNS::Bytes mac_expected = compute_mac(keys + 32, blob, HEADER_LEN, ct, ct_len);
    if (mac_expected.size() != MAC_LEN ||
        ct_memcmp(mac_expected.data(), mac_stored, MAC_LEN) != 0) {
        secureZero(keys, sizeof(keys));
        return false;
    }

    uint8_t* pt = out_identity_key.writable(ct_len);
    if (!pt) {
        secureZero(keys, sizeof(keys));
        return false;
    }
    {
        CTR<AES256> ctr;
        ctr.setKey(keys, 32);
        ctr.setIV(iv, IV_LEN);
        ctr.decrypt(pt, ct, ct_len);
    }
    out_identity_key.resize(ct_len);

    secureZero(keys, sizeof(keys));
    return true;
}

bool unwrap(const String& password, const RNS::Bytes& blob, RNS::Bytes& out_identity_key) {
    return unwrap(password.c_str(), password.length(),
                  blob.data(), blob.size(), out_identity_key);
}

}  // namespace IdentityCrypto
