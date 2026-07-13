#pragma once

#include <Arduino.h>
#include "config/Config.h"
#include "storage/FlashStore.h"
#include "storage/SDStore.h"
#include "storage/WriteQueue.h"
#include "reticulum/LXMFMessage.h"
#include <Identity.h>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <atomic>
#include <functional>

class MessageStore {
public:
    bool begin(FlashStore* flash, SDStore* sd = nullptr);

    // ── At-rest encryption ──────────────────────────────────────────────
    // Provide the loaded RNS::Identity (with private key) so message blobs
    // can be encrypted before they hit disk and decrypted on load.
    // Identity is held by pointer; lifetime must outlive MessageStore.
    // Calling with nullptr disables encryption (legacy/test path).
    void setIdentity(const RNS::Identity* identity) { _identity = identity; }
    bool encryptionEnabled() const { return _identity != nullptr && *_identity; }

    // Walk every stored conversation and re-encrypt any plaintext message
    // file in place. Safe to call repeatedly — already-encrypted files are
    // skipped. Progress callback fires per-file (current/total).
    using MigrationProgressCallback = std::function<void(int current, int total, const char* peerHex)>;
    int migratePlaintextMessages(MigrationProgressCallback cb = nullptr);


    // Save a message (incoming or outgoing) — fully non-blocking (<1ms)
    bool saveMessage(LXMFMessage& msg);

    // Load messages for a conversation with pagination (newest last)
    std::vector<LXMFMessage> loadConversation(const std::string& peerHex, int limit = 20, int offset = 0) const;

    // Get list of conversations (peer hex hashes)
    const std::vector<std::string>& conversations() const { return _conversations; }

    // Refresh conversation list from filesystem
    void refreshConversations();

    // Get message count for a conversation
    int messageCount(const std::string& peerHex) const;

    // Delete a conversation
    bool deleteConversation(const std::string& peerHex);

    // Mark all incoming messages in a conversation as read (non-blocking)
    void markConversationRead(const std::string& peerHex);

    // Restore/update outgoing state across reboots and delivery proofs
    std::vector<LXMFMessage> loadPendingOutgoing() const;
    std::vector<std::string> loadRecentMessageIds(size_t maxIds) const;
    bool updateMessageStatus(const std::string& peerHex, double timestamp, bool incoming, LXMFStatus newStatus);
    bool updateMessageStatusByCounter(const std::string& peerHex, uint32_t counter, bool incoming, LXMFStatus newStatus);

    // Get unread count for a peer using read-status file
    int unreadCountForPeer(const std::string& peerHex) const;

    // Access write queue for diagnostics
    WriteQueue& writeQueue() { return _writeQueue; }

    // Get current receive counter
    uint32_t currentReceiveCounter() const { return _nextReceiveCounter; }

    // Global message capacity
    int totalMessageCount() const { return _totalMessageCount; }
    int messageLimit() const;
    bool isFull() const { return _totalMessageCount >= messageLimit(); }
    bool isNearFull() const { return _totalMessageCount >= messageLimit() * RSCARDPUTER_MSG_WARN_PCT / 100; }
    int remainingCapacity() const { return std::max(0, messageLimit() - _totalMessageCount); }

private:
    // Directory token for a conversation: an identity-keyed blind index of
    // peerHex, NOT the hash (or a truncation of it) itself -- see
    // dirTokenForPeer()'s definition for why.
    std::string dirTokenForPeer(const std::string& peerHex) const;
    String conversationDir(const std::string& peerHex) const;
    String sdConversationDir(const std::string& peerHex) const;

    void enforceFlashCache(const std::string& peerHex);
    void enforceFlashCacheDir(const String& dir, const std::string& logLabel);
    void migrateFlashToSD();
    void migrateOldFilenames();
    // One-time (NVS-flagged) rename of every existing conversation
    // directory from its old peerHex-derived name to dirTokenForPeer()'s
    // blind index, for installs that had conversations on disk before that
    // scheme existed. See docs/encryption-known-destinations.md's sibling
    // note in threat-model.md for why the old naming leaked.
    void migrateConversationDirTokens();
    // One-time (NVS-flagged) pass over every existing message file, on both
    // backends, resetting its filesystem mtime/atime to a fixed sentinel —
    // see FlashStore::scrubTimestamp()/SDStore::scrubTimestamp(). Needed
    // because files written before this fix existed already carry a real
    // wall-clock write time from LittleFS/FAT; new writes are scrubbed as
    // they happen (see WriteQueue::processJob()), but that doesn't reach
    // files already on disk.
    void scrubExistingMessageTimestamps();
    // The directory itself is named with an opaque per-peer token (see
    // conversationDir()) -- not derivable back into a destination hash.
    // Every message file inside still carries the full src/dst hex, so peek
    // at one to recover it instead of handing the opaque directory name out
    // to callers that need a real destination hash (e.g. to send a reply).
    std::string resolveConversationPeerHex(const String& dirPath, bool useSD) const;
    void initReceiveCounter();
    void countTotalMessages();
    uint32_t readLastReadCounter(const std::string& peerHex) const;

    // Ensure conversation directories exist (cached — only creates once per peer)
    void ensureConvDirs(const std::string& peerHex);

    FlashStore* _flash = nullptr;
    SDStore* _sd = nullptr;
    const RNS::Identity* _identity = nullptr;
    WriteQueue _writeQueue;
    std::vector<std::string> _conversations;
    std::atomic<uint32_t> _nextReceiveCounter{0};
    int _totalMessageCount = 0;

    // Directory creation cache — avoids repeated ensureDir calls
    std::set<std::string> _ensuredDirs;
};
