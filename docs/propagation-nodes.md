# Propagation Nodes (Offline Messaging)

Module: [`src/reticulum/PropagationClient.{h,cpp}`](../src/reticulum/PropagationClient.h),
integration in [`src/reticulum/LXMFManager.cpp`](../src/reticulum/LXMFManager.cpp)
(`tryPropagationFallback()`, `syncPropagationNode()`,
`setPropagationClient()`)

LXMF propagation nodes (PNs) are store-and-forward mail drops: a sender
hands a message to a PN when the recipient isn't directly reachable,
and a recipient later pulls it down from the PN once back online. Before
this feature, this firmware had no PN support — a message either
reached its recipient directly within the retry budget, or it just
failed.

## Scope: client-only, by design

This device discovers PNs, hands off outbound mail to one, and pulls
down its own waiting mail. It never **hosts** other identities' mail or
peers with other PNs (`LXMPeer.py`'s peering/admin protocol). Hosting
needs persistent storage for strangers' mail plus peering logic, which
would compete with this device's own message budget (200 messages
flash-only / 5000 with SD — see [lxmf-messaging.md](lxmf-messaging.md))
on a device with no PSRAM and ~55KB free heap. A client-only role gets
the actual user-facing benefit (offline messaging) without that
storage/complexity trade.

## Discovery: passive, background, no action required

`PropagationClient` is itself an `RNS::AnnounceHandler` registered for
the `lxmf.propagation` aspect (a separate registration from
`AnnounceManager`'s `lxmf.delivery` handler, so PN announces never get
folded into the regular contact list). Every PN announce that's heard
over the radio — whether or not the user has Settings open — gets
decoded (`decodePNAnnounce()`, the 7-element
`[legacy, timebase, is_pn, transfer_limit, sync_limit,
[stamp_cost, stamp_cost_flex, peering_cost], metadata]` shape from the
LXMF spec) and kept in a small, deduplicated, bounded list
(`_nodes`, capped at 8 — PNs are rare enough that eviction of the
stalest entry on overflow should almost never trigger in practice).

Settings > Messaging > Propagation Node opens a picker
(`SettingsScreen::buildPNSelectMenu()`) listing whatever's been
passively discovered so far (name, short hash, stamp cost), plus a
manual hex-entry fallback for a PN that hasn't announced within radio
range yet. Picking one — discovered or typed — sets
`UserConfig.propagationNodeHash` and pushes it live to the running
`LXMFManager` immediately (`SettingsScreen::applyAndSave()`), no reboot
needed.

## Outbound: handing off when direct delivery fails

```
sendDirect(msg)
  │  direct delivery's own retry budget exhausted
  │  (LXMF_DISCOVERY_MAX_ATTEMPTS, ~60s — no path, or no identity yet)
  ▼
tryPropagationFallback(msg)
  │  preferred PN configured? one submission already in flight? → wait
  ▼
PropagationClient::prepareSubmission()
  │  needs the recipient's identity already seen via a prior announce
  │  (RNS::Identity::recall) -- if truly never seen, nothing to encrypt
  │  to, and the message still fails, same as before this feature
  ▼
encrypt [src:16][sig:64][content] to recipient, compute transient_id
  = SHA256(dest_hash + encrypted)   -- this IS the PoW material, not
                                        the regular messageId
  ▼
stamp cost from the PN's own announce (mandatory if > 0, expand_rounds=1000,
see lxmf-stamps.md)
  ▼
proceedSubmission(stamp): link to the PN, send [time, [lxmf_data]] as a
packet (or resource transfer if it doesn't fit one packet)
  ▼
proof received → status = SENT (not DELIVERED -- handing off to a PN
                  isn't the recipient receiving it, matching the Python
                  reference's __mark_propagated)
timeout/link closed → status = FAILED
```

One submission (and one stamp job) is in flight at a time — the same
single-flow design as the existing outbound link (`_outLink`). A second
message that also needs the PN just waits its turn rather than racing
or failing outright.

## Inbound: pulling down mail that arrived while offline

Manual trigger only for now (Settings > Messaging > Sync Now;
auto-sync on a timer is a real `UserConfig.propagationAutoSync` toggle
in the UI today, but nothing currently reads it to schedule a sync —
see "Known gaps" below).

```
LXMFManager::syncPropagationNode()
  ▼
PropagationClient::startSync()
  │  link to the PN, link.identify(our identity)
  ▼
request "/get" [nil, nil]           -- "what transient_ids do you have for me?"
  ▼
handleListResponse()                -- list empty -> done, successful no-op
  ▼
request "/get" [wants=all, haves=[], limitKB from the PN's own announce]
  ▼
handleMessagesResponse()
  │  for each [dest_hash:16][encrypted] blob:
  │    decrypt with our identity, reassemble as
  │    [dest:16][src:16][sig:64][msgpack] -- the exact shape
  │    LXMFManager::processIncoming() already accepts from link delivery
  │  feed straight into that EXISTING pipeline, unchanged --
  │  dedup/storage/unread-count/UI callback all just work
  ▼
fire-and-forget request "/get" [nil, haves=everything just received]
  -- purges delivered mail from the PN so it isn't re-offered next sync
```

### Why there's no persisted "already delivered" set

The Python reference keeps a `locally_delivered_transient_ids` set so a
PN that re-offers a message (e.g. the purge request got lost) doesn't
get re-processed. This firmware deliberately skips persisting that set:
`LXMFManager`'s existing `_seenMessageIds` (capped, in-memory,
deduplicates by the message's real `messageId` — see
[lxmf-messaging.md](lxmf-messaging.md)) already absorbs a same-boot
re-offer harmlessly, and a purge failing immediately followed by a
reboot before the next sync is rare enough, and low-stakes enough (one
possible duplicate message shown, not data loss or corruption), that
adding a second persisted dedup structure purely to close that gap
wasn't worth the storage/complexity cost on a device this constrained.

## Non-goals (explicit)

- No PN hosting, peering, or admin protocol.
- No ratchet-key use in propagation encryption — consistent with direct
  delivery, which already has ratchets disabled here due to an open
  interop bug (see `Destination.cpp`).
- No compression (`SF_COMPRESSION`) — already signaled as unsupported
  in this device's own announce (see [lxmf-stamps.md](lxmf-stamps.md)),
  not changing that for propagation either.
- No ticket fields (`FIELD_TICKET`), no paper/QR delivery.

## Known gaps

- **Auto-sync isn't wired up yet.** `UserConfig.propagationAutoSync`
  exists and is user-editable in Settings, but nothing currently reads
  it to schedule a periodic `syncPropagationNode()` call — manual
  "Sync Now" is the only way to pull mail today. Wiring a timer is
  straightforward future work; the toggle is left in the UI ahead of
  that so the setting survives once it's implemented rather than being
  added alongside it later.
- The default `UserConfig.propagationNodeHash` ships pointed at a known
  test PN for development convenience — see the `TODO` in
  [`UserConfig.h`](../src/config/UserConfig.h). Clear it (Settings >
  Messaging > Propagation Node > Clear) or point it at a PN you trust
  before relying on this for real offline messaging.
