// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Chain-aware companions to evo/snapshot.cpp, deliberately header-less: these
// implementations are declared in evo/snapshot.h but need validation.h and
// llmq internals, so they live in their own compilation unit to keep the
// snapshot codec free of an evo/snapshot -> validation -> evo/snapshot cycle.

#include <evo/snapshot.h>

#include <chain.h>
#include <chainparams.h>
#include <deploymentstatus.h>
#include <evo/chainhelper.h>
#include <evo/creditpool.h>
#include <evo/mnhftx.h>
#include <llmq/blockprocessor.h>
#include <llmq/options.h>
#include <llmq/snapshot.h>
#include <llmq/utils.h>
#include <tinyformat.h>
#include <util/check.h>
#include <validation.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace evo {
namespace {

// Mirrors the canonical ordering used by the codec in evo/snapshot.cpp.
template <typename T>
std::vector<T> Sorted(std::vector<T> values)
{
    std::sort(values.begin(), values.end(), [](const T& a, const T& b) {
        if constexpr (std::is_same_v<T, CMinedQuorumCommitment>) {
            return std::tie(a.quorum_base_block_hash, a.mined_block_hash) <
                   std::tie(b.quorum_base_block_hash, b.mined_block_hash);
        } else if constexpr (std::is_same_v<T, CQuorumSnapshotEntry>) {
            return a.cycle_base_block_hash < b.cycle_base_block_hash;
        } else if constexpr (std::is_same_v<T, CHistoricalMNListDiff>) {
            return std::tie(a.height, a.block_hash) > std::tie(b.height, b.block_hash);
        } else if constexpr (std::is_same_v<T, CQuorumModifier>) {
            return std::tie(a.llmq_type, a.work_block_hash) < std::tie(b.llmq_type, b.work_block_hash);
        } else {
            return a.llmq_type < b.llmq_type;
        }
    });
    return values;
}

bool ValidateCommitmentAgainstChain(const CMinedQuorumCommitment& entry, const ChainstateManager& chainman,
                                    const CBlockIndex* base_index, const Consensus::LLMQParams& params,
                                    bool rotation_enabled) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    const CBlockIndex* quorum_index{chainman.m_blockman.LookupBlockIndex(entry.quorum_base_block_hash)};
    const CBlockIndex* mined_index{chainman.m_blockman.LookupBlockIndex(entry.mined_block_hash)};
    if (quorum_index == nullptr || mined_index == nullptr ||
        base_index->GetAncestor(quorum_index->nHeight) != quorum_index ||
        base_index->GetAncestor(mined_index->nHeight) != mined_index) return false;
    const int cycle_height{quorum_index->nHeight - quorum_index->nHeight % params.dkgInterval};
    if (rotation_enabled) {
        if (entry.commitment.quorumIndex != quorum_index->nHeight % params.dkgInterval ||
            entry.commitment.quorumIndex < 0 ||
            entry.commitment.quorumIndex >= params.signingActiveQuorumCount) return false;
    } else if (quorum_index->nHeight != cycle_height || entry.commitment.quorumIndex != 0) {
        return false;
    }
    const int mined_cycle{mined_index->nHeight - mined_index->nHeight % params.dkgInterval};
    if (mined_cycle != cycle_height || mined_index->nHeight % params.dkgInterval < params.dkgMiningWindowStart ||
        mined_index->nHeight % params.dkgInterval > params.dkgMiningWindowEnd) return false;
    const uint16_t expected_version{llmq::CFinalCommitment::GetVersion(
        rotation_enabled, DeploymentActiveAfter(quorum_index, chainman.GetConsensus(), Consensus::DEPLOYMENT_V19))};
    return entry.commitment.nVersion == expected_version && entry.commitment.VerifySizes(params);
}

CMinedQuorumCommitment ReadCommitment(const llmq::CQuorumBlockProcessor& qblockman, Consensus::LLMQType type,
                                      const CBlockIndex* quorum_index, const CBlockIndex* work_index,
                                      std::string& error)
{
    auto [commitment, mined_hash] = qblockman.GetMinedCommitment(type, quorum_index->GetBlockHash());
    if (mined_hash.IsNull()) error = "mined quorum commitment not found for " + quorum_index->GetBlockHash().ToString();
    return {quorum_index->GetBlockHash(), work_index->GetBlockHash(), std::move(commitment), mined_hash};
}


} // namespace

bool BuildEvoSnapshot(const CChainParams& chainparams, const ChainstateManager& chainman,
                      CDeterministicMNManager& dmnman,
                      const llmq::CQuorumBlockProcessor& qblockman, llmq::CQuorumSnapshotManager& qsnapman,
                      CCreditPoolManager& cpoolman, CMNHFManager& mnhfman, const CBlockIndex* base_index,
                      CEvoSnapshot& snapshot, std::string& error)
{
    AssertLockHeld(::cs_main);
    error.clear();
    if (base_index == nullptr) {
        error = "evo snapshot base block is null";
        return false;
    }

    CEvoSnapshot result;
    result.base_block_hash = base_index->GetBlockHash();
    if (!DeploymentActiveAt(*base_index, chainparams.GetConsensus(), Consensus::DEPLOYMENT_DIP0003)) {
        result.mn_list = CDeterministicMNList{base_index->GetBlockHash(), base_index->nHeight, 0};
        result.Validate();
        snapshot = std::move(result);
        return true;
    }
    result.mn_list = dmnman.GetListForBlock(base_index);
    std::map<uint256, std::pair<const CBlockIndex*, CDeterministicMNList>> historical;
    std::map<std::pair<Consensus::LLMQType, uint256>, uint256> modifiers;

    const auto register_work_block = [&](const Consensus::LLMQParams& params,
                                         bool rotation_enabled,
                                         const CBlockIndex* quorum_index) -> const CBlockIndex* {
        const CBlockIndex* modifier_base{rotation_enabled
            ? quorum_index->GetAncestor(quorum_index->nHeight - quorum_index->nHeight % params.dkgInterval)
            : quorum_index};
        if (modifier_base == nullptr) return nullptr;
        const CBlockIndex* work_index{
            (rotation_enabled ||
             DeploymentActiveAfter(modifier_base, chainparams.GetConsensus(), Consensus::DEPLOYMENT_V20))
                ? modifier_base->GetAncestor(modifier_base->nHeight - llmq::WORK_DIFF_DEPTH)
                : modifier_base};
        if (work_index == nullptr) return nullptr;
        historical.try_emplace(work_index->GetBlockHash(), work_index, dmnman.GetListForBlock(work_index));
        modifiers.emplace(std::make_pair(params.type, work_index->GetBlockHash()),
                          llmq::utils::GetQuorumHashModifier(params, chainparams.GetConsensus(), modifier_base));
        return work_index;
    };

    for (const auto& params : chainparams.GetConsensus().llmqs) {
        if (!chainman.IsQuorumTypeEnabled(params.type, base_index)) continue;
        CQuorumSnapshotData data;
        data.llmq_type = params.type;
        data.rotation_enabled = llmq::IsQuorumRotationEnabled(params, base_index);

        const size_t active_count{static_cast<size_t>(params.signingActiveQuorumCount)};
        const size_t total_count{SnapshotCommitmentCount(params, data.rotation_enabled)};
        std::vector<const CBlockIndex*> indexes;
        if (data.rotation_enabled) {
            indexes = qblockman.GetLastMinedCommitmentsPerQuorumIndexUntilBlock(params.type, base_index, 0);
        } else {
            indexes = qblockman.GetMinedCommitmentsUntilBlock(params.type, base_index, total_count);
        }
        // A young chain (or a freshly activated type) legitimately has fewer
        // mined commitments than the parameter-derived horizon. Emit what
        // exists: the CbTx quorum merkle root pins the active set at
        // completion, so a shortfall cannot be used to hide commitments.
        const size_t emit_active{std::min(indexes.size(), active_count)};
        for (size_t i{0}; i < emit_active; ++i) {
            const CBlockIndex* work_index{register_work_block(params, data.rotation_enabled, indexes[i])};
            if (work_index == nullptr) {
                error = "missing active quorum work block";
                return false;
            }
            auto entry{ReadCommitment(qblockman, params.type, indexes[i], work_index, error)};
            if (!error.empty()) return false;
            data.active_commitments.emplace_back(std::move(entry));
        }

        if (data.rotation_enabled) {
            indexes = qblockman.GetLastMinedCommitmentsPerQuorumIndexUntilBlock(params.type, base_index, 1);
        } else {
            indexes.erase(indexes.begin(), indexes.begin() + emit_active);
        }
        const size_t safety_count{std::min(indexes.size(), total_count - active_count)};
        for (size_t i{0}; i < safety_count; ++i) {
            const CBlockIndex* work_index{register_work_block(params, data.rotation_enabled, indexes[i])};
            if (work_index == nullptr) {
                error = "missing safety quorum work block";
                return false;
            }
            auto entry{ReadCommitment(qblockman, params.type, indexes[i], work_index, error)};
            if (!error.empty()) return false;
            data.safety_commitments.emplace_back(std::move(entry));
        }

        if (data.rotation_enabled) {
            std::vector<Consensus::LLMQParams> one_type{params};
            for (const auto& required : EvoSnapshotReconstructionHeights(base_index->nHeight, one_type)) {
                const int cycle_height{required.quorum_height};
                const int work_height{required.work_height};
                const CBlockIndex* cycle_index{cycle_height >= 0 ? base_index->GetAncestor(cycle_height) : nullptr};
                const CBlockIndex* work_index{cycle_index && work_height >= 0
                                                   ? cycle_index->GetAncestor(work_height)
                                                   : nullptr};
                // Horizons preceding the chain, and cycles that predate the
                // type's first rotation DKG, have no snapshot to carry. A gap
                // on a mature chain surfaces at completion, where quorum
                // reconstruction from the carried state must match the chain.
                if (cycle_index == nullptr || work_index == nullptr) continue;
                auto stored{qsnapman.GetSnapshotForBlock(params.type, cycle_index)};
                if (!stored) continue;
                data.rotation_snapshots.push_back(
                    {cycle_index->GetBlockHash(), work_index->GetBlockHash(), *stored});
                historical.try_emplace(work_index->GetBlockHash(), work_index, dmnman.GetListForBlock(work_index));
                modifiers.emplace(std::make_pair(params.type, work_index->GetBlockHash()),
                                  llmq::utils::GetQuorumHashModifier(params, chainparams.GetConsensus(), cycle_index));
            }
        }
        data.active_commitments = Sorted(std::move(data.active_commitments));
        data.safety_commitments = Sorted(std::move(data.safety_commitments));
        data.rotation_snapshots = Sorted(std::move(data.rotation_snapshots));
        result.quorums.emplace_back(std::move(data));
    }

    std::vector<std::pair<const CBlockIndex*, CDeterministicMNList>> ordered_history;
    ordered_history.reserve(historical.size());
    for (auto& [_, indexed_list] : historical) ordered_history.emplace_back(std::move(indexed_list));
    std::sort(ordered_history.begin(), ordered_history.end(), [](const auto& a, const auto& b) {
        return std::make_tuple(a.first->nHeight, a.first->GetBlockHash()) >
               std::make_tuple(b.first->nHeight, b.first->GetBlockHash());
    });
    CDeterministicMNList previous_list{result.mn_list};
    uint256 previous_hash{result.base_block_hash};
    for (const auto& [index, list] : ordered_history) {
        if (index->GetBlockHash() == result.base_block_hash) continue;
        result.historical_mn_list_diffs.push_back({previous_hash, index->GetBlockHash(), index->nHeight,
                                                   list.GetTotalRegisteredCount(), CanonicalMNListHash(list),
                                                   previous_list.BuildDiff(list)});
        previous_hash = index->GetBlockHash();
        previous_list = list;
    }
    for (const auto& [key, modifier] : modifiers) {
        result.quorum_modifiers.push_back({key.first, key.second, modifier});
    }
    result.credit_pool = cpoolman.GetCreditPool(base_index);
    result.mnhf_signals = mnhfman.GetSignalsStage(base_index);
    result.quorums = Sorted(std::move(result.quorums));
    result.historical_mn_list_diffs = Sorted(std::move(result.historical_mn_list_diffs));
    result.quorum_modifiers = Sorted(std::move(result.quorum_modifiers));
    try {
        result.Validate();
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    snapshot = std::move(result);
    return true;
}

bool ValidateEvoSnapshotAgainstChain(const CEvoSnapshot& snapshot, const ChainstateManager& chainman,
                                     const CBlockIndex* base_index, std::string& error)
{
    AssertLockHeld(::cs_main);
    error.clear();
    const auto fail = [&](const std::string& message) {
        error = message;
        return false;
    };
    if (base_index == nullptr || snapshot.base_block_hash != base_index->GetBlockHash() ||
        snapshot.mn_list.GetBlockHash() != base_index->GetBlockHash() ||
        snapshot.mn_list.GetHeightForSnapshotCodec() != base_index->nHeight) {
        return fail("evo snapshot base block/height mismatch");
    }
    try {
        snapshot.Validate(/*require_canonical_order=*/true);
    } catch (const std::exception& e) {
        return fail(e.what());
    }

    const auto& consensus{chainman.GetConsensus()};
    if (!DeploymentActiveAt(*base_index, consensus, Consensus::DEPLOYMENT_DIP0003)) {
        if (!snapshot.quorums.empty() || !snapshot.historical_mn_list_diffs.empty() ||
            !snapshot.quorum_modifiers.empty() ||
            snapshot.credit_pool.locked != 0 || snapshot.credit_pool.currentLimit != 0 ||
            snapshot.credit_pool.latelyUnlocked != 0 || !snapshot.credit_pool.indexes.IsEmpty() ||
            !snapshot.mnhf_signals.empty()) {
            return fail("nonempty pre-DIP3 evo snapshot");
        }
        return true;
    }

    std::map<uint256, CDeterministicMNList> historical_lists;
    if (!ReconstructHistoricalMNLists(snapshot, historical_lists, error)) return false;
    for (const auto& entry : snapshot.historical_mn_list_diffs) {
        const CBlockIndex* index{chainman.m_blockman.LookupBlockIndex(entry.block_hash)};
        if (index == nullptr || base_index->GetAncestor(index->nHeight) != index ||
            entry.height != index->nHeight) {
            return fail("invalid historical evo MN list chain data");
        }
    }

    std::map<Consensus::LLMQType, const CQuorumSnapshotData*> actual;
    for (const auto& data : snapshot.quorums) actual.emplace(data.llmq_type, &data);
    size_t enabled_count{0};
    std::set<uint256> required_work_hashes;
    for (const auto& params : consensus.llmqs) {
        if (!chainman.IsQuorumTypeEnabled(params.type, base_index)) continue;
        ++enabled_count;
        const auto it{actual.find(params.type)};
        if (it == actual.end()) return fail("missing enabled evo quorum type");
        const auto& data{*it->second};
        const bool rotation_enabled{llmq::IsQuorumRotationEnabled(params, base_index)};
        const size_t active_count{static_cast<size_t>(params.signingActiveQuorumCount)};
        const size_t total_count{SnapshotCommitmentCount(params, rotation_enabled)};
        if (data.rotation_enabled != rotation_enabled || data.active_commitments.size() > active_count ||
            data.safety_commitments.size() > total_count - active_count) {
            return fail("evo quorum params/count mismatch");
        }

        std::set<int16_t> active_indexes;
        const auto validate_work_block = [&](const CMinedQuorumCommitment& entry) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
            const CBlockIndex* quorum_index{chainman.m_blockman.LookupBlockIndex(entry.quorum_base_block_hash)};
            if (quorum_index == nullptr) return false;
            const CBlockIndex* modifier_base{rotation_enabled
                ? quorum_index->GetAncestor(quorum_index->nHeight - quorum_index->nHeight % params.dkgInterval)
                : quorum_index};
            if (modifier_base == nullptr) return false;
            const CBlockIndex* expected_work{
                (rotation_enabled || DeploymentActiveAfter(modifier_base, consensus, Consensus::DEPLOYMENT_V20))
                    ? modifier_base->GetAncestor(modifier_base->nHeight - llmq::WORK_DIFF_DEPTH)
                    : modifier_base};
            return expected_work != nullptr && entry.work_block_hash == expected_work->GetBlockHash();
        };
        for (const auto& entry : data.active_commitments) {
            if (!ValidateCommitmentAgainstChain(entry, chainman, base_index, params, rotation_enabled) ||
                !validate_work_block(entry) ||
                (rotation_enabled && !active_indexes.insert(entry.commitment.quorumIndex).second)) {
                return fail("invalid active evo quorum commitment chain data");
            }
            required_work_hashes.insert(entry.work_block_hash);
        }
        for (const auto& entry : data.safety_commitments) {
            if (!ValidateCommitmentAgainstChain(entry, chainman, base_index, params, rotation_enabled) ||
                !validate_work_block(entry)) {
                return fail("invalid safety evo quorum commitment chain data");
            }
            required_work_hashes.insert(entry.work_block_hash);
        }
        std::map<uint256, const CQuorumSnapshotEntry*> rotations;
        for (const auto& entry : data.rotation_snapshots) rotations.emplace(entry.cycle_base_block_hash, &entry);
        const auto heights{EvoSnapshotReconstructionHeights(base_index->nHeight, {params})};
        if (rotations.size() > (rotation_enabled ? heights.size() : size_t{0})) {
            return fail("evo rotation snapshot count mismatch");
        }
        if (rotation_enabled) {
            // Every carried rotation snapshot must sit at a derived horizon
            // cycle with the matching work ancestor. Horizons the chain or the
            // type's rotation history cannot provide are legitimately absent;
            // completion-time quorum reconstruction establishes sufficiency.
            size_t matched{0};
            for (const auto& required : heights) {
                const int cycle_height{required.quorum_height};
                const int work_height{required.work_height};
                const CBlockIndex* cycle{cycle_height >= 0 ? base_index->GetAncestor(cycle_height) : nullptr};
                const CBlockIndex* work{work_height >= 0 ? base_index->GetAncestor(work_height) : nullptr};
                if (cycle == nullptr || work == nullptr) continue;
                const auto rotation{rotations.find(cycle->GetBlockHash())};
                if (rotation == rotations.end()) continue;
                if (rotation->second->work_block_hash != work->GetBlockHash()) {
                    return fail("evo rotation cycle/work ancestor mismatch");
                }
                const auto historical{historical_lists.find(work->GetBlockHash())};
                if (historical == historical_lists.end() ||
                    rotation->second->snapshot.activeQuorumMembers.size() !=
                        historical->second.GetCounts().total()) {
                    return fail("evo rotation bitset/work-block MN count mismatch");
                }
                required_work_hashes.insert(work->GetBlockHash());
                ++matched;
            }
            if (matched != rotations.size()) return fail("unknown evo rotation cycle");
        }
    }
    if (actual.size() != enabled_count) return fail("unexpected disabled evo quorum type");

    std::set<uint256> historical_hashes;
    for (const auto& [hash, list] : historical_lists) historical_hashes.insert(hash);
    historical_hashes.erase(base_index->GetBlockHash());
    required_work_hashes.erase(base_index->GetBlockHash());
    if (historical_hashes != required_work_hashes) return fail("missing or extra historical evo MN list");

    std::map<std::pair<Consensus::LLMQType, uint256>, uint256> seeded_modifiers;
    for (const auto& entry : snapshot.quorum_modifiers) {
        seeded_modifiers.emplace(std::make_pair(entry.llmq_type, entry.work_block_hash), entry.modifier);
    }
    for (const auto& data : snapshot.quorums) {
        const auto params{chainman.GetParams().GetLLMQ(data.llmq_type)};
        if (!params) return fail("unknown chain LLMQ parameters for modifier");
        const auto check_modifier = [&](const uint256& quorum_hash, const uint256& work_hash) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
            const auto seeded{seeded_modifiers.find(std::make_pair(data.llmq_type, work_hash))};
            const CBlockIndex* quorum_index{chainman.m_blockman.LookupBlockIndex(quorum_hash)};
            const CBlockIndex* work_index{chainman.m_blockman.LookupBlockIndex(work_hash)};
            if (seeded == seeded_modifiers.end() || quorum_index == nullptr || work_index == nullptr) return false;
            if ((work_index->nStatus & BLOCK_HAVE_DATA) != 0 &&
                seeded->second != llmq::utils::GetQuorumHashModifier(*params, consensus, quorum_index)) return false;
            return true;
        };
        for (const auto* commitments : {&data.active_commitments, &data.safety_commitments}) {
            for (const auto& entry : *commitments) {
                const CBlockIndex* quorum_index{chainman.m_blockman.LookupBlockIndex(entry.quorum_base_block_hash)};
                const CBlockIndex* modifier_base{data.rotation_enabled && quorum_index != nullptr
                    ? quorum_index->GetAncestor(quorum_index->nHeight - quorum_index->nHeight % params->dkgInterval)
                    : quorum_index};
                if (modifier_base == nullptr ||
                    !check_modifier(modifier_base->GetBlockHash(), entry.work_block_hash)) {
                    return fail("evo seeded quorum modifier mismatch");
                }
            }
        }
        for (const auto& entry : data.rotation_snapshots) {
            if (!check_modifier(entry.cycle_base_block_hash, entry.work_block_hash)) {
                return fail("evo seeded rotation modifier mismatch");
            }
        }
    }

    if (!MoneyRange(snapshot.credit_pool.locked) || !MoneyRange(snapshot.credit_pool.currentLimit) ||
        !MoneyRange(snapshot.credit_pool.latelyUnlocked)) return fail("invalid evo credit pool monetary value");
    for (const auto& [bit, height] : snapshot.mnhf_signals) {
        if (bit >= VERSIONBITS_NUM_BITS || height < 0 || height > base_index->nHeight) {
            return fail("invalid evo MNHF signal bit/height");
        }
    }
    return true;
}

} // namespace evo
