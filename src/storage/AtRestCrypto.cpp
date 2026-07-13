#include "AtRestCrypto.h"

#include <CTR.h>
#include <AES.h>
#include <Cryptography/HKDF.h>
#include <Cryptography/HMAC.h>
#include <Cryptography/Random.h>
#include <string.h>
#include <algorithm>

namespace AtRestCrypto {

void secureZero(void* p, size_t n) {
    if (!p || n == 0) return;
    volatile uint8_t* v = (volatile uint8_t*)p;
    while (n--) *v++ = 0;
}

static int ct_memcmp(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= a[i] ^ b[i];
    return diff;
}

bool isEncrypted(const Domain& domain, const uint8_t* data, size_t len) {
    if (!data || len < OVERHEAD) return false;
    return memcmp(data, domain.magic, MAGIC_LEN) == 0;
}

// Derive enc_key (32) || mac_key (32) from identity, namespaced by the
// domain's HKDF info string and salted with the identity hash.
static bool deriveKeys(const Domain& domain, const RNS::Identity& identity, uint8_t out64[64]) {
    if (!identity) return false;
    const RNS::Bytes& priv = identity.encryptionPrivateKey();
    if (priv.size() == 0) return false;
    RNS::Bytes salt = identity.hash();
    RNS::Bytes info((const uint8_t*)domain.hkdfInfo, strlen(domain.hkdfInfo));
    RNS::Bytes derived = RNS::Cryptography::hkdf(64, priv, salt, info);
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

bool encrypt(const Domain& domain, const RNS::Identity& identity,
             const uint8_t* plaintext, size_t pt_len,
             RNS::Bytes& out) {
    if (!plaintext && pt_len > 0) return false;

    uint8_t keys[64];
    if (!deriveKeys(domain, identity, keys)) return false;

    RNS::Bytes iv_bytes = RNS::Cryptography::random(IV_LEN);
    if (iv_bytes.size() != IV_LEN) {
        secureZero(keys, sizeof(keys));
        return false;
    }

    size_t envelope_len = HEADER_LEN + pt_len + MAC_LEN;
    uint8_t* buf = out.writable(envelope_len);
    if (!buf) {
        secureZero(keys, sizeof(keys));
        return false;
    }

    size_t off = 0;
    memcpy(buf + off, domain.magic, MAGIC_LEN);       off += MAGIC_LEN;
    buf[off++] = VERSION;
    memcpy(buf + off, iv_bytes.data(), IV_LEN);       off += IV_LEN;

    // Encrypt directly into the envelope's ciphertext region.
    uint8_t* ct = buf + off;
    if (pt_len > 0) {
        CTR<AES256> ctr;
        ctr.setKey(keys, 32);
        ctr.setIV(iv_bytes.data(), IV_LEN);
        ctr.encrypt(ct, plaintext, pt_len);
    }
    off += pt_len;

    RNS::Bytes mac = compute_mac(keys + 32, buf, HEADER_LEN, ct, pt_len);
    if (mac.size() != MAC_LEN) {
        secureZero(keys, sizeof(keys));
        out = RNS::Bytes();
        return false;
    }
    memcpy(buf + off, mac.data(), MAC_LEN);
    out.resize(envelope_len);

    secureZero(keys, sizeof(keys));
    return true;
}

bool encryptString(const Domain& domain, const RNS::Identity& identity,
                    const String& plaintext, String& out) {
    RNS::Bytes envelope;
    if (!encrypt(domain, identity,
                 (const uint8_t*)plaintext.c_str(), plaintext.length(),
                 envelope)) {
        return false;
    }
    // Copy raw envelope bytes into the String. Arduino's String happily
    // carries non-text bytes; downstream storage paths use String::length()
    // and c_str() to write the full buffer.
    out = "";
    out.reserve(envelope.size());
    for (size_t i = 0; i < envelope.size(); i++) {
        out.concat((char)envelope.data()[i]);
    }
    return true;
}

bool decrypt(const Domain& domain, const RNS::Identity& identity,
             const uint8_t* blob, size_t blob_len,
             RNS::Bytes& out_plaintext) {
    if (!blob || blob_len < OVERHEAD) return false;
    if (memcmp(blob, domain.magic, MAGIC_LEN) != 0) return false;

    uint8_t version = blob[MAGIC_LEN];
    if (version != VERSION) return false;

    const uint8_t* iv         = blob + MAGIC_LEN + 1;
    size_t         ct_len     = blob_len - OVERHEAD;
    const uint8_t* ct         = iv + IV_LEN;
    const uint8_t* mac_stored = ct + ct_len;

    uint8_t keys[64];
    if (!deriveKeys(domain, identity, keys)) return false;

    RNS::Bytes mac_expected = compute_mac(keys + 32, blob, HEADER_LEN, ct, ct_len);
    if (mac_expected.size() != MAC_LEN ||
        ct_memcmp(mac_expected.data(), mac_stored, MAC_LEN) != 0) {
        secureZero(keys, sizeof(keys));
        return false;
    }

    uint8_t* pt = out_plaintext.writable(ct_len);
    if (!pt) {
        secureZero(keys, sizeof(keys));
        return false;
    }
    if (ct_len > 0) {
        CTR<AES256> ctr;
        ctr.setKey(keys, 32);
        ctr.setIV(iv, IV_LEN);
        ctr.decrypt(pt, ct, ct_len);
    }
    out_plaintext.resize(ct_len);
    secureZero(keys, sizeof(keys));
    return true;
}

bool decryptOrPassthrough(const Domain& domain, const RNS::Identity& identity,
                           const String& blob, String& out) {
    if (blob.length() == 0) { out = ""; return true; }
    const uint8_t* data = (const uint8_t*)blob.c_str();
    size_t len = blob.length();

    if (!isEncrypted(domain, data, len)) {
        // Legacy plaintext file (pre-encryption migration) — pass through.
        out = blob;
        return true;
    }

    RNS::Bytes plaintext;
    if (!decrypt(domain, identity, data, len, plaintext)) return false;

    // Copy plaintext bytes into String (JSON is always ASCII/UTF-8).
    out = "";
    out.reserve(plaintext.size());
    for (size_t i = 0; i < plaintext.size(); i++) {
        out.concat((char)plaintext.data()[i]);
    }
    return true;
}

bool blindIndexHex(const RNS::Identity& identity, const char* hkdfInfo,
                    const uint8_t* input, size_t input_len,
                    std::string& out_hex, size_t hexChars) {
    if (!identity) return false;
    const RNS::Bytes& priv = identity.encryptionPrivateKey();
    if (priv.size() == 0) return false;

    RNS::Bytes salt = identity.hash();
    RNS::Bytes info((const uint8_t*)hkdfInfo, strlen(hkdfInfo));
    RNS::Bytes key = RNS::Cryptography::hkdf(32, priv, salt, info);
    if (key.size() != 32) return false;

    RNS::Cryptography::HMAC hmac(key);
    hmac.update(RNS::Bytes(input, input_len));
    RNS::Bytes mac = hmac.digest();
    secureZero((void*)key.data(), key.size());
    if (mac.size() == 0) return false;

    static const char* hexDigits = "0123456789abcdef";
    hexChars = std::min(hexChars, mac.size() * 2);
    out_hex.clear();
    out_hex.reserve(hexChars);
    for (size_t i = 0; i < hexChars; i++) {
        uint8_t byte = mac.data()[i / 2];
        uint8_t nibble = (i % 2 == 0) ? (byte >> 4) : (byte & 0x0F);
        out_hex.push_back(hexDigits[nibble]);
    }
    return true;
}

}  // namespace AtRestCrypto
