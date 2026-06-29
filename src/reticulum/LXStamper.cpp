#include "LXStamper.h"

#include <Identity.h>
#include <Cryptography/HKDF.h>
#include <Cryptography/Random.h>
#include <SHA256.h>

#if defined(ESP_PLATFORM)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#define LXSTAMPER_YIELD() taskYIELD()
#else
// Native build (host-side test harness only) -- no FreeRTOS scheduler to yield to.
#define LXSTAMPER_YIELD() ((void)0)
#endif

namespace {

// SHA256's internal state is a plain POD struct (h[8], w[16] scratch,
// length, chunkSize) with no heap pointers -- see lib/Crypto/SHA256.h.
// Every workblock round feeds exactly 256 bytes (4 full 64-byte blocks)
// through the hasher, so chunkSize is always back to 0 between rounds and
// at the snapshot point: only h[] and length need to be captured to resume
// hashing correctly afterward. `state` is `protected`, so a thin subclass
// is the simplest way to reach it without patching the vendored library.
class SnapshotableSHA256 : public SHA256 {
public:
    void exportState(uint32_t outH[8], uint64_t& outLength) const {
        for (int i = 0; i < 8; i++) outH[i] = state.h[i];
        outLength = state.length;
    }
    void importState(const uint32_t inH[8], uint64_t inLength) {
        for (int i = 0; i < 8; i++) state.h[i] = inH[i];
        state.length = inLength;
        state.chunkSize = 0;
    }
};

// msgpack-encodes a small non-negative int -- positive fixint / uint8 /
// uint16, matching umsgpack.packb()'s smallest-format-that-fits choice
// exactly (sufficient for round indices up to WORKBLOCK_EXPAND_ROUNDS,
// 3000). Picking the wrong format here silently desyncs the workblock from
// Python's for every round index past the boundary -- this must match
// byte-for-byte, not just be "a valid msgpack uint".
void appendMsgpackUint(RNS::Bytes& buf, uint32_t n) {
    if (n < 128) {
        uint8_t b = (uint8_t)n;
        buf.append(&b, 1);
    } else if (n < 256) {
        uint8_t b[2] = {0xCC, (uint8_t)n};
        buf.append(b, 2);
    } else {
        uint8_t b[3] = {0xCD, (uint8_t)((n >> 8) & 0xFF), (uint8_t)(n & 0xFF)};
        buf.append(b, 3);
    }
}

int countLeadingZeroBits(const uint8_t* hash, size_t len) {
    int value = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = hash[i];
        if (b == 0) { value += 8; continue; }
        for (int bit = 7; bit >= 0; bit--) {
            if (b & (1 << bit)) return value;
            value++;
        }
        return value;
    }
    return value;
}

} // namespace

bool LXStamper::Workblock::build(const RNS::Bytes& material, int expandRounds,
                                  const std::function<bool()>& shouldAbort) {
    _ready = false;
    SnapshotableSHA256 hasher;

    for (int n = 0; n < expandRounds; n++) {
        if (shouldAbort && (n & 0x3F) == 0 && shouldAbort()) return false;

        RNS::Bytes saltMaterial(material.data(), material.size());
        appendMsgpackUint(saltMaterial, (uint32_t)n);
        RNS::Bytes salt = RNS::Identity::full_hash(saltMaterial);
        RNS::Bytes chunk = RNS::Cryptography::hkdf(256, material, salt);

        hasher.update(chunk.data(), chunk.size());

        if ((n & 0x1F) == 0) LXSTAMPER_YIELD();
    }

    hasher.exportState(_h, _length);
    _ready = true;
    return true;
}

int LXStamper::Workblock::valueFor(const RNS::Bytes& stamp) const {
    if (!_ready || stamp.size() < STAMP_SIZE) return 0;

    SnapshotableSHA256 hasher;
    hasher.importState(_h, _length);
    hasher.update(stamp.data(), STAMP_SIZE);

    uint8_t hash[32];
    hasher.finalize(hash, sizeof(hash));
    return countLeadingZeroBits(hash, sizeof(hash));
}

RNS::Bytes LXStamper::generateStamp(const RNS::Bytes& material, int targetCost, int expandRounds,
                                     const std::function<bool()>& shouldAbort,
                                     uint32_t* attemptsOut) {
    Workblock wb;
    if (!wb.build(material, expandRounds, shouldAbort)) {
        if (attemptsOut) *attemptsOut = 0;
        return {};
    }

    uint32_t attempts = 0;
    while (true) {
        if (shouldAbort && (attempts & 0xFF) == 0 && shouldAbort()) break;

        RNS::Bytes candidate = RNS::Cryptography::random(STAMP_SIZE);
        attempts++;

        if (wb.valueFor(candidate) >= targetCost) {
            if (attemptsOut) *attemptsOut = attempts;
            return candidate;
        }

        if ((attempts & 0x3FF) == 0) LXSTAMPER_YIELD();
    }

    if (attemptsOut) *attemptsOut = attempts;
    return {};
}

bool LXStamper::validateStamp(const RNS::Bytes& material, const RNS::Bytes& stamp,
                               int targetCost, int expandRounds) {
    if (stamp.size() < STAMP_SIZE) return false;

    Workblock wb;
    if (!wb.build(material, expandRounds, nullptr)) return false;
    return wb.valueFor(stamp) >= targetCost;
}
