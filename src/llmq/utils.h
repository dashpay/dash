// Copyright (c) 2018-2021 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LLMQ_UTILS_H
#define BITCOIN_LLMQ_UTILS_H

#include <random.h>
#include <versionbits.h>
#include <sync.h>

#include <vector>
#include <memory>

class CConnman;
class CBlockIndex;
class CDeterministicMN;
class CDeterministicMNList;
using CDeterministicMNCPtr = std::shared_ptr<const CDeterministicMN>;
class CBLSPublicKey;

namespace llmq
{

// If true, we will connect to all new quorums and watch their communication
static constexpr bool DEFAULT_WATCH_QUORUMS{false};


class CQuorumSnapshot;

// Use a separate cache instance instead of versionbitscache to avoid locking cs_main
// and dealing with all kinds of deadlocks.
extern CCriticalSection cs_llmq_vbc;
extern VersionBitsCache llmq_versionbitscache GUARDED_BY(cs_llmq_vbc);

static const bool DEFAULT_ENABLE_QUORUM_DATA_RECOVERY = true;

enum class QvvecSyncMode {
    Invalid = -1,
    Always = 0,
    OnlyIfTypeMember = 1,
};

class CLLMQUtils
{
public:
    static uint256 BuildCommitmentHash(Consensus::LLMQType llmqType, const uint256& blockHash, const std::vector<bool>& validMembers, const CBLSPublicKey& pubKey, const uint256& vvecHash);
    static uint256 BuildSignHash(Consensus::LLMQType llmqType, const uint256& quorumHash, const uint256& id, const uint256& msgHash);

    static bool IsAllMembersConnectedEnabled(Consensus::LLMQType llmqType);
    static bool IsQuorumPoseEnabled(Consensus::LLMQType llmqType);
    static uint256 DeterministicOutboundConnection(const uint256& proTxHash1, const uint256& proTxHash2);


    static bool IsQuorumRotationEnabled(Consensus::LLMQType llmqType, const CBlockIndex* pindex);
    static Consensus::LLMQType GetInstantSendLLMQType(bool deterministic);
    static bool IsDIP0024Active(const CBlockIndex* pindex);

    /// Returns the state of `-llmq-data-recovery`
    static bool QuorumDataRecoveryEnabled();

    /// Returns the state of `-watchquorums`
    static bool IsWatchQuorumsEnabled();

    /// Returns the parsed entries given by `-llmq-qvvec-sync`
    static std::map<Consensus::LLMQType, QvvecSyncMode> GetEnabledQuorumVvecSyncEntries();

    template<typename NodesContainer, typename Continue, typename Callback>
    static void IterateNodesRandom(NodesContainer& nodeStates, Continue&& cont, Callback&& callback, FastRandomContext& rnd)
    {
        std::vector<typename NodesContainer::iterator> rndNodes;
        rndNodes.reserve(nodeStates.size());
        for (auto it = nodeStates.begin(); it != nodeStates.end(); ++it) {
            rndNodes.emplace_back(it);
        }
        if (rndNodes.empty()) {
            return;
        }
        Shuffle(rndNodes.begin(), rndNodes.end(), rnd);

        size_t idx = 0;
        while (!rndNodes.empty() && cont()) {
            auto nodeId = rndNodes[idx]->first;
            auto& ns = rndNodes[idx]->second;

            if (callback(nodeId, ns)) {
                idx = (idx + 1) % rndNodes.size();
            } else {
                rndNodes.erase(rndNodes.begin() + idx);
                if (rndNodes.empty()) {
                    break;
                }
                idx %= rndNodes.size();
            }
        }
    }
};

const Consensus::LLMQParams& GetLLMQParams(Consensus::LLMQType llmqType);

} // namespace llmq

#endif // BITCOIN_LLMQ_UTILS_H
