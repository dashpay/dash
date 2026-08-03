// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/dkgsessionhandler.h>

#include <bls/bls.h>
#include <llmq/params.h>
#include <logging.h>
#include <protocol.h>
#include <streams.h>
#include <uint256.h>

#include <algorithm>
#include <optional>
#include <stdexcept>

namespace llmq {
size_t MaxDKGMessageSize(std::string_view msg_type, const Consensus::LLMQParams& params)
{
    constexpr size_t COMPACT{5};
    constexpr size_t PREFIX{1 + 32 + 32};
    constexpr size_t PUBKEY{BLS_CURVE_PUBKEY_SIZE};
    constexpr size_t SIG{BLS_CURVE_SIG_SIZE};
    constexpr size_t SECKEY{BLS_CURVE_SECKEY_SIZE};
    constexpr size_t BLOB{COMPACT + 128};
    constexpr size_t SLACK{1024};
    constexpr size_t HARD_CEILING{size_t{1} << 20};

    const size_t size = params.size > 0 ? static_cast<size_t>(params.size) : 0;
    const size_t threshold = params.threshold > 0 ? static_cast<size_t>(params.threshold) : 0;

    size_t cap{0};
    if (msg_type == NetMsgType::QCONTRIB) {
        cap = PREFIX + (COMPACT + threshold * PUBKEY) + (PUBKEY + 32 + COMPACT + size * BLOB) + SIG;
    } else if (msg_type == NetMsgType::QJUSTIFICATION) {
        cap = PREFIX + (COMPACT + size * (4 + SECKEY)) + SIG;
    } else if (msg_type == NetMsgType::QCOMPLAINT) {
        cap = PREFIX + 2 * (COMPACT + (size + 7) / 8) + SIG;
    } else if (msg_type == NetMsgType::QPCOMMITMENT) {
        cap = PREFIX + (COMPACT + (size + 7) / 8) + PUBKEY + 32 + 2 * SIG;
    } else {
        return HARD_CEILING;
    }
    cap += SLACK;
    return std::min(cap, HARD_CEILING);
}

namespace {
size_t MaxMessagesPerNode(const Consensus::LLMQParams& params)
{
    return params.size > 0 ? static_cast<size_t>(params.size) * 2 : 0;
}

size_t MaxPendingBytes(std::string_view msg_type, const Consensus::LLMQParams& params)
{
    // A round needs at most one message per member. Keep the existing doubled
    // allowance for equivocation evidence, but apply it once to the whole queue
    // instead of once for each ephemeral NodeId.
    return MaxMessagesPerNode(params) * MaxDKGMessageSize(msg_type, params);
}
} // namespace

CDKGSessionHandler::CDKGSessionHandler(const Consensus::LLMQParams& _params) :
    params{_params},
    // we allow size*2 messages as we need to make sure we see bad behavior (double messages)
    pendingContributions{MaxMessagesPerNode(_params), MaxPendingBytes(NetMsgType::QCONTRIB, _params)},
    pendingComplaints{MaxMessagesPerNode(_params), MaxPendingBytes(NetMsgType::QCOMPLAINT, _params)},
    pendingJustifications{MaxMessagesPerNode(_params), MaxPendingBytes(NetMsgType::QJUSTIFICATION, _params)},
    pendingPrematureCommitments{MaxMessagesPerNode(_params), MaxPendingBytes(NetMsgType::QPCOMMITMENT, _params)}
{
    if (params.type == Consensus::LLMQType::LLMQ_NONE) {
        throw std::runtime_error("Can't initialize CDKGSessionHandler with LLMQ_NONE type.");
    }
}

CDKGSessionHandler::~CDKGSessionHandler() = default;

std::list<CDKGPendingMessages::PendingMessage>::iterator CDKGPendingMessages::EraseEntry(
    std::list<PendingMessage>::iterator it)
{
    seenMessages.erase(it->hash);
    if (it->from >= 0) pendingBytes -= it->bytes;
    if (auto qit = queuedBytesPerNode.find(it->from); qit != queuedBytesPerNode.end()) {
        qit->second -= it->bytes;
        if (qit->second == 0) queuedBytesPerNode.erase(qit);
    }
    return pendingMessages.erase(it);
}

bool CDKGPendingMessages::EvictGreediestNode()
{
    // Prefer the peer pinning the most payload memory.
    std::optional<NodeId> victim;
    size_t victim_bytes{0};
    for (const auto& [node, bytes] : queuedBytesPerNode) {
        if (node < 0) continue; // never evict our own messages
        if (bytes > victim_bytes) {
            victim = node;
            victim_bytes = bytes;
        }
    }
    if (!victim.has_value()) return false;

    // Drop that peer's oldest message: it is the least likely to still be
    // relevant to the current phase.
    for (auto it = pendingMessages.begin(); it != pendingMessages.end(); ++it) {
        if (it->from == *victim) {
            LogPrint(BCLog::LLMQ_DKG, "CDKGPendingMessages::%s -- byte cap reached (%d), evicting oldest of peer=%d (%d bytes queued)\n",
                     __func__, maxPendingBytes, *victim, victim_bytes);
            EraseEntry(it);
            return true;
        }
    }
    return false;
}

void CDKGPendingMessages::PushPendingMessage(NodeId from, std::shared_ptr<CDataStream> pm, const uint256& hash)
{
    LOCK(cs_messages);

    if (pm == nullptr) return;

    // Our own messages (from < 0) are produced by our phase handler, are a
    // handful per round, and are the only way our contribution reaches the
    // quorum. They must never be dropped by a peer-driven bound.
    const bool is_own = from < 0;

    // A duplicate must be side-effect free. In particular, it must not consume
    // the per-peer quota or evict a different peer from a full queue.
    if (seenMessages.count(hash) != 0) {
        LogPrint(BCLog::LLMQ_DKG, "CDKGPendingMessages::%s -- already seen %s, peer=%d\n", __func__, hash.ToString(), from);
        return;
    }

    if (!is_own && messagesPerNode[from] >= maxMessagesPerNode) {
        // TODO ban?
        LogPrint(BCLog::LLMQ_DKG, "CDKGPendingMessages::%s -- too many messages, peer=%d\n", __func__, from);
        return;
    }

    const size_t bytes = pm->size();
    if (!is_own && (bytes == 0 || bytes > maxPendingBytes)) {
        LogPrint(BCLog::LLMQ_DKG, "CDKGPendingMessages::%s -- invalid message size (%d, cap %d), peer=%d\n",
                 __func__, bytes, maxPendingBytes, from);
        return;
    }

    // NodeId is ephemeral, so the queue-wide retention guarantee must be
    // independent of how many times a peer reconnects. Account actual payload
    // bytes and evict until the new message fits.
    while (!is_own && pendingBytes > maxPendingBytes - bytes) {
        if (!EvictGreediestNode()) {
            return;
        }
    }

    seenMessages.emplace(hash);
    pendingMessages.emplace_back(PendingMessage{from, std::move(pm), bytes, hash});
    if (!is_own) {
        messagesPerNode[from]++;
        pendingBytes += bytes;
        queuedBytesPerNode[from] += bytes;
    }
}

std::list<CDKGPendingMessages::BinaryMessage> CDKGPendingMessages::PopPendingMessages(size_t maxCount)
{
    LOCK(cs_messages);

    std::list<BinaryMessage> ret;
    while (!pendingMessages.empty() && ret.size() < maxCount) {
        auto& front = pendingMessages.front();
        ret.emplace_back(front.from, std::move(front.msg));
        // Popped messages are handed to the DKG session; their content hash stays
        // in seenMessages so AlreadyHave() keeps suppressing re-requests.
        if (front.from >= 0) {
            pendingBytes -= front.bytes;
        }
        if (auto qit = queuedBytesPerNode.find(front.from); qit != queuedBytesPerNode.end()) {
            qit->second -= front.bytes;
            if (qit->second == 0) queuedBytesPerNode.erase(qit);
        }
        pendingMessages.pop_front();
    }

    return ret;
}

bool CDKGPendingMessages::HasSeen(const uint256& hash) const
{
    LOCK(cs_messages);
    return seenMessages.count(hash) != 0;
}

void CDKGPendingMessages::Clear()
{
    LOCK(cs_messages);
    pendingMessages.clear();
    pendingBytes = 0;
    messagesPerNode.clear();
    queuedBytesPerNode.clear();
    seenMessages.clear();
}

void CDKGPendingMessages::RemoveNode(NodeId nodeId)
{
    // Own/local enqueues use from=-1 and are not tied to a peer disconnect.
    if (nodeId < 0) {
        return;
    }

    LOCK(cs_messages);
    messagesPerNode.erase(nodeId);

    // Runs under ::cs_main (via PeerManagerImpl::FinalizeNode), so skip the list
    // scan entirely for the overwhelmingly common case of a peer that never
    // queued a DKG message.
    if (queuedBytesPerNode.find(nodeId) == queuedBytesPerNode.end()) {
        return;
    }

    for (auto it = pendingMessages.begin(); it != pendingMessages.end();) {
        if (it->from == nodeId) {
            // Free the content-hash slot too; otherwise a reconnecting attacker
            // can grow seenMessages without bound even after payloads are dropped
            // (especially in observer mode where Clear() never runs). The hash is
            // stored alongside the payload, so no re-hashing happens here.
            it = EraseEntry(it);
        } else {
            ++it;
        }
    }
}

size_t CDKGPendingMessages::Size() const
{
    LOCK(cs_messages);
    return pendingMessages.size();
}

size_t CDKGPendingMessages::SizeBytes() const
{
    LOCK(cs_messages);
    return pendingBytes;
}

void CDKGSessionHandler::ClearPendingMessages()
{
    pendingContributions.Clear();
    pendingComplaints.Clear();
    pendingJustifications.Clear();
    pendingPrematureCommitments.Clear();
}

void CDKGSessionHandler::RemoveNode(NodeId nodeId)
{
    pendingContributions.RemoveNode(nodeId);
    pendingComplaints.RemoveNode(nodeId);
    pendingJustifications.RemoveNode(nodeId);
    pendingPrematureCommitments.RemoveNode(nodeId);
}
} // namespace llmq
