// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LLMQ_DKGSESSIONHANDLER_H
#define BITCOIN_LLMQ_DKGSESSIONHANDLER_H

#include <net.h> // for NodeId
#include <sync.h>
#include <uint256.h>

#include <list>
#include <map>
#include <memory>
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

/**
 * Acts as a FIFO queue for incoming DKG messages. The reason we need this is that deserialization of these messages
 * is too slow to be processed in the main message handler thread. So, instead of processing them directly from the
 * main handler thread, we push them into a CDKGPendingMessages object and later pop+deserialize them in the DKG phase
 * handler thread.
 *
 * Each message type has its own instance of this class.
 */
class CDKGPendingMessages
{
public:
    using BinaryMessage = std::pair<NodeId, std::shared_ptr<CDataStream>>;

private:
    const size_t maxMessagesPerSender;
    const size_t maxPendingRemoteMessages;
    const size_t maxPendingMessages;
    mutable Mutex cs_messages;
    std::list<BinaryMessage> pendingMessages GUARDED_BY(cs_messages);
    size_t pendingRemoteMessageCount GUARDED_BY(cs_messages){0};
    // Keyed by the sender's MNAuth-verified proTxHash, not by NodeId: a peer that
    // reconnects gets a fresh NodeId but keeps the same proTxHash, so its quota
    // survives the reconnect instead of being reset.
    //
    // Entries deliberately live for the whole round rather than being released on
    // pop: the quota is cumulative, so that a sender cannot regain retention slots
    // simply by waiting for the worker to drain the queue. Size is therefore not
    // bounded by the queue caps but by the number of distinct senders that get a
    // message accepted in one round, which MNAuth pins to the registered
    // masternode set (see CMNAuth::ProcessMessage: the proTxHash must resolve in
    // the deterministic MN list and carry a valid operator-key signature).
    std::map<uint256, size_t> messagesPerSender GUARDED_BY(cs_messages);
    Uint256HashSet seenMessages GUARDED_BY(cs_messages);

public:
    explicit CDKGPendingMessages(size_t _maxMessagesPerSender) :
        maxMessagesPerSender(_maxMessagesPerSender),
        // Belt-and-braces bound on live queue occupancy. The per-sender quota is
        // the primary limit; this only caps the total across distinct senders.
        maxPendingRemoteMessages(_maxMessagesPerSender * 2),
        // Reserve one slot for the message produced by this node during the
        // matching phase.
        maxPendingMessages(maxPendingRemoteMessages + 1)
    {
    }

    /**
     * Enqueue a serialized DKG message under @p from with content hash @p hash.
     * @p sender_protx is the sender's MNAuth-verified proTxHash and keys the
     * per-sender quota; pass a null hash for messages this node produced itself
     * (@p from == -1), which are exempt from that quota.
     * Caller is responsible for hashing the payload and (for real peers)
     * routing the erase-request to PeerManager. Drops the message silently on
     * per-sender or queue-wide capacity overflow, or duplicate hash.
     */
    void PushPendingMessage(NodeId from, const uint256& sender_protx, std::shared_ptr<CDataStream> pm,
                            const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(!cs_messages);

    std::list<BinaryMessage> PopPendingMessages(size_t maxCount) EXCLUSIVE_LOCKS_REQUIRED(!cs_messages);
    bool HasSeen(const uint256& hash) const EXCLUSIVE_LOCKS_REQUIRED(!cs_messages);
    void Clear() EXCLUSIVE_LOCKS_REQUIRED(!cs_messages);
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
