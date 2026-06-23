#pragma once

#include "storage/AtRestCrypto.h"

// =============================================================================
// ContactsEncryption — at-rest encryption for saved contacts + name cache
// =============================================================================
//
// Thin domain wrapper around AtRestCrypto (see AtRestCrypto.h for the shared
// envelope format and threat model). Protects:
//   - Per-contact JSON files under /contacts and /ratcom/contacts
//   - The hash→display-name cache (names.json)
//
// Distinct magic ("RCN1") and HKDF info string from messages/settings, so a
// key derived for this domain cannot decrypt those, even though all three
// derive from the same identity private key.
//
// Note: this only protects locally-stored contact metadata (the name you
// gave a peer, and the fact that you saved them). It does NOT change
// anything about announces on the air — those remain public by the
// Reticulum protocol's design, same as before.
// =============================================================================

namespace ContactsEncryption {

extern const AtRestCrypto::Domain kDomain;

inline bool isEncrypted(const uint8_t* data, size_t len) {
    return AtRestCrypto::isEncrypted(kDomain, data, len);
}
inline bool isEncrypted(const String& s) {
    return AtRestCrypto::isEncrypted(kDomain, s);
}

inline bool encryptString(const RNS::Identity& identity, const String& plaintext, String& out) {
    return AtRestCrypto::encryptString(kDomain, identity, plaintext, out);
}

inline bool decryptOrPassthrough(const RNS::Identity& identity, const String& blob, String& out) {
    return AtRestCrypto::decryptOrPassthrough(kDomain, identity, blob, out);
}

}  // namespace ContactsEncryption
