// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LLMQ_UTILS_H
#define BITCOIN_LLMQ_UTILS_H

#include <bls/bls.h>
#include <evo/types.h>
#include <llmq/params.h>
#include <saltedhasher.h>

#include <chainparams.h>
#include <sync.h>
#include <uint256.h>

#include <gsl/pointers.h>

#include <optional>
#include <unordered_set>
#include <vector>

class CBlockIndex;
class CChainParams;
class CDeterministicMNList;
class CDeterministicMNManager;
class ChainstateManager;
class CSporkManager;
namespace llmq {
class CQuorumSnapshotManager;
} // namespace llmq

namespace llmq {
struct UtilParameters {
    CDeterministicMNManager& m_dmnman;
    CQuorumSnapshotManager& m_qsnapman;
    const ChainstateManager& m_chainman;
    gsl::not_null<const CBlockIndex*> m_base_index;

public:
    UtilParameters replace_index(gsl::not_null<const CBlockIndex*> base_index) const
    {
        return {m_dmnman, m_qsnapman, m_chainman, base_index};
    }
};

namespace utils {
struct BlsCheck {
    CBLSSignature m_sig;
    std::vector<CBLSPublicKey> m_pubkeys;
    uint256 m_msg_hash;
    std::string m_id_string;

public:
    BlsCheck();
    BlsCheck(CBLSSignature sig, std::vector<CBLSPublicKey> pubkeys, uint256 msg_hash, std::string id_string);
    ~BlsCheck();

    bool operator()();
    void swap(BlsCheck& obj);
};

uint256 DeterministicOutboundConnection(const uint256& proTxHash1, const uint256& proTxHash2);

std::unordered_set<size_t> CalcDeterministicWatchConnections(Consensus::LLMQType llmqType,
                                                             gsl::not_null<const CBlockIndex*> pQuorumBaseBlockIndex,
                                                             size_t memberCount, size_t connectionCount);

// includes members which failed DKG
std::vector<CDeterministicMNCPtr> GetAllQuorumMembers(Consensus::LLMQType llmqType, const UtilParameters& util_params,
                                                      bool reset_cache = false);

// Predicts the members of a future non-rotated v20 quorum from its already-known work block,
// before the quorum's own base block exists on chain. Returns std::nullopt when prediction is
// not possible (e.g. V20 not yet active at the work block, or V19 activation state at the
// future quorum base block cannot be confirmed from pDeploymentTipIndex).
std::optional<std::vector<CDeterministicMNCPtr>> ComputeNonRotatedQuorumMembersFromWorkBlock(
    Consensus::LLMQType llmqType, const CChainParams& chainparams, const CDeterministicMNList& mn_list,
    gsl::not_null<const CBlockIndex*> pWorkBlockIndex, int quorumHeight,
    gsl::not_null<const CBlockIndex*> pDeploymentTipIndex);

Uint256HashSet GetQuorumConnections(const Consensus::LLMQParams& llmqParams, const CSporkManager& sporkman,
                                    const UtilParameters& util_params, const uint256& forMember, bool onlyOutbound);

Uint256HashSet GetQuorumRelayMembers(const Consensus::LLMQParams& llmqParams, const UtilParameters& util_params,
                                     const uint256& forMember, bool onlyOutbound);

template <typename CacheType>
inline void InitQuorumsCache(CacheType& cache, const Consensus::Params& consensus_params, bool limit_by_connections = true)
{
    for (const auto& llmq : consensus_params.llmqs) {
        cache.emplace(std::piecewise_construct, std::forward_as_tuple(llmq.type),
                      std::forward_as_tuple(limit_by_connections ? llmq.keepOldConnections : llmq.keepOldKeys));
    }
}
} // namespace utils
} // namespace llmq

#endif // BITCOIN_LLMQ_UTILS_H
