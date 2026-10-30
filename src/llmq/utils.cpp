// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/utils.h>

#include <bls/bls.h>
#include <evo/deterministicmns.h>
#include <llmq/cache.h>
#include <llmq/options.h>
#include <llmq/snapshot.h>
#include <llmq/types.h>
#include <masternode/meta.h>
#include <util/helpers.h>
#include <util/std23.h>

#include <chainparams.h>
#include <deploymentstatus.h>
#include <random.h>
#include <util/time.h>
#include <validation.h>

#include <atomic>
#include <functional>
#include <limits>
#include <map>
#include <optional>

/**
 * Forward declarations
 */
std::optional<std::pair<CBLSSignature, uint32_t>> GetNonNullCoinbaseChainlock(const CBlockIndex* pindex);

namespace {
using llmq::utils::QuorumMembers;

std::string ToString(const QuorumMembers& members)
{
    std::stringstream ss;
    for (const auto& mn : members) {
        ss << mn->proTxHash.ToString().substr(0, 4) << "|";
    }
    return ss.str();
}

struct MasternodeScore {
    arith_uint256 m_score;
    CDeterministicMNCPtr m_node;
};

struct QuorumQuarter : public llmq::CycleBase {
    std::vector<QuorumMembers> m_members;

public:
    explicit QuorumQuarter(size_t size) : m_members(size) {}
};

// QuorumMembers per quorumIndex at heights H-Cycle, H-2Cycles, H-3Cycles
struct PreviousQuorumQuarters {
    QuorumQuarter quarterHMinusC;
    QuorumQuarter quarterHMinus2C;
    QuorumQuarter quarterHMinus3C;

public:
    explicit PreviousQuorumQuarters(size_t s) : quarterHMinusC(s), quarterHMinus2C(s), quarterHMinus3C(s) {}

    std::vector<QuorumQuarter*> GetCycles() { return {&quarterHMinusC, &quarterHMinus2C, &quarterHMinus3C}; }
    std::vector<const QuorumQuarter*> GetCycles() const
    {
        return {&quarterHMinusC, &quarterHMinus2C, &quarterHMinus3C};
    }
};

arith_uint256 calculateQuorumScore(const CDeterministicMNCPtr& dmn, const uint256& modifier)
{
    // calculate sha256(sha256(proTxHash, confirmedHash), modifier) per MN
    // Please note that this is not a double-sha256 but a single-sha256
    // The first part is already precalculated (confirmedHashWithProRegTxHash)
    // TODO When https://github.com/bitcoin/bitcoin/pull/13191 gets backported, implement something that is similar but for single-sha256
    uint256 h;
    CSHA256 sha256;
    sha256.Write(dmn->pdmnState->confirmedHashWithProRegTxHash.begin(),
                 dmn->pdmnState->confirmedHashWithProRegTxHash.size());
    sha256.Write(modifier.begin(), modifier.size());
    sha256.Finalize(h.begin());
    return UintToArith256(h);
}

uint256 GetHashModifierFromWorkBlock(const Consensus::LLMQParams& llmqParams, const CBlockIndex* pWorkBlockIndex)
{
    auto cbcl = GetNonNullCoinbaseChainlock(pWorkBlockIndex);
    if (cbcl.has_value()) {
        // We have a non-null CL signature: calculate modifier using this CL signature
        auto& [bestCLSignature, bestCLHeightDiff] = cbcl.value();
        return ::SerializeHash(std::make_tuple(llmqParams.type, pWorkBlockIndex->nHeight, bestCLSignature));
    }
    // No non-null CL signature found in coinbase: calculate modifier using block hash only
    return ::SerializeHash(std::make_pair(llmqParams.type, pWorkBlockIndex->GetBlockHash()));
}

uint256 GetHashModifier(const Consensus::LLMQParams& llmqParams, const Consensus::Params& consensus_params,
                        gsl::not_null<const CBlockIndex*> pCycleQuorumBaseBlockIndex)
{
    ASSERT_IF_DEBUG(pCycleQuorumBaseBlockIndex->nHeight % llmqParams.dkgInterval == 0);
    const CBlockIndex* pWorkBlockIndex = pCycleQuorumBaseBlockIndex->GetAncestor(pCycleQuorumBaseBlockIndex->nHeight - llmq::WORK_DIFF_DEPTH);

    if (DeploymentActiveAfter(pWorkBlockIndex, consensus_params, Consensus::DEPLOYMENT_V20)) {
        // v20 is active: calculate modifier using the new way.
        return GetHashModifierFromWorkBlock(llmqParams, pWorkBlockIndex);
    }

    // v20 isn't active yet: calculate modifier using the usual way
    if (llmqParams.useRotation) {
        return ::SerializeHash(std::make_pair(llmqParams.type, pWorkBlockIndex->GetBlockHash()));
    }
    return ::SerializeHash(std::make_pair(llmqParams.type, pCycleQuorumBaseBlockIndex->GetBlockHash()));
}

std::vector<MasternodeScore> CalculateScoresForQuorum(QuorumMembers&& dmns, const uint256& modifier, const bool onlyEvoNodes)
{
    std::vector<MasternodeScore> scores;
    scores.reserve(dmns.size());

    for (auto& dmn : dmns) {
        if (dmn->pdmnState->IsBanned()) continue;
        if (dmn->pdmnState->confirmedHash.IsNull()) {
            // we only take confirmed MNs into account to avoid hash grinding on the ProRegTxHash to sneak MNs into a
            // future quorums
            continue;
        }
        if (onlyEvoNodes && dmn->nType != MnType::Evo) {
            continue;
        }
        scores.emplace_back(calculateQuorumScore(dmn, modifier), std::move(dmn));
    };
    return scores;
}

std::vector<MasternodeScore> CalculateScoresForQuorum(const CDeterministicMNList& mn_list, const uint256& modifier,
                                                      const bool onlyEvoNodes)
{
    std::vector<MasternodeScore> scores;
    scores.reserve(mn_list.GetCounts().total());

    mn_list.ForEachMNShared(/*onlyValid=*/true, [&](const auto& dmn) {
        if (dmn->pdmnState->confirmedHash.IsNull()) {
            // we only take confirmed MNs into account to avoid hash grinding on the ProRegTxHash to sneak MNs into a
            // future quorums
            return;
        }
        if (onlyEvoNodes && dmn->nType != MnType::Evo) {
            return;
        }
        scores.emplace_back(calculateQuorumScore(dmn, modifier), dmn);
    });
    return scores;
}

/**
 * Calculate a quorum based on the modifier. The resulting list is deterministically sorted by score
 */
template <typename List>
QuorumMembers CalculateQuorum(List&& mn_list, const uint256& modifier, size_t maxSize = 0, const bool onlyEvoNodes = false)
{
    auto scores = CalculateScoresForQuorum(std::forward<List>(mn_list), modifier, onlyEvoNodes);

    // sort is descending order
    std::sort(scores.rbegin(), scores.rend(), [](const MasternodeScore& a, const MasternodeScore& b) {
        if (a.m_score == b.m_score) {
            // this should actually never happen, but we should stay compatible with how the non-deterministic MNs did the sorting
            // TODO - add assert ?
            return a.m_node->collateralOutpoint < b.m_node->collateralOutpoint;
        }
        return a.m_score < b.m_score;
    });

    // return top maxSize entries only (if specified)
    if (maxSize > 0 && scores.size() > maxSize) {
        scores.resize(maxSize);
    }

    QuorumMembers result;
    result.reserve(scores.size());
    for (auto& [_, node] : scores) {
        result.emplace_back(std::move(node));
    }
    return result;
}

std::vector<QuorumMembers> GetQuorumQuarterMembersBySnapshot(const Consensus::LLMQParams& llmqParams,
                                                             CDeterministicMNManager& dmnman,
                                                             const Consensus::Params& consensus_params,
                                                             const CBlockIndex* pCycleQuorumBaseBlockIndex,
                                                             const llmq::CQuorumSnapshot& snapshot, int nHeight,
                                                             bool quarter_steps)
{
    if (!llmqParams.useRotation || pCycleQuorumBaseBlockIndex->nHeight % llmqParams.dkgInterval != 0) {
        ASSERT_IF_DEBUG(false);
        return {};
    }

    std::vector<CDeterministicMNCPtr> sortedCombinedMns;
    {
        const CBlockIndex* pWorkBlockIndex = pCycleQuorumBaseBlockIndex->GetAncestor(
            pCycleQuorumBaseBlockIndex->nHeight - llmq::WORK_DIFF_DEPTH);
        auto mn_list = dmnman.GetListForBlock(pWorkBlockIndex);
        const auto modifier = GetHashModifier(llmqParams, consensus_params, pCycleQuorumBaseBlockIndex);
        auto sortedAllMns = CalculateQuorum(mn_list, modifier);

        std::vector<CDeterministicMNCPtr> usedMNs;
        size_t i{0};
        for (const auto& dmn : sortedAllMns) {
            if (snapshot.activeQuorumMembers[i]) {
                usedMNs.push_back(dmn);
            } else {
                if (!dmn->pdmnState->IsBanned()) {
                    // the list begins with all the unused MNs
                    sortedCombinedMns.push_back(dmn);
                }
            }
            i++;
        }

        // Now add the already used MNs to the end of the list
        std::move(usedMNs.begin(), usedMNs.end(), std::back_inserter(sortedCombinedMns));
    }

    if (LogAcceptDebug(BCLog::LLMQ)) {
        LogPrint(BCLog::LLMQ, "%s h[%d] from[%d] sortedCombinedMns[%s]\n", __func__,
                 pCycleQuorumBaseBlockIndex->nHeight, nHeight, ToString(sortedCombinedMns));
    }

    size_t numQuorums = static_cast<size_t>(llmqParams.signingActiveQuorumCount);
    size_t quorumSize = static_cast<size_t>(llmqParams.size);
    auto quarterSize{quorumSize / 4};

    std::vector<QuorumMembers> quarterQuorumMembers(numQuorums);

    if (!quarter_steps && sortedCombinedMns.empty()) {
        return quarterQuorumMembers;
    }

    const auto map_positions = [&](const std::vector<std::vector<size_t>>& positions) {
        for (const size_t i : util::irange(numQuorums)) {
            for (const size_t pos : positions[i]) {
                quarterQuorumMembers[i].push_back(sortedCombinedMns[pos]);
            }
        }
        return quarterQuorumMembers;
    };
    const auto replay_steps = [&](Span<const int> skip_list) {
        const auto positions = llmq::utils::ReplayNewQuarters(sortedCombinedMns.size(), numQuorums, quarterSize, skip_list);
        if (!positions) {
            LogPrintf("GetQuorumQuarterMembersBySnapshot -- malformed skip list in snapshot, llmqType[%d] h[%d] from[%d]\n",
                      std23::to_underlying(llmqParams.type), pCycleQuorumBaseBlockIndex->nHeight, nHeight);
            return std::vector<QuorumMembers>(numQuorums);
        }
        return map_positions(*positions);
    };

    switch (snapshot.mnSkipListMode) {
    case SnapshotSkipMode::MODE_NO_SKIPPING:
        if (quarter_steps) return replay_steps({});
        return map_positions(llmq::utils::ReplayLegacyQuarters(sortedCombinedMns.size(), numQuorums, quarterSize, {}));
    case SnapshotSkipMode::MODE_SKIPPING_ENTRIES: // List holds entries to be skipped
        if (quarter_steps) return replay_steps(snapshot.mnSkipList);
        return map_positions(llmq::utils::ReplayLegacyQuarters(sortedCombinedMns.size(), numQuorums, quarterSize,
                                                               snapshot.mnSkipList));
    case SnapshotSkipMode::MODE_NO_SKIPPING_ENTRIES: // List holds entries to be kept
    case SnapshotSkipMode::MODE_ALL_SKIPPED:         // Every node was skipped. Returning empty quarterQuorumMembers
    default:
        return quarterQuorumMembers;
    }
}

QuorumMembers ComputeQuorumMembers(Consensus::LLMQType llmqType, const CChainParams& chainparams,
                                   const CDeterministicMNList& mn_list, const CBlockIndex* pQuorumBaseBlockIndex)
{
    bool EvoOnly = (chainparams.GetConsensus().llmqTypePlatform == llmqType) &&
                   DeploymentActiveAfter(pQuorumBaseBlockIndex, chainparams.GetConsensus(), Consensus::DEPLOYMENT_V19);
    const auto& llmq_params_opt = chainparams.GetLLMQ(llmqType);
    assert(llmq_params_opt.has_value());
    if (llmq_params_opt->useRotation || pQuorumBaseBlockIndex->nHeight % llmq_params_opt->dkgInterval != 0) {
        ASSERT_IF_DEBUG(false);
        return {};
    }

    const auto modifier = GetHashModifier(llmq_params_opt.value(), chainparams.GetConsensus(), pQuorumBaseBlockIndex);
    return CalculateQuorum(mn_list, modifier, llmq_params_opt->size, EvoOnly);
}

/**
 * Candidate order with quarter_steps: the masternodes of allMns that are not in used, sorted by score, then the
 * ones in used, sorted by score. Both parts come from allMns, the list the snapshot's activeQuorumMembers refers
 * to, so GetQuorumQuarterMembersBySnapshot rebuilds the same order from it.
 */
QuorumMembers SortedCandidatesByProTxHash(const CDeterministicMNList& allMns, const Uint256HashSet& used,
                                          const uint256& modifier)
{
    QuorumMembers notUsedMns;
    QuorumMembers usedMns;
    allMns.ForEachMNShared(/*onlyValid=*/true, [&](const auto& dmn) {
        (used.count(dmn->proTxHash) != 0 ? usedMns : notUsedMns).push_back(dmn);
    });
    auto sorted = CalculateQuorum(std::move(notUsedMns), modifier);
    for (auto& dmn : CalculateQuorum(std::move(usedMns), modifier)) {
        sorted.push_back(std::move(dmn));
    }
    return sorted;
}

void BuildQuorumSnapshot(const Consensus::LLMQParams& llmqParams, const Consensus::Params& consensus_params,
                         const CDeterministicMNList& allMns, const std::function<bool(const uint256&)>& is_used,
                         llmq::CQuorumSnapshot& quorumSnapshot, std::vector<int>& skipList,
                         const CBlockIndex* pCycleQuorumBaseBlockIndex)
{
    if (!llmqParams.useRotation || pCycleQuorumBaseBlockIndex->nHeight % llmqParams.dkgInterval != 0) {
        ASSERT_IF_DEBUG(false);
        return;
    }

    const auto allMnsTotal = allMns.GetCounts().total();
    quorumSnapshot.activeQuorumMembers.resize(allMnsTotal);
    const auto modifier = GetHashModifier(llmqParams, consensus_params, pCycleQuorumBaseBlockIndex);
    auto sortedAllMns = CalculateQuorum(allMns, modifier);

    LogPrint(BCLog::LLMQ, "BuildQuorumSnapshot h[%d] numMns[%d]\n", pCycleQuorumBaseBlockIndex->nHeight,
             allMnsTotal);

    std::fill(quorumSnapshot.activeQuorumMembers.begin(), quorumSnapshot.activeQuorumMembers.end(), false);
    size_t index = {};
    for (const auto& dmn : sortedAllMns) {
        if (is_used(dmn->proTxHash)) {
            quorumSnapshot.activeQuorumMembers[index] = true;
        }
        index++;
    }

    if (skipList.empty()) {
        quorumSnapshot.mnSkipListMode = SnapshotSkipMode::MODE_NO_SKIPPING;
        quorumSnapshot.mnSkipList.clear();
    } else {
        quorumSnapshot.mnSkipListMode = SnapshotSkipMode::MODE_SKIPPING_ENTRIES;
        quorumSnapshot.mnSkipList = std::move(skipList);
    }
}

// nullopt only when the step-encoded walk overflows (quarter_steps); no snapshot is stored then
std::optional<std::vector<QuorumMembers>> BuildNewQuorumQuarterMembers(const Consensus::LLMQParams& llmqParams,
                                                                       const llmq::UtilParameters& util_params,
                                                                       const CDeterministicMNList& allMns,
                                                                       const PreviousQuorumQuarters& previousQuarters,
                                                                       const uint256& modifier, int cycleBaseHeight,
                                                                       bool storeSnapshot, bool quarter_steps)
{
    if (!llmqParams.useRotation || cycleBaseHeight % llmqParams.dkgInterval != 0) {
        ASSERT_IF_DEBUG(false);
        return std::vector<QuorumMembers>{};
    }

    size_t nQuorums = static_cast<size_t>(llmqParams.signingActiveQuorumCount);
    std::vector<QuorumMembers> quarterQuorumMembers{nQuorums};

    size_t quorumSize = static_cast<size_t>(llmqParams.size);
    auto quarterSize{quorumSize / 4};

    if (!quarter_steps && allMns.GetCounts().enabled() < quarterSize) {
        return quarterQuorumMembers;
    }

    auto MnsUsedAtH = CDeterministicMNList();
    std::vector<CDeterministicMNList> MnsUsedAtHIndexed{nQuorums};
    // With quarter_steps, previous members are tracked by proTxHash only: members kept from different cycles
    // can carry conflicting unique properties (e.g. one MN's former address now used by another) and must
    // still all be excluded
    Uint256HashSet usedAtH;
    std::vector<Uint256HashSet> usedAtHIndexed{nQuorums};

    // V19 is a buried deployment, so its state at the cycle base is a pure height check that
    // also works when the cycle base block is not mined yet
    bool skipRemovedMNs = cycleBaseHeight + 1 >=
                              util_params.m_chainman.GetConsensus().DeploymentHeight(Consensus::DEPLOYMENT_V19) ||
                          (util_params.m_chainman.GetParams().NetworkIDString() == CBaseChainParams::TESTNET);

    for (const size_t idx : util::irange(nQuorums)) {
        for (auto* prev_cycle : previousQuarters.GetCycles()) {
            for (const auto& mn : prev_cycle->m_members[idx]) {
                if (skipRemovedMNs && !allMns.HasMN(mn->proTxHash)) {
                    continue;
                }
                if (allMns.IsMNPoSeBanned(mn->proTxHash)) {
                    continue;
                }
                if (quarter_steps) {
                    usedAtH.insert(mn->proTxHash);
                    usedAtHIndexed[idx].insert(mn->proTxHash);
                    continue;
                }
                try {
                    MnsUsedAtH.AddMN(mn);
                } catch (const std::runtime_error& e) {
                }
                try {
                    MnsUsedAtHIndexed[idx].AddMN(mn);
                } catch (const std::runtime_error& e) {
                }
            }
        }
    }

    QuorumMembers sortedCombinedMnsList;
    if (quarter_steps) {
        sortedCombinedMnsList = SortedCandidatesByProTxHash(allMns, usedAtH, modifier);
    } else {
        std::vector<CDeterministicMNCPtr> MnsNotUsedAtH;
        allMns.ForEachMNShared(/*onlyValid=*/false, [&MnsUsedAtH, &MnsNotUsedAtH](const auto& dmn) {
            if (!MnsUsedAtH.HasMN(dmn->proTxHash)) {
                if (!dmn->pdmnState->IsBanned()) {
                    MnsNotUsedAtH.push_back(dmn);
                }
            }
        });

        auto sortedMnsUsedAtHM = CalculateQuorum(MnsUsedAtH, modifier);
        sortedCombinedMnsList = CalculateQuorum(std::move(MnsNotUsedAtH), modifier);
        for (auto& m : sortedMnsUsedAtHM) {
            sortedCombinedMnsList.push_back(std::move(m));
        }
    }

    if (LogAcceptDebug(BCLog::LLMQ)) {
        LogPrint(BCLog::LLMQ, "%s h[%d] sortedCombinedMns[%s]\n", __func__, cycleBaseHeight,
                 ToString(sortedCombinedMnsList));
    }

    std::vector<size_t> usedCounts;
    usedCounts.reserve(nQuorums);
    for (const size_t i : util::irange(nQuorums)) {
        usedCounts.push_back(MnsUsedAtHIndexed[i].GetCounts().total());
    }

    const auto can_add = [&](size_t i, size_t pos) {
        if (quarter_steps) return usedAtHIndexed[i].insert(sortedCombinedMnsList[pos]->proTxHash).second;
        if (MnsUsedAtHIndexed[i].HasMN(sortedCombinedMnsList[pos]->proTxHash)) return false;
        try {
            // NOTE: AddMN is the one that can throw exceptions, must be exicuted first
            MnsUsedAtHIndexed[i].AddMN(sortedCombinedMnsList[pos]);
            return true;
        } catch (const std::runtime_error& e) {
            return false;
        }
    };
    std::optional<llmq::utils::QuarterWalkResult> walk;
    if (quarter_steps) {
        walk = llmq::utils::WalkNewQuarters(sortedCombinedMnsList.size(), nQuorums, quarterSize, can_add);
        if (!walk) {
            LogPrintf("%s -- skip list overflow, llmqType[%d] h[%d]\n", __func__, std23::to_underlying(llmqParams.type),
                      cycleBaseHeight);
            return std::nullopt;
        }
    } else {
        walk = llmq::utils::WalkLegacyQuarters(sortedCombinedMnsList.size(), nQuorums, quarterSize, usedCounts, can_add);
        if (!walk) {
            return std::vector<QuorumMembers>(nQuorums);
        }
    }
    for (const size_t i : util::irange(nQuorums)) {
        for (const size_t pos : walk->quarters[i]) {
            quarterQuorumMembers[i].push_back(sortedCombinedMnsList[pos]);
        }
    }

    if (storeSnapshot) {
        llmq::CQuorumSnapshot quorumSnapshot{};
        const auto is_used = [&](const uint256& proTxHash) {
            return quarter_steps ? usedAtH.count(proTxHash) != 0 : MnsUsedAtH.HasMN(proTxHash);
        };
        BuildQuorumSnapshot(llmqParams, util_params.m_chainman.GetConsensus(), allMns, is_used, quorumSnapshot,
                            walk->skip_list, util_params.m_base_index);
        util_params.m_qsnapman.StoreSnapshotForBlock(llmqParams.type, util_params.m_base_index, quorumSnapshot);
    }

    return quarterQuorumMembers;
}

std::vector<QuorumMembers> ComputeQuorumMembersByQuarterRotation(const Consensus::LLMQParams& llmqParams,
                                                                 const llmq::UtilParameters& util_params,
                                                                 gsl::not_null<const CBlockIndex*> pWorkBlockIndex,
                                                                 int cycleBaseHeight, const uint256& modifier,
                                                                 bool predicting)
{
    auto quarters = llmq::utils::ComputeRotationQuarters(llmqParams, util_params, pWorkBlockIndex, cycleBaseHeight,
                                                         modifier, predicting);
    std::vector<QuorumMembers> quorumMembers(quarters.size());
    for (const size_t i : util::irange(quarters.size())) {
        for (auto& quarter : quarters[i]) {
            std::move(quarter.begin(), quarter.end(), std::back_inserter(quorumMembers[i]));
        }
        if (LogAcceptDebug(BCLog::LLMQ)) {
            LogPrint(BCLog::LLMQ, "QuorumComposition h[%d] i[%d]: [%s]\n", cycleBaseHeight, i,
                     ToString(quorumMembers[i]));
        }
    }

    return quorumMembers;
}
} // anonymous namespace

namespace llmq {
namespace utils {
std::optional<QuarterWalkResult> WalkLegacyQuarters(size_t list_size, size_t quorum_count, size_t quarter_size,
                                                    const std::vector<size_t>& used_counts,
                                                    const std::function<bool(size_t, size_t)>& can_add)
{
    QuarterWalkResult result;
    result.quarters.resize(quorum_count);
    size_t firstSkippedIndex = 0;
    size_t idx{0};
    for (const size_t i : util::irange(quorum_count)) {
        auto usedMNsCount = used_counts[i];
        bool updated{false};
        size_t initial_loop_idx = idx;
        while (result.quarters[i].size() < quarter_size && (usedMNsCount + result.quarters[i].size() < list_size)) {
            bool skip{true};
            if (can_add(i, idx)) {
                result.quarters[i].push_back(idx);
                updated = true;
                skip = false;
            }
            if (skip) {
                if (firstSkippedIndex == 0) {
                    firstSkippedIndex = idx;
                    result.skip_list.push_back(idx);
                } else {
                    result.skip_list.push_back(idx - firstSkippedIndex);
                }
            }
            if (++idx == list_size) {
                idx = 0;
            }
            if (idx == initial_loop_idx) {
                // we made full "while" loop
                if (!updated) {
                    // there are not enough MNs, there is nothing we can do here
                    return std::nullopt;
                }
                // reset and try again
                updated = false;
            }
        }
    }
    return result;
}

std::vector<std::vector<size_t>> ReplayLegacyQuarters(size_t list_size, size_t quorum_count, size_t quarter_size,
                                                       Span<const int> skip_list)
{
    size_t first_entry_index{0};
    std::vector<int> processesdSkipList;
    for (const auto& s : skip_list) {
        if (first_entry_index == 0) {
            first_entry_index = s;
            processesdSkipList.push_back(s);
        } else {
            processesdSkipList.push_back(first_entry_index + s);
        }
    }

    std::vector<std::vector<size_t>> quarters(quorum_count);
    int idx = 0;
    auto itsk = processesdSkipList.begin();
    for (const size_t i : util::irange(quorum_count)) {
        while (quarters[i].size() < quarter_size) {
            if (itsk != processesdSkipList.end() && idx == *itsk) {
                itsk++;
            } else {
                quarters[i].push_back(idx);
            }
            idx++;
            if (idx == static_cast<int>(list_size)) {
                idx = 0;
            }
        }
    }
    return quarters;
}

std::optional<QuarterWalkResult> WalkNewQuarters(size_t list_size, size_t quorum_count, size_t quarter_size,
                                                 const std::function<bool(size_t, size_t)>& can_add)
{
    QuarterWalkResult result;
    result.quarters.resize(quorum_count);
    size_t step{0};
    size_t prev_skip_step{0};
    for (const size_t i : util::irange(quorum_count)) {
        size_t consecutive_skips{0};
        while (list_size > 0 && result.quarters[i].size() < quarter_size && consecutive_skips < list_size) {
            const size_t pos = step % list_size;
            if (can_add(i, pos)) {
                result.quarters[i].push_back(pos);
                consecutive_skips = 0;
            } else {
                const size_t delta = step - prev_skip_step;
                if (delta > static_cast<size_t>(std::numeric_limits<int>::max())) return std::nullopt;
                result.skip_list.push_back(static_cast<int>(delta));
                prev_skip_step = step;
                ++consecutive_skips;
            }
            ++step;
        }
    }
    return result;
}

std::optional<std::vector<std::vector<size_t>>> ReplayNewQuarters(size_t list_size, size_t quorum_count,
                                                                  size_t quarter_size, Span<const int> skip_list)
{
    std::vector<size_t> skip_steps;
    skip_steps.reserve(skip_list.size());
    for (const int entry : skip_list) {
        if (skip_steps.empty() ? entry < 0 : entry <= 0) return std::nullopt;
        const size_t delta = static_cast<size_t>(entry);
        if (!skip_steps.empty() && delta > std::numeric_limits<size_t>::max() - skip_steps.back()) return std::nullopt;
        skip_steps.push_back(skip_steps.empty() ? delta : skip_steps.back() + delta);
    }

    std::vector<std::vector<size_t>> quarters(quorum_count);
    size_t step{0};
    size_t next_skip{0};
    for (const size_t i : util::irange(quorum_count)) {
        size_t consecutive_skips{0};
        std::vector<bool> in_quarter(list_size, false);
        while (list_size > 0 && quarters[i].size() < quarter_size && consecutive_skips < list_size) {
            if (next_skip < skip_steps.size() && skip_steps[next_skip] == step) {
                ++next_skip;
                ++consecutive_skips;
            } else {
                const size_t pos = step % list_size;
                if (in_quarter[pos]) return std::nullopt;
                in_quarter[pos] = true;
                quarters[i].push_back(pos);
                consecutive_skips = 0;
            }
            ++step;
        }
    }
    if (next_skip != skip_steps.size()) return std::nullopt;
    return quarters;
}

BlsCheck::BlsCheck() = default;

BlsCheck::BlsCheck(CBLSSignature sig, std::vector<CBLSPublicKey> pubkeys, uint256 msg_hash, std::string id_string) :
    m_sig(sig),
    m_pubkeys(std::move(pubkeys)),
    m_msg_hash(msg_hash),
    m_id_string(std::move(id_string))
{
}

BlsCheck::~BlsCheck() = default;

bool BlsCheck::operator()()
{
    if (m_pubkeys.size() > 1) {
        if (!m_sig.VerifySecureAggregated(m_pubkeys, m_msg_hash)) {
            LogPrint(BCLog::LLMQ, "%s\n", m_id_string);
            return false;
        }
    } else if (m_pubkeys.size() == 1) {
        if (!m_sig.VerifyInsecure(m_pubkeys.back(), m_msg_hash)) {
            LogPrint(BCLog::LLMQ, "%s\n", m_id_string);
            return false;
        }
    } else {
        // we should not get there ever
        LogPrint(BCLog::LLMQ, "%s - no public keys are provided\n", m_id_string);
        return false;
    }
    return true;
}

void BlsCheck::swap(BlsCheck& obj) noexcept
{
    std::swap(m_sig, obj.m_sig);
    std::swap(m_pubkeys, obj.m_pubkeys);
    std::swap(m_msg_hash, obj.m_msg_hash);
    std::swap(m_id_string, obj.m_id_string);
}

// Computes the quarters of all quorum indexes of the cycle starting at cycleBaseHeight, which
// is not required to be mined yet: everything is derived from the cycle's work block. No
// snapshot is stored when predicting a future cycle.
std::vector<std::array<QuorumMembers, 4>> ComputeRotationQuarters(const Consensus::LLMQParams& llmqParams,
                                                                  const UtilParameters& util_params,
                                                                  gsl::not_null<const CBlockIndex*> pWorkBlockIndex,
                                                                  int cycleBaseHeight, const uint256& modifier,
                                                                  bool predicting)
{
    const int cycleLength = llmqParams.dkgInterval;
    if (!llmqParams.useRotation || cycleBaseHeight % llmqParams.dkgInterval != 0) {
        ASSERT_IF_DEBUG(false);
        return {};
    }
    const auto nQuorums{static_cast<size_t>(llmqParams.signingActiveQuorumCount)};
    const ChainstateManager& chainman{util_params.m_chainman};

    bool quarter_steps{false};
    if (predicting) {
        const auto rule = QuarterStepsRuleForPrediction(*pWorkBlockIndex, chainman);
        if (!rule) return {};
        quarter_steps = *rule;
    } else {
        quarter_steps = QuarterStepsActive(*util_params.m_base_index, chainman);
    }

    CDeterministicMNList allMns = util_params.m_dmnman.GetListForBlock(pWorkBlockIndex);
    LogPrint(BCLog::LLMQ, "ComputeQuorumMembersByQuarterRotation llmqType[%d] nHeight[%d] allMns[%d]\n",
             std23::to_underlying(llmqParams.type), cycleBaseHeight, allMns.GetCounts().enabled());

    PreviousQuorumQuarters previousQuarters(nQuorums);
    auto prev_cycles{previousQuarters.GetCycles()};
    for (size_t idx{0}; idx < prev_cycles.size(); idx++) {
        prev_cycles[idx]->m_cycle_index = pWorkBlockIndex->GetAncestor(cycleBaseHeight - (cycleLength * (idx + 1)));
        if (prev_cycles[idx]->m_cycle_index == nullptr) {
            if (predicting) return {};
            break;
        }
        if (auto opt_snap = util_params.m_qsnapman.GetSnapshotForBlock(llmqParams.type, prev_cycles[idx]->m_cycle_index);
            opt_snap.has_value()) {
            prev_cycles[idx]->m_snap = opt_snap.value();
        } else {
            if (predicting) return {};
            // TODO: Check if it is triggered from outside (P2P, block validation) and maybe throw an exception
            // assert(false);
            break;
        }
        prev_cycles[idx]->m_members = GetQuorumQuarterMembersBySnapshot(
            llmqParams, util_params.m_dmnman, chainman.GetConsensus(), prev_cycles[idx]->m_cycle_index,
            prev_cycles[idx]->m_snap, cycleBaseHeight, QuarterStepsActive(*prev_cycles[idx]->m_cycle_index, chainman));
    }

    auto newQuarterMembers = BuildNewQuorumQuarterMembers(llmqParams, util_params, allMns, previousQuarters, modifier,
                                                          cycleBaseHeight, /*storeSnapshot=*/!predicting, quarter_steps);
    if (!newQuarterMembers) {
        return {};
    }
    // TODO: Check if it is triggered from outside (P2P, block validation) and maybe throw an exception
    // assert (!newQuarterMembers->empty());

    if (LogAcceptDebug(BCLog::LLMQ)) {
        for (const size_t i : util::irange(nQuorums)) {
            std::stringstream ss;
            for (size_t idx = prev_cycles.size(); idx > 0; --idx) {
                const size_t cycle = idx - 1;
                ss << strprintf(" %dCmns[%s]", cycle, ToString(prev_cycles[cycle]->m_members[i]));
            }
            ss << strprintf(" new[%s]", ToString((*newQuarterMembers)[i]));
            LogPrint(BCLog::LLMQ, "QuarterComposition h[%d] i[%d]:%s\n", cycleBaseHeight, i, ss.str());
        }
    }

    std::vector<std::array<QuorumMembers, 4>> quarters(nQuorums);
    for (const size_t i : util::irange(nQuorums)) {
        quarters[i] = {std::move(previousQuarters.quarterHMinus3C.m_members[i]),
                       std::move(previousQuarters.quarterHMinus2C.m_members[i]),
                       std::move(previousQuarters.quarterHMinusC.m_members[i]), std::move((*newQuarterMembers)[i])};
    }
    return quarters;
}

bool QuarterStepsActive(const CBlockIndex& cycle_base, const ChainstateManager& chainman)
{
    return DeploymentActiveAt(cycle_base, chainman, Consensus::DEPLOYMENT_QUARTER_STEPS);
}

std::optional<bool> QuarterStepsRuleForPrediction(ThresholdState state_after_work_block, int period)
{
    // A block's versionbits state is that of the first block of its period, so the state can only change
    // where a period starts. Between the block after the work block and the cycle base at most this many
    // periods start, each moving the state by at most one step towards ACTIVE.
    const int max_transitions{period > 0 ? 1 + (WORK_DIFF_DEPTH - 2) / period : WORK_DIFF_DEPTH};
    switch (state_after_work_block) {
    case ThresholdState::ACTIVE:
        return true;
    case ThresholdState::FAILED:
        return false;
    case ThresholdState::DEFINED:
        // DEFINED -> STARTED -> LOCKED_IN -> ACTIVE
        if (max_transitions < 3) return false;
        return std::nullopt;
    case ThresholdState::STARTED:
        // STARTED -> LOCKED_IN -> ACTIVE
        if (max_transitions < 2) return false;
        return std::nullopt;
    case ThresholdState::LOCKED_IN:
        return std::nullopt;
    } // no default case, so the compiler can warn about missing cases
    return std::nullopt;
}

std::optional<bool> QuarterStepsRuleForPrediction(const CBlockIndex& work_block, const ChainstateManager& chainman)
{
    const Consensus::Params& consensus{chainman.GetConsensus()};
    auto& vbcache{chainman.m_versionbitscache};
    return QuarterStepsRuleForPrediction(vbcache.State(&work_block, consensus, Consensus::DEPLOYMENT_QUARTER_STEPS),
                                         vbcache.Statistics(nullptr, consensus, Consensus::DEPLOYMENT_QUARTER_STEPS).period);
}

std::optional<std::vector<CDeterministicMNCPtr>> ComputeQuorumMembersFromWorkBlock(
    Consensus::LLMQType llmqType, const UtilParameters& util_params, gsl::not_null<const CBlockIndex*> pWorkBlockIndex,
    int quorumHeight)
{
    const Consensus::Params& consensus{util_params.m_chainman.GetConsensus()};
    const auto& llmq_params_opt = util_params.m_chainman.GetParams().GetLLMQ(llmqType);
    if (!llmq_params_opt.has_value()) {
        ASSERT_IF_DEBUG(false);
        return std::nullopt;
    }
    const auto& llmq_params = llmq_params_opt.value();

    const int quorumIndex{quorumHeight % llmq_params.dkgInterval};
    const int cycleBaseHeight{quorumHeight - quorumIndex};
    if (quorumIndex < 0 || quorumIndex >= (llmq_params.useRotation ? llmq_params.signingActiveQuorumCount : 1) ||
        pWorkBlockIndex->nHeight != cycleBaseHeight - llmq::WORK_DIFF_DEPTH) {
        ASSERT_IF_DEBUG(false);
        return std::nullopt;
    }

    if (!DeploymentActiveAfter(pWorkBlockIndex.get(), consensus, Consensus::DEPLOYMENT_V20)) {
        // pre-v20 modifier calculation needs the future (cycle) quorum base block context, which isn't known yet.
        return std::nullopt;
    }

    const auto modifier = GetHashModifierFromWorkBlock(llmq_params, pWorkBlockIndex.get());

    if (!llmq_params.useRotation) {
        // Canonical member selection gates EvoOnly on V19 at the future quorum base block; V19
        // is a buried deployment, so that state is a pure height check even though the block
        // itself is not mined yet.
        const bool EvoOnly = llmq_params.type == consensus.llmqTypePlatform &&
                             quorumHeight + 1 >= consensus.DeploymentHeight(Consensus::DEPLOYMENT_V19);
        return CalculateQuorum(util_params.m_dmnman.GetListForBlock(pWorkBlockIndex), modifier, llmq_params.size,
                               EvoOnly);
    }

    auto quorumMembers = ComputeQuorumMembersByQuarterRotation(llmq_params, util_params, pWorkBlockIndex,
                                                               cycleBaseHeight, modifier, /*predicting=*/true);
    if (quorumMembers.empty()) {
        return std::nullopt;
    }
    return quorumMembers[quorumIndex];
}

QuorumMembers GetAllQuorumMembers(Consensus::LLMQType llmqType, const UtilParameters& util_params, bool reset_cache)
{
    static RecursiveMutex cs_members;
    static PerLlmqTypeCache<QuorumMembers> mapQuorumMembers GUARDED_BY(cs_members);
    static RecursiveMutex cs_indexed_members;
    static PerLlmqTypeCache<QuorumMembers, std::pair<uint256, int>> mapIndexedQuorumMembers GUARDED_BY(cs_indexed_members);

    // A parentless base index (genesis) can never host a quorum. IsQuorumTypeEnabled() handles the
    // null, but say so explicitly here: this is reached with an attacker-supplied quorumHash via
    // CFinalCommitment::Verify(), so it must reject rather than trip a precondition.
    if (util_params.m_base_index->pprev == nullptr ||
        !util_params.m_chainman.IsQuorumTypeEnabled(llmqType, util_params.m_base_index->pprev)) {
        return {};
    }

    std::vector<CDeterministicMNCPtr> quorumMembers;
    {
        LOCK(cs_members);
        if (!mapQuorumMembers.IsInitialized()) {
            mapQuorumMembers.Init(util_params.m_chainman.GetConsensus());
        }
        if (reset_cache) {
            mapQuorumMembers.clear(llmqType);
        } else if (mapQuorumMembers.get(llmqType, util_params.m_base_index->GetBlockHash(), quorumMembers)) {
            return quorumMembers;
        }
    }

    const auto& llmq_params_opt = util_params.m_chainman.GetParams().GetLLMQ(llmqType);
    assert(llmq_params_opt.has_value());
    const auto& llmq_params = llmq_params_opt.value();

    if (IsQuorumRotationEnabled(llmq_params, util_params.m_base_index)) {
        if (LOCK(cs_indexed_members); !mapIndexedQuorumMembers.IsInitialized()) {
            mapIndexedQuorumMembers.Init(util_params.m_chainman.GetConsensus());
        }
        /*
         * Quorums created with rotation are now created in a different way. All signingActiveQuorumCount are created
         * during the period of dkgInterval. But they are not created exactly in the same block, they are spread
         * overtime: one quorum in each block until all signingActiveQuorumCount are created. The new concept of
         * quorumIndex is introduced in order to identify them. In every dkgInterval blocks (also called
         * CycleQuorumBaseBlock), the spread quorum creation starts like this: For quorumIndex = 0 :
         * signingActiveQuorumCount Quorum Q with quorumIndex is created at height CycleQuorumBaseBlock + quorumIndex
         */

        int quorumIndex = util_params.m_base_index->nHeight % llmq_params.dkgInterval;
        if (quorumIndex >= llmq_params.signingActiveQuorumCount) {
            return {};
        }
        int cycleQuorumBaseHeight = util_params.m_base_index->nHeight - quorumIndex;
        const CBlockIndex* pCycleQuorumBaseBlockIndex = util_params.m_base_index->GetAncestor(cycleQuorumBaseHeight);

        /*
         * Since mapQuorumMembers stores Quorum members per block hash, and we don't know yet the block hashes of blocks
         * for all quorumIndexes (since these blocks are not created yet) We store them in a second cache
         * mapIndexedQuorumMembers which stores them by {CycleQuorumBaseBlockHash, quorumIndex}
         */
        if (reset_cache) {
            LOCK(cs_indexed_members);
            mapIndexedQuorumMembers.clear(llmqType);
        } else if (LOCK(cs_indexed_members); mapIndexedQuorumMembers.get(
                       llmqType, std::pair(pCycleQuorumBaseBlockIndex->GetBlockHash(), quorumIndex), quorumMembers)) {
            LOCK(cs_members);
            mapQuorumMembers.insert(llmqType, util_params.m_base_index->GetBlockHash(), quorumMembers);
            return quorumMembers;
        }

        const CBlockIndex* pWorkBlockIndex = pCycleQuorumBaseBlockIndex->GetAncestor(cycleQuorumBaseHeight -
                                                                                     WORK_DIFF_DEPTH);
        const auto modifier = GetHashModifier(llmq_params, util_params.m_chainman.GetConsensus(),
                                              pCycleQuorumBaseBlockIndex);
        auto q = ComputeQuorumMembersByQuarterRotation(llmq_params, util_params.replace_index(pCycleQuorumBaseBlockIndex),
                                                       pWorkBlockIndex, cycleQuorumBaseHeight, modifier,
                                                       /*predicting=*/false);
        if (q.empty()) {
            return {};
        }
        quorumMembers = q[quorumIndex];

        LOCK(cs_indexed_members);
        for (const size_t i : util::irange(q.size())) {
            mapIndexedQuorumMembers.emplace(llmqType, std::make_pair(pCycleQuorumBaseBlockIndex->GetBlockHash(), i),
                                            std::move(q[i]));
        }
    } else {
        const CBlockIndex* pWorkBlockIndex = DeploymentActiveAfter(util_params.m_base_index,
                                                                   util_params.m_chainman.GetConsensus(),
                                                                   Consensus::DEPLOYMENT_V20)
                                                 ? util_params.m_base_index->GetAncestor(
                                                       util_params.m_base_index->nHeight - WORK_DIFF_DEPTH)
                                                 : util_params.m_base_index.get();
        CDeterministicMNList mn_list = util_params.m_dmnman.GetListForBlock(pWorkBlockIndex);
        quorumMembers = ComputeQuorumMembers(llmqType, util_params.m_chainman.GetParams(), mn_list,
                                             util_params.m_base_index);
    }

    LOCK(cs_members);
    mapQuorumMembers.insert(llmqType, util_params.m_base_index->GetBlockHash(), quorumMembers);
    return quorumMembers;
}

uint256 DeterministicOutboundConnection(const uint256& proTxHash1, const uint256& proTxHash2)
{
    // We need to deterministically select who is going to initiate the connection. The naive way would be to simply
    // return the min(proTxHash1, proTxHash2), but this would create a bias towards MNs with a numerically low
    // hash. To fix this, we return the proTxHash that has the lowest value of:
    //   hash(min(proTxHash1, proTxHash2), max(proTxHash1, proTxHash2), proTxHashX)
    // where proTxHashX is the proTxHash to compare
    uint256 h1;
    uint256 h2;
    if (proTxHash1 < proTxHash2) {
        h1 = ::SerializeHash(std::make_tuple(proTxHash1, proTxHash2, proTxHash1));
        h2 = ::SerializeHash(std::make_tuple(proTxHash1, proTxHash2, proTxHash2));
    } else {
        h1 = ::SerializeHash(std::make_tuple(proTxHash2, proTxHash1, proTxHash1));
        h2 = ::SerializeHash(std::make_tuple(proTxHash2, proTxHash1, proTxHash2));
    }
    if (h1 < h2) {
        return proTxHash1;
    }
    return proTxHash2;
}

Uint256HashSet GetQuorumConnections(const Consensus::LLMQParams& llmqParams, const CSporkManager& sporkman,
                                    const UtilParameters& util_params, const uint256& forMember, bool onlyOutbound)
{
    if (IsAllMembersConnectedEnabled(llmqParams.type, sporkman)) {
        auto mns = GetAllQuorumMembers(llmqParams.type, util_params);
        Uint256HashSet result;

        for (const auto& dmn : mns) {
            if (dmn->proTxHash == forMember) {
                continue;
            }
            // Determine which of the two MNs (forMember vs dmn) should initiate the outbound connection and which
            // one should wait for the inbound connection. We do this in a deterministic way, so that even when we
            // end up with both connecting to each other, we know which one to disconnect
            uint256 deterministicOutbound = DeterministicOutboundConnection(forMember, dmn->proTxHash);
            if (!onlyOutbound || deterministicOutbound == dmn->proTxHash) {
                result.emplace(dmn->proTxHash);
            }
        }
        return result;
    }
    return GetQuorumRelayMembers(llmqParams, util_params, forMember, onlyOutbound);
}

Uint256HashSet GetQuorumRelayMembers(const Consensus::LLMQParams& llmqParams, const UtilParameters& util_params,
                                     const uint256& forMember, bool onlyOutbound)
{
    auto mns = GetAllQuorumMembers(llmqParams.type, util_params);
    Uint256HashSet result;

    auto calcOutbound = [&](size_t i, const uint256& proTxHash) {
        // Relay to nodes at indexes (i+2^k)%n, where
        //   k: 0..max(1, floor(log2(n-1))-1)
        //   n: size of the quorum/ring
        Uint256HashSet r{};
        if (mns.size() == 1) {
            // No outbound connections are needed when there is one MN only.
            // Also note that trying to calculate results via the algorithm below
            // would result in an endless loop.
            return r;
        }
        int gap = 1;
        int gap_max = static_cast<int>(mns.size()) - 1;
        int k = 0;
        while ((gap_max >>= 1) || k <= 1) {
            size_t idx = (i + gap) % mns.size();
            // It doesn't matter if this node is going to be added to the resulting set or not,
            // we should always bump the gap and the k (step count) regardless.
            // Refusing to bump the gap results in an incomplete set in the best case scenario
            // (idx won't ever change again once we hit `==`). Not bumping k guarantees an endless
            // loop when the first or the second node we check is the one that should be skipped
            // (k <= 1 forever).
            gap <<= 1;
            k++;
            const auto& otherDmn = mns[idx];
            if (otherDmn->proTxHash == proTxHash) {
                continue;
            }
            r.emplace(otherDmn->proTxHash);
        }
        return r;
    };

    for (const auto i : util::irange(mns.size())) {
        const auto& dmn = mns[i];
        if (dmn->proTxHash == forMember) {
            auto r = calcOutbound(i, dmn->proTxHash);
            result.insert(r.begin(), r.end());
        } else if (!onlyOutbound) {
            auto r = calcOutbound(i, dmn->proTxHash);
            if (r.count(forMember)) {
                result.emplace(dmn->proTxHash);
            }
        }
    }

    return result;
}

std::unordered_set<size_t> CalcDeterministicWatchConnections(Consensus::LLMQType llmqType,
                                                             gsl::not_null<const CBlockIndex*> pQuorumBaseBlockIndex,
                                                             size_t memberCount, size_t connectionCount)
{
    static uint256 qwatchConnectionSeed;
    static std::atomic<bool> qwatchConnectionSeedGenerated{false};
    static RecursiveMutex qwatchConnectionSeedCs;
    if (!qwatchConnectionSeedGenerated) {
        LOCK(qwatchConnectionSeedCs);
        qwatchConnectionSeed = GetRandHash();
        qwatchConnectionSeedGenerated = true;
    }

    std::unordered_set<size_t> result;
    uint256 rnd = qwatchConnectionSeed;
    for ([[maybe_unused]] const auto _ : util::irange(connectionCount)) {
        rnd = ::SerializeHash(std::make_pair(rnd, std::make_pair(llmqType, pQuorumBaseBlockIndex->GetBlockHash())));
        result.emplace(rnd.GetUint64(0) % memberCount);
    }
    return result;
}

} // namespace utils
} // namespace llmq
