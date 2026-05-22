#include "LXMFManager.h"
#include "config/Config.h"
#include <Transport.h>
#include <time.h>
#include <algorithm>

LXMFManager* LXMFManager::_instance = nullptr;
std::map<std::string, LXMFManager::PendingProof> LXMFManager::_pendingProofs;

namespace {
constexpr unsigned long LXMF_DISCOVERY_RETRY_INTERVAL_MS = 10000;
constexpr int LXMF_DISCOVERY_MAX_ATTEMPTS = 7;  // immediate attempt + six 10s retries ~= 60s
constexpr unsigned long LXMF_LINK_ESTABLISH_TIMEOUT_MS = 30000;
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
            if ((int)_outQueue.size() >= RATCOM_MAX_OUTQUEUE) break;
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

        if (msg.retries > 0 && (millis() - msg.lastRetryMs) < LXMF_DISCOVERY_RETRY_INTERVAL_MS) {
            ++it;
            continue;
        }

        msg.lastRetryMs = millis();

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

    if ((int)_outQueue.size() >= RATCOM_MAX_OUTQUEUE) {
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

bool LXMFManager::sendDirect(LXMFMessage& msg) {
    Serial.printf("[LXMF] sendDirect: dest=%s link=%s pending=%s\n",
        msg.destHash.toHex().substr(0, 12).c_str(),
        _outLink ? (_outLink.status() == RNS::Type::Link::ACTIVE ? "ACTIVE" : "INACTIVE") : "NONE",
        _outLinkPending ? "yes" : "no");

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
        msg.retries++;
        RNS::Transport::request_path(msg.destHash);
        Serial.printf("[LXMF] Requesting path/identity for %s (attempt %d/%d)\n",
                      msg.destHash.toHex().substr(0, 8).c_str(),
                      msg.retries, LXMF_DISCOVERY_MAX_ATTEMPTS);
        if (msg.retries >= LXMF_DISCOVERY_MAX_ATTEMPTS) {
            Serial.printf("[LXMF] No identity/path for %s after ~60s — FAILED\n",
                          msg.destHash.toHex().substr(0, 8).c_str());
            msg.status = LXMFStatus::FAILED;
            return true;
        }
        return false;
    }

    Serial.printf("[LXMF] recall OK: identity for %s\n",
                  msg.destHash.toHex().substr(0, 8).c_str());

    // Ensure path exists — without a path, Transport::outbound() broadcasts as
    // Header1 which the Python hub silently drops
    if (!RNS::Transport::has_path(msg.destHash)) {
        msg.retries++;
        Serial.printf("[LXMF] No path for %s, requesting (attempt %d/%d)\n",
                      msg.destHash.toHex().substr(0, 8).c_str(),
                      msg.retries, LXMF_DISCOVERY_MAX_ATTEMPTS);
        RNS::Transport::request_path(msg.destHash);
        if (msg.retries >= LXMF_DISCOVERY_MAX_ATTEMPTS) {
            Serial.printf("[LXMF] No path for %s after ~60s — FAILED\n",
                          msg.destHash.toHex().substr(0, 8).c_str());
            msg.status = LXMFStatus::FAILED;
            return true;
        }
        return false;  // keep in queue, retry later
    }

    Serial.printf("[LXMF] path OK: %s hops=%d\n",
                  msg.destHash.toHex().substr(0, 8).c_str(),
                  RNS::Transport::hops_to(msg.destHash));

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
            // Too large for opportunistic — need link + resource transfer
            Serial.printf("[LXMF] Message too large for opportunistic (%d bytes > MDU), needs link (retry %d)\n",
                          (int)payloadBytes.size(), msg.retries);
            if (msg.retries % 3 == 0 && (!_outLink || _outLinkDestHash != msg.destHash
                || _outLink.status() != RNS::Type::Link::ACTIVE)) {
                ensureOutboundLink(outDest, msg.destHash, "resource transfer");
            }
            msg.retries++;
            if (msg.retries >= 30) {
                Serial.printf("[LXMF] Link for %s not established after %d retries — FAILED\n",
                              msg.destHash.toHex().substr(0, 8).c_str(), msg.retries);
                msg.status = LXMFStatus::FAILED;
                return true;
            }
            return false;  // Keep in queue, retry when link is established
        }
    }

    if (sent) {
        msg.status = LXMFStatus::SENT;
        Serial.printf("[LXMF] SENT OK: msgId=%s\n", msg.messageId.toHex().substr(0, 8).c_str());
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
    bool retry = pending.proofAttempts < 3 && (int)_instance->_outQueue.size() < RATCOM_MAX_OUTQUEUE;
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
