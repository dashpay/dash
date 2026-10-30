// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LLMQ_UTILS_H
#define BITCOIN_LLMQ_UTILS_H

#include <bls/bls.h>
#include <evo/types.h>
#include <llmq/params.h>
#include <saltedhasher.h>
#include <span.h>

#include <chainparams.h>
#include <sync.h>
#include <uint256.h>
#include <versionbits.h>

#include <gsl/pointers.h>

#include <array>
#include <functional>
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
using QuorumMembers = std::vector<CDeterministicMNCPtr>;

struct QuarterWalkResult {
    //! Candidate positions chosen for each quorum's new quarter
    std::vector<std::vector<size_t>> quarters;
    //! Skip list in the format stored in CQuorumSnapshot
    std::vector<int> skip_list;
};

//! Legacy (pre-quarter_steps) builder loop over positions; returns quarters and the legacy skip list (incl. the
//! firstSkippedIndex == 0 quirk); nullopt where the legacy code returns empty quarters early.
std::optional<QuarterWalkResult> WalkLegacyQuarters(size_t list_size, size_t quorum_count, size_t quarter_size,
                                                    const std::vector<size_t>& used_counts,
                                                    const std::function<bool(size_t, size_t)>& can_add);
//! Legacy (pre-quarter_steps) MODE_SKIPPING_ENTRIES / MODE_NO_SKIPPING rebuild loop over positions.
std::vector<std::vector<size_t>> ReplayLegacyQuarters(size_t list_size, size_t quorum_count, size_t quarter_size,
                                                       Span<const int> skip_list);

//! Step-encoded walk: skip_list holds the steps (visits across all quorum indices) at which a candidate was
//! skipped, the first absolute and each later one as a positive delta. can_add(quorum_index, position) returns
//! true if the candidate joins that index and must record the addition. nullopt if a delta doesn't fit in int.
std::optional<QuarterWalkResult> WalkNewQuarters(size_t list_size, size_t quorum_count, size_t quarter_size,
                                                 const std::function<bool(size_t, size_t)>& can_add);
//! Inverse of WalkNewQuarters; nullopt if skip_list is malformed, not fully consumed or would repeat a
//! position within a quarter.
std::optional<std::vector<std::vector<size_t>>> ReplayNewQuarters(size_t list_size, size_t quorum_count,
                                                                  size_t quarter_size, Span<const int> skip_list);

//! Whether the cycle starting at cycle_base builds and rebuilds its quarter with WalkNewQuarters/ReplayNewQuarters
bool QuarterStepsActive(const CBlockIndex& cycle_base, const ChainstateManager& chainman);
//! Maps the DEPLOYMENT_QUARTER_STEPS state of the block after the work block to the rule at the cycle base
//! WORK_DIFF_DEPTH blocks later, given the deployment's period: true or false if no sequence of state changes
//! at the period starts in between can change it, nullopt (unknown) otherwise. ACTIVE -> true; FAILED ->
//! false; DEFINED, STARTED -> false unless the period is short enough to reach ACTIVE; LOCKED_IN -> nullopt.
std::optional<bool> QuarterStepsRuleForPrediction(ThresholdState state_after_work_block, int period);
//! QuarterStepsRuleForPrediction for the cycle whose work block is work_block
std::optional<bool> QuarterStepsRuleForPrediction(const CBlockIndex& work_block, const ChainstateManager& chainman);

//! Per quorum index, the four quarters in assembly order (H-3C, H-2C, H-C, new). Empty outer vector when
//! the cycle can't be computed (prediction unknown or encoder overflow). GetAllQuorumMembers flattens this.
std::vector<std::array<QuorumMembers, 4>> ComputeRotationQuarters(const Consensus::LLMQParams& llmqParams,
                                                                  const UtilParameters& util_params,
                                                                  gsl::not_null<const CBlockIndex*> pWorkBlockIndex,
                                                                  int cycleBaseHeight, const uint256& modifier,
                                                                  bool predicting);

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
    void swap(BlsCheck& obj) noexcept;
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
