#pragma once

#include "storage/AtRestCrypto.h"

// =============================================================================
// KnownDestEncryption — at-rest encryption for microReticulum's own
// known_destinations file
// =============================================================================
//
// Thin domain wrapper around AtRestCrypto (see AtRestCrypto.h for the shared
// envelope format and threat model). Protects the `known_destinations` file
// that RNS::Identity persists directly to flash (via our microReticulum
// fork's known_destinations_crypto() hook, wired in ReticulumManager) —
// independently of, and in addition to, ContactsEncryption's own
// names.json/per-contact files.
//
// That file exists whether or not a peer was ever explicitly "saved" as a
// contact: it records the last-heard announce app_data for every
// destination we've validated an announce from, which for an LXMF peer is
// their announced display name. You can't have a conversation with a peer
// without having validated at least one of their announces, so this file
// alone is enough to recover "who you've talked to" from a raw storage
// dump if left unencrypted, even for peers that were never added to
// contacts. See threat-model.md.
//
// Distinct magic ("RKD1") and HKDF info string from messages/contacts/
// settings, so a key derived for this domain cannot decrypt those.
//
// Unlike the other domains, this one works on raw RNS::Bytes rather than
// String — the known_destinations format is packed binary (hash lengths,
// timestamps, public keys), not JSON, and can legitimately contain 0x00
// bytes throughout.
// =============================================================================

namespace KnownDestEncryption {

extern const AtRestCrypto::Domain kDomain;

inline bool encrypt(const RNS::Identity& identity, const RNS::Bytes& plaintext, RNS::Bytes& out) {
    return AtRestCrypto::encrypt(kDomain, identity, plaintext.data(), plaintext.size(), out);
}

// Decrypts; if `blob` doesn't carry this domain's magic header (legacy
// plaintext file from before this hook existed), passes it through
// unchanged so the next save transparently upgrades it in place.
inline bool decryptOrPassthrough(const RNS::Identity& identity, const RNS::Bytes& blob, RNS::Bytes& out) {
    if (blob.size() == 0) { out = RNS::Bytes(); return true; }
    if (!AtRestCrypto::isEncrypted(kDomain, blob.data(), blob.size())) {
        out = blob;
        return true;
    }
    return AtRestCrypto::decrypt(kDomain, identity, blob.data(), blob.size(), out);
}

}  // namespace KnownDestEncryption
