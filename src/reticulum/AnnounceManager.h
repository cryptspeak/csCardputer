#pragma once

#include <Transport.h>
#include <Identity.h>
#include <Bytes.h>
#include <vector>
#include <string>
#include <map>
#include <unordered_map>
#include <functional>
#include "config/Config.h"

class SDStore;
class FlashStore;
class LoRaInterface;
class PropagationClient;

struct DiscoveredNode {
    RNS::Bytes hash;
    std::string name;
    std::string identityHex;
    int rssi = 0;
    float snr = 0;
    uint8_t hops = 0;
    unsigned long lastSeen = 0;
    bool saved = false;  // Persisted as contact

    // Name of the RNS::Interface (Interface::name(), e.g. "LoRa"/"TCP Hub")
    // this peer was last heard from directly (hops()==1, unrelayed -- see
    // AnnounceManager::notePeerInterface()) -- not necessarily the
    // interface Transport's path table currently has on file for them,
    // which only tracks the single lowest-hop route and has no notion of
    // "which interface this conversation is actually on". Persisted for
    // saved contacts so a reply still prefers the right radio after a
    // reboot, before any fresh announce has been heard.
    std::string preferredInterfaceName;
};

class AnnounceManager : public RNS::AnnounceHandler {
public:
    AnnounceManager(const char* aspectFilter = nullptr);
    virtual ~AnnounceManager() = default;

    virtual void received_announce(
        const RNS::Bytes& destination_hash,
        const RNS::Identity& announced_identity,
        const RNS::Bytes& app_data) override;

    // Storage for contact persistence
    void setStorage(SDStore* sd, FlashStore* flash);
    void setLocalDestHash(const RNS::Bytes& hash) { _localDestHash = hash; }
    void setLoRaInterface(LoRaInterface* li) { _loraIf = li; }
    // So a PN's operator identity (e.g. a NomadNet node with propagation
    // enabled, which announces lxmf.propagation AND lxmf.delivery under the
    // same identity) never gets passively discovered as a regular contact
    // here -- it belongs exclusively in the Propagation Node picker.
    void setPropagationClient(PropagationClient* pc) { _propagation = pc; }

    // ── At-rest encryption ──────────────────────────────────────────────
    // Provide the loaded RNS::Identity (with private key) so contact files
    // and the name cache are encrypted before they hit disk and decrypted
    // on load. Identity is held by pointer; lifetime must outlive this
    // object. Calling with nullptr disables encryption (legacy/test path).
    void setIdentity(const RNS::Identity* identity) { _identity = identity; }
    bool encryptionEnabled() const { return _identity != nullptr && *_identity; }

    // Save/load persisted contacts
    void saveContacts();
    void loadContacts();
    void loop();  // Call from main loop — handles deferred saves

    // Name cache: persists hash→name mappings so names survive reboots
    std::string lookupName(const std::string& hexHash) const;
    void saveNameCache();
    void loadNameCache();

    // Node list access
    const std::vector<DiscoveredNode>& nodes() const { return _nodes; }
    int nodeCount() const { return _nodes.size(); }

    // Bumped every time a node's display name is learned/changed from an
    // announce. Screens that cache a peer-hex -> name label (e.g.
    // MessagesScreen's conversation list) only rebuild that label on their
    // own onEnter()/explicit refresh -- an announce arriving while such a
    // screen is already on-screen wouldn't otherwise reach it. Cheap to
    // poll once per render() and compare against a locally cached value,
    // same pattern NodesScreen already uses for nodeCount().
    uint32_t nameVersion() const { return _nameVersion; }

    // Fired whenever a name change bumps nameVersion() above -- lets
    // main.cpp push a UIManager::markContentDirty() so a screen idling
    // on-screen (UIManager::render() otherwise skips redraws entirely when
    // no dirty flag is set) actually picks the new name up instead of
    // waiting on the next unrelated dirty flag or a tab switch.
    using NameChangedCallback = std::function<void()>;
    void setOnNameChanged(NameChangedCallback cb) { _onNameChanged = cb; }

    // Find node by hash
    const DiscoveredNode* findNode(const RNS::Bytes& hash) const;
    const DiscoveredNode* findNodeByHex(const std::string& hexHash) const;

    // Manual contact add
    void addManualContact(const std::string& hexHash, const std::string& name);

    // Records which interface a peer was last heard from directly on (see
    // DiscoveredNode::preferredInterfaceName). Creates a lightweight,
    // unsaved node entry if this hash isn't known yet -- same as a passive
    // announce would -- so stickiness still works for peers messaging us
    // before they've ever announced.
    void notePeerInterface(const RNS::Bytes& hash, const std::string& interfaceName);

    // Save/unsave a discovered node as contact by hex hash
    void saveNode(const std::string& hexHash);
    void unsaveNode(const std::string& hexHash);

    // Evict old entries
    void evictStale(unsigned long maxAgeMs = 3600000);  // 1 hour default

    // Clear all nodes and state (used during first-boot data wipe)
    void clearAll();

    void rebuildIndex();

private:
    void saveContact(const DiscoveredNode& node);
    void removeContact(const std::string& hexHash);
    void bumpNameVersion() { _nameVersion++; if (_onNameChanged) _onNameChanged(); }

    std::vector<DiscoveredNode> _nodes;
    std::unordered_map<std::string, int> _hashIndex;  // raw hash bytes → _nodes index

    static std::string makeKey(const RNS::Bytes& hash) {
        return std::string((const char*)hash.data(), hash.size());
    }

    SDStore* _sd = nullptr;
    FlashStore* _flash = nullptr;
    LoRaInterface* _loraIf = nullptr;
    PropagationClient* _propagation = nullptr;
    const RNS::Identity* _identity = nullptr;
    RNS::Bytes _localDestHash;
    bool _contactsDirty = false;
    // mutable: lookupName() is logically const (it answers "what name do we
    // currently know for this hash") but opportunistically self-heals this
    // cache from RNS::Identity::recall_app_data() when it resolves a name
    // these don't yet have -- see lookupName().
    mutable bool _nameCacheDirty = false;
    uint32_t _nameVersion = 0;
    NameChangedCallback _onNameChanged;
    unsigned long _lastContactSave = 0;
    unsigned long _lastNameCacheSave = 0;
    mutable std::map<std::string, std::string> _nameCache;  // hexHash → displayName
    unsigned long _globalAnnounceWindowStart = 0;
    unsigned int _globalAnnounceCount = 0;
    static constexpr unsigned int MAX_GLOBAL_ANNOUNCES_PER_SEC = 8;
    static constexpr int MAX_NODES = RSCARDPUTER_MAX_NODES;
    static constexpr int MAX_NAME_CACHE = 60;
    static constexpr unsigned long CONTACT_SAVE_INTERVAL_MS = 30000;
    static constexpr unsigned long NAME_CACHE_SAVE_INTERVAL_MS = 5000;
    static constexpr unsigned long ANNOUNCE_MIN_INTERVAL_MS = 200;  // Rate-limit announce processing
};
