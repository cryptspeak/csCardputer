#include "LXMFManager.h"
#include "AnnounceManager.h"
#include "config/Config.h"
#include <Transport.h>
#include <time.h>
#include <algorithm>
#include <cstring>

LXMFManager* LXMFManager::_instance = nullptr;
std::map<std::string, LXMFManager::PendingProof> LXMFManager::_pendingProofs;

namespace {
constexpr unsigned long LXMF_DISCOVERY_RETRY_INTERVAL_MS = 10000;
// How long sendDirect() will hold a message once it notices Transport's
// path table disagrees with the peer's recorded preferred interface (see
// AnnounceManager::preferredInterfaceName), while an interface-scoped
// request_path() correction is in flight. Short on purpose -- this is a
// local re-check ("does this specific interface still have a route to this
// destination"), not a full discovery search, so it shouldn't share
// LXMF_DISCOVERY_RETRY_INTERVAL_MS's much longer multi-hop budget. If the
// correction doesn't land in time, the message goes out on whatever the
// table says rather than blocking indefinitely on a route that may no
// longer exist.
constexpr unsigned long LXMF_INTERFACE_CORRECTION_WINDOW_MS = 3000;
// Immediate attempt + N-1 10s retries. A flat 60s (the old budget of 7) is
// only enough for a peer within direct earshot to answer. Behind a transport
// node -- e.g. a multi-interface LoRa gateway bridging to the internet --
// the request has to make it out over LoRa, get relayed, and the response
// has to make the same trip back, all at LoRa's airtime/duty-cycle pace;
// 60s routinely isn't enough, and giving up that fast either fails the
// message outright or hands it to a propagation node when direct delivery
// would've worked fine given a few more seconds. ~3 minutes still bounds
// the wait, just no longer assumes every peer is one hop away.
constexpr int LXMF_DISCOVERY_MAX_ATTEMPTS = 18;
constexpr unsigned long LXMF_LINK_ESTABLISH_TIMEOUT_MS = 30000;

// LXMF's "version 0.5.0+" announce app_data format: a msgpack array of
// [display_name(bin), stamp_cost(nil|uint), supported_functionality(array)].
// This is the spec-wide convention (matches Python LXMF.stamp_cost_from_app_data),
// not specific to this firmware's own announces -- see main.cpp's encodeAnnounceName
// for the encoder side. Returns 0 if no stamp is required or the format isn't
// recognized (treated the same as "no stamp" -- the conservative default that
// doesn't change behavior for peers we have no information about).
int stampCostFromAppData(const RNS::Bytes& appData) {
    if (appData.size() < 2) return 0;
    const uint8_t* data = appData.data();
    size_t len = appData.size();
    uint8_t b = data[0];
    size_t pos;
    size_t arrLen;
    if ((b & 0xF0) == 0x90) { arrLen = b & 0x0F; pos = 1; }
    else if (b == 0xDC && len >= 3) { arrLen = ((size_t)data[1] << 8) | data[2]; pos = 3; }
    else return 0;
    if (arrLen < 2) return 0;

    // Skip element 0 (display name -- bin/str/nil, any length/format).
    if (pos >= len) return 0;
    uint8_t e0 = data[pos];
    if ((e0 & 0xE0) == 0xA0) { pos += 1 + (e0 & 0x1F); }
    else if (e0 == 0xC4 && pos + 1 < len) { pos += 2 + data[pos + 1]; }
    else if (e0 == 0xC5 && pos + 2 < len) { pos += 3 + (((size_t)data[pos + 1] << 8) | data[pos + 2]); }
    else if (e0 == 0xD9 && pos + 1 < len) { pos += 2 + data[pos + 1]; }
    else if (e0 == 0xDA && pos + 2 < len) { pos += 3 + (((size_t)data[pos + 1] << 8) | data[pos + 2]); }
    else if (e0 == 0xC0) { pos += 1; }
    else return 0;  // unrecognized name encoding -- bail rather than misread element 1

    // Element 1: stamp_cost (nil = none required, else a small uint).
    if (pos >= len) return 0;
    uint8_t e1 = data[pos];
    if (e1 == 0xC0) return 0;                                  // nil
    if (e1 <= 0x7F) return e1;                                 // positive fixint
    if (e1 == 0xCC && pos + 1 < len) return data[pos + 1];     // uint8
    if (e1 == 0xCD && pos + 2 < len) return ((int)data[pos + 1] << 8) | data[pos + 2];  // uint16
    return 0;
}

// Short, UI-facing label for an interface -- "LoRa.915" -> "LoRa",
// "TCP.example.com" -> "TCP". Falls back to the full name if there's no
// dot, "?" if the interface is unknown (e.g. next_hop_interface() couldn't
// resolve a path yet).
std::string shortIfaceTag(const RNS::Interface& iface) {
    if (!iface) return "?";
    std::string name = iface.name();
    size_t dot = name.find('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}
}

bool LXMFManager::begin(ReticulumManager* rns, MessageStore* store) {
    _rns = rns;
    _store = store;
    _instance = this;

    // Register callbacks on the LXMF delivery destination
    RNS::Destination& dest = _rns->destination();
    dest.set_packet_callback(onPacketReceived);
    dest.set_link_established_callback(onLinkEstablished);

    // Pre-compute unread counts at boot — NEVER do this lazily in callbacks
    computeUnreadFromDisk();

    if (_store) {
        for (const auto& id : _store->loadRecentMessageIds(MAX_SEEN_IDS)) {
            rememberMessageId(id);
        }
        std::vector<LXMFMessage> pending = _store->loadPendingOutgoing();
        for (auto& msg : pending) {
            if ((int)_outQueue.size() >= RSCARDPUTER_MAX_OUTQUEUE) break;
            msg.lastRetryMs = 0;
            _outQueue.push_back(msg);
        }
        if (!pending.empty()) {
            Serial.printf("[LXMF] Restored %d pending outgoing messages\n", (int)_outQueue.size());
        }
    }

    Serial.println("[LXMF] Manager started");
    return true;
}

void LXMFManager::rememberMessageId(const std::string& msgIdHex) {
    if (msgIdHex.empty() || _seenMessageIds.count(msgIdHex)) return;
    _seenMessageIds.insert(msgIdHex);
    _seenMessageOrder.push_back(msgIdHex);
    while ((int)_seenMessageOrder.size() > MAX_SEEN_IDS) {
        _seenMessageIds.erase(_seenMessageOrder.front());
        _seenMessageOrder.pop_front();
    }
}

void LXMFManager::loop() {
    unsigned long now = millis();
    pollStampResult();
    if (_propagation) _propagation->loop();
    for (auto it = _pendingProofs.begin(); it != _pendingProofs.end(); ) {
        if ((now - it->second.createdMs) > 65000UL) {
            std::string receiptHash = it->first;
            ++it;
            handleProofTimeoutHash(receiptHash);
        } else {
            ++it;
        }
    }

    // Process incoming messages queued by packet callbacks (safe context — main loop)
    while (!_incomingQueue.empty()) {
        LXMFMessage msg = std::move(_incomingQueue.front());
        _incomingQueue.erase(_incomingQueue.begin());

        Serial.printf("[LXMF] Processing: from %s \"%s\"\n",
                      msg.sourceHash.toHex().substr(0, 8).c_str(),
                      msg.content.c_str());

        // Store to disk (takes mutex — safe here in main loop)
        if (_store) {
            if (!_store->saveMessage(msg)) {
                Serial.println("[LXMF] Incoming message store failed");
                continue;
            }
        }

        // Track unread
        std::string peerHex = msg.sourceHash.toHex();
        _unread[peerHex]++;

        // Notify UI
        if (_onMessage) {
            _onMessage(msg);
        }
    }

    if (_outQueue.empty()) return;
    int processed = 0;

    for (auto it = _outQueue.begin(); it != _outQueue.end(); ) {
        // Time-budgeted: process up to 3 messages within 10ms
        if (processed >= 3 || (processed > 0 && millis() - now >= 10)) break;

        LXMFMessage& msg = *it;

        // sendDirect() is always attempted (this loop already only runs at
        // 10Hz, see main.cpp's lxmf.loop() throttle) -- it does its own
        // internal throttling of the actual network-visible retry actions
        // (request_path()/ensureOutboundLink(), gated on msg.lastRetryMs)
        // so that a path/identity/link that becomes ready between scheduled
        // retries is picked up on the very next 100ms tick instead of
        // sitting unused until the next LXMF_DISCOVERY_RETRY_INTERVAL_MS
        // boundary.
        if (sendDirect(msg)) {
            processed++;
            Serial.printf("[LXMF] Queue drain: status=%s dest=%s\n",
                          msg.statusStr(), msg.destHash.toHex().substr(0, 8).c_str());
            std::string peerHex = msg.destHash.toHex();
            if (_store && msg.savedCounter > 0) {
                _store->updateMessageStatusByCounter(peerHex, msg.savedCounter, false, msg.status);
            } else if (_store) {
                _store->updateMessageStatus(peerHex, msg.timestamp, false, msg.status);
            }
            if (_statusCb) {
                _statusCb(peerHex, msg.timestamp, msg.status);
            }
            it = _outQueue.erase(it);
        } else {
            ++it;
        }
    }
}

bool LXMFManager::sendMessage(const RNS::Bytes& destHash, const std::string& content, const std::string& title) {
    LXMFMessage msg;
    msg.sourceHash = _rns->destination().hash();
    msg.destHash = destHash;
    time_t now = time(nullptr);
    if (now > 1700000000) {
        msg.timestamp = (double)now;
    } else {
        msg.timestamp = millis() / 1000.0;
    }
    msg.content = content;
    msg.title = title;
    msg.incoming = false;
    msg.status = LXMFStatus::QUEUED;

    if ((int)_outQueue.size() >= RSCARDPUTER_MAX_OUTQUEUE) {
        Serial.printf("[LXMF] Outgoing queue full (%d), refusing new message\n",
                      (int)_outQueue.size());
        return false;
    }

    // Validate and generate messageId before saving the queued message.
    std::vector<uint8_t> packed = msg.packFull(_rns->identity());
    if (packed.empty()) {
        Serial.println("[LXMF] Message pack failed");
        return false;
    }

    _outQueue.push_back(msg);

    // Save outgoing message to disk immediately (creates conversation entry)
    if (_store) {
        if (!_store->saveMessage(_outQueue.back())) {
            _outQueue.pop_back();
            return false;
        }
    }

    // Proactively request path so it's ready when sendDirect runs
    if (!RNS::Transport::has_path(destHash)) {
        RNS::Transport::request_path(destHash);
        Serial.printf("[LXMF] Message queued for %s (%d bytes) — requesting path\n",
                      destHash.toHex().substr(0, 8).c_str(), (int)content.size());
    } else {
        Serial.printf("[LXMF] Message queued for %s (%d bytes) — path known\n",
                      destHash.toHex().substr(0, 8).c_str(), (int)content.size());
    }
    return true;
}

bool LXMFManager::ensureOutboundLink(const RNS::Destination& dest, const RNS::Bytes& destHash, const char* reason) {
    if (_outLink && _outLinkDestHash == destHash
        && _outLink.status() == RNS::Type::Link::ACTIVE) {
        return true;
    }

    unsigned long now = millis();
    bool samePending = _outLinkPending && _outLinkPendingHash == destHash;
    bool pendingAlive = samePending && _outLink
        && _outLink.status() != RNS::Type::Link::CLOSED
        && (now - _outLinkPendingSinceMs) < LXMF_LINK_ESTABLISH_TIMEOUT_MS;
    if (pendingAlive) return false;

    _outLinkPendingHash = destHash;
    _outLinkDestHash = destHash;
    _outLinkPending = true;
    _outLinkPendingSinceMs = now;
    Serial.printf("[LXMF] Establishing link to %s for %s\n",
                  destHash.toHex().substr(0, 8).c_str(), reason ? reason : "delivery");
    RNS::Link newLink(dest, onOutLinkEstablished, onOutLinkClosed);
    _outLink = newLink;
    return false;
}

RNS::Interface LXMFManager::findInterfaceByName(const std::string& name) {
    for (auto& [hash, iface] : RNS::Transport::get_interfaces()) {
        if (iface.name() == name) return iface;
    }
    return {RNS::Type::NONE};
}

bool LXMFManager::sendDirect(LXMFMessage& msg) {
    // A direct-delivery stamp grind, or a full propagation-node handoff
    // (stamp + link + send, see tryPropagationFallback()), is already in
    // flight for this message -- both are async and self-resolving
    // (pollStampResult() / onPropagationSubmitDone()), so there's nothing
    // for the normal direct-delivery state machine below to do. Checked
    // first, before identity/path lookups, so it short-circuits every
    // retry path uniformly (PN handoff can be triggered from more than
    // one of them).
    if (msg.status == LXMFStatus::STAMPING) return false;

    // Reset stale link-pending state
    if (_outLinkPending) {
        bool linkReady = _outLink && _outLink.status() == RNS::Type::Link::ACTIVE;
        bool linkClosed = !_outLink || _outLink.status() == RNS::Type::Link::CLOSED;
        bool linkTimedOut = _outLinkPendingSinceMs > 0
            && (millis() - _outLinkPendingSinceMs) >= LXMF_LINK_ESTABLISH_TIMEOUT_MS;
        if (linkReady) {
            _outLinkPending = false;
        } else if (linkClosed || linkTimedOut) {
            _outLinkPending = false;
            _outLink = {RNS::Type::NONE};
            _outLinkPendingSinceMs = 0;
            Serial.println("[LXMF] Clearing stale link-pending");
        }
    }

    RNS::Identity recipientId = RNS::Identity::recall(msg.destHash);
    if (!recipientId) {
        // Re-issuing request_path() is throttled to once per
        // LXMF_DISCOVERY_RETRY_INTERVAL_MS (we don't want to flood the
        // radio every 100ms while waiting) but the !recipientId check
        // itself runs every call, so identity recall succeeding between
        // throttled attempts is picked up on the very next tick.
        unsigned long nowMs = millis();
        if (msg.retries == 0 || (nowMs - msg.lastRetryMs) >= LXMF_DISCOVERY_RETRY_INTERVAL_MS) {
            msg.retries++;
            msg.lastRetryMs = nowMs;
            RNS::Transport::request_path(msg.destHash);
            Serial.printf("[LXMF] Requesting path/identity for %s (attempt %d/%d)\n",
                          msg.destHash.toHex().substr(0, 8).c_str(),
                          msg.retries, LXMF_DISCOVERY_MAX_ATTEMPTS);
            if (msg.retries >= LXMF_DISCOVERY_MAX_ATTEMPTS) {
                Serial.printf("[LXMF] No identity/path for %s after ~%lus — FAILED\n",
                              msg.destHash.toHex().substr(0, 8).c_str(),
                              (unsigned long)LXMF_DISCOVERY_MAX_ATTEMPTS * LXMF_DISCOVERY_RETRY_INTERVAL_MS / 1000UL);
                msg.status = LXMFStatus::FAILED;
                return true;
            }
        }
        return false;
    }

    // Ensure path exists — without a path, Transport::outbound() broadcasts as
    // Header1 which the Python hub silently drops
    if (!RNS::Transport::has_path(msg.destHash)) {
        // Same throttle pattern as the identity check above: has_path() is
        // polled every call (so a path arriving moments after the last
        // request is sent gets used on the next ~100ms tick, not after a
        // full LXMF_DISCOVERY_RETRY_INTERVAL_MS wait), only the actual
        // request_path() transmission is rate-limited.
        unsigned long nowMs = millis();
        if (msg.retries == 0 || (nowMs - msg.lastRetryMs) >= LXMF_DISCOVERY_RETRY_INTERVAL_MS) {
            msg.retries++;
            msg.lastRetryMs = nowMs;
            Serial.printf("[LXMF] No path for %s, requesting (attempt %d/%d)\n",
                          msg.destHash.toHex().substr(0, 8).c_str(),
                          msg.retries, LXMF_DISCOVERY_MAX_ATTEMPTS);
            RNS::Transport::request_path(msg.destHash);
            if (msg.retries >= LXMF_DISCOVERY_MAX_ATTEMPTS) {
                if (tryPropagationFallback(msg)) return false;
                Serial.printf("[LXMF] No path for %s after ~%lus — FAILED\n",
                              msg.destHash.toHex().substr(0, 8).c_str(),
                              (unsigned long)LXMF_DISCOVERY_MAX_ATTEMPTS * LXMF_DISCOVERY_RETRY_INTERVAL_MS / 1000UL);
                msg.status = LXMFStatus::FAILED;
                return true;
            }
        }
        return false;  // keep in queue, retry later
    }

    // Interface stickiness: Transport's path table holds exactly one route
    // per destination (lowest hops wins, ties by announce recency) -- it
    // has no notion of "the interface this peer actually talks to us on",
    // so e.g. a TCP hub also bridging this same LoRa peer's announces can
    // silently steal the route. AnnounceManager remembers which interface
    // we last received a genuine, unrelayed packet from this peer on (see
    // notePeerInterface(), called from onPacketReceived()) and persists it
    // across reboots. If that disagrees with what Transport currently has
    // on file, ask it to recheck specifically on that interface and give
    // the correction a short, bounded window to land before sending on
    // whatever the table says now -- rather than racing a one-shot nudge
    // against the send, or trusting the table unconditionally.
    {
        std::string key = msg.destHash.toHex();
        const DiscoveredNode* node = _announceManager ? _announceManager->findNode(msg.destHash) : nullptr;
        RNS::Interface preferred = (node && !node->preferredInterfaceName.empty())
            ? findInterfaceByName(node->preferredInterfaceName) : RNS::Interface{RNS::Type::NONE};
        RNS::Interface current = RNS::Transport::next_hop_interface(msg.destHash);
        if (preferred && (!current || current.name() != preferred.name())) {
            unsigned long nowMs = millis();
            auto it = _interfaceCorrectionStart.find(key);
            if (it == _interfaceCorrectionStart.end()) {
                _interfaceCorrectionStart[key] = nowMs;
                Serial.printf("[LXMF] Path for %s is via %s, peer's known interface is %s -- requesting recheck\n",
                              msg.destHash.toHex().substr(0, 8).c_str(),
                              current ? current.name().c_str() : "(none)",
                              preferred.name().c_str());
                RNS::Transport::request_path(msg.destHash, preferred);
                while (_interfaceCorrectionStart.size() > 50) {
                    _interfaceCorrectionStart.erase(_interfaceCorrectionStart.begin());
                }
                return false;
            } else if (nowMs - it->second < LXMF_INTERFACE_CORRECTION_WINDOW_MS) {
                return false;  // still waiting on the recheck
            } else {
                Serial.printf("[LXMF] Interface recheck for %s timed out -- sending via %s anyway\n",
                              msg.destHash.toHex().substr(0, 8).c_str(),
                              current ? current.name().c_str() : "(none)");
                _interfaceCorrectionStart.erase(it);
            }
        } else {
            _interfaceCorrectionStart.erase(key);
        }
    }

    // Stamp gating: some recipients require a proof-of-work stamp before
    // they'll accept a message (see LXStamper.h). Looked up fresh from
    // their last announce each time -- mirrors how identity/path lookups
    // already work here (RNS::Identity::recall above), no separate cache.
    if (msg.stamp.size() == 0) {
        int cost = stampCostFromAppData(RNS::Identity::recall_app_data(msg.destHash));
        if (cost > 0) {
            std::string peerHex = msg.destHash.toHex();
            if (cost > _stampCostCeiling && !_stampApprovedOverCeiling.count(peerHex)) {
                msg.status = LXMFStatus::STAMPING;
                if (!_stampAwaitingConfirm.count(peerHex)) {
                    _stampAwaitingConfirm.insert(peerHex);
                    Serial.printf("[LXMF] Stamp cost %d for %s exceeds ceiling %d -- awaiting confirm\n",
                                  cost, peerHex.substr(0, 8).c_str(), _stampCostCeiling);
                    if (_stampConfirmCb) _stampConfirmCb(peerHex, cost);
                }
                return false;
            }
            if (_stampInFlight) return false;  // one stamp job at a time; retry next pass
            beginStamping(msg, cost);
            return false;
        }
    }

    RNS::Destination outDest(
        recipientId,
        RNS::Type::Destination::OUT,
        RNS::Type::Destination::SINGLE,
        "lxmf",
        "delivery"
    );

    // packFull returns opportunistic format: [src:16][sig:64][msgpack]
    std::vector<uint8_t> payload = msg.packFull(_rns->identity());
    if (payload.empty()) {
        Serial.println("[LXMF] Failed to pack message");
        msg.status = LXMFStatus::FAILED;
        return true;
    }

    msg.status = LXMFStatus::SENDING;
    bool sent = false;

    // Try link-based delivery if we have an active link to this peer
    if (_outLink && _outLinkDestHash == msg.destHash
        && _outLink.status() == RNS::Type::Link::ACTIVE) {
        // Link delivery: prepend dest_hash (Python DIRECT format)
        std::vector<uint8_t> linkPayload;
        linkPayload.reserve(16 + payload.size());
        linkPayload.insert(linkPayload.end(), msg.destHash.data(), msg.destHash.data() + 16);
        linkPayload.insert(linkPayload.end(), payload.begin(), payload.end());
        RNS::Bytes linkBytes(linkPayload.data(), linkPayload.size());
        if (linkBytes.size() <= RNS::Type::Reticulum::MDU) {
            // Small enough for single link packet
            Serial.printf("[LXMF] sending via link packet: %d bytes to %s\n",
                          (int)linkBytes.size(), msg.destHash.toHex().substr(0, 8).c_str());
            RNS::Packet packet(_outLink, linkBytes);
            RNS::PacketReceipt receipt = packet.send();
            if (receipt) { sent = true; }
        } else {
            // Too large for single packet — use Resource transfer
            Serial.printf("[LXMF] sending via link resource: %d bytes to %s\n",
                          (int)linkBytes.size(), msg.destHash.toHex().substr(0, 8).c_str());
            if (_outLink.start_resource_transfer(linkBytes)) {
                sent = true;
            } else {
                Serial.println("[LXMF] resource transfer failed to start");
            }
        }
    }

    // Fallback: opportunistic or queue for link-based resource transfer
    if (!sent) {
        RNS::Bytes payloadBytes(payload.data(), payload.size());
        if (payloadBytes.size() <= RNS::Type::Reticulum::MDU) {
            // Small enough for single opportunistic packet
            Serial.printf("[LXMF] sending opportunistic: %d bytes to %s\n",
                          (int)payloadBytes.size(), msg.destHash.toHex().substr(0, 8).c_str());
            RNS::Packet packet(outDest, payloadBytes);
            RNS::PacketReceipt receipt = packet.send();
            if (receipt) { sent = true; registerProofTracking(receipt, msg); }
        } else {
            // Too large for opportunistic — need link + resource transfer.
            // The link-ACTIVE check near the top of this function already
            // runs every call, so once ensureOutboundLink() below succeeds
            // the actual send happens on the very next tick; only the
            // (re-)establish attempt itself is throttled here.
            unsigned long nowMs = millis();
            if (msg.retries == 0 || (nowMs - msg.lastRetryMs) >= LXMF_DISCOVERY_RETRY_INTERVAL_MS) {
                msg.lastRetryMs = nowMs;
                Serial.printf("[LXMF] Message too large for opportunistic (%d bytes > MDU), needs link (retry %d)\n",
                              (int)payloadBytes.size(), msg.retries);
                if (msg.retries % 3 == 0 && (!_outLink || _outLinkDestHash != msg.destHash
                    || _outLink.status() != RNS::Type::Link::ACTIVE)) {
                    ensureOutboundLink(outDest, msg.destHash, "resource transfer");
                }
                msg.retries++;
                if (msg.retries >= 30) {
                    if (tryPropagationFallback(msg)) return false;
                    Serial.printf("[LXMF] Link for %s not established after %d retries — FAILED\n",
                                  msg.destHash.toHex().substr(0, 8).c_str(), msg.retries);
                    msg.status = LXMFStatus::FAILED;
                    return true;
                }
            }
            return false;  // Keep in queue, retry when link is established
        }
    }

    if (sent) {
        msg.status = LXMFStatus::SENT;
        Serial.printf("[LXMF] SENT OK: msgId=%s\n", msg.messageId.toHex().substr(0, 8).c_str());
        if (_routeInfoCb) {
            bool viaLink = _outLink && _outLinkDestHash == msg.destHash
                && _outLink.status() == RNS::Type::Link::ACTIVE;
            RNS::Interface iface = viaLink ? _outLink.attached_interface()
                                            : RNS::Transport::next_hop_interface(msg.destHash);
            _routeInfoCb(msg.destHash.toHex(), shortIfaceTag(iface));
        }
    } else {
        msg.status = LXMFStatus::FAILED;
        Serial.printf("[LXMF] Send FAILED to %s\n",
                      msg.destHash.toHex().substr(0, 8).c_str());
    }

    // Link establishment is now only triggered on-demand when a message
    // is too large for opportunistic delivery (see large-message path above).

    return true;
}

void LXMFManager::onPacketReceived(const RNS::Bytes& data, const RNS::Packet& packet) {
    if (!_instance) return;
    // Non-link delivery: dest_hash is NOT in LXMF payload (it's in the RNS packet header).
    // Reconstruct full format by prepending it, matching Python LXMRouter.delivery_packet().
    const RNS::Bytes& destHash = packet.destination_hash();
    std::vector<uint8_t> fullData;
    fullData.reserve(destHash.size() + data.size());
    fullData.insert(fullData.end(), destHash.data(), destHash.data() + destHash.size());
    fullData.insert(fullData.end(), data.data(), data.data() + data.size());
    _instance->processIncoming(fullData.data(), fullData.size(), destHash);

    // Record which interface this peer actually talks to us on, for the
    // per-peer interface stickiness check in sendDirect(). Transport bumps
    // a packet's hop count by exactly 1 on every receipt (see
    // Transport::inbound()), so hops()==1 is the strongest available signal
    // that this is genuine, unrelayed point-to-point traffic on
    // `packet.receiving_interface()`, as opposed to something relayed in
    // from elsewhere. This just durably remembers that ground truth --
    // sendDirect() is what decides whether/how to act on it, with a bounded
    // wait instead of racing a one-shot path nudge against the reply it's
    // meant to fix.
    if (data.size() >= 16 && packet.hops() == 1 && packet.receiving_interface() && _instance->_announceManager) {
        RNS::Bytes sourceHash(data.data(), 16);
        _instance->_announceManager->notePeerInterface(sourceHash, packet.receiving_interface().name());
    }
}

void LXMFManager::onOutLinkEstablished(RNS::Link& link) {
    if (!_instance) return;
    _instance->_outLink = link;
    _instance->_outLinkDestHash = _instance->_outLinkPendingHash;
    _instance->_outLinkPending = false;
    _instance->_outLinkPendingSinceMs = 0;
    Serial.printf("[LXMF] Outbound link established to %s\n",
                  _instance->_outLinkDestHash.toHex().substr(0, 8).c_str());
}

void LXMFManager::onOutLinkClosed(RNS::Link& link) {
    if (!_instance) return;
    _instance->_outLink = {RNS::Type::NONE};
    _instance->_outLinkPending = false;
    _instance->_outLinkPendingSinceMs = 0;
    Serial.println("[LXMF] Outbound link closed");
}

void LXMFManager::onLinkEstablished(RNS::Link& link) {
    if (!_instance) return;
    Serial.printf("[LXMF-DIAG] onLinkEstablished fired! link_id=%s status=%d\n",
        link.link_id().toHex().substr(0, 16).c_str(), (int)link.status());
    link.set_packet_callback([](const RNS::Bytes& data, const RNS::Packet& packet) {
        if (!_instance) return;
        Serial.printf("[LXMF-DIAG] Link packet received! %d bytes pkt_dest=%s\n",
            (int)data.size(), packet.destination_hash().toHex().substr(0, 16).c_str());
        // Link delivery: data already contains [dest:16][src:16][sig:64][msgpack]
        // Do NOT use packet.destination_hash() — that's the link_id, not the LXMF dest.
        _instance->processIncoming(data.data(), data.size(), RNS::Bytes());
    });
}

void LXMFManager::processIncoming(const uint8_t* data, size_t len, const RNS::Bytes& destHash) {
    // This runs in the packet callback context — MUST be non-blocking.
    // Only unpack and dedup here, defer storage/UI to loop().

    LXMFMessage msg;
    if (!LXMFMessage::unpackFull(data, len, msg)) {
        Serial.println("[LXMF] Failed to unpack message");
        return;
    }

    // Drop self-messages (loopback from Transport)
    if (_rns && msg.sourceHash == _rns->destination().hash()) {
        return;
    }

    // Deduplication
    std::string msgIdHex = msg.messageId.toHex();
    if (_seenMessageIds.count(msgIdHex)) {
        return;
    }
    rememberMessageId(msgIdHex);

    if (destHash.size() > 0) {
        msg.destHash = destHash;
    }

    // Use local receive time for incoming messages so all timestamps in
    // the conversation reflect THIS device's clock, not the sender's.
    // Prevents confusing display when a peer's clock is wrong/unsynced.
    {
        time_t now = time(nullptr);
        if (now > 1700000000) {
            msg.timestamp = (double)now;
        }
    }

    // Queue for processing in loop() — no I/O, no mutex, no callbacks here
    if ((int)_incomingQueue.size() < MAX_INCOMING_QUEUE) {
        _incomingQueue.push_back(msg);
        Serial.printf("[LXMF] Queued incoming from %s\n",
                      msg.sourceHash.toHex().substr(0, 8).c_str());
    } else {
        Serial.println("[LXMF] Incoming queue full, dropping message");
    }
}

const std::vector<std::string>& LXMFManager::conversations() const {
    if (_store) return _store->conversations();
    static std::vector<std::string> empty;
    return empty;
}

std::vector<LXMFMessage> LXMFManager::getMessages(const std::string& peerHex, int limit) const {
    if (_store) return _store->loadConversation(peerHex, limit);
    return {};
}

int LXMFManager::unreadCount(const std::string& peerHex) const {
    // Never do lazy disk I/O here — this is called from packet callbacks.
    // Unread counts are pre-computed at boot and updated incrementally.
    if (peerHex.empty()) {
        int total = 0;
        for (auto& kv : _unread) total += kv.second;
        return total;
    }
    auto it = _unread.find(peerHex);
    return (it != _unread.end()) ? it->second : 0;
}

void LXMFManager::computeUnreadFromDisk() {
    _unreadComputed = true;
    if (!_store) return;

    // Use efficient file-based unread counting (no JSON parsing needed)
    for (auto& conv : _store->conversations()) {
        int count = _store->unreadCountForPeer(conv);
        if (count > 0) _unread[conv] = count;
    }

    int totalUnread = 0;
    for (auto& kv : _unread) totalUnread += kv.second;
    if (totalUnread > 0) {
        Serial.printf("[LXMF] Restored %d unread messages\n", totalUnread);
    }
}

void LXMFManager::markRead(const std::string& peerHex) {
    _unread[peerHex] = 0;
    if (_store) {
        _store->markConversationRead(peerHex);
    }
}

void LXMFManager::deleteConversation(const std::string& peerHex) {
    for (auto it = _outQueue.begin(); it != _outQueue.end(); ) {
        if (it->destHash.toHex() == peerHex) it = _outQueue.erase(it);
        else ++it;
    }
    for (auto it = _pendingProofs.begin(); it != _pendingProofs.end(); ) {
        if (it->second.peerHex == peerHex) it = _pendingProofs.erase(it);
        else ++it;
    }
    if (_store) {
        _store->deleteConversation(peerHex);
    }
    _unread.erase(peerHex);
    Serial.printf("[LXMF] Conversation deleted: %s\n", peerHex.substr(0, 8).c_str());
}

void LXMFManager::registerProofTracking(RNS::PacketReceipt& receipt, const LXMFMessage& msg) {
    if (!receipt) return;

    std::string receiptHash = receipt.hash().toHex();
    PendingProof pending;
    pending.peerHex = msg.destHash.toHex();
    pending.timestamp = msg.timestamp;
    pending.savedCounter = msg.savedCounter;
    pending.createdMs = millis();
    pending.proofAttempts = (uint8_t)std::min(msg.retries, 255);
    pending.msg = msg;
    pending.msg.status = LXMFStatus::SENT;
    _pendingProofs[receiptHash] = pending;

    receipt.set_delivery_callback(&LXMFManager::onProofDelivered);
    receipt.set_timeout(60);
    receipt.set_timeout_callback(&LXMFManager::onProofTimeout);
}

void LXMFManager::onProofDelivered(const RNS::PacketReceipt& receipt) {
    if (!_instance) return;
    std::string receiptHash = receipt.hash().toHex();
    auto it = _pendingProofs.find(receiptHash);
    if (it == _pendingProofs.end()) return;

    PendingProof pending = it->second;
    _pendingProofs.erase(it);

    if (_instance->_store) {
        if (pending.savedCounter > 0) {
            _instance->_store->updateMessageStatusByCounter(
                pending.peerHex, pending.savedCounter, false, LXMFStatus::DELIVERED);
        } else {
            _instance->_store->updateMessageStatus(
                pending.peerHex, pending.timestamp, false, LXMFStatus::DELIVERED);
        }
    }
    if (_instance->_statusCb) {
        _instance->_statusCb(pending.peerHex, pending.timestamp, LXMFStatus::DELIVERED);
    }
    Serial.printf("[LXMF] DELIVERED proof for %s @ %.0f\n",
                  pending.peerHex.substr(0, 12).c_str(), pending.timestamp);
}

void LXMFManager::onProofTimeout(const RNS::PacketReceipt& receipt) {
    handleProofTimeoutHash(receipt.hash().toHex());
}

void LXMFManager::handleProofTimeoutHash(const std::string& receiptHash) {
    if (!_instance) return;
    auto it = _pendingProofs.find(receiptHash);
    if (it == _pendingProofs.end()) return;

    PendingProof pending = it->second;
    _pendingProofs.erase(it);

    pending.proofAttempts++;
    bool retry = pending.proofAttempts < 3 && (int)_instance->_outQueue.size() < RSCARDPUTER_MAX_OUTQUEUE;
    LXMFStatus nextStatus = retry ? LXMFStatus::QUEUED : LXMFStatus::FAILED;

    if (_instance->_store) {
        if (pending.savedCounter > 0) {
            _instance->_store->updateMessageStatusByCounter(
                pending.peerHex, pending.savedCounter, false, nextStatus);
        } else {
            _instance->_store->updateMessageStatus(
                pending.peerHex, pending.timestamp, false, nextStatus);
        }
    }
    if (_instance->_statusCb) {
        _instance->_statusCb(pending.peerHex, pending.timestamp, nextStatus);
    }

    if (retry) {
        pending.msg.status = LXMFStatus::QUEUED;
        pending.msg.retries = pending.proofAttempts;
        pending.msg.lastRetryMs = 0;
        _instance->_outQueue.push_back(pending.msg);
        Serial.printf("[LXMF] Proof timeout; requeued %s attempt %u/3\n",
                      pending.peerHex.substr(0, 8).c_str(), (unsigned)pending.proofAttempts + 1);
    } else {
        Serial.printf("[LXMF] Proof timeout; marking FAILED for %s\n",
                      pending.peerHex.substr(0, 8).c_str());
    }
}

// ── Anti-spam stamps ─────────────────────────────────────────────────────

void LXMFManager::startStampTaskIfNeeded() {
    if (_stampTask) return;
    _stampRequestQueue = xQueueCreate(1, sizeof(StampReq));
    _stampResultQueue = xQueueCreate(1, sizeof(StampRes));
    // Core 0, not core 1: unlike the persist task (ReticulumManager), this
    // does no filesystem or radio I/O, so there's no shared-mutex reason to
    // stay on the UI/radio core -- core 0 is the comparatively idle one
    // (see network-interfaces.md), so this genuinely runs in parallel.
    xTaskCreatePinnedToCore(stampTaskFunc, "lxmf_stamp", 4096, this, 1, &_stampTask, 0);
}

void LXMFManager::stampTaskFunc(void* param) {
    LXMFManager* self = (LXMFManager*)param;
    for (;;) {
        StampReq req;
        if (xQueueReceive(self->_stampRequestQueue, &req, portMAX_DELAY) != pdTRUE) continue;

        RNS::Bytes material(req.material, sizeof(req.material));
        uint32_t attempts = 0;
        RNS::Bytes stamp = LXStamper::generateStamp(material, req.targetCost, req.expandRounds,
                                                     nullptr, &attempts);

        StampRes res;
        memcpy(res.material, req.material, sizeof(res.material));
        if (stamp.size() == LXStamper::STAMP_SIZE) {
            memcpy(res.stamp, stamp.data(), sizeof(res.stamp));
            res.ok = 1;
        } else {
            res.ok = 0;
        }
        Serial.printf("[LXMF] Stamp %s after %u attempts\n", res.ok ? "found" : "FAILED", (unsigned)attempts);
        xQueueOverwrite(self->_stampResultQueue, &res);
    }
}

void LXMFManager::beginStamping(LXMFMessage& msg, int targetCost) {
    startStampTaskIfNeeded();

    // Dry-run pack (stamp still empty) just to derive messageId -- the hash
    // it's computed from excludes any stamp by construction (see
    // LXMFMessage::packFull), so this is safe to call again, identically,
    // once the real stamp is attached and ready to actually send.
    msg.packFull(_rns->identity());
    if (msg.messageId.size() != 32) {
        Serial.println("[LXMF] Stamp: could not derive messageId, marking FAILED");
        msg.status = LXMFStatus::FAILED;
        return;
    }

    StampReq req;
    memcpy(req.material, msg.messageId.data(), sizeof(req.material));
    req.targetCost = targetCost;
    req.expandRounds = LXStamper::WORKBLOCK_EXPAND_ROUNDS;

    msg.status = LXMFStatus::STAMPING;
    _stampPurpose = StampPurpose::DIRECT;
    _stampInFlight = true;
    Serial.printf("[LXMF] Stamping started for %s, cost=%d\n",
                  msg.destHash.toHex().substr(0, 8).c_str(), targetCost);
    xQueueOverwrite(_stampRequestQueue, &req);
}

void LXMFManager::beginPropagationStamping(const RNS::Bytes& transientId, int targetCost) {
    startStampTaskIfNeeded();

    StampReq req;
    memcpy(req.material, transientId.data(), sizeof(req.material));
    req.targetCost = targetCost;
    req.expandRounds = LXStamper::WORKBLOCK_EXPAND_ROUNDS_PN;

    _stampPurpose = StampPurpose::PROPAGATION;
    _stampInFlight = true;
    Serial.printf("[LXMF] Propagation stamp started, cost=%d\n", targetCost);
    xQueueOverwrite(_stampRequestQueue, &req);
}

void LXMFManager::setPropagationClient(PropagationClient* pc) {
    _propagation = pc;
    if (_propagation) {
        _propagation->setSubmitDoneCallback([this](const RNS::Bytes& recipientHash, bool delivered) {
            onPropagationSubmitDone(recipientHash, delivered);
        });
    }
}

bool LXMFManager::tryPropagationFallback(LXMFMessage& msg) {
    if (!_propagation || _propagationNodeHash.size() != 16 || !_rns) return false;
    // One PN flow (and one stamp job) in flight at a time, same "single
    // flow" design as direct delivery's _outLink -- a second message
    // needing the PN just waits its turn rather than failing outright.
    if (_propagation->submitInFlight() || _stampInFlight) return true;

    RNS::Bytes transientId;
    int stampCost = 0;
    auto outcome = _propagation->prepareSubmission(_propagationNodeHash, msg, _rns->identity(),
                                                     transientId, stampCost);
    if (outcome != PropagationClient::SubmitOutcome::STARTED) return false;

    // Same stamp-cost gating as direct delivery (sendDirect() above) -- a PN
    // can advertise an arbitrarily high stamp_cost in its announce, and
    // unlike direct delivery this path had no ceiling check at all. Brute-
    // forcing even a handful of bits past the ceiling can take far longer
    // than is practical on this CPU; without this check that grind would
    // occupy the single stamp-task/PN-flow slot indefinitely, permanently
    // blocking every other queued message (PN or direct) behind it and
    // eventually filling _outQueue with messages that can never send.
    std::string peerHex = msg.destHash.toHex();
    if (stampCost > _stampCostCeiling && !_stampApprovedOverCeiling.count(peerHex)) {
        _propagation->abortSubmission();
        msg.status = LXMFStatus::STAMPING;
        if (!_stampAwaitingConfirm.count(peerHex)) {
            _stampAwaitingConfirm.insert(peerHex);
            Serial.printf("[LXMF] PN stamp cost %d for %s exceeds ceiling %d -- awaiting confirm\n",
                          stampCost, peerHex.substr(0, 8).c_str(), _stampCostCeiling);
            if (_stampConfirmCb) _stampConfirmCb(peerHex, stampCost);
        }
        return true;  // keep queued -- confirmStamping() decides FAILED vs proceed
    }

    msg.status = LXMFStatus::STAMPING;
    Serial.printf("[LXMF] No direct route to %s -- handing off to propagation node (stamp cost=%d)\n",
                  msg.destHash.toHex().substr(0, 8).c_str(), stampCost);
    if (stampCost > 0) {
        beginPropagationStamping(transientId, stampCost);
    } else {
        _propagation->proceedSubmission(RNS::Bytes());
    }
    return true;
}

void LXMFManager::onPropagationSubmitDone(const RNS::Bytes& recipientHash, bool delivered) {
    for (auto it = _outQueue.begin(); it != _outQueue.end(); ++it) {
        if (it->destHash != recipientHash || it->status != LXMFStatus::STAMPING) continue;
        std::string peerHex = recipientHash.toHex();
        LXMFMessage msg = *it;
        // Handing off to a PN means "sent," not "delivered" -- the
        // recipient still has to pull it down themselves. Matches the
        // distinction already drawn for direct delivery (SENT vs DELIVERED).
        msg.status = delivered ? LXMFStatus::SENT : LXMFStatus::FAILED;
        Serial.printf("[LXMF] PN handoff for %s: %s\n",
                      peerHex.substr(0, 8).c_str(), delivered ? "SENT" : "FAILED");
        if (delivered && _routeInfoCb) {
            std::string tag = "PN/" + shortIfaceTag(RNS::Transport::next_hop_interface(_propagationNodeHash));
            _routeInfoCb(peerHex, tag);
        }
        if (_store) {
            if (msg.savedCounter > 0) _store->updateMessageStatusByCounter(peerHex, msg.savedCounter, false, msg.status);
            else _store->updateMessageStatus(peerHex, msg.timestamp, false, msg.status);
        }
        if (_statusCb) _statusCb(peerHex, msg.timestamp, msg.status);
        _outQueue.erase(it);
        break;
    }
}

bool LXMFManager::syncPropagationNode() {
    if (!_propagation || _propagationNodeHash.size() != 16 || !_rns) return false;
    return _propagation->startSync(_propagationNodeHash, _rns->identity(),
        [this](const uint8_t* data, size_t len) { onPropagationMessageDecoded(data, len); },
        [this](bool ok, int count) {
            Serial.printf("[LXMF] PN sync finished: ok=%d, %d message(s)\n", ok, count);
            _lastSyncAtMs = millis();
            _lastSyncOk = ok;
            _lastSyncMsgCount = count;
        });
}

bool LXMFManager::propagationSyncInFlight() const {
    return _propagation && _propagation->syncInFlight();
}

void LXMFManager::onPropagationMessageDecoded(const uint8_t* data, size_t len) {
    // Already the full [dest:16][src:16][sig:64][msgpack] shape (see
    // PropagationClient's DecodedMessageCallback contract) -- same as
    // link delivery, so no destHash override needed.
    processIncoming(data, len, RNS::Bytes());
}

void LXMFManager::pollStampResult() {
    if (!_stampInFlight || !_stampResultQueue) return;

    StampRes res;
    if (xQueueReceive(_stampResultQueue, &res, 0) != pdTRUE) return;
    _stampInFlight = false;

    if (_stampPurpose == StampPurpose::PROPAGATION) {
        if (_propagation) {
            if (res.ok) {
                _propagation->proceedSubmission(RNS::Bytes(res.stamp, sizeof(res.stamp)));
            } else {
                Serial.println("[LXMF] Propagation stamp FAILED");
                _propagation->abortSubmission();
            }
        }
        return;
    }

    for (auto it = _outQueue.begin(); it != _outQueue.end(); ++it) {
        LXMFMessage& msg = *it;
        if (msg.status != LXMFStatus::STAMPING || msg.messageId.size() != 32) continue;
        if (memcmp(msg.messageId.data(), res.material, 32) != 0) continue;

        if (res.ok) {
            msg.stamp = RNS::Bytes(res.stamp, sizeof(res.stamp));
            msg.status = LXMFStatus::QUEUED;
            msg.lastRetryMs = 0;  // retry immediately, not after the discovery backoff
        } else {
            std::string peerHex = msg.destHash.toHex();
            if (_store) _store->updateMessageStatus(peerHex, msg.timestamp, false, LXMFStatus::FAILED);
            if (_statusCb) _statusCb(peerHex, msg.timestamp, LXMFStatus::FAILED);
            // Erase rather than leaving status=FAILED in place -- every other
            // give-up path in sendDirect() pairs FAILED with `return true`,
            // which is what makes the loop() drain erase the entry. Left in
            // place, this would re-enter sendDirect() next pass (status is no
            // longer STAMPING) and re-trigger stamp gating from scratch,
            // forever occupying a slot in _outQueue.
            _outQueue.erase(it);
        }
        break;
    }
}

void LXMFManager::confirmStamping(const std::string& peerHex, bool proceed) {
    if (!_stampAwaitingConfirm.count(peerHex)) return;
    _stampAwaitingConfirm.erase(peerHex);

    if (proceed) {
        _stampApprovedOverCeiling.insert(peerHex);
        // sendDirect() bails immediately on any STAMPING-status message
        // (see its top-of-function guard) -- that's correct while a grind
        // or PN handoff is actually in flight, but the messages parked here
        // were only marked STAMPING to *hold* them during the confirm
        // prompt, with nothing in flight yet. Reset to QUEUED so the next
        // loop() pass actually re-enters stamp gating, sees this peer now
        // in _stampApprovedOverCeiling, and proceeds (beginStamping() or
        // tryPropagationFallback()) -- without this they'd sit in _outQueue
        // forever, occupying a slot and never sending.
        for (auto& msg : _outQueue) {
            if (msg.destHash.toHex() != peerHex || msg.status != LXMFStatus::STAMPING) continue;
            msg.status = LXMFStatus::QUEUED;
            msg.lastRetryMs = 0;
        }
        return;
    }

    Serial.printf("[LXMF] Stamp declined by user for %s -- failing queued messages\n",
                  peerHex.substr(0, 8).c_str());
    // Erase rather than leaving status=FAILED in place -- see pollStampResult()
    // for why a FAILED entry left in _outQueue becomes a permanent zombie that
    // re-triggers stamp gating (and this same confirm prompt) every pass.
    for (auto it = _outQueue.begin(); it != _outQueue.end(); ) {
        if (it->destHash.toHex() != peerHex || it->status != LXMFStatus::STAMPING) { ++it; continue; }
        if (_store) _store->updateMessageStatus(peerHex, it->timestamp, false, LXMFStatus::FAILED);
        if (_statusCb) _statusCb(peerHex, it->timestamp, LXMFStatus::FAILED);
        it = _outQueue.erase(it);
    }
}
