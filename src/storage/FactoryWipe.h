#pragma once

class FlashStore;
class SDStore;

// Shared "erase everything" routine — destroys the identity, messages,
// contacts, and settings on every storage tier. Used by both the
// user-initiated Factory Reset (Settings menu) and the duress-password
// wipe trigger (boot password gate) so the two paths can never drift out
// of sync with each other.
//
// Does NOT reboot — callers decide when/whether to call ESP.restart().
namespace FactoryWipe {

void wipeAll(FlashStore* flash, SDStore* sd);

}  // namespace FactoryWipe
