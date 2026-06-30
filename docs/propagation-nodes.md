# Propagation Nodes (removed)

This firmware briefly had a client implementation of LXMF propagation
nodes (PNs) — the store-and-forward "offline messaging" mechanism where a
sender hands a message to a PN when the recipient isn't directly
reachable, and the recipient later pulls it down once back online.

It's been removed. In practice it turned out to be a source of crashes
and routing instability, and chasing it down kept colliding with the
parts of this firmware that actually need to be rock solid (delivery,
routing, the radio link). Rather than keep carrying it half-working,
it's been dropped entirely so effort can stay on stability and security.

Direct LXMF delivery (the normal send/receive path, anti-spam stamps,
routing, retries) is unaffected — see [lxmf-messaging.md](lxmf-messaging.md)
and [lxmf-stamps.md](lxmf-stamps.md). A message either reaches its
recipient directly, or it fails — there is no fallback hop through a PN.

PN support may come back later, properly isolated and tested, but it's
not a near-term priority.
