// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/snapshot.h>

#include <clientversion.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <crypto/sha256.h>
#include <evo/cbtx.h>
#include <evo/mnhftx.h>
#include <evo/simplifiedmns.h>
#include <hash.h>
#include <llmq/options.h>
#include <streams.h>
#include <tinyformat.h>
#include <util/check.h>

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
        // The effective quorum size is runtime-configurable, so this layer
        // enforces only internal consistency and the format ceiling; the
        // chain-aware validation checks exact sizes against effective params.
        if (entry.commitment.signers.size() != entry.commitment.validMembers.size() ||
            entry.commitment.signers.empty() ||
            entry.commitment.signers.size() > EVO_SNAPSHOT_MAX_QUORUM_SIZE) {
            throw std::ios_base::failure("invalid evo quorum commitment sizes");
        }
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
    // ConstructCreditPool guarantees 0 <= currentLimit <= locked in every
    // deployment branch, and all three amounts are money-range window sums.
    if (!MoneyRange(credit_pool.locked) || !MoneyRange(credit_pool.currentLimit) ||
        !MoneyRange(credit_pool.latelyUnlocked) || credit_pool.currentLimit > credit_pool.locked) {
        throw std::ios_base::failure("invalid evo snapshot credit pool amounts");
    }
    // Consensus admits MNHF signals only for bits below VERSIONBITS_NUM_BITS
    // and records the mined height, which cannot exceed the base height.
    for (const auto& [bit, height] : mnhf_signals) {
        if (bit >= VERSIONBITS_NUM_BITS || height < 0 || height > mn_list.GetHeightForSnapshotCodec()) {
            throw std::ios_base::failure("invalid evo snapshot MNHF signal");
        }
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
        // Parameter-derived counts are maxima, not exact requirements: a young
        // chain carries however much quorum history exists. The chain-aware
        // validation and the completion-time CbTx quorum merkle root establish
        // that nothing available was withheld.
        if (data.active_commitments.size() > active_count ||
            data.safety_commitments.size() > total_count - active_count ||
            data.rotation_snapshots.size() > (data.rotation_enabled ? EVO_SNAPSHOT_ROTATION_CYCLES : size_t{0})) {
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
                entry.snapshot.mnSkipList.size() > EVO_SNAPSHOT_MAX_SKIPLIST_ENTRIES ||
                // Only the first entry is an absolute index; later entries are
                // deltas that legitimately go negative once the build wraps the
                // combined MN list. Semantic validity is established by quorum
                // reconstruction against chain state, not here.
                (!entry.snapshot.mnSkipList.empty() && entry.snapshot.mnSkipList.front() < 0)) {
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
