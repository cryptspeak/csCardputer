#include "Duress.h"
#include "reticulum/IdentityCrypto.h"

#include <Bytes.h>
#include <Preferences.h>

namespace Duress {

namespace {
constexpr const char* NVS_NAMESPACE = "ratcom_duress";
constexpr const char* NVS_KEY       = "v";

// Fixed, public plaintext wrapped by the duress password. It carries no
// secret of its own — IdentityCrypto::unwrap() only returns true if the MAC
// (computed from the password-derived key) verifies, so a successful
// unwrap is already proof the candidate password is correct. The payload
// just needs to be deterministic.
const uint8_t kVerifierPayload[] = "rscardputer-duress-verifier-v1";
}  // namespace

bool isConfigured() {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, true)) return false;
    size_t len = prefs.getBytesLength(NVS_KEY);
    prefs.end();
    return len > 0;
}

bool setup(const String& password) {
    if (password.length() == 0) return false;

    RNS::Bytes payload(kVerifierPayload, sizeof(kVerifierPayload) - 1);
    RNS::Bytes wrapped;
    if (!IdentityCrypto::wrap(password, payload, wrapped)) return false;
    if (wrapped.size() == 0) return false;

    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, false)) return false;
    size_t written = prefs.putBytes(NVS_KEY, wrapped.data(), wrapped.size());
    prefs.end();
    return written == wrapped.size();
}

bool check(const String& candidate) {
    if (candidate.length() == 0) return false;

    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, true)) return false;
    size_t len = prefs.getBytesLength(NVS_KEY);
    if (len == 0 || len > 256) {
        prefs.end();
        return false;
    }
    uint8_t buf[256];
    prefs.getBytes(NVS_KEY, buf, len);
    prefs.end();

    RNS::Bytes blob(buf, len);
    RNS::Bytes plain;
    return IdentityCrypto::unwrap(candidate, blob, plain);
}

void disable() {
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE, false)) {
        prefs.clear();
        prefs.end();
    }
}

}  // namespace Duress
