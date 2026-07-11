// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/snapshot.h>

#include <chain.h>
#include <chainparams.h>
#include <clientversion.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <crypto/sha256.h>
#include <evo/cbtx.h>
#include <evo/mnhftx.h>
#include <evo/simplifiedmns.h>
#include <deploymentstatus.h>
#include <hash.h>
#include <llmq/blockprocessor.h>
#include <llmq/options.h>
#include <llmq/utils.h>
#include <streams.h>
#include <tinyformat.h>
#include <util/check.h>
#include <validation.h>

#include <algorithm>
#include <ios>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <type_traits>

namespace evo {
namespace {

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

template <typename T>
bool IsStrictlySorted(const std::vector<T>& values)
{
    return std::adjacent_find(values.begin(), values.end(), [](const T& a, const T& b) {
               if constexpr (std::is_same_v<T, CMinedQuorumCommitment>) {
                   return std::tie(a.quorum_base_block_hash, a.mined_block_hash) >=
                          std::tie(b.quorum_base_block_hash, b.mined_block_hash);
               } else if constexpr (std::is_same_v<T, CQuorumSnapshotEntry>) {
                   return !(a.cycle_base_block_hash < b.cycle_base_block_hash);
               } else if constexpr (std::is_same_v<T, CHistoricalMNListDiff>) {
                   return !(std::tie(a.height, a.block_hash) > std::tie(b.height, b.block_hash));
               } else if constexpr (std::is_same_v<T, CQuorumModifier>) {
                   return !(std::tie(a.llmq_type, a.work_block_hash) < std::tie(b.llmq_type, b.work_block_hash));
               } else {
                   return a.llmq_type >= b.llmq_type;
               }
           }) == values.end();
}

void ValidateCommitments(const CQuorumSnapshotData& data, const std::vector<CMinedQuorumCommitment>& commitments,
                         std::set<uint256>& quorum_hashes, bool require_canonical_order)
{
    if (require_canonical_order && !IsStrictlySorted(commitments)) {
        throw std::ios_base::failure("noncanonical evo quorum commitments");
    }
    std::set<int16_t> quorum_indexes;
    const auto& params{SnapshotLLMQParams(data.llmq_type)};
    for (const auto& entry : commitments) {
        const bool known_version{
            entry.commitment.nVersion == llmq::CFinalCommitment::LEGACY_BLS_NON_INDEXED_QUORUM_VERSION ||
            entry.commitment.nVersion == llmq::CFinalCommitment::LEGACY_BLS_INDEXED_QUORUM_VERSION ||
            entry.commitment.nVersion == llmq::CFinalCommitment::BASIC_BLS_NON_INDEXED_QUORUM_VERSION ||
            entry.commitment.nVersion == llmq::CFinalCommitment::BASIC_BLS_INDEXED_QUORUM_VERSION};
        const bool indexed{entry.commitment.nVersion == llmq::CFinalCommitment::LEGACY_BLS_INDEXED_QUORUM_VERSION ||
                           entry.commitment.nVersion == llmq::CFinalCommitment::BASIC_BLS_INDEXED_QUORUM_VERSION};
        if (!entry.commitment.VerifySizes(params)) throw std::ios_base::failure("invalid evo quorum commitment sizes");
        if (!known_version) throw std::ios_base::failure("unknown evo quorum commitment version");
        if (entry.quorum_base_block_hash.IsNull() || entry.work_block_hash.IsNull() || entry.mined_block_hash.IsNull()) {
            throw std::ios_base::failure("null evo quorum commitment block hash");
        }
        if (entry.commitment.llmqType != data.llmq_type) {
            throw std::ios_base::failure("mismatched evo quorum commitment type");
        }
        if (entry.commitment.quorumHash != entry.quorum_base_block_hash) {
            throw std::ios_base::failure("mismatched evo quorum commitment base hash");
        }
        if (indexed != data.rotation_enabled) throw std::ios_base::failure("mismatched evo quorum rotation version");
        if (indexed && (entry.commitment.quorumIndex < 0 ||
                        entry.commitment.quorumIndex >= params.signingActiveQuorumCount)) {
            throw std::ios_base::failure("invalid evo quorum index");
        }
        if (!quorum_hashes.insert(entry.quorum_base_block_hash).second) {
            throw std::ios_base::failure("duplicate evo quorum base hash");
        }
        if (indexed && !quorum_indexes.insert(entry.commitment.quorumIndex).second) {
            throw std::ios_base::failure("duplicate evo quorum index");
        }
    }
}

CMinedQuorumCommitment ReadCommitment(const llmq::CQuorumBlockProcessor& qblockman, Consensus::LLMQType type,
                                      const CBlockIndex* quorum_index, const CBlockIndex* work_index,
                                      std::string& error)
{
    auto [commitment, mined_hash] = qblockman.GetMinedCommitment(type, quorum_index->GetBlockHash());
    if (mined_hash.IsNull()) error = "mined quorum commitment not found for " + quorum_index->GetBlockHash().ToString();
    return {quorum_index->GetBlockHash(), work_index->GetBlockHash(), std::move(commitment), mined_hash};
}

void ValidateCanonicalMNInvariants(const CDeterministicMNList& list)
{
    const size_t count{list.GetCounts().total()};
    if (count > EVO_SNAPSHOT_MAX_MNS) throw std::ios_base::failure("oversized canonical MN list");
    uint64_t max_internal_id{0};
    list.ForEachMN(/*onlyValid=*/false, [&](const auto& dmn) {
        max_internal_id = std::max(max_internal_id, dmn.GetInternalId());
        if (dmn.pdmnState->payouts.size() > EVO_SNAPSHOT_MAX_PAYOUT_SHARES ||
            dmn.pdmnState->netInfo->Validate() != NetInfoStatus::Success) {
            throw std::ios_base::failure("invalid canonical MN nested collection");
        }
    });
    if (count != 0 && max_internal_id >= list.GetTotalRegisteredCount()) {
        throw std::ios_base::failure("canonical MN-list internalId exceeds registration counter");
    }
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

} // namespace

std::vector<CQuorumReconstructionHeight> EvoSnapshotReconstructionHeights(
    int base_height, const std::vector<Consensus::LLMQParams>& enabled_llmqs)
{
    if (base_height < 0) throw std::invalid_argument("invalid reconstruction base height");
    std::vector<CQuorumReconstructionHeight> heights;
    for (const auto& params : enabled_llmqs) {
        if (params.dkgInterval <= 0 || params.signingActiveQuorumCount <= 0) {
            throw std::invalid_argument("invalid reconstruction LLMQ parameters");
        }
        const int h{base_height - base_height % params.dkgInterval};
        const size_t count{params.useRotation ? EVO_SNAPSHOT_ROTATION_CYCLES
                                              : SnapshotCommitmentCount(params, /*rotation_enabled=*/false)};
        const size_t first{params.useRotation ? 1U : 0U};
        for (size_t i{first}; i < first + count; ++i) {
            const int quorum_height{h - static_cast<int>(i) * params.dkgInterval};
            heights.push_back({params.type, params.useRotation, quorum_height,
                               quorum_height - llmq::WORK_DIFF_DEPTH});
        }
    }
    return heights;
}

uint256 CanonicalMNListHash(const CDeterministicMNList& list)
{
    CHashWriter writer{SER_DISK, CLIENT_VERSION};
    SerializeCanonicalMNList(writer, list);
    return writer.GetHash();
}

bool ReconstructHistoricalMNLists(const CEvoSnapshot& snapshot,
                                  std::map<uint256, CDeterministicMNList>& lists, std::string& error)
{
    lists.clear();
    error.clear();
    CDeterministicMNList current{snapshot.mn_list};
    uint256 previous_hash{snapshot.base_block_hash};
    int previous_height{current.GetHeightForSnapshotCodec()};
    try {
        const auto history{Sorted(snapshot.historical_mn_list_diffs)};
        for (const auto& entry : history) {
            if (entry.previous_block_hash != previous_hash || entry.block_hash.IsNull() ||
                entry.height < 0 || entry.height >= previous_height || entry.canonical_list_hash.IsNull()) {
                throw std::ios_base::failure("broken historical MN-list diff chain");
            }
            current.ApplyDiffForSnapshot(entry.block_hash, entry.height, entry.total_registered_count, entry.diff);
            ValidateCanonicalMNInvariants(current);
            if (CanonicalMNListHash(current) != entry.canonical_list_hash) {
                throw std::ios_base::failure("historical MN-list diff hash mismatch");
            }
            if (!lists.emplace(entry.block_hash, current).second) {
                throw std::ios_base::failure("duplicate historical MN-list diff target");
            }
            previous_hash = entry.block_hash;
            previous_height = entry.height;
        }
    } catch (const std::exception& e) {
        error = e.what();
        lists.clear();
        return false;
    }
    return true;
}

void CEvoSnapshot::Validate(bool require_canonical_order) const
{
    if (version != EVO_SNAPSHOT_VERSION) throw std::ios_base::failure("unsupported evo snapshot version");
    if (base_block_hash.IsNull() || mn_list.GetBlockHash() != base_block_hash) {
        throw std::ios_base::failure("evo snapshot base block mismatch");
    }
    ValidateCanonicalMNInvariants(mn_list);
    if (quorums.size() > Consensus::available_llmqs.size() ||
        historical_mn_list_diffs.size() > EvoSnapshotMaxHistoricalMNLists() ||
        quorum_modifiers.size() > EVO_SNAPSHOT_MAX_MODIFIERS ||
        mnhf_signals.size() > Consensus::MAX_VERSION_BITS_DEPLOYMENTS) {
        throw std::ios_base::failure("oversized evo snapshot collection");
    }
    if (require_canonical_order && (!IsStrictlySorted(quorums) || !IsStrictlySorted(historical_mn_list_diffs) ||
                                    !IsStrictlySorted(quorum_modifiers))) {
        throw std::ios_base::failure("noncanonical evo snapshot top-level order");
    }

    std::map<uint256, CDeterministicMNList> reconstructed;
    std::string reconstruction_error;
    if (!ReconstructHistoricalMNLists(*this, reconstructed, reconstruction_error)) {
        throw std::ios_base::failure(reconstruction_error);
    }
    std::set<uint256> historical_hashes;
    for (const auto& [hash, _] : reconstructed) historical_hashes.insert(hash);

    std::set<std::pair<Consensus::LLMQType, uint256>> required_modifiers;
    std::set<uint256> required_work_hashes;

    std::set<Consensus::LLMQType> quorum_types;
    for (const auto& data : quorums) {
        const auto& params{SnapshotLLMQParams(data.llmq_type)};
        if (!quorum_types.insert(data.llmq_type).second || (data.rotation_enabled && !params.useRotation)) {
            throw std::ios_base::failure("invalid evo quorum type");
        }
        const size_t active_count{static_cast<size_t>(params.signingActiveQuorumCount)};
        const size_t total_count{SnapshotCommitmentCount(params, data.rotation_enabled)};
        if (data.active_commitments.size() != active_count ||
            data.safety_commitments.size() != total_count - active_count ||
            data.rotation_snapshots.size() != (data.rotation_enabled ? EVO_SNAPSHOT_ROTATION_CYCLES : 0)) {
            throw std::ios_base::failure("invalid params-derived evo per-type quorum counts");
        }
        std::set<uint256> quorum_hashes;
        ValidateCommitments(data, data.active_commitments, quorum_hashes, require_canonical_order);
        ValidateCommitments(data, data.safety_commitments, quorum_hashes, require_canonical_order);
        for (const auto* commitments : {&data.active_commitments, &data.safety_commitments}) {
            for (const auto& entry : *commitments) {
                required_work_hashes.insert(entry.work_block_hash);
                required_modifiers.emplace(data.llmq_type, entry.work_block_hash);
            }
        }
        if (require_canonical_order && !IsStrictlySorted(data.rotation_snapshots)) {
            throw std::ios_base::failure("noncanonical evo quorum rotation snapshots");
        }
        std::set<uint256> cycle_hashes;
        for (const auto& entry : data.rotation_snapshots) {
            if (entry.cycle_base_block_hash.IsNull() || entry.work_block_hash.IsNull() ||
                !cycle_hashes.insert(entry.cycle_base_block_hash).second ||
                !historical_hashes.contains(entry.work_block_hash) ||
                entry.snapshot.mnSkipListMode < SnapshotSkipMode::MODE_NO_SKIPPING ||
                entry.snapshot.mnSkipListMode > SnapshotSkipMode::MODE_ALL_SKIPPED ||
                entry.snapshot.activeQuorumMembers.size() > EVO_SNAPSHOT_MAX_MNS ||
                entry.snapshot.mnSkipList.size() > static_cast<size_t>(params.size) ||
                std::ranges::any_of(entry.snapshot.mnSkipList, [](int index) { return index < 0; })) {
                throw std::ios_base::failure("invalid evo quorum rotation snapshot");
            }
            required_work_hashes.insert(entry.work_block_hash);
            required_modifiers.emplace(data.llmq_type, entry.work_block_hash);
        }
    }
    if (historical_hashes != required_work_hashes) {
        throw std::ios_base::failure("missing or extra historical MN-list diff target");
    }
    std::set<std::pair<Consensus::LLMQType, uint256>> actual_modifiers;
    for (const auto& entry : quorum_modifiers) {
        SnapshotLLMQParams(entry.llmq_type);
        if (entry.work_block_hash.IsNull() || entry.modifier.IsNull() ||
            !actual_modifiers.emplace(entry.llmq_type, entry.work_block_hash).second) {
            throw std::ios_base::failure("invalid or duplicate evo quorum modifier");
        }
    }
    if (actual_modifiers != required_modifiers) {
        throw std::ios_base::failure("missing or extra evo quorum modifier");
    }
}

uint256 GetEvoSnapshotHash(const CEvoSnapshot& snapshot)
{
    snapshot.Validate();
    CDataStream stream{SER_DISK, CLIENT_VERSION};
    stream << snapshot;
    uint256 hash;
    CSHA256().Write(UCharCast(stream.data()), stream.size()).Finalize(hash.begin());
    return hash;
}

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
        if (indexes.size() < active_count) {
            error = strprintf("not enough active quorum commitments for LLMQ type %d", static_cast<int>(params.type));
            return false;
        }
        for (size_t i{0}; i < active_count; ++i) {
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
            indexes.erase(indexes.begin(), indexes.begin() + active_count);
        }
        const size_t safety_count{total_count - active_count};
        if (indexes.size() < safety_count) {
            error = strprintf("not enough safety quorum commitments for LLMQ type %d", static_cast<int>(params.type));
            return false;
        }
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
                if (cycle_index == nullptr || work_index == nullptr) {
                    error = "missing quorum rotation cycle/work block";
                    return false;
                }
                auto stored{qsnapman.GetSnapshotForBlock(params.type, cycle_index)};
                if (!stored) {
                    error = "missing quorum rotation snapshot at " + cycle_index->GetBlockHash().ToString();
                    return false;
                }
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
        if (data.rotation_enabled != rotation_enabled || data.active_commitments.size() != active_count ||
            data.safety_commitments.size() != total_count - active_count) {
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
        if (rotation_enabled && active_indexes.size() != active_count) {
            return fail("incomplete active rotated quorum indexes");
        }

        std::map<uint256, const CQuorumSnapshotEntry*> rotations;
        for (const auto& entry : data.rotation_snapshots) rotations.emplace(entry.cycle_base_block_hash, &entry);
        const auto heights{EvoSnapshotReconstructionHeights(base_index->nHeight, {params})};
        if (rotations.size() != (rotation_enabled ? heights.size() : 0)) {
            return fail("evo rotation snapshot count mismatch");
        }
        if (rotation_enabled) {
            for (const auto& required : heights) {
                const int cycle_height{required.quorum_height};
                const int work_height{required.work_height};
                const CBlockIndex* cycle{base_index->GetAncestor(cycle_height)};
                const CBlockIndex* work{base_index->GetAncestor(work_height)};
                if (cycle == nullptr || work == nullptr) return fail("evo rotation horizon precedes chain");
                const auto rotation{rotations.find(cycle->GetBlockHash())};
                if (rotation == rotations.end() || rotation->second->work_block_hash != work->GetBlockHash()) {
                    return fail("evo rotation cycle/work ancestor mismatch");
                }
                const auto historical{historical_lists.find(work->GetBlockHash())};
                if (historical == historical_lists.end() ||
                    rotation->second->snapshot.activeQuorumMembers.size() !=
                        historical->second.GetCounts().total()) {
                    return fail("evo rotation bitset/work-block MN count mismatch");
                }
                required_work_hashes.insert(work->GetBlockHash());
            }
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

bool VerifyEvoSnapshotCbTx(const CEvoSnapshot& snapshot, const CCbTx& cbtx, std::string& error)
{
    error.clear();
    try {
        snapshot.Validate();
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    bool mutated{false};
    const uint256 mn_root{snapshot.mn_list.to_sml()->CalcMerkleRoot(&mutated)};
    if (mutated || mn_root != cbtx.merkleRootMNList) {
        error = "evo snapshot masternode merkle root mismatch";
        return false;
    }
    if (cbtx.nVersion >= CCbTx::Version::MERKLE_ROOT_QUORUMS) {
        std::vector<uint256> hashes;
        for (const auto& data : snapshot.quorums) {
            for (const auto& entry : data.active_commitments) hashes.emplace_back(SerializeHash(entry.commitment));
        }
        std::sort(hashes.begin(), hashes.end());
        const uint256 quorum_root{ComputeMerkleRoot(hashes, &mutated)};
        if (mutated || quorum_root != cbtx.merkleRootQuorums) {
            error = "evo snapshot quorum merkle root mismatch";
            return false;
        }
    }
    if (cbtx.nVersion >= CCbTx::Version::CLSIG_AND_BALANCE && snapshot.credit_pool.locked != cbtx.creditPoolBalance) {
        error = "evo snapshot credit pool balance mismatch";
        return false;
    }
    return true;
}

} // namespace evo
