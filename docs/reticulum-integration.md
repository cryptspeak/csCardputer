# Reticulum Integration

Module: [`src/reticulum/ReticulumManager.{h,cpp}`](../src/reticulum/ReticulumManager.h)

This is the layer between the firmware and
[microReticulum](https://github.com/ratspeak/microReticulum) (a fork),
pinned to a specific commit of our own fork of that fork
([cryptspeak/microReticulum](https://github.com/cryptspeak/microReticulum))
in `platformio.ini`'s `lib_deps` — not vendored in this repo.
`ReticulumManager` owns the `RNS::Reticulum`
instance, the `RNS::Identity`, the LXMF delivery `RNS::Destination`, and
the LoRa interface registration. Everything else that needs the
identity (`MessageStore`, `AnnounceManager`, `UserConfig`) gets it from
here — see [firmware-architecture.md](firmware-architecture.md).

## Endpoint, not router

`RNS::Reticulum::transport_enabled(false)` is set in `begin()`
([ReticulumManager.cpp:249](../src/reticulum/ReticulumManager.cpp)). This
device never rebroadcasts other nodes' traffic — it only originates and
receives its own. Two things follow directly from that:

- **No transport-table persistence to SD.** The comment in `begin()`
  says it plainly: "Endpoint node: routing tables are rebuilt from
  announces on each boot." A transport node would need to persist its
  routing table so it can keep forwarding immediately after a reboot;
  an endpoint just waits for fresh announces, so skipping that save
  costs nothing and saves ~12KB of heap and boot time.
- **Table sizes are tuned down.** `RNS::Transport::path_table_maxsize(48)`
  and `announce_table_maxsize(48)` ([ReticulumManager.cpp:252-253](../src/reticulum/ReticulumManager.cpp))
  are far smaller than a transport node would carry. Combined with the
  `RNS_*_MAX` build flags in `platformio.ini` (known destinations 48,
  hashlist 32, receipts 8, queued announces 8, etc.), this is a heap
  budget decision: the 8-bit canvas (see [ui-framework.md](ui-framework.md))
  frees ~32KB specifically so these tables have room.

`RNS::Reticulum::probe_destination_enabled(true)` stays on — replying to
probes is cheap and doesn't require acting as a router.

## The LXMF delivery destination

`begin()` creates one `RNS::Destination`:

```
RNS::Destination(_identity, IN, SINGLE, "lxmf", "delivery")
```

with `PROVE_ALL` proof strategy and `accepts_links(true)`. `PROVE_ALL`
means every packet addressed to this destination gets a proof sent back
automatically (this is what lets `LXMFManager` track delivery — see
[lxmf-messaging.md](lxmf-messaging.md)). `accepts_links(true)` is what
allows the link-based delivery path for messages too large for a single
opportunistic packet.

## Filesystem bridge

microReticulum's `RNS::FileSystemImpl` interface is implemented by
`LittleFSFileSystem` (top of `ReticulumManager.cpp`), which is just a
thin adapter onto the Arduino `LittleFS` API — `FlashStore` has already
mounted it by the time `ReticulumManager::begin()` registers this
filesystem. Every method takes an `FSLock` (a RAII wrapper around
`FlashStore::mutex()`) before touching `LittleFS`, because the
background persist task (below) and the main loop can both be doing
filesystem I/O. This is the same mutex `FlashStore` itself uses, so
microReticulum's own persistence calls (`RNS::Transport::persist_data()`,
`RNS::Identity::persist_data()`) can't race with, say, a message save
or a contacts write happening in `FlashStore` from the main loop.

## Background persist task

LittleFS writes can take 0.5-4+ seconds. Doing that on the main loop
(core 1, same core as UI/radio/keyboard) would freeze the device for the
duration. `startPersistTask()` spins up a FreeRTOS task — pinned to
**core 1**, not core 0 — and `persistData()` hands it work via a
1-deep queue (`xQueueOverwrite`, so a pending request is simply replaced
rather than queuing up duplicates).

Pinning to core 1 instead of core 0 looks backwards at first (core 0
is otherwise idle-ish — WiFi/lwIP lives there), but the comment in
`startPersistTask()` explains why: sharing LittleFS access across cores
corrupts the filesystem. Since the main loop (core 1) already owns all
LittleFS access via `FlashStore`'s mutex, the persist task has to live
on the same core to stay inside that same locking discipline.

`persistData()` rotates between two cycles every call
(`PATH_PERSIST_INTERVAL_MS` = 5 minutes, from `loop()`):

| Cycle | Action |
|---|---|
| 0 | `RNS::Transport::persist_data()` |
| 1 | `RNS::Identity::persist_data()` |

(A third cycle for SD-based transport-table backup was removed — see
the "Endpoint, not router" section above for why an endpoint doesn't
need it.)

## Announce flood defense — layer 1 of 3

Three independent layers exist between "a packet arrives" and "the UI
shows a new node." This doc covers layer 1; layers 2 and 3 live in
[announce-discovery.md](announce-discovery.md).

**Layer 1 — Transport-level packet filter**, installed via
`RNS::Transport::set_filter_packet_callback()` in `begin()`
([ReticulumManager.cpp:258-296](../src/reticulum/ReticulumManager.cpp)).
This runs **before Ed25519 signature verification**, so it's the
cheapest possible place to drop hostile traffic — verifying a forged
announce's signature is wasted CPU if the packet was going to be
dropped anyway. It only inspects `RNS::Type::Packet::ANNOUNCE` packets;
everything else passes through unfiltered. Two independent checks:

1. **Adaptive per-second rate cap.** Tighter during the first 60
   seconds after boot (`maxRate = 2`) when the announce flood from
   nearby nodes replying to discovery is heaviest, looser afterward
   (`RSCARDPUTER_MAX_ANNOUNCES_PER_SEC` = 4), and clamped to 1 if free
   heap drops below 15KB regardless of how long the device has been up.
   This is a hard ceiling on *processing announces at all* — it runs
   ahead of every other layer.
2. **Path-revalidation skip.** Once `RNS::Transport::has_path()` is
   already true for a destination, re-verifying that destination's
   signature on every single announce it sends is wasted work (~100ms
   of Ed25519 per announce on this CPU). A `std::unordered_map` keyed
   by the raw destination-hash bytes (not the hex string — avoids a
   `toHex()` allocation per packet) remembers the last time a known
   path was allowed through; subsequent announces from that same
   destination are dropped for 5 minutes unless something about the
   route actually needs to change. The map is capped at 40 entries and
   cleared wholesale if it grows past that, trading a little
   re-verification work for a hard bound on heap use.

## Identity-at-rest plumbing

`ReticulumManager` is also where the password-protected identity lives
operationally — `probeIdentityState()`, `loadOrCreateIdentity()`,
`saveIdentityToAll()`, `migrateLegacyIdentity()`, and the
`preloadIdentityKey()` fast path used when `main.cpp` has already
verified the password and unwrapped the blob itself. The crypto design
behind these is documented in
[encryption-identity.md](encryption-identity.md) and the exact boot
ordering in [boot-sequence.md](boot-sequence.md) — this doc only covers
the Reticulum-specific side: once `loadOrCreateIdentity()` returns,
`_identity` holds a real `RNS::Identity` with a private key in RAM, and
everything downstream (the LXMF destination, `MessageStore`,
`AnnounceManager`, `UserConfig`) can rely on it being valid.

One detail worth calling out: `saveIdentityToAll()` is also invoked
opportunistically on the **preload path** (`loadOrCreateIdentity()`,
when adopting an already-unwrapped key) — "to heal out-of-sync state."
If, say, the SD card was missing on a previous boot and only flash/NVS
got the encrypted blob written, plugging the SD card back in causes the
next boot to backfill it automatically, with no explicit migration step
required.

## Identity hash display formatting

`identityHash()` and `destinationHashStr()` both produce
human-shortened forms of full hashes for on-screen display (colon- or
double-colon-separated fragments) — purely cosmetic, see
`ReticulumManager.cpp` if the exact grouping ever needs to change. The
full, untruncated forms (`destinationHashHex()`) are what gets handed to
other peers or shown in Settings → About.
