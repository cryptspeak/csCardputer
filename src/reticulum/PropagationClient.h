#pragma once

#include "LXMFMessage.h"
#include <Transport.h>
#include <Identity.h>
#include <Destination.h>
#include <Link.h>
#include <Packet.h>
#include <Resource.h>
#include <Bytes.h>
#include <vector>
#include <string>
#include <functional>

// Client-side support for LXMF propagation nodes (store-and-forward mail
// drops) -- this device never hosts mail for anyone else (see
// docs/propagation-nodes.md for why), only: (1) discovers PNs from their
// announces, (2) hands a message to a chosen PN when direct delivery to the
// recipient fails, and (3) pulls down mail waiting at a PN for this
// device's own identity. Mirrors the relevant subset of Python LXMF's
// LXMRouter -- see docs/propagation-nodes.md for the protocol citations.
struct PropagationNodeInfo {
    RNS::Bytes hash;
    RNS::Bytes identityHash;  // for cross-referencing against AnnounceManager's contacts
    std::string name;
    int stampCost = 0;
    int stampCostFlex = 0;
    int transferLimitKB = 0;
    int syncLimitKB = 0;
    uint8_t hops = 0;
    unsigned long lastSeenMs = 0;
};

class PropagationClient : public RNS::AnnounceHandler {
public:
    PropagationClient();

    void received_announce(const RNS::Bytes& destination_hash,
                            const RNS::Identity& announced_identity,
                            const RNS::Bytes& app_data) override;

    const std::vector<PropagationNodeInfo>& nodes() const { return _nodes; }
    const PropagationNodeInfo* find(const RNS::Bytes& hash) const;

    // True if `identityHash` is known to run a PN (regardless of which of
    // its destinations announced) -- lets AnnounceManager keep a PN's own
    // operator identity out of the regular contact list even when that
    // same identity also announces an unrelated lxmf.delivery destination
    // (e.g. a NomadNet node with propagation enabled uses one identity for
    // both -- see docs/propagation-nodes.md).
    bool isPnIdentity(const RNS::Bytes& identityHash) const;

    // Drives the link/request state machines below -- call once per
    // LXMFManager::loop() iteration.
    void loop();

    // ── Outbound: hand a message to a PN when direct delivery isn't
    // possible. Two phases, split specifically so the (mandatory, see
    // docs/lxmf-stamps.md) propagation stamp can be generated on
    // LXMFManager's existing background stamp task rather than blocking
    // here -- generating it inline would freeze the main loop/UI for the
    // duration, the exact problem that task exists to avoid.
    //
    // 1. prepareSubmission(): encrypts the message to the recipient,
    //    computes transient_id, and caches everything needed to finish.
    //    Returns the transient_id (the PoW material for the propagation
    //    stamp) via `transientIdOut`, and the cost the PN requires via
    //    `stampCostOut` (0 = none required -- proceed with an empty stamp).
    // 2. proceedSubmission(): appends `stamp` (may be empty) and actually
    //    links to the PN and sends. Caller must have already obtained a
    //    valid stamp meeting `stampCostOut` if it was nonzero.
    //
    // One submission in flight at a time, matching this device's
    // single-flow design elsewhere (LXMFManager's _outLink).
    enum class SubmitOutcome { STARTED, BUSY, NO_RECIPIENT_IDENTITY };
    SubmitOutcome prepareSubmission(const RNS::Bytes& pnHash, LXMFMessage& msg, const RNS::Identity& ourIdentity,
                                     RNS::Bytes& transientIdOut, int& stampCostOut);
    void proceedSubmission(const RNS::Bytes& stamp);
    // Cancels a PREPARED submission that never got a stamp (e.g. the
    // background stamp job failed) -- fires the done callback with
    // delivered=false instead of silently leaking the prepared state.
    void abortSubmission();
    bool submitInFlight() const { return _submitState != SubmitState::IDLE; }

    // Fired once when an in-flight submission concludes either way.
    using SubmitDoneCallback = std::function<void(const RNS::Bytes& recipientHash, bool delivered)>;
    void setSubmitDoneCallback(SubmitDoneCallback cb) { _submitDoneCb = cb; }

    // ── Inbound: pull down mail waiting at `pnHash` for `ourIdentity`.
    // Each decoded message is handed to `onMessage` as a full
    // [dest:16][src:16][sig:64][msgpack] buffer -- the same shape
    // LXMFManager::processIncoming() already accepts from link delivery
    // (dest_hash included, not split out), so the caller can feed it
    // straight into the existing pipeline (dedup/storage/UI) unchanged,
    // passing an empty destHash override just like onLinkEstablished does.
    using DecodedMessageCallback = std::function<void(const uint8_t* data, size_t len)>;
    using SyncDoneCallback = std::function<void(bool ok, int messagesReceived)>;
    bool startSync(const RNS::Bytes& pnHash, const RNS::Identity& ourIdentity,
                   DecodedMessageCallback onMessage, SyncDoneCallback onDone);
    bool syncInFlight() const { return _syncState != SyncState::IDLE; }

private:
    std::vector<PropagationNodeInfo> _nodes;
    static constexpr size_t MAX_NODES = 8;  // PNs are rare; tiny bound is plenty

    // ── Outbound submission state machine ───────────────────────────────
    // DISCOVERING: the PN's identity isn't cached yet (no announce heard
    // from it this boot) -- request_path() was issued and we're waiting
    // for it to resolve, same ~60s budget as LXMFManager's own direct-
    // delivery discovery. Only needed for the PN's identity: the
    // recipient's identity is already confirmed by the time
    // tryPropagationFallback() ever calls prepareSubmission() (direct
    // delivery's own discovery loop already required it).
    enum class SubmitState { IDLE, PREPARED, DISCOVERING, LINKING, SENDING, AWAITING_PROOF };
    SubmitState _submitState = SubmitState::IDLE;
    RNS::Link _submitLink{RNS::Type::NONE};
    RNS::Bytes _submitRecipientHash;
    RNS::Bytes _submitPnHash;
    RNS::Bytes _submitLxmfData;     // dest_hash(16) + encrypted(src+sig+content), stamp appended in proceedSubmission()
    unsigned long _submitStateSinceMs = 0;
    unsigned long _submitDiscoveryLastRequestMs = 0;  // throttles repeat request_path() calls

    void beginSubmitLink(const RNS::Identity& pnIdentity);
    void finishSubmit(bool delivered);
    static void onSubmitLinkEstablished(RNS::Link& link);
    static void onSubmitLinkClosed(RNS::Link& link);
    static void onSubmitProof(const RNS::PacketReceipt& receipt);
    static void onSubmitTimeout(const RNS::PacketReceipt& receipt);
    static void onSubmitResourceConcluded(const RNS::Resource& resource);
    SubmitDoneCallback _submitDoneCb;

    // ── Inbound sync state machine ──────────────────────────────────────
    // DISCOVERING mirrors the submission side above -- same reason, same
    // budget, just for the sync link instead of the submission link.
    enum class SyncState { IDLE, DISCOVERING, LINKING, IDENTIFYING, AWAITING_LIST, AWAITING_MESSAGES };
    SyncState _syncState = SyncState::IDLE;
    RNS::Link _syncLink{RNS::Type::NONE};
    RNS::Bytes _syncPnHash;
    std::vector<RNS::Bytes> _syncWantedIds;  // remembered so the post-fetch purge can name them
    unsigned long _syncStateSinceMs = 0;
    unsigned long _syncDiscoveryLastRequestMs = 0;  // throttles repeat request_path() calls
    const RNS::Identity* _syncIdentity = nullptr;
    DecodedMessageCallback _onMessage;
    SyncDoneCallback _onSyncDone;
    int _syncMessagesReceived = 0;

    void beginSyncLink(const RNS::Identity& pnIdentity);
    void finishSync(bool ok);
    void handleListResponse(const RNS::Bytes& response);
    void handleMessagesResponse(const RNS::Bytes& response);
    static void onSyncLinkEstablished(RNS::Link& link);
    static void onSyncLinkClosed(RNS::Link& link);
    static void onListResponse(const RNS::RequestReceipt& receipt);
    static void onListFailed(const RNS::RequestReceipt& receipt);
    static void onMessagesResponse(const RNS::RequestReceipt& receipt);
    static void onMessagesFailed(const RNS::RequestReceipt& receipt);

    static PropagationClient* _instance;
};
