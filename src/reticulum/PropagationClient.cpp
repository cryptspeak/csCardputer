#include "PropagationClient.h"
#include "LXStamper.h"
#include <Transport.h>
#include <time.h>
#include <cstring>
#include <algorithm>

namespace {
constexpr unsigned long PN_LINK_TIMEOUT_MS = 30000;
// Discovering the PN's identity/path (DISCOVERING states below) gets the
// same overall budget and retry cadence as LXMFManager's own direct-
// delivery discovery loop (LXMF_DISCOVERY_RETRY_INTERVAL_MS / ~60s total) --
// no reason for a PN to be held to a stricter standard than a normal peer.
constexpr unsigned long PN_DISCOVERY_TIMEOUT_MS = 60000;
constexpr unsigned long PN_DISCOVERY_RETRY_INTERVAL_MS = 10000;
constexpr double PN_REQUEST_TIMEOUT_S = 15.0;
// Our own cap on how much a PN should hand back in one "/get" transfer
// response (LXMRouter.py's `client_transfer_limit`, sent as the request's
// 3rd array element) -- this is the REQUESTER's own preference, not the
// PN's announced sync_limit (that's a separate, unrelated field describing
// what the PN is willing to accept from incoming syncs). Kept small given
// this device's ~55KB free heap / no PSRAM.
constexpr int PN_CLIENT_TRANSFER_LIMIT_KB = 32;

// msgpack-packs [time.time(), [lxmf_data]] -- the PROPAGATED wire envelope
// (LXMessage.pack()'s propagation_packed, see LXMessage.py:433). Small,
// fixed shape: doesn't need a general-purpose encoder.
std::vector<uint8_t> packPropagationEnvelope(const RNS::Bytes& lxmfData) {
    std::vector<uint8_t> buf;
    buf.push_back(0x92);  // fixarray(2)
    buf.push_back(0xCB);  // float64
    double now = time(nullptr) > 1700000000 ? (double)time(nullptr) : millis() / 1000.0;
    uint64_t bits; memcpy(&bits, &now, 8);
    for (int i = 7; i >= 0; i--) buf.push_back((bits >> (i * 8)) & 0xFF);
    buf.push_back(0x91);  // fixarray(1) -- just our one message
    size_t n = lxmfData.size();
    if (n < 256) { buf.push_back(0xC4); buf.push_back((uint8_t)n); }
    else { buf.push_back(0xC5); buf.push_back((n >> 8) & 0xFF); buf.push_back(n & 0xFF); }
    buf.insert(buf.end(), lxmfData.data(), lxmfData.data() + n);
    return buf;
}

// Builds the "/get" list-request payload: msgpack [nil, nil] (asks for the
// transient_id list) or [wants(array of bin), haves(array of bin), limitKB]
// -- see LXMPeer.py's message_get_request / LXMRouter.py:1426-1549.
RNS::Bytes packListRequest() {
    uint8_t buf[3] = {0x92, 0xC0, 0xC0};  // fixarray(2) [nil, nil]
    return RNS::Bytes(buf, sizeof(buf));
}

void appendBinArray(std::vector<uint8_t>& buf, const std::vector<RNS::Bytes>& items) {
    size_t n = items.size();
    if (n < 16) buf.push_back(0x90 | (uint8_t)n);
    else { buf.push_back(0xDC); buf.push_back((n >> 8) & 0xFF); buf.push_back(n & 0xFF); }
    for (auto& item : items) {
        size_t len = item.size();
        if (len < 256) { buf.push_back(0xC4); buf.push_back((uint8_t)len); }
        else { buf.push_back(0xC5); buf.push_back((len >> 8) & 0xFF); buf.push_back(len & 0xFF); }
        buf.insert(buf.end(), item.data(), item.data() + len);
    }
}

RNS::Bytes packTransferRequest(const std::vector<RNS::Bytes>& wants, const std::vector<RNS::Bytes>& haves, int limitKB) {
    std::vector<uint8_t> buf;
    buf.push_back(0x93);  // fixarray(3)
    appendBinArray(buf, wants);
    appendBinArray(buf, haves);
    if (limitKB < 128) { buf.push_back((uint8_t)limitKB); }
    else { buf.push_back(0xCD); buf.push_back((limitKB >> 8) & 0xFF); buf.push_back(limitKB & 0xFF); }
    return RNS::Bytes(buf.data(), buf.size());
}

RNS::Bytes packPurgeRequest(const std::vector<RNS::Bytes>& haves) {
    std::vector<uint8_t> buf;
    buf.push_back(0x92);  // fixarray(2)
    buf.push_back(0xC0);  // nil (wants)
    appendBinArray(buf, haves);
    return RNS::Bytes(buf.data(), buf.size());
}
}

PropagationClient* PropagationClient::_instance = nullptr;

namespace {

// ── Minimal msgpack readers for the PN announce app_data shape ──────────
// [legacy_bool, timebase, is_pn, transfer_limit, sync_limit,
//  [stamp_cost, stamp_cost_flex, peering_cost], metadata_dict]
// See LXMF.py's pn_announce_data_is_valid()/pn_*_from_app_data() -- this is
// the spec-wide format, not specific to any one implementation.

bool mpSkip(const uint8_t* d, size_t len, size_t& pos) {
    if (pos >= len) return false;
    uint8_t b = d[pos];
    if (b <= 0x7F || b >= 0xE0) { pos += 1; return pos <= len; }                         // fixint
    if ((b & 0xF0) == 0x80) { size_t n = (b & 0x0F) * 2; pos++; for (size_t i = 0; i < n; i++) if (!mpSkip(d, len, pos)) return false; return true; }  // fixmap
    if ((b & 0xF0) == 0x90) { size_t n = b & 0x0F; pos++; for (size_t i = 0; i < n; i++) if (!mpSkip(d, len, pos)) return false; return true; }        // fixarray
    if ((b & 0xE0) == 0xA0) { pos += 1 + (b & 0x1F); return pos <= len; }                 // fixstr
    if (b == 0xC0 || b == 0xC2 || b == 0xC3) { pos += 1; return pos <= len; }             // nil/false/true
    if (b == 0xC4 && pos + 1 < len) { pos += 2 + d[pos + 1]; return pos <= len; }         // bin8
    if (b == 0xC5 && pos + 2 < len) { pos += 3 + (((size_t)d[pos+1] << 8) | d[pos+2]); return pos <= len; }  // bin16
    if (b == 0xCA) { pos += 5; return pos <= len; }                                       // float32
    if (b == 0xCB) { pos += 9; return pos <= len; }                                        // float64
    if (b == 0xCC || b == 0xD0) { pos += 2; return pos <= len; }                          // uint8/int8
    if (b == 0xCD || b == 0xD1) { pos += 3; return pos <= len; }                          // uint16/int16
    if (b == 0xCE || b == 0xD2) { pos += 5; return pos <= len; }                          // uint32/int32
    if (b == 0xCF || b == 0xD3) { pos += 9; return pos <= len; }                          // uint64/int64
    if (b == 0xD9 && pos + 1 < len) { pos += 2 + d[pos+1]; return pos <= len; }            // str8
    if (b == 0xDA && pos + 2 < len) { pos += 3 + (((size_t)d[pos+1] << 8) | d[pos+2]); return pos <= len; }  // str16
    if (b == 0xDC && pos + 2 < len) {                                                      // array16
        size_t n = ((size_t)d[pos+1] << 8) | d[pos+2]; pos += 3;
        for (size_t i = 0; i < n; i++) if (!mpSkip(d, len, pos)) return false;
        return true;
    }
    return false;  // unrecognized -- caller should bail
}

// Reads a non-negative int from fixint/uint8/uint16/uint32. Returns -1 on
// anything else (including negative ints, which never occur in this shape).
long mpReadUint(const uint8_t* d, size_t len, size_t& pos) {
    if (pos >= len) return -1;
    uint8_t b = d[pos];
    if (b <= 0x7F) { pos += 1; return b; }
    if (b == 0xCC && pos + 1 < len) { long v = d[pos+1]; pos += 2; return v; }
    if (b == 0xCD && pos + 2 < len) { long v = ((long)d[pos+1] << 8) | d[pos+2]; pos += 3; return v; }
    if (b == 0xCE && pos + 4 < len) {
        long v = ((long)d[pos+1] << 24) | ((long)d[pos+2] << 16) | ((long)d[pos+3] << 8) | d[pos+4];
        pos += 5; return v;
    }
    return -1;
}

bool mpReadBool(const uint8_t* d, size_t len, size_t& pos, bool& out) {
    if (pos >= len) return false;
    if (d[pos] == 0xC2) { out = false; pos++; return true; }
    if (d[pos] == 0xC3) { out = true; pos++; return true; }
    return false;
}

size_t mpArrayLen(const uint8_t* d, size_t len, size_t& pos) {
    if (pos >= len) return 0;
    uint8_t b = d[pos];
    if ((b & 0xF0) == 0x90) { pos++; return b & 0x0F; }
    if (b == 0xDC && pos + 2 < len) { size_t n = ((size_t)d[pos+1] << 8) | d[pos+2]; pos += 3; return n; }
    return 0;
}

size_t mpMapLen(const uint8_t* d, size_t len, size_t& pos) {
    if (pos >= len) return 0;
    uint8_t b = d[pos];
    if ((b & 0xF0) == 0x80) { pos++; return b & 0x0F; }
    return 0;
}

std::string mpReadBinOrStr(const uint8_t* d, size_t len, size_t& pos) {
    if (pos >= len) return "";
    uint8_t b = d[pos];
    size_t slen = 0, hdr = 0;
    if ((b & 0xE0) == 0xA0) { slen = b & 0x1F; hdr = 1; }
    else if (b == 0xC4 && pos + 1 < len) { slen = d[pos+1]; hdr = 2; }
    else if (b == 0xC5 && pos + 2 < len) { slen = ((size_t)d[pos+1] << 8) | d[pos+2]; hdr = 3; }
    else if (b == 0xD9 && pos + 1 < len) { slen = d[pos+1]; hdr = 2; }
    else { return ""; }
    if (pos + hdr + slen > len) return "";
    std::string out((const char*)&d[pos + hdr], slen);
    pos += hdr + slen;
    return out;
}

bool decodePNAnnounce(const RNS::Bytes& appData, PropagationNodeInfo& out) {
    if (appData.size() < 2) return false;
    const uint8_t* d = appData.data();
    size_t len = appData.size();
    size_t pos = 0;

    size_t arrLen = mpArrayLen(d, len, pos);
    if (arrLen < 7) return false;

    if (!mpSkip(d, len, pos)) return false;        // [0] legacy bool -- unused

    long timebase = mpReadUint(d, len, pos);        // [1] timebase
    if (timebase < 0) return false;

    bool isPn = false;                               // [2] is_pn
    if (!mpReadBool(d, len, pos, isPn)) return false;
    if (!isPn) return false;

    long transferLimit = mpReadUint(d, len, pos);    // [3]
    long syncLimit = mpReadUint(d, len, pos);         // [4]
    if (transferLimit < 0 || syncLimit < 0) return false;

    size_t costArrPos = pos;
    size_t costArrLen = mpArrayLen(d, len, pos);      // [5] [stamp_cost, flex, peering_cost]
    if (costArrLen < 2) return false;
    long stampCost = mpReadUint(d, len, pos);
    long stampCostFlex = mpReadUint(d, len, pos);
    if (stampCost < 0) stampCost = 0;
    if (stampCostFlex < 0) stampCostFlex = 0;
    (void)costArrPos;
    for (size_t i = 2; i < costArrLen; i++) { if (!mpSkip(d, len, pos)) break; }  // skip peering_cost etc.

    std::string name;
    size_t metaLen = mpMapLen(d, len, pos);           // [6] metadata dict
    for (size_t i = 0; i < metaLen; i++) {
        long key = mpReadUint(d, len, pos);
        if (key == 0x01) {  // PN_META_NAME
            name = mpReadBinOrStr(d, len, pos);
        } else {
            if (!mpSkip(d, len, pos)) break;
        }
    }

    out.name = name;
    out.stampCost = (int)stampCost;
    out.stampCostFlex = (int)stampCostFlex;
    out.transferLimitKB = (int)transferLimit;
    out.syncLimitKB = (int)syncLimit;
    return true;
}

} // namespace

PropagationClient::PropagationClient()
    : RNS::AnnounceHandler("lxmf.propagation")
{
    _instance = this;
    _nodes.reserve(MAX_NODES);
}

void PropagationClient::received_announce(const RNS::Bytes& destination_hash,
                                           const RNS::Identity& announced_identity,
                                           const RNS::Bytes& app_data) {
    PropagationNodeInfo info;
    if (!decodePNAnnounce(app_data, info)) {
        Serial.printf("[PN] Ignoring malformed/non-PN announce from %s\n",
                      destination_hash.toHex().substr(0, 8).c_str());
        return;
    }
    info.hash = destination_hash;
    info.identityHash = announced_identity ? announced_identity.hash() : RNS::Bytes();
    info.hops = RNS::Transport::hops_to(destination_hash);
    info.lastSeenMs = millis();

    for (auto& n : _nodes) {
        if (n.hash == destination_hash) { n = info; return; }
    }
    if (_nodes.size() >= MAX_NODES) {
        // Evict the stalest entry rather than refusing new PNs outright --
        // PNs are rare enough this should almost never actually trigger.
        auto oldest = std::min_element(_nodes.begin(), _nodes.end(),
            [](const PropagationNodeInfo& a, const PropagationNodeInfo& b) { return a.lastSeenMs < b.lastSeenMs; });
        if (oldest != _nodes.end()) *oldest = info;
        return;
    }
    Serial.printf("[PN] Discovered propagation node %s '%s' cost=%d xfer=%dKB sync=%dKB\n",
                  destination_hash.toHex().substr(0, 8).c_str(), info.name.c_str(),
                  info.stampCost, info.transferLimitKB, info.syncLimitKB);
    _nodes.push_back(info);
}

const PropagationNodeInfo* PropagationClient::find(const RNS::Bytes& hash) const {
    for (auto& n : _nodes) {
        if (n.hash == hash) return &n;
    }
    return nullptr;
}

bool PropagationClient::isPnIdentity(const RNS::Bytes& identityHash) const {
    if (identityHash.size() == 0) return false;
    for (auto& n : _nodes) {
        if (n.identityHash.size() > 0 && n.identityHash == identityHash) return true;
    }
    return false;
}

// ── Outbound submission ──────────────────────────────────────────────────

PropagationClient::SubmitOutcome PropagationClient::prepareSubmission(
        const RNS::Bytes& pnHash, LXMFMessage& msg, const RNS::Identity& ourIdentity,
        RNS::Bytes& transientIdOut, int& stampCostOut) {
    if (_submitState != SubmitState::IDLE) return SubmitOutcome::BUSY;

    RNS::Identity recipientId = RNS::Identity::recall(msg.destHash);
    if (!recipientId) return SubmitOutcome::NO_RECIPIENT_IDENTITY;

    // The PN's own identity isn't required yet -- only proceedSubmission()
    // (which actually links to the PN) needs it, and that's where
    // discovery + retry for it lives if it isn't cached already.

    // src(16)+sig(64)+content -- exactly Python's self.packed[DESTINATION_LENGTH:]
    // (LXMessage.py:430, where self.packed already starts with dest_hash).
    std::vector<uint8_t> packed = msg.packFull(ourIdentity);
    if (packed.empty()) return SubmitOutcome::NO_RECIPIENT_IDENTITY;

    RNS::Bytes plaintext(packed.data(), packed.size());
    RNS::Bytes encrypted = recipientId.encrypt(plaintext);

    RNS::Bytes lxmfData(msg.destHash.data(), msg.destHash.size());
    lxmfData.append(encrypted);
    RNS::Bytes transientId = RNS::Identity::full_hash(lxmfData);

    const PropagationNodeInfo* pnInfo = find(pnHash);
    int stampCost = pnInfo ? pnInfo->stampCost : 0;

    _submitRecipientHash = msg.destHash;
    _submitPnHash = pnHash;
    _submitLxmfData = lxmfData;
    _submitState = SubmitState::PREPARED;

    transientIdOut = transientId;
    stampCostOut = stampCost;
    return SubmitOutcome::STARTED;
}

void PropagationClient::proceedSubmission(const RNS::Bytes& stamp) {
    if (_submitState != SubmitState::PREPARED) return;

    if (stamp.size() == LXStamper::STAMP_SIZE) {
        _submitLxmfData.append(stamp);
    }

    RNS::Identity pnIdentity = RNS::Identity::recall(_submitPnHash);
    if (pnIdentity) {
        beginSubmitLink(pnIdentity);
        return;
    }

    // PN identity not cached (no announce heard from it yet this boot) --
    // ask Transport to find it and wait. loop()'s DISCOVERING branch below
    // calls beginSubmitLink() the moment it resolves, or gives up after
    // PN_DISCOVERY_TIMEOUT_MS.
    Serial.printf("[PN] PN %s identity unknown -- requesting path\n",
                  _submitPnHash.toHex().substr(0, 8).c_str());
    RNS::Transport::request_path(_submitPnHash);
    _submitState = SubmitState::DISCOVERING;
    _submitStateSinceMs = millis();
    _submitDiscoveryLastRequestMs = _submitStateSinceMs;
}

void PropagationClient::beginSubmitLink(const RNS::Identity& pnIdentity) {
    RNS::Destination pnDest(pnIdentity, RNS::Type::Destination::OUT, RNS::Type::Destination::SINGLE,
                             "lxmf", "propagation");
    RNS::Link link(pnDest, onSubmitLinkEstablished, onSubmitLinkClosed);
    link.set_resource_concluded_callback(onSubmitResourceConcluded);
    _submitLink = link;
    _submitState = SubmitState::LINKING;
    _submitStateSinceMs = millis();
    Serial.printf("[PN] Submitting message for %s via PN %s\n",
                  _submitRecipientHash.toHex().substr(0, 8).c_str(),
                  _submitPnHash.toHex().substr(0, 8).c_str());
}

void PropagationClient::finishSubmit(bool delivered) {
    RNS::Bytes recipientHash = _submitRecipientHash;
    _submitState = SubmitState::IDLE;
    _submitLink = RNS::Link{RNS::Type::NONE};
    _submitRecipientHash = RNS::Bytes();
    _submitPnHash = RNS::Bytes();
    _submitLxmfData = RNS::Bytes();
    Serial.printf("[PN] Submission %s for %s\n", delivered ? "SENT" : "FAILED",
                  recipientHash.toHex().substr(0, 8).c_str());
    if (_submitDoneCb) _submitDoneCb(recipientHash, delivered);
}

void PropagationClient::onSubmitLinkEstablished(RNS::Link& link) {
    if (!_instance) return;
    PropagationClient* self = _instance;
    if (self->_submitState != SubmitState::LINKING) return;

    self->_submitState = SubmitState::SENDING;
    std::vector<uint8_t> envelopeVec = packPropagationEnvelope(self->_submitLxmfData);
    RNS::Bytes envelope(envelopeVec.data(), envelopeVec.size());

    if (envelope.size() <= RNS::Type::Reticulum::MDU) {
        RNS::Packet packet(link, envelope);
        RNS::PacketReceipt receipt = packet.send();
        if (!receipt) { self->finishSubmit(false); return; }
        receipt.set_delivery_callback(&PropagationClient::onSubmitProof);
        receipt.set_timeout(60);
        receipt.set_timeout_callback(&PropagationClient::onSubmitTimeout);
        self->_submitState = SubmitState::AWAITING_PROOF;
    } else {
        if (!link.start_resource_transfer(envelope)) { self->finishSubmit(false); return; }
        self->_submitState = SubmitState::AWAITING_PROOF;  // concluded via onSubmitResourceConcluded
    }
}

void PropagationClient::onSubmitLinkClosed(RNS::Link& link) {
    if (!_instance) return;
    if (_instance->_submitState != SubmitState::IDLE) {
        _instance->finishSubmit(false);
    }
}

void PropagationClient::onSubmitProof(const RNS::PacketReceipt& receipt) {
    if (!_instance) return;
    // Propagation acceptance proof, not recipient delivery -- matches
    // Python's __mark_propagated (LXMessage.py:570): handing off to a PN
    // is "sent", not "delivered".
    _instance->finishSubmit(true);
}

void PropagationClient::onSubmitTimeout(const RNS::PacketReceipt& receipt) {
    if (!_instance) return;
    _instance->finishSubmit(false);
}

void PropagationClient::onSubmitResourceConcluded(const RNS::Resource& resource) {
    if (!_instance) return;
    _instance->finishSubmit(resource.status() == RNS::Type::Resource::COMPLETE);
}

void PropagationClient::abortSubmission() {
    if (_submitState != SubmitState::PREPARED) return;
    finishSubmit(false);
}

// ── Inbound sync ────────────────────────────────────────────────────────

bool PropagationClient::startSync(const RNS::Bytes& pnHash, const RNS::Identity& ourIdentity,
                                   DecodedMessageCallback onMessage, SyncDoneCallback onDone) {
    if (_syncState != SyncState::IDLE) return false;

    _syncPnHash = pnHash;
    _syncIdentity = &ourIdentity;
    _onMessage = onMessage;
    _onSyncDone = onDone;
    _syncMessagesReceived = 0;
    _syncWantedIds.clear();

    RNS::Identity pnIdentity = RNS::Identity::recall(pnHash);
    if (pnIdentity) {
        beginSyncLink(pnIdentity);
        return true;
    }

    // PN identity not cached (no announce heard from it yet this boot) --
    // ask Transport to find it and wait. loop()'s DISCOVERING branch below
    // calls beginSyncLink() the moment it resolves, or gives up after
    // PN_DISCOVERY_TIMEOUT_MS -- this is what used to surface as an
    // immediate, unconditional "Node not reachable yet" with no actual
    // discovery attempt at all.
    Serial.printf("[PN] PN %s identity unknown -- requesting path\n", pnHash.toHex().substr(0, 8).c_str());
    RNS::Transport::request_path(pnHash);
    _syncState = SyncState::DISCOVERING;
    _syncStateSinceMs = millis();
    _syncDiscoveryLastRequestMs = _syncStateSinceMs;
    return true;
}

void PropagationClient::beginSyncLink(const RNS::Identity& pnIdentity) {
    Serial.printf("[PN] Starting sync with PN %s\n", _syncPnHash.toHex().substr(0, 8).c_str());
    RNS::Destination pnDest(pnIdentity, RNS::Type::Destination::OUT, RNS::Type::Destination::SINGLE,
                             "lxmf", "propagation");
    RNS::Link link(pnDest, onSyncLinkEstablished, onSyncLinkClosed);
    _syncLink = link;
    _syncState = SyncState::LINKING;
    _syncStateSinceMs = millis();
}

void PropagationClient::finishSync(bool ok) {
    int count = _syncMessagesReceived;
    SyncDoneCallback cb = _onSyncDone;
    _syncState = SyncState::IDLE;
    _syncLink = RNS::Link{RNS::Type::NONE};
    _syncPnHash = RNS::Bytes();
    _syncIdentity = nullptr;
    _onMessage = nullptr;
    _onSyncDone = nullptr;
    _syncWantedIds.clear();
    _syncMessagesReceived = 0;
    Serial.printf("[PN] Sync %s, %d message(s) received\n", ok ? "complete" : "failed", count);
    if (cb) cb(ok, count);
}

// Array-of-bin/str msgpack payload -> vector<Bytes>. Used for both the
// transient_id list response and the lxmf_data list response -- same shape.
namespace {
bool parseBinArray(const RNS::Bytes& data, std::vector<RNS::Bytes>& out) {
    if (data.size() < 1) return false;
    const uint8_t* d = data.data();
    size_t len = data.size();
    size_t pos = 0;
    size_t n = mpArrayLen(d, len, pos);
    for (size_t i = 0; i < n; i++) {
        if (pos >= len) return false;
        uint8_t b = d[pos];
        size_t slen = 0, hdr = 0;
        if (b == 0xC4 && pos + 1 < len) { slen = d[pos+1]; hdr = 2; }
        else if (b == 0xC5 && pos + 2 < len) { slen = ((size_t)d[pos+1] << 8) | d[pos+2]; hdr = 3; }
        else if ((b & 0xE0) == 0xA0) { slen = b & 0x1F; hdr = 1; }
        else return false;
        if (pos + hdr + slen > len) return false;
        out.emplace_back(&d[pos + hdr], slen);
        pos += hdr + slen;
    }
    return true;
}
}

void PropagationClient::handleListResponse(const RNS::Bytes& response) {
    std::vector<RNS::Bytes> ids;
    if (!parseBinArray(response, ids) || ids.empty()) {
        finishSync(true);  // nothing waiting at the PN -- a successful no-op
        return;
    }

    Serial.printf("[PN] %d message(s) waiting\n", (int)ids.size());

    // No persisted "already delivered" set on this device -- see
    // docs/propagation-nodes.md for why that's a deliberate simplification,
    // not an oversight. We always want everything currently offered; the
    // PN-side purge below (and LXMFManager's existing messageId dedup as a
    // backstop) is what prevents re-delivery on a future sync.
    _syncWantedIds = ids;
    RNS::Bytes req = packTransferRequest(ids, {}, PN_CLIENT_TRANSFER_LIMIT_KB);
    RNS::Bytes path("/get");
    RNS::RequestReceipt r = _syncLink.request(path, req, &onMessagesResponse, &onMessagesFailed, nullptr, PN_REQUEST_TIMEOUT_S);
    if (!r) { finishSync(false); return; }
    _syncState = SyncState::AWAITING_MESSAGES;
    _syncStateSinceMs = millis();
}

void PropagationClient::handleMessagesResponse(const RNS::Bytes& response) {
    std::vector<RNS::Bytes> blobs;
    if (!parseBinArray(response, blobs)) { finishSync(false); return; }

    for (auto& blob : blobs) {
        if (blob.size() < 16) continue;
        RNS::Bytes destHash = blob.left(16);
        RNS::Bytes encrypted = blob.mid(16);
        RNS::Bytes decrypted = _syncIdentity ? _syncIdentity->decrypt(encrypted) : RNS::Bytes();
        if (decrypted.size() == 0) {
            Serial.println("[PN] Could not decrypt a synced message -- skipping");
            continue;
        }
        RNS::Bytes full = destHash;
        full.append(decrypted);
        if (_onMessage) _onMessage(full.data(), full.size());
        _syncMessagesReceived++;
    }

    // Fire-and-forget purge of everything we asked for -- matches the
    // Python reference's own one-shot purge (LXMRouter.py). We don't wait
    // for its response: the messages are already processed locally by
    // this point, so purge failing just means the PN re-offers them next
    // sync, which the dedup backstop above absorbs harmlessly.
    if (!_syncWantedIds.empty()) {
        RNS::Bytes purgeReq = packPurgeRequest(_syncWantedIds);
        RNS::Bytes path("/get");
        _syncLink.request(path, purgeReq, nullptr, nullptr, nullptr, PN_REQUEST_TIMEOUT_S);
    }
    finishSync(true);
}

void PropagationClient::onSyncLinkEstablished(RNS::Link& link) {
    if (!_instance || _instance->_syncState != SyncState::LINKING) return;
    PropagationClient* self = _instance;

    self->_syncState = SyncState::IDENTIFYING;
    link.identify(*self->_syncIdentity);

    RNS::Bytes path("/get");
    RNS::RequestReceipt r = link.request(path, packListRequest(), &onListResponse, &onListFailed, nullptr, PN_REQUEST_TIMEOUT_S);
    if (!r) { self->finishSync(false); return; }
    self->_syncState = SyncState::AWAITING_LIST;
    self->_syncStateSinceMs = millis();
}

void PropagationClient::onSyncLinkClosed(RNS::Link& link) {
    if (!_instance) return;
    if (_instance->_syncState != SyncState::IDLE) {
        _instance->finishSync(false);
    }
}

void PropagationClient::onListResponse(const RNS::RequestReceipt& receipt) {
    if (!_instance) return;
    _instance->handleListResponse(receipt.get_response());
}

void PropagationClient::onListFailed(const RNS::RequestReceipt& receipt) {
    if (!_instance) return;
    _instance->finishSync(false);
}

void PropagationClient::onMessagesResponse(const RNS::RequestReceipt& receipt) {
    if (!_instance) return;
    _instance->handleMessagesResponse(receipt.get_response());
}

void PropagationClient::onMessagesFailed(const RNS::RequestReceipt& receipt) {
    if (!_instance) return;
    _instance->finishSync(false);
}

// ── Periodic upkeep ─────────────────────────────────────────────────────

void PropagationClient::loop() {
    unsigned long now = millis();

    if (_submitState == SubmitState::DISCOVERING) {
        RNS::Identity pnIdentity = RNS::Identity::recall(_submitPnHash);
        if (pnIdentity) {
            beginSubmitLink(pnIdentity);
        } else if (now - _submitStateSinceMs >= PN_DISCOVERY_TIMEOUT_MS) {
            Serial.println("[PN] Submission: PN identity/path discovery timed out");
            finishSubmit(false);
        } else if (now - _submitDiscoveryLastRequestMs >= PN_DISCOVERY_RETRY_INTERVAL_MS) {
            RNS::Transport::request_path(_submitPnHash);
            _submitDiscoveryLastRequestMs = now;
        }
    }

    if (_submitState == SubmitState::LINKING
        && now - _submitStateSinceMs >= PN_LINK_TIMEOUT_MS) {
        Serial.println("[PN] Submission link establishment timed out");
        finishSubmit(false);
    }

    if (_syncState == SyncState::DISCOVERING) {
        RNS::Identity pnIdentity = RNS::Identity::recall(_syncPnHash);
        if (pnIdentity) {
            beginSyncLink(pnIdentity);
        } else if (now - _syncStateSinceMs >= PN_DISCOVERY_TIMEOUT_MS) {
            Serial.println("[PN] Sync: PN identity/path discovery timed out");
            finishSync(false);
        } else if (now - _syncDiscoveryLastRequestMs >= PN_DISCOVERY_RETRY_INTERVAL_MS) {
            RNS::Transport::request_path(_syncPnHash);
            _syncDiscoveryLastRequestMs = now;
        }
    }

    if ((_syncState == SyncState::LINKING || _syncState == SyncState::IDENTIFYING)
        && now - _syncStateSinceMs >= PN_LINK_TIMEOUT_MS) {
        Serial.println("[PN] Sync link establishment timed out");
        finishSync(false);
    }
}
