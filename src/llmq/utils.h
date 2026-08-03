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

// Predicts the members of a future v20 quorum from its already-known work block, before the
// quorum's own (cycle) base block exists on chain. Returns std::nullopt when V20 is not yet
// active at the work block (the pre-v20 modifier needs the future base block hash) or, for
// rotated types, when the historical snapshots needed for quarter rotation are not available.
std::optional<std::vector<CDeterministicMNCPtr>> ComputeQuorumMembersFromWorkBlock(
    Consensus::LLMQType llmqType, const UtilParameters& util_params, gsl::not_null<const CBlockIndex*> pWorkBlockIndex,
    int quorumHeight);

Uint256HashSet GetQuorumConnections(const Consensus::LLMQParams& llmqParams, const CSporkManager& sporkman,
                                    const UtilParameters& util_params, const uint256& forMember, bool onlyOutbound);

Uint256HashSet GetQuorumRelayMembers(const Consensus::LLMQParams& llmqParams, const UtilParameters& util_params,
                                     const uint256& forMember, bool onlyOutbound);
} // namespace utils
} // namespace llmq

#endif // BITCOIN_LLMQ_UTILS_H
