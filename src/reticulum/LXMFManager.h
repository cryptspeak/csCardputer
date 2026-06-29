#pragma once

#include "LXMFMessage.h"
#include "LXStamper.h"
#include "PropagationClient.h"
#include "ReticulumManager.h"
#include "storage/MessageStore.h"
#include <Destination.h>
#include <Packet.h>
#include <Link.h>
#include <Identity.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <functional>
#include <deque>
#include <set>
#include <map>

class AnnounceManager;

class LXMFManager {
public:
    using MessageCallback = std::function<void(const LXMFMessage&)>;
    using StatusCallback = std::function<void(const std::string& peerHex, double timestamp, LXMFStatus status)>;

    bool begin(ReticulumManager* rns, MessageStore* store);
    void loop();

    // Send a text message to a destination hash
    bool sendMessage(const RNS::Bytes& destHash, const std::string& content, const std::string& title = "");

    // Incoming message callback
    void setMessageCallback(MessageCallback cb) { _onMessage = cb; }

    // Status callback (fires when send completes with SENT/FAILED)
    void setStatusCallback(StatusCallback cb) { _statusCb = cb; }

    // Fires once per successful send (direct or PN handoff) with a short
    // tag describing how it actually went out -- e.g. "LoRa", "TCP",
    // "PN/LoRa" -- so the UI can show real routing instead of leaving the
    // user to guess whether propagation-node fallback was used.
    using RouteInfoCallback = std::function<void(const std::string& peerHex, const std::string& routeTag)>;
    void setRouteInfoCallback(RouteInfoCallback cb) { _routeInfoCb = cb; }

    // Source hash (our LXMF destination hash)
    RNS::Bytes getSourceHash() const { return _rns ? _rns->destination().hash() : RNS::Bytes(); }

    // Queue info
    int queuedCount() const { return _outQueue.size(); }

    // Get all conversations (destination hashes with messages)
    const std::vector<std::string>& conversations() const;

    // Get messages for a conversation (paginated, last N messages)
    std::vector<LXMFMessage> getMessages(const std::string& peerHex, int limit = 20) const;

    // Unread count for a peer (or total)
    int unreadCount(const std::string& peerHex = "") const;

    // Mark conversation as read
    void markRead(const std::string& peerHex);

    // Delete a conversation and its messages
    void deleteConversation(const std::string& peerHex);

    // ── Anti-spam stamps (see LXStamper.h) ──────────────────────────────
    // Costs at or below this are generated automatically in the background;
    // above it, setStampConfirmCallback() fires before any CPU time is
    // spent. Default (12) is a few seconds of work on this hardware.
    void setStampCostCeiling(int ceiling) { _stampCostCeiling = ceiling; }
    int stampCostCeiling() const { return _stampCostCeiling; }

    // Fired (at most once per peer per session) when a queued message needs
    // a stamp costlier than the ceiling allows automatically. The message
    // stays queued -- neither generating nor failed -- until confirmStamping()
    // is called with the user's answer.
    using StampConfirmCallback = std::function<void(const std::string& peerHex, int cost)>;
    void setStampConfirmCallback(StampConfirmCallback cb) { _stampConfirmCb = cb; }
    void confirmStamping(const std::string& peerHex, bool proceed);

    // ── Propagation node (offline messaging) ────────────────────────────
    // Externally-owned (main.cpp registers it as an AnnounceHandler too,
    // same lifetime as ReticulumManager). Direct delivery still tried
    // first, every time -- this is only consulted once direct delivery's
    // own retry budget is exhausted (see sendDirect()).
    void setPropagationClient(PropagationClient* pc);

    // For per-peer interface stickiness in sendDirect() (see there) --
    // externally owned, same lifetime/wiring pattern as setPropagationClient().
    void setAnnounceManager(AnnounceManager* am) { _announceManager = am; }
    // Empty hash (default) disables PN fallback -- messages just FAIL
    // when direct delivery can't reach the peer, same as before this
    // feature existed.
    void setPreferredPropagationNode(const RNS::Bytes& hash) { _propagationNodeHash = hash; }

    // Manually pull mail waiting at the preferred PN. Returns false if no
    // PN is configured, one isn't reachable yet, or a sync is already
    // running. Decoded messages flow through the normal incoming pipeline
    // (dedup/storage/UI) automatically.
    bool syncPropagationNode();
    bool propagationSyncInFlight() const;
    // Status of the last completed sync, for UI display (Settings > Messaging).
    // lastSyncAtMs() == 0 means "never synced this boot."
    unsigned long lastSyncAtMs() const { return _lastSyncAtMs; }
    bool lastSyncOk() const { return _lastSyncOk; }
    int lastSyncMessageCount() const { return _lastSyncMsgCount; }

private:
    bool sendDirect(LXMFMessage& msg);
    bool ensureOutboundLink(const RNS::Destination& dest, const RNS::Bytes& destHash, const char* reason);
    static RNS::Interface findInterfaceByName(const std::string& name);
    void processIncoming(const uint8_t* data, size_t len, const RNS::Bytes& destHash);
    void rememberMessageId(const std::string& msgIdHex);

    // Static callbacks for microReticulum
    static void onPacketReceived(const RNS::Bytes& data, const RNS::Packet& packet);
    static void onLinkEstablished(RNS::Link& link);
    static void onOutLinkEstablished(RNS::Link& link);
    static void onOutLinkClosed(RNS::Link& link);

    ReticulumManager* _rns = nullptr;
    MessageStore* _store = nullptr;
    MessageCallback _onMessage;
    StatusCallback _statusCb;
    RouteInfoCallback _routeInfoCb;
    std::deque<LXMFMessage> _outQueue;

    // Outbound link state (opportunistic-first, link upgrades in background)
    RNS::Link _outLink{RNS::Type::NONE};
    RNS::Bytes _outLinkDestHash;       // Destination the ACTIVE _outLink is for
    RNS::Bytes _outLinkPendingHash;    // Destination being connected to (not yet established)
    bool _outLinkPending = false;
    unsigned long _outLinkPendingSinceMs = 0;

    // Unread tracking (lazy-loaded on first access)
    void computeUnreadFromDisk();
    mutable bool _unreadComputed = false;
    mutable std::map<std::string, int> _unread;

    // Deduplication: recently seen message IDs
    std::set<std::string> _seenMessageIds;
    std::deque<std::string> _seenMessageOrder;
    static constexpr int MAX_SEEN_IDS = 100;

    // Incoming message queue — packet callbacks push here, loop() processes
    std::vector<LXMFMessage> _incomingQueue;
    static constexpr int MAX_INCOMING_QUEUE = 8;

    // Per-peer interface stickiness (see AnnounceManager::preferredInterfaceName
    // and the check near the top of sendDirect()). Tracks, per destination
    // hex, when we first noticed Transport's path table disagreed with the
    // peer's recorded preferred interface and asked it to recheck -- bounds
    // how long a send waits for that correction before giving up and using
    // whatever the table says. Cheap, arbitrary cap (matches the old
    // _lastPathRefresh pattern this replaces); this is a throttle/wait
    // window, not a record that needs to survive eviction.
    std::map<std::string, unsigned long> _interfaceCorrectionStart;
    AnnounceManager* _announceManager = nullptr;

    struct PendingProof {
        std::string peerHex;
        double timestamp = 0;
        uint32_t savedCounter = 0;
        unsigned long createdMs = 0;
        uint8_t proofAttempts = 0;
        LXMFMessage msg;
    };
    static std::map<std::string, PendingProof> _pendingProofs;
    static void onProofDelivered(const RNS::PacketReceipt& receipt);
    static void onProofTimeout(const RNS::PacketReceipt& receipt);
    static void handleProofTimeoutHash(const std::string& receiptHash);
    void registerProofTracking(RNS::PacketReceipt& receipt, const LXMFMessage& msg);

    // ── Anti-spam stamps ─────────────────────────────────────────────────
    // Stamp generation is real CPU work (see LXStamper.h) -- it runs on its
    // own low-priority task on core 0 so it never blocks the main loop
    // (radio/UI, core 1), exactly one job in flight at a time, matching this
    // device's "one peer/one flow at a time" design elsewhere (see _outLink).
    struct StampReq { uint8_t material[32]; int targetCost; int expandRounds; };
    struct StampRes { uint8_t material[32]; uint8_t stamp[32]; uint8_t ok; };
    enum class StampPurpose : uint8_t { DIRECT, PROPAGATION };

    void startStampTaskIfNeeded();
    static void stampTaskFunc(void* param);
    void pollStampResult();
    // Begins background generation for `msg` (sets status=STAMPING, submits
    // to the stamp task). Caller must already hold the single in-flight slot
    // free (`!_stampInFlight`).
    void beginStamping(LXMFMessage& msg, int targetCost);
    // Same background task, but for a propagation-submission stamp (keyed
    // by transient_id, not messageId -- see PropagationClient::prepareSubmission).
    void beginPropagationStamping(const RNS::Bytes& transientId, int targetCost);

    int _stampCostCeiling = 12;
    StampConfirmCallback _stampConfirmCb;
    std::set<std::string> _stampAwaitingConfirm;      // peerHex currently showing a confirm prompt
    std::set<std::string> _stampApprovedOverCeiling;   // peerHex the user said "yes" to this session

    TaskHandle_t _stampTask = nullptr;
    QueueHandle_t _stampRequestQueue = nullptr;  // depth 1 -- one job in flight
    QueueHandle_t _stampResultQueue = nullptr;   // depth 1
    bool _stampInFlight = false;
    StampPurpose _stampPurpose = StampPurpose::DIRECT;

    // ── Propagation node fallback/sync ───────────────────────────────────
    // Called once direct delivery's own retry budget is exhausted but the
    // recipient's identity IS known (we just can't currently route to
    // them) -- see sendDirect(). Returns true if the message is now
    // pending via the PN (keep it queued, don't mark FAILED), false if PN
    // fallback isn't available/applicable (caller should fail as before).
    bool tryPropagationFallback(LXMFMessage& msg);
    void onPropagationSubmitDone(const RNS::Bytes& recipientHash, bool delivered);
    void onPropagationMessageDecoded(const uint8_t* data, size_t len);

    PropagationClient* _propagation = nullptr;
    RNS::Bytes _propagationNodeHash;
    unsigned long _lastSyncAtMs = 0;
    bool _lastSyncOk = false;
    int _lastSyncMsgCount = 0;

    static LXMFManager* _instance;
};
