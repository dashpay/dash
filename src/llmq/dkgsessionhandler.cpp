// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/dkgsessionhandler.h>

#include <logging.h>
#include <uint256.h>

#include <stdexcept>

namespace llmq {
CDKGSessionHandler::CDKGSessionHandler(const Consensus::LLMQParams& _params) :
    params{_params},
    // we allow size*2 messages per sender as we need to make sure we see bad behavior (double messages)
    pendingContributions{(size_t)_params.size * 2},
    pendingComplaints{(size_t)_params.size * 2},
    pendingJustifications{(size_t)_params.size * 2},
    pendingPrematureCommitments{(size_t)_params.size * 2}
{
    if (params.type == Consensus::LLMQType::LLMQ_NONE) {
        throw std::runtime_error("Can't initialize CDKGSessionHandler with LLMQ_NONE type.");
    }
}

CDKGSessionHandler::~CDKGSessionHandler() = default;

void CDKGPendingMessages::PushPendingMessage(NodeId from, const uint256& sender_protx,
                                             std::shared_ptr<CDataStream> pm, const uint256& hash)
{
    LOCK(cs_messages);

    // Check duplicates before charging the per-sender quota so a peer that
    // resends the same hash cannot exhaust its budget with dupes.
    if (seenMessages.count(hash) != 0) {
        LogPrint(BCLog::LLMQ_DKG, "CDKGPendingMessages::%s -- already seen %s, peer=%d\n", __func__, hash.ToString(), from);
        return;
    }

    const bool is_remote = from != -1;
    if (is_remote) {
        const auto sender_it = messagesPerSender.find(sender_protx);
        if (sender_it != messagesPerSender.end() && sender_it->second >= maxMessagesPerSender) {
            // TODO ban?
            LogPrint(BCLog::LLMQ_DKG, "CDKGPendingMessages::%s -- too many messages from %s, peer=%d\n", __func__,
                     sender_protx.ToString(), from);
            return;
        }
    }

    if ((is_remote && pendingRemoteMessageCount >= maxPendingRemoteMessages) ||
        pendingMessages.size() >= maxPendingMessages) {
        LogPrint(BCLog::LLMQ_DKG, "CDKGPendingMessages::%s -- pending queue full, peer=%d\n", __func__, from);
        return;
    }
    if (is_remote) {
        messagesPerSender[sender_protx]++;
        pendingRemoteMessageCount++;
    }

    seenMessages.emplace(hash);
    pendingMessages.emplace_back(std::make_pair(from, std::move(pm)));
}

std::list<CDKGPendingMessages::BinaryMessage> CDKGPendingMessages::PopPendingMessages(size_t maxCount)
{
    LOCK(cs_messages);

    std::list<BinaryMessage> ret;
    while (!pendingMessages.empty() && ret.size() < maxCount) {
        if (pendingMessages.front().first != -1) {
            pendingRemoteMessageCount--;
        }
        ret.emplace_back(std::move(pendingMessages.front()));
        pendingMessages.pop_front();
    }

    return ret;
}

void CDKGPendingMessages::Clear()
{
    LOCK(cs_messages);
    pendingMessages.clear();
    pendingRemoteMessageCount = 0;
    messagesPerSender.clear();
    seenMessages.clear();
}

bool CDKGPendingMessages::HasSeen(const uint256& hash) const
{
    LOCK(cs_messages);
    return seenMessages.count(hash) != 0;
}

void CDKGSessionHandler::ClearPendingMessages()
{
    pendingContributions.Clear();
    pendingComplaints.Clear();
    pendingJustifications.Clear();
    pendingPrematureCommitments.Clear();
}
} // namespace llmq
