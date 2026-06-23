#pragma once

#include "storage/AtRestCrypto.h"

// =============================================================================
// SettingsEncryption — at-rest encryption for device settings (UserConfig)
// =============================================================================
//
// Thin domain wrapper around AtRestCrypto (see AtRestCrypto.h for the shared
// envelope format and threat model). Protects the user.json blob on SD and
// flash, and the NVS backup copy — including WiFi AP/STA passwords, TCP
// hub endpoints, LoRa radio parameters, and the display name.
//
// Distinct magic ("RUC1") and HKDF info string from messages/contacts, so a
// key derived for this domain cannot decrypt those, even though all three
// derive from the same identity private key.
// =============================================================================

namespace SettingsEncryption {

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

}  // namespace SettingsEncryption
