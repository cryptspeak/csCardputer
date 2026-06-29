#pragma once

#include <Bytes.h>
#include <cstdint>
#include <functional>

// Proof-of-work "stamp" generation/validation for LXMF anti-spam stamps
// (direct delivery) and propagation-node submission stamps. See
// docs/lxmf-stamps.md for the wire format and the feasibility story.
//
// The reference Python implementation (LXMF's LXStamper.py) builds a large
// "workblock" -- 3000 HKDF rounds x 256 bytes = ~750KB for a normal message
// stamp, 1000 rounds = ~250KB for a propagation stamp -- and re-hashes the
// *entire* workblock plus a random 32-byte candidate on every brute-force
// attempt. That's infeasible on this device's heap (~55KB free, no PSRAM).
//
// Instead, Workblock streams the HKDF rounds through one SHA256 instance as
// they're generated (never holding more than one 256-byte chunk at a time),
// then snapshots that hasher's state once, after the whole workblock has
// been fed through it. Each brute-force attempt clones the ~40-byte
// snapshot, hashes only the 32-byte candidate, and checks the result --
// this computes the exact same SHA256(workblock || stamp) the spec and the
// Python reference both validate against, just without materializing or
// re-hashing the workblock per attempt.
namespace LXStamper {

constexpr size_t STAMP_SIZE = 32;                  // RNS::Identity::HASHLENGTH / 8
constexpr int WORKBLOCK_EXPAND_ROUNDS    = 3000;   // normal (direct-delivery) message stamp
constexpr int WORKBLOCK_EXPAND_ROUNDS_PN = 1000;   // propagation-node submission stamp

// A workblock snapshot for one piece of `material` (a message_id or
// transient_id). Build once, then check as many candidate stamps against
// it as needed -- this is what makes brute-forcing affordable here.
class Workblock {
public:
    // Streams `expandRounds` HKDF chunks derived from `material` through a
    // SHA256 instance and snapshots its state. `shouldAbort`, if given, is
    // polled periodically; returns false if it fires (workblock left unready).
    bool build(const RNS::Bytes& material, int expandRounds,
               const std::function<bool()>& shouldAbort = nullptr);

    // Leading-zero-bit count of SHA256(workblock || stamp) -- higher means
    // more proof-of-work spent. `stamp` must be STAMP_SIZE bytes.
    int valueFor(const RNS::Bytes& stamp) const;

    bool ready() const { return _ready; }

private:
    uint32_t _h[8] = {0};
    uint64_t _length = 0;
    bool _ready = false;
};

// Brute-forces a stamp for `material` meeting `targetCost` leading-zero
// bits. Polls `shouldAbort()` periodically and returns an empty Bytes if it
// fires (cooperative cancellation -- intended to be called from a
// background task, see LXMFManager's stamping pipeline). `attemptsOut`, if
// non-null, receives the number of candidates tried (diagnostics only).
RNS::Bytes generateStamp(const RNS::Bytes& material, int targetCost, int expandRounds,
                          const std::function<bool()>& shouldAbort = nullptr,
                          uint32_t* attemptsOut = nullptr);

// Cheap (one workblock build + one hash check) validity test.
bool validateStamp(const RNS::Bytes& material, const RNS::Bytes& stamp,
                    int targetCost, int expandRounds);

}
