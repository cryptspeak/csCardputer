#pragma once

#include <Arduino.h>

// =============================================================================
// Duress — optional second password that wipes the device instead of
// unlocking it.
// =============================================================================
//
// Threat model: a user is coerced into entering their at-rest password
// (border search, robbery, abuse situation, ...). If they set up a duress
// password ahead of time, entering THAT password at the unlock screen
// instead of the real one triggers FactoryWipe::wipeAll() and reboots the
// device into the normal "fresh device" first-time-setup flow — the same
// screen a brand-new device shows. There is no separate UI state for "you
// just triggered duress" — to anyone watching, it looks like the device
// reset itself.
//
// Storage: a single wrapped verifier blob in NVS (namespace "ratcom_duress"),
// reusing IdentityCrypto's PBKDF2+AES-256-CTR+HMAC envelope ("RID1" format)
// to wrap a fixed, public constant. The duress password is never compared
// directly — checking it means attempting to unwrap this blob, which
// succeeds iff the candidate password was the one used to set it up.
// "Configured" is whether this blob exists; there is intentionally no
// separate plaintext "duress enabled" flag to keep in sync.
//
// The duress password is NEVER used to derive any real key — it has no
// relationship to the identity, messages, contacts, or settings encryption.
// Its only job is "does this string match what was set up", which is also
// why it can't accidentally decrypt anything if guessed.
// =============================================================================

namespace Duress {

// True if a duress password has been set up (verifier blob exists in NVS).
bool isConfigured();

// Wrap `password` into the verifier blob and persist it. Returns false on
// crypto/storage failure. Caller is responsible for policy checks (minimum
// length, must differ from the real at-rest password) before calling this —
// this function does not enforce them.
bool setup(const String& password);

// Attempt to unwrap the stored verifier blob with `candidate`. Returns true
// iff candidate is the configured duress password. Returns false (cheaply)
// if no duress password is configured.
bool check(const String& candidate);

// Remove the verifier blob — duress password becomes unconfigured.
void disable();

}  // namespace Duress
