# LXMF Messaging

Modules: [`src/reticulum/LXMFManager.{h,cpp}`](../src/reticulum/LXMFManager.h),
[`src/storage/MessageStore.{h,cpp}`](../src/storage/MessageStore.h)

`LXMFManager` is the send/receive state machine; `MessageStore` is
where messages live on disk. `LXMFManager` never touches a file
directly — it always goes through `MessageStore`, which applies at-rest
encryption (see [encryption-messages.md](encryption-messages.md)) and
file-format details.

## Outgoing flow

```
sendMessage(destHash, content)
   │  pack with msg.packFull(identity) — opportunistic wire format:
   │  [src:16][sig:64][msgpack-encoded fields]
   ▼
_outQueue.push_back(msg)            status = QUEUED
MessageStore::saveMessage()         persisted immediately (survives reboot)
RNS::Transport::request_path()      if no path known yet
   │
   ▼  (drained from loop(), time-budgeted: max 3 messages per 10ms)
sendDirect(msg)
   │
   ├─ no identity for destHash yet  → request_path(), retry (≤7 attempts, ~60s) → FAILED
   ├─ no path yet                   → request_path(), retry (≤7 attempts, ~60s) → FAILED
   │
   ├─ payload fits in one packet (≤ Reticulum MDU)
   │    ├─ active link to this dest → send as a link packet
   │    └─ else                     → send opportunistically (no link needed)
   │                                   delivery proof requested (PROVE_ALL on our
   │                                   destination makes the *peer* prove receipt
   │                                   back to *us* the same way)
   │
   └─ payload too large for one packet
        → on-demand link establishment (only triggered here, not eagerly)
        → once link is ACTIVE, send via link resource transfer
        → retry budget: 30 attempts before FAILED
```

A message is only ever in one of `QUEUED → SENDING → SENT/FAILED`, with
`SENT` further upgraded to `DELIVERED` asynchronously if a proof comes
back (see below). The queue is capped at `RSCARDPUTER_MAX_OUTQUEUE` (20)
— `sendMessage()` refuses new messages past that rather than growing
unbounded.

### Why opportunistic-first, not link-first

Establishing a Reticulum link costs a round trip before anything can be
sent. Most LXMF messages are small enough to fit in a single packet, so
`sendDirect()` always tries the opportunistic path first and only pays
for a link when a message is too big for one packet
(`payload.size() > RNS::Type::Reticulum::MDU`). This keeps the common
case (a short text message) to the minimum number of radio
transmissions, which matters most on LoRa where every packet costs
airtime (see [network-interfaces.md](network-interfaces.md)).

The one outbound link kept by `LXMFManager` (`_outLink`) is reused
across messages to the same destination as long as it stays `ACTIVE`;
a link to a different destination replaces it. There's no link pool —
this device only ever maintains one outbound link at a time, which
matches its role as an endpoint talking to one peer at a time rather
than a multi-peer relay.

### Delivery proofs and retries

`registerProofTracking()` attaches a delivery callback and a 60-second
timeout to the `RNS::PacketReceipt` returned by an opportunistic send.
On delivery, the message's stored status flips from `SENT` to
`DELIVERED` and the UI status callback fires. On timeout,
`handleProofTimeoutHash()` requeues the message (status back to
`QUEUED`) up to 2 more times (3 attempts total) before giving up and
marking it `FAILED`. Pending proofs are also swept in `loop()` every
iteration for anything older than 65 seconds, as a backstop in case the
library's own timeout callback never fires.

## Incoming flow

```
onPacketReceived() / onLinkEstablished()'s packet callback
   │  runs in the packet-callback context — MUST be non-blocking
   │  (no disk I/O, no delays, no heavy computation here)
   ▼
processIncoming()
   ├─ unpack (LXMFMessage::unpackFull)
   ├─ drop if sourceHash == our own destination (loopback from Transport)
   ├─ drop if messageId already seen (last 100 IDs, _seenMessageIds)
   └─ push to _incomingQueue (capped at 8; queue full → drop with a log line)
   ▼
   (drained from loop(), which DOES run in normal main-loop context)
loop()
   ├─ MessageStore::saveMessage()   — disk I/O happens here, not in the callback
   ├─ _unread[peerHex]++
   └─ fire the registered MessageCallback → UI updates
```

Non-link delivery and link delivery hand `processIncoming()` differently
shaped buffers — `onPacketReceived()` has to **reconstruct** the
LXMF wire format by prepending the destination hash (which non-link
packets carry in the Reticulum packet header, not the LXMF payload)
before unpacking, while link delivery already has `[dest:16][src:16]
[sig:64][msgpack]` in one buffer. Getting this wrong is the single
easiest way to break incoming messages silently, since `unpackFull`
will simply fail to parse a misaligned buffer — see
`LXMFManager::onPacketReceived()` and `onLinkEstablished()`'s inline
packet callback for the two cases side by side.

Incoming-message timestamps are overwritten with this device's own
receive time (`time(nullptr)`, only if NTP/GPS has produced a
plausible epoch — see the `> 1700000000` sanity checks throughout this
code) rather than trusting the sender's clock, "so all timestamps in
the conversation reflect THIS device's clock" and an unsynced peer
doesn't produce a confusing out-of-order conversation view.

## Unread counts

Unread counts are computed **once, at boot**
(`computeUnreadFromDisk()`), from a per-conversation `.read_ctr` file
(see `MessageStore::unreadCountForPeer`) — never lazily, and never from
inside a packet callback. The comment in the header is explicit about
why: `unreadCount()` can be called from contexts where a filesystem
read would be unsafe or too slow. After boot, counts are maintained
incrementally (`_unread[peerHex]++` on receive, reset in `markRead()`).

## MessageStore: on-disk layout

Each conversation lives in its own directory, named by the peer's
16-hex-character truncated destination hash:
`/messages/<peerHex16>/` (flash) and `/ratcom/messages/<peerHex16>/`
(SD, when present). Within that directory, each message is one file:

```
<13-digit zero-padded receive counter>_<i|o>.json
```

`i`/`o` mark incoming vs. outgoing. The counter is a single
monotonically-increasing value shared across **all** conversations
(`_nextReceiveCounter`), not per-conversation — this is what makes
`loadRecentMessageIds()` and `loadPendingOutgoing()` able to recover a
global chronological order across conversations just by sorting
filenames, with no separate index file. The counter is persisted to
NVS (`msgctr`) and only rebuilt by scanning every file on disk if NVS
has no value yet (first boot, or recovering from an NVS wipe) —
day-to-day boots skip that scan entirely.

A separate hidden `.read_ctr` file per conversation stores "the highest
receive counter the user has seen" — comparing a message's own counter
against it is how `loadConversation()` decides each message's `read`
flag, with no JSON parsing required just to compute unread state.

### Capacity and the flash/SD split

```
messageLimit() = RSCARDPUTER_MSG_LIMIT_SD (5000)    if SD card present
               = RSCARDPUTER_MSG_LIMIT_FLASH (200)   flash-only
```

This is a **global** cap across every conversation, not per-peer —
`MessageStore::saveMessage()` refuses new messages outright once
`_totalMessageCount` reaches the limit (`isFull()`), and
`isNearFull()`/`remainingCapacity()` exist so the UI can warn at 90%
(`RSCARDPUTER_MSG_WARN_PCT`) before that happens. The flash-only number
is deliberately tight: flash is an 1.88MB partition shared with
everything else the firmware stores, so 200 small JSON files is a
realistic ceiling, not a round number picked for convenience.

When SD is present, flash is kept as a bounded **cache**, not a second
copy of everything: `enforceFlashCache()` trims each conversation's
flash-side files down to `FLASH_MSG_CACHE_LIMIT` (20) by deleting the
oldest filenames once a conversation grows past that, on a 30-second
timer from `saveMessage()`. SD is authoritative once present; flash
only needs to hold enough recent history to be useful if the SD card
is briefly removed.

### One-time migrations

Two migrations run exactly once, gated by a single NVS flag
(`ratcom`/`fs_migrated`) so they're skipped on every subsequent boot:

1. **Flash → SD backfill** (`migrateFlashToSD()`) — if an SD card is
   present and this device previously ran flash-only, existing
   messages are copied onto the SD card (and the flash side trimmed
   down to the cache limit) the first time SD becomes available.
2. **Old filename format → counter format** (`migrateOldFilenames()`)
   — an earlier on-disk format used a different (non-zero-padded,
   non-counter) filename scheme; this renames any leftover old-format
   files into the current `<counter>_<i|o>.json` scheme, assigning
   fresh sequential counters in their original chronological order
   (sorted by the *old* filename's numeric prefix first).

Both migrations are pure file operations independent of at-rest
encryption — `migratePlaintextMessages()` (the opt-in encryption
migration described in
[boot-sequence.md](boot-sequence.md#why-contactssettings-migration-is-unconditional-but-message-migration-isnt))
is a separate, explicit step that only runs when the user opts in
during the legacy-identity migration screen.
