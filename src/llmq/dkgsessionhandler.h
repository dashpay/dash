// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LLMQ_DKGSESSIONHANDLER_H
#define BITCOIN_LLMQ_DKGSESSIONHANDLER_H

#include <net.h> // for NodeId
#include <sync.h>
#include <uint256.h> // PendingMessage stores a uint256 by value

#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

class CDataStream;
class CBlockIndex;

namespace Consensus {
struct LLMQParams;
} // namespace Consensus

namespace llmq
{
class CDKGContribution;
class CDKGComplaint;
class CDKGJustification;
class CDKGPrematureCommitment;
class CDKGSessionManager;

enum class QuorumPhase {
    Initialized = 1,
    Contribute,
    Complain,
    Justify,
    Commit,
    Finalize,
    Idle,
};

//! Upper bound used both for DKG intake and pending-queue byte budgets.
size_t MaxDKGMessageSize(std::string_view msg_type, const Consensus::LLMQParams& params);

/**
 * Acts as a FIFO queue for incoming DKG messages. The reason we need this is that deserialization of these messages
 * is too slow to be processed in the main message handler thread. So, instead of processing them directly from the
 * main handler thread, we push them into a CDKGPendingMessages object and later pop+deserialize them in the DKG phase
 * handler thread.
 *
 * Each message type has it's own instance of this class.
 *
 * Retention is bounded both per NodeId (@ref maxMessagesPerNode) and by serialized
 * payload bytes across all NodeIds (@ref maxPendingBytes). The byte bound is
 * required because NodeId is ephemeral: a reconnecting peer receives a fresh id
 * and would otherwise obtain a fresh full per-node quota forever. @ref RemoveNode
 * drops a disconnected peer's counter and still-queued payloads so live peers can
 * use the capacity.
 *
 * To avoid letting the first peer to fill the queue reserve it indefinitely,
 * overflow evicts the oldest payload belonging to the peer that currently
 * occupies the most bytes. Duplicate hashes are rejected before this policy
 * runs, so a replay cannot evict anything. Locally generated messages
 * (@c from < 0) bypass the bound entirely — they are produced by our own phase
 * handler, are a handful per round, and are the only path by which our own
 * contribution reaches the quorum.
 */
class CDKGPendingMessages
{
public:
    using BinaryMessage = std::pair<NodeId, std::shared_ptr<CDataStream>>;

private:
    struct PendingMessage {
        NodeId from;
        std::shared_ptr<CDataStream> msg;
        size_t bytes;
        //! Content hash as inserted into @ref seenMessages. Stored so eviction
        //! and disconnect cleanup never have to re-hash the payload (which would
        //! put unbounded SHA256 work under ::cs_main via FinalizeNode).
        uint256 hash;
    };

    const size_t maxMessagesPerNode;
    const size_t maxPendingBytes;
    mutable Mutex cs_messages;
    std::list<PendingMessage> pendingMessages GUARDED_BY(cs_messages);
    size_t pendingBytes GUARDED_BY(cs_messages){0};
    //! Per-connection lifetime quota counter. Deliberately not decremented when a
    //! message is popped: the quota is per connection, not per queue slot, so a
    //! peer cannot regain capacity by getting its messages processed. Released
    //! only by @ref RemoveNode and @ref Clear.
    std::map<NodeId, size_t> messagesPerNode GUARDED_BY(cs_messages);
    //! Live queued bytes per NodeId, kept in sync with @ref pendingMessages.
    //! Used to pick the greediest peer on overflow and to skip the list scan in
    //! @ref RemoveNode when the disconnecting peer has nothing queued.
    std::map<NodeId, size_t> queuedBytesPerNode GUARDED_BY(cs_messages);
    Uint256HashSet seenMessages GUARDED_BY(cs_messages);

    //! Erase one queue entry, keeping byte accounting and seenMessages in sync.
    //! Returns the iterator following the erased entry.
    std::list<PendingMessage>::iterator EraseEntry(std::list<PendingMessage>::iterator it)
        EXCLUSIVE_LOCKS_REQUIRED(cs_messages);
    //! Drop the oldest queued message of the peer occupying the most bytes.
    //! Never evicts our own (from < 0) messages. Returns false if nothing could
    //! be evicted (i.e. the queue holds only own messages).
    bool EvictGreediestNode() EXCLUSIVE_LOCKS_REQUIRED(cs_messages);

public:
    explicit CDKGPendingMessages(size_t _maxMessagesPerNode, size_t _maxPendingBytes) :
        maxMessagesPerNode(_maxMessagesPerNode),
        maxPendingBytes(_maxPendingBytes) {};

    /**
     * Enqueue a serialized DKG message under @p from with content hash @p hash.
     * Caller is responsible for hashing the payload and (for real peers)
     * routing the erase-request to PeerManager. Drops the message silently on
     * per-node capacity overflow, an individually over-budget payload, or duplicate
     * hash. On queue-wide byte-capacity overflow, the greediest peer is evicted
     * instead (see the class comment).
     */
    void PushPendingMessage(NodeId from, std::shared_ptr<CDataStream> pm, const uint256& hash)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_messages);

    std::list<BinaryMessage> PopPendingMessages(size_t maxCount) EXCLUSIVE_LOCKS_REQUIRED(!cs_messages);
    bool HasSeen(const uint256& hash) const EXCLUSIVE_LOCKS_REQUIRED(!cs_messages);
    void Clear() EXCLUSIVE_LOCKS_REQUIRED(!cs_messages);

    /**
     * Drop @p nodeId's per-node counter, any still-queued payloads from that
     * peer, and the corresponding content hashes in @ref seenMessages. Called
     * on disconnect so a reconnecting attacker cannot pin memory (or grow
     * seenMessages) under abandoned NodeIds.
     *
     * Only *un-processed* messages are dropped. Anything already popped by the
     * phase handler has been accepted into the DKG session state and is
     * unaffected, so disconnecting a peer never invalidates a contribution the
     * session already took — the DKG completes as before.
     */
    void RemoveNode(NodeId nodeId) EXCLUSIVE_LOCKS_REQUIRED(!cs_messages);

    //! Number of still-queued binary messages (for tests / diagnostics).
    size_t Size() const EXCLUSIVE_LOCKS_REQUIRED(!cs_messages);
    //! Serialized payload bytes currently retained for remote peers.
    size_t SizeBytes() const EXCLUSIVE_LOCKS_REQUIRED(!cs_messages);

    // Might return nullptr messages, which indicates that deserialization failed for some reason
    template <typename Message>
    std::vector<std::pair<NodeId, std::shared_ptr<Message>>> PopAndDeserializeMessages(size_t maxCount)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_messages)
    {
        auto binaryMessages = PopPendingMessages(maxCount);
        if (binaryMessages.empty()) {
            return {};
        }

        std::vector<std::pair<NodeId, std::shared_ptr<Message>>> ret;
        ret.reserve(binaryMessages.size());
        for (const auto& bm : binaryMessages) {
            auto msg = std::make_shared<Message>();
            try {
                *bm.second >> *msg;
            } catch (...) {
                msg = nullptr;
            }
            ret.emplace_back(std::make_pair(bm.first, std::move(msg)));
        }

        return ret;
    }
};

/**
 * Handles multiple sequential sessions of one specific LLMQ type. There is one instance of this class per LLMQ type.
 */
class CDKGSessionHandler
{
public:
    const Consensus::LLMQParams& params;

    // Do not guard these, they protect their internals themselves
    CDKGPendingMessages pendingContributions;
    CDKGPendingMessages pendingComplaints;
    CDKGPendingMessages pendingJustifications;
    CDKGPendingMessages pendingPrematureCommitments;

public:
    explicit CDKGSessionHandler(const Consensus::LLMQParams& _params);
    virtual ~CDKGSessionHandler();

    void ClearPendingMessages();
    void RemoveNode(NodeId nodeId);

public:
    virtual bool GetContribution(const uint256& hash, CDKGContribution& ret) const { return false; }
    virtual bool GetComplaint(const uint256& hash, CDKGComplaint& ret) const { return false; }
    virtual bool GetJustification(const uint256& hash, CDKGJustification& ret) const { return false; }
    virtual bool GetPrematureCommitment(const uint256& hash, CDKGPrematureCommitment& ret) const { return false; }
    virtual QuorumPhase GetPhase() const { return QuorumPhase::Idle; }
    virtual void UpdatedBlockTip(const CBlockIndex* pindexNew) {}
};
} // namespace llmq

#endif // BITCOIN_LLMQ_DKGSESSIONHANDLER_H
