# LXMF Anti-Spam Stamps

Modules: [`src/reticulum/LXStamper.{h,cpp}`](../src/reticulum/LXStamper.h),
[`LXMFMessage.{h,cpp}`](https://github.com/0x00001312/microReticulum/blob/master/src/LXMFMessage.h)
(in the pinned [microReticulum fork](https://github.com/0x00001312/microReticulum),
not this repo — see `platformio.ini`'s `lib_deps`),
[`src/reticulum/LXMFManager.cpp`](../src/reticulum/LXMFManager.cpp) (stamp
gating in `sendDirect()`, the background stamp task, `confirmStamping()`)

LXMF's anti-spam stamp is a proof-of-work value a sender attaches to a
message when the recipient demands one. Before this feature, this
firmware had no stamp support at all: any peer that required a stamp
(optional but increasingly common for direct delivery in clients like
Sideband and NomadNet) silently dropped this device's messages.

## Wire format

`LXMFMessage`'s packed content is `[timestamp, title, content, fields]`
as a 4-element msgpack array (`packContent()`,
[LXMFMessage.cpp:92](https://github.com/0x00001312/microReticulum/blob/master/src/LXMFMessage.cpp)).
`fields` is always emitted as an empty fixmap (`0x80`) — real per-field
support (e.g. `FIELD_TICKET`) wasn't needed for stamps and stays a
non-goal for now.

When a stamp is attached, the array grows to 5 elements:
`[timestamp, title, content, fields, stamp]`
(`appendStampToPacked()`). The stamp is **not** part of the
hash/signature input — it can't be, since the stamp is itself a
function of the message's hash. `canonicalHashInput()` reconstructs the
4-element form for signature verification regardless of whether a 5th
element is present on the wire, so stamped and unstamped messages from
the same sender verify identically.

## The feasibility problem, and how it's solved here

The reference Python implementation (`LXMF`'s `LXStamper.py`) builds a
"workblock" — `WORKBLOCK_EXPAND_ROUNDS` (3000) HKDF rounds × 256 bytes,
i.e. ~750KB — then re-hashes the *entire* workblock plus a random
32-byte candidate on **every** brute-force attempt. That's flatly
impossible on this device's heap (~55KB free, no PSRAM).

`LXStamper::Workblock` instead streams the HKDF rounds through one
`SHA256` instance as they're generated, 256 bytes at a time, never
holding more than one chunk. Once the whole workblock has been fed
through, it snapshots that hasher's internal state (`h[8]` + `length`,
~40 bytes — `lib/Crypto/SHA256.h`'s state is plain POD with no
pointers, which is what makes a cheap snapshot possible at all). Every
brute-force attempt afterward just clones that snapshot, hashes the
32-byte candidate stamp against it, and checks the leading-zero-bit
count (`valueFor()`). This computes the exact same
`SHA256(workblock || stamp)` the spec and the Python reference both
validate against — not a protocol shortcut, just skipping the
re-materialization of 750KB per attempt.

`generateStamp()` brute-forces candidates until `targetCost` is met,
polling an optional `shouldAbort` callback periodically for cooperative
cancellation. `validateStamp()` is the cheap inverse (one workblock
build + one hash check), used to sanity-check our own output before
sending.

## Background generation, never blocking the UI/radio loop

Stamp generation runs on its own FreeRTOS task
(`LXMFManager::startStampTaskIfNeeded()`,
[LXMFManager.cpp:680](../src/reticulum/LXMFManager.cpp)), pinned to core
0 — unlike `ReticulumManager`'s persist task, stamping does no
filesystem or radio I/O, so there's no shared-mutex reason to keep it
on the UI/radio core, and core 0 is the comparatively idle one (see
[network-interfaces.md](network-interfaces.md)). Request/result are
each a depth-1 queue (`_stampRequestQueue`/`_stampResultQueue`,
`xQueueOverwrite`) — only one stamp job is ever in flight, matching
this device's single-flow design elsewhere (`_outLink`).

```
sendDirect(msg)
  │  msg.stamp.size() == 0 and peer's announce decodes a stamp_cost > 0
  ▼
cost <= ceiling?
  ├─ yes → beginStamping(msg, cost)   status = STAMPING
  │           │  background task generates the stamp
  │           ▼  (polled from loop() via pollStampResult())
  │        ok  → msg.stamp set, status = QUEUED, retried immediately
  │        fail→ status = FAILED
  └─ no  → status = STAMPING, fire stampConfirmCallback once per peer,
            wait for confirmStamping(peerHex, proceed)
              proceed  → peer added to _stampApprovedOverCeiling,
                          next sendDirect() pass proceeds as above
              decline  → all STAMPING messages to that peer → FAILED
```

## Cost discovery and the ceiling

A peer's required stamp cost rides in their LXMF announce's `app_data`
as a 3-element array: `[display_name, stamp_cost, supported_functionality]`
(`encodeAnnounceName()`, [main.cpp:403](../src/main.cpp); decoded by
`stampCostFromAppData()`, [LXMFManager.cpp:23](../src/reticulum/LXMFManager.cpp)).
This device always announces `stamp_cost = nil` (no stamp required to
reach it) and an empty `supported_functionality` (no `SF_COMPRESSION`
support) — only the *decode* side is new; nothing about how others can
reach this device changed.

`UserConfig.stampCostCeiling` (default 12, clamped 1–24 in
`sanitizeSettings()`) is the line between "just generate it" and "ask
first." A cost at or below the ceiling is generated automatically in
the background with no prompt; above it, `StampConfirmOverlay` asks
once per peer (`_stampAwaitingConfirm` de-dupes repeat prompts for
messages still queued to the same peer) with a rough time estimate
(`roughEstimate()` in `StampConfirmOverlay.cpp` — order-of-magnitude
framing, not a measured number, since actual grind time depends on how
unlucky the brute force gets). The ceiling can be changed any time in
Settings > Messaging; `SettingsScreen::applyAndSave()` pushes it to the
live `LXMFManager` immediately, not just on next boot.

## Why this device doesn't *require* stamps from others (yet)

`validateStamp()` is fully implemented and unit-cross-checked against
the Python reference, but nothing currently calls it on the receive
path — incoming messages are accepted regardless of whether they carry
a stamp. Always announcing `stamp_cost = nil` keeps that consistent: a
sender has no reason to attach a stamp this device won't check. Turning
on enforcement (announcing a real cost, then rejecting unstamped/
underpriced incoming messages) is possible future work using the
validation path that already exists, not a wire-format change.
