# Announce Handling & Node Discovery

Module: [`src/reticulum/AnnounceManager.{h,cpp}`](../src/reticulum/AnnounceManager.h)

`AnnounceManager` is a `RNS::AnnounceHandler` registered with an aspect
filter of `"lxmf.delivery"` (set in `main.cpp`) — so it only ever sees
announces from other LXMF delivery destinations, not every aspect a
peer might announce (`lxmf.propagation`, `nomadnetwork.node`, etc. are
filtered out before `received_announce()` is even called). This is
what builds the Nodes screen's peer list and the contacts/name-cache
data covered in
[encryption-contacts-settings.md](encryption-contacts-settings.md).

## Announce flood defense — layers 2 and 3

[reticulum-integration.md](reticulum-integration.md#announce-flood-defense--layer-1-of-3)
covers layer 1 (the Transport-level filter, before signature
verification). Two more layers run inside `received_announce()`
itself, after the packet has already been verified:

- **Layer 2 — global rate limit** (`MAX_GLOBAL_ANNOUNCES_PER_SEC` = 8):
  a simple per-second counter, reset every 1000ms, that caps how many
  *verified* announces get application-level processing at all,
  regardless of source.
- **Layer 3 — per-node rate limit** (`ANNOUNCE_MIN_INTERVAL_MS` = 200):
  once a destination is already a known node, repeat announces from
  that same destination within 200ms of the last one are dropped. A
  chatty peer re-announcing can't dominate processing time at the
  expense of every other node being tracked.

There's also a heap-pressure escape valve independent of all three
layers: if free heap drops below 25KB, `received_announce()` calls
`evictStale(300000)` (a 5-minute TTL instead of the normal 1 hour)
before doing anything else, to claw back memory from the node table
under pressure rather than letting an announce burst push the device
toward an allocation failure.

## Extracting a display name from `app_data`

Announce `app_data` is an arbitrary byte blob — by convention LXMF
clients encode it as MessagePack, but not every implementation agrees
on the exact shape. `extractMsgPackName()` is a small, hand-rolled
MsgPack scanner (not a full decoder — there's no MsgPack library
dependency for this) that handles exactly the shapes seen in practice:

1. **fixarray / array16** — the expected case, this firmware's own
   `encodeAnnounceName()` in `main.cpp` always emits a 3-element
   fixarray (`[display_name, stamp_cost, supported_functionality]`,
   see the comment there). The scanner does **two passes** over the
   array: first for the first non-empty `str` element (the display
   name, if MsgPack-encoded as a string type), then — only if that
   pass found nothing — for the first non-empty `bin` element (some
   clients, noted as "NomadNet compat" in the source, encode the name
   as binary instead of string).
2. **Bare str/bin** — not an array at all, just a single MsgPack string
   or binary value with no wrapping structure.
3. **Neither** — `received_announce()` falls back to treating the raw
   `app_data` bytes as text directly, accepting them only if every
   byte is printable ASCII, tab, or a well-formed multi-byte UTF-8
   sequence (handles Rust LXMF implementations that announce a raw
   UTF-8 name with no MsgPack envelope at all). If even that fails,
   the bytes are logged as hex and no name is extracted — the node
   still gets tracked, just with its hash as a fallback display name.

`mpSkipValue()` is the generic "skip one MsgPack value, return the new
read position" helper both scanning passes use to step over array
elements they're not interested in (ints, nils, floats, nested
maps/arrays) without needing a full decode.

Whatever name comes out of this — from any of the three paths — is run
through `sanitizeName()`: an allowlist of ASCII letters/digits/`-_.'/`
plus space, with valid multi-byte UTF-8 sequences passed through
untouched (so accented names and emoji survive) and anything else
(control characters, malformed UTF-8) silently stripped. This runs
**before** the name is ever written to a contact file or the name
cache, so a hostile or buggy peer can't smuggle arbitrary bytes into
either.

## The node table

`_nodes` is a flat `std::vector<DiscoveredNode>` with a parallel
`std::unordered_map<std::string, int>` (`_hashIndex`) mapping raw hash
bytes to a vector index, giving O(1) lookup by destination hash instead
of a linear scan on every announce. Capacity is capped at
`RSCARDPUTER_MAX_NODES` (50, shared with the contacts cap mentioned in
[encryption-contacts-settings.md](encryption-contacts-settings.md)).

When the table is full and a genuinely new node announces, eviction
prefers the **worst non-contact**: among nodes not marked `saved`, the
one with the most hops (furthest/least useful) and, as a tiebreak, the
oldest `lastSeen`. Saved contacts are never evicted by this path — only
`unsaveNode()` or the first-boot data wipe removes a saved contact.
Eviction itself is an erase-via-swap-with-last-element followed by
`rebuildIndex()` rather than a `vector::erase()` in the middle, avoiding
an O(n) shift for what's already an O(n) scan to find the eviction
candidate.

Each node tracks `rssi`/`snr` (pulled from the LoRa interface's last
receive, if the announce arrived over LoRa) and `hops` (from
`RNS::Transport::hops_to()`) — but **only for saved contacts**; the
comment at the end of `received_announce()`'s new-node path notes this
explicitly ("Skip identityHex for non-contacts — saves ~64 bytes per
node"). Tracking every transient, never-saved node at full detail would
be the more expensive default; this firmware inverts that and only
pays the extra bytes for nodes the user actually cared enough about to
save.

## Name cache vs. contacts — two different lifetimes

These are easy to conflate but serve different purposes:

- **Contacts** (`saveContact()`/`loadContacts()`) — only nodes the user
  explicitly saved (`node.saved == true`). One JSON file per contact,
  keyed by a 16-hex-character filename prefix of the destination hash.
  This is what the Nodes screen's "saved" list and Messages screen's
  contact lookups are built from.
- **Name cache** (`saveNameCache()`/`loadNameCache()`) — every name
  ever seen from *any* announce, saved or not, capped at
  `MAX_NAME_CACHE` (60) with oldest-first eviction
  (`std::map::begin()` on a map keyed by hex hash — insertion order
  isn't preserved by `std::map`, so this eviction is closer to
  "arbitrary over-capacity entry" than strict LRU; harmless for a
  cosmetic display-name cache, but worth knowing if it's ever repurposed
  for something order-sensitive). This exists purely so names survive
  a reboot for nodes you haven't saved as contacts yet —
  `lookupName()` checks live `_nodes` first, falls back to this cache,
  then falls back to a hash-prefix match for truncated hashes (the
  16-character conversation-ID form used elsewhere in the UI).

Both are written through the same `maybeEncryptContact()` /
`maybeDecryptContact()` helpers as actual contact files — see
[encryption-contacts-settings.md](encryption-contacts-settings.md) for
the at-rest format, which is identical for both.

## Deferred saves

Neither contacts nor the name cache write to disk synchronously from
`received_announce()`. Both set a dirty flag (`_contactsDirty` /
`_nameCacheDirty`) and get flushed from `AnnounceManager::loop()` on
their own timers — `CONTACT_SAVE_INTERVAL_MS` (30s) and
`NAME_CACHE_SAVE_INTERVAL_MS` (5s) respectively. This batches what
could otherwise be a disk write per incoming announce during a busy
discovery burst down to at most one write per interval, regardless of
how many announces arrived in between. See
[storage-layer.md](storage-layer.md#writequeue) for why this is a
different (simpler, synchronous-when-it-fires) pattern than
`MessageStore`'s `WriteQueue` — contacts and the name cache are both
small, capped, infrequent writes, so they don't need a background task,
just rate-limiting.
