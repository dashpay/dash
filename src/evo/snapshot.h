// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_SNAPSHOT_H
#define BITCOIN_EVO_SNAPSHOT_H

#include <consensus/params.h>
#include <evo/creditpool.h>
#include <evo/deterministicmns.h>
#include <llmq/commitment.h>
#include <llmq/params.h>
#include <llmq/snapshot.h>
#include <versionbits.h>

#include <serialize.h>
#include <uint256.h>
#include <util/check.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

class CBlockIndex;
class CChainParams;
class ChainstateManager;
class CCbTx;
class CCreditPoolManager;
class CMNHFManager;

namespace llmq {
class CQuorumBlockProcessor;
class CQuorumSnapshotManager;
} // namespace llmq

namespace evo {

class SnapshotStateMismatchError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

static constexpr uint16_t EVO_SNAPSHOT_VERSION{3};
/** Serialized little-endian bytes are "DASHEVO\0". */
static constexpr uint64_t EVO_SNAPSHOT_MARKER{0x004f564548534144ULL};
// ComputeQuorumMembersByQuarterRotation consumes H-C, H-2C and H-3C. To
// reconstruct both H and the safety cycle H-C, the union is H-C..H-4C.
static constexpr size_t EVO_SNAPSHOT_ROTATION_CYCLES{4};
// A hard allocation bound, not a network population target. 100,000 full MN
// records is already far beyond today's list while limiting hostile snapshots
// to a tractable decode. Changes above this require a format-version review.
static constexpr size_t EVO_SNAPSHOT_MAX_MNS{100'000};
// Asset-unlock indexes are uint64_t and have no consensus upper bound. This is
// a range-count allocation/work bound, chosen far above any plausible live
// state. Raising it requires an evo snapshot format-version review.
static constexpr size_t EVO_SNAPSHOT_MAX_RANGES{100'000};
// IsPayoutListTriviallyValid() is the protocol admission rule for MultiPayout.
static constexpr size_t EVO_SNAPSHOT_MAX_PAYOUT_SHARES{8};
// CDeterministicMN contains several consensus/P2P CompactSize collections
// (scripts, payout shares, and ExtNetInfo maps/lists). Snapshot decoding gives
// each MN a cumulative budget so nested counts cannot multiply decode work.
// This comfortably covers protocol-valid scripts and network information.
static constexpr size_t EVO_SNAPSHOT_MAX_MN_COMPACT_ITEMS{10'000};
static constexpr size_t EVO_SNAPSHOT_MAX_MODIFIERS{4'096};
static_assert(std::ranges::all_of(Consensus::available_llmqs, [](const auto& params) {
    return !params.useRotation || params.keepOldConnections <= 2 * params.signingActiveQuorumCount;
}), "rotated LLMQ retention exceeds the two serialized cycles");

template <typename Stream>
size_t ReadBoundedCompactSize(Stream& s, size_t limit, const char* field)
{
    const uint64_t size{ReadCompactSize(s)};
    if (size > limit) throw std::ios_base::failure(std::string{"oversized evo snapshot "} + field);
    return static_cast<size_t>(size);
}

inline const Consensus::LLMQParams& SnapshotLLMQParams(Consensus::LLMQType type)
{
    const auto it{std::ranges::find_if(Consensus::available_llmqs,
                                       [type](const auto& params) { return params.type == type; })};
    if (it == Consensus::available_llmqs.end()) throw std::ios_base::failure("unknown evo snapshot LLMQ type");
    return *it;
}

inline size_t SnapshotCommitmentCount(const Consensus::LLMQParams& params, bool rotation_enabled)
{
    if (!rotation_enabled) {
        return static_cast<size_t>(std::max(params.signingActiveQuorumCount + 1, params.keepOldConnections));
    }
    const size_t active{static_cast<size_t>(params.signingActiveQuorumCount)};
    const size_t retained{static_cast<size_t>(params.keepOldConnections)};
    // Rotation seeding promises the active and previous complete cycles. A
    // future parameter set retaining more must extend the serialized cycles.
    if (retained > 2 * active) throw std::ios_base::failure("rotated LLMQ retention exceeds two cycles");
    return 2 * active;
}

/**
 * Maximum number of distinct historical work-block lists a snapshot can need.
 * The serialized set is deduplicated, so summing every enabled-type horizon is
 * conservative: two retained commitment cycles plus H-C..H-4C for rotated
 * types, or the retained commitment horizon for non-rotated types.
 */
inline size_t EvoSnapshotMaxHistoricalMNLists()
{
    size_t count{0};
    for (const auto& params : Consensus::available_llmqs) {
        count += params.useRotation
                     ? SnapshotCommitmentCount(params, /*rotation_enabled=*/true) + EVO_SNAPSHOT_ROTATION_CYCLES
                     : SnapshotCommitmentCount(params, /*rotation_enabled=*/false);
    }
    return count;
}

/**
 * A historical diff covers one required quorum work-block transition. Allow
 * 4,096 net add/update/remove operations per transition (already far above
 * plausible per-block MN churn), across the entire params-derived horizon.
 * This generous cumulative ceiling prevents individually-valid 100k-entry
 * diffs from multiplying decode work across every historical entry.
 */
inline size_t EvoSnapshotMaxHistoricalMNOperations()
{
    return EvoSnapshotMaxHistoricalMNLists() * 4'096;
}

template <typename Stream>
class SnapshotBoundedInput
{
private:
    Stream& m_stream;
    uint64_t m_compact_budget;

public:
    SnapshotBoundedInput(Stream& stream, uint64_t compact_budget) :
        m_stream{stream}, m_compact_budget{compact_budget} {}

    int GetType() const { return m_stream.GetType(); }
    int GetVersion() const { return m_stream.GetVersion(); }
    void read(Span<std::byte> dst) { m_stream.read(dst); }
    void ignore(size_t size) { m_stream.ignore(size); }

    uint64_t ReadBudgetedCompactSize()
    {
        const uint64_t size{::ReadCompactSize(m_stream)};
        if (size > m_compact_budget) throw std::ios_base::failure("canonical MN nested CompactSize budget exceeded");
        m_compact_budget -= size;
        return size;
    }

    template <typename T>
    SnapshotBoundedInput& operator>>(T&& obj)
    {
        ::Unserialize(*this, obj);
        return *this;
    }
};

template <typename Stream>
uint64_t ReadCompactSize(SnapshotBoundedInput<Stream>& stream)
{
    return stream.ReadBudgetedCompactSize();
}

/**
 * NetInfoEntry overrides the stream version while decoding its payload. Keep
 * the snapshot-local CompactSize budget visible through that transparent
 * wrapper so strings are rejected before their deserializer resizes them.
 */
template <typename Stream>
uint64_t ReadCompactSize(OverrideStream<SnapshotBoundedInput<Stream>>& stream)
{
    return stream.GetStream().ReadBudgetedCompactSize();
}

/**
 * Snapshot-local canonical deterministic-MN encoding.
 *
 * internalId and nTotalRegisteredCount are intentionally retained. They are
 * consensus-deterministic for nodes synced from genesis: registrations assign
 * internalId in on-chain order and advance the counter identically. Thus a
 * from-genesis background validation re-derives the dumper's exact values.
 * Entries are sorted by the full proTxHash, never by immer iteration order.
 */
template <typename Stream>
void SerializeCanonicalMNList(Stream& s, const CDeterministicMNList& list)
{
    s << list.GetBlockHash() << list.GetHeightForSnapshotCodec() << list.GetTotalRegisteredCount();
    std::vector<CDeterministicMNCPtr> mns;
    mns.reserve(list.GetCounts().total());
    list.ForEachMNShared(/*onlyValid=*/false, [&](const auto& dmn) { mns.emplace_back(dmn); });
    std::sort(mns.begin(), mns.end(), [](const auto& a, const auto& b) { return a->proTxHash < b->proTxHash; });
    WriteCompactSize(s, mns.size());
    for (const auto& dmn : mns) s << *dmn;
}

template <typename Stream>
CDeterministicMNList UnserializeCanonicalMNList(Stream& s)
{
    uint256 block_hash;
    int height;
    uint32_t total_registered;
    s >> block_hash >> height >> total_registered;
    if (height < 0) throw std::ios_base::failure("negative canonical MN-list height");
    CDeterministicMNList list{block_hash, height, total_registered};
    const size_t count{ReadBoundedCompactSize(s, EVO_SNAPSHOT_MAX_MNS, "MN count")};
    uint256 previous;
    bool have_previous{false};
    uint64_t max_internal_id{0};
    for (size_t i{0}; i < count; ++i) {
        SnapshotBoundedInput bounded{s, EVO_SNAPSHOT_MAX_MN_COMPACT_ITEMS};
        auto dmn{std::make_shared<CDeterministicMN>(deserialize, bounded)};
        if (dmn->pdmnState->payouts.size() > EVO_SNAPSHOT_MAX_PAYOUT_SHARES) {
            throw std::ios_base::failure("oversized canonical MN payout list");
        }
        if (dmn->pdmnState->netInfo->Validate() != NetInfoStatus::Success) {
            throw std::ios_base::failure("invalid canonical MN network info");
        }
        if (have_previous && !(previous < dmn->proTxHash)) {
            throw std::ios_base::failure("noncanonical canonical MN-list order");
        }
        previous = dmn->proTxHash;
        have_previous = true;
        max_internal_id = std::max(max_internal_id, dmn->GetInternalId());
        try {
            list.AddMN(dmn, /*fBumpTotalCount=*/false);
        } catch (const std::exception& e) {
            throw std::ios_base::failure(std::string{"invalid canonical MN list: "} + e.what());
        }
    }
    if (count != 0 && max_internal_id >= total_registered) {
        throw std::ios_base::failure("canonical MN-list internalId exceeds registration counter");
    }
    return list;
}

/** Canonical hash shared by snapshot encoding and M3 completion comparison. */
uint256 CanonicalMNListHash(const CDeterministicMNList& list);

/** Canonical snapshot-local encoding of a deterministic-MN list diff. */
template <typename Stream>
void SerializeCanonicalMNListDiff(Stream& s, const CDeterministicMNListDiff& diff)
{
    auto added{diff.addedMNs};
    std::sort(added.begin(), added.end(), [](const auto& a, const auto& b) {
        return std::make_tuple(a->GetInternalId(), a->proTxHash) <
               std::make_tuple(b->GetInternalId(), b->proTxHash);
    });
    WriteCompactSize(s, added.size());
    for (const auto& dmn : added) s << *dmn;

    std::vector<uint64_t> updated;
    updated.reserve(diff.updatedMNs.size());
    for (const auto& [internal_id, _] : diff.updatedMNs) updated.emplace_back(internal_id);
    std::sort(updated.begin(), updated.end());
    WriteCompactSize(s, updated.size());
    for (const uint64_t internal_id : updated) {
        WriteVarInt<Stream, VarIntMode::DEFAULT, uint64_t>(s, internal_id);
        s << diff.updatedMNs.at(internal_id);
    }
    WriteCompactSize(s, diff.removedMns.size());
    for (const uint64_t internal_id : diff.removedMns) {
        WriteVarInt<Stream, VarIntMode::DEFAULT, uint64_t>(s, internal_id);
    }
}

template <typename Stream>
CDeterministicMNListDiff UnserializeCanonicalMNListDiff(Stream& s, size_t& remaining_operations)
{
    CDeterministicMNListDiff diff;
    const size_t added_count{ReadBoundedCompactSize(s, EVO_SNAPSHOT_MAX_MNS, "MN-diff additions")};
    if (added_count > remaining_operations) throw std::ios_base::failure("historical MN-diff operation budget exceeded");
    remaining_operations -= added_count;
    uint64_t previous_id{0};
    bool have_previous{false};
    diff.addedMNs.reserve(added_count);
    for (size_t i{0}; i < added_count; ++i) {
        SnapshotBoundedInput bounded{s, EVO_SNAPSHOT_MAX_MN_COMPACT_ITEMS};
        auto dmn{std::make_shared<CDeterministicMN>(deserialize, bounded)};
        if ((have_previous && previous_id >= dmn->GetInternalId()) ||
            dmn->pdmnState->payouts.size() > EVO_SNAPSHOT_MAX_PAYOUT_SHARES ||
            dmn->pdmnState->netInfo->Validate() != NetInfoStatus::Success) {
            throw std::ios_base::failure("noncanonical canonical MN-diff addition");
        }
        previous_id = dmn->GetInternalId();
        have_previous = true;
        diff.addedMNs.emplace_back(std::move(dmn));
    }

    const size_t updated_count{ReadBoundedCompactSize(s, EVO_SNAPSHOT_MAX_MNS, "MN-diff updates")};
    if (updated_count > remaining_operations) throw std::ios_base::failure("historical MN-diff operation budget exceeded");
    remaining_operations -= updated_count;
    previous_id = 0;
    have_previous = false;
    for (size_t i{0}; i < updated_count; ++i) {
        const uint64_t internal_id{ReadVarInt<Stream, VarIntMode::DEFAULT, uint64_t>(s)};
        if (have_previous && previous_id >= internal_id) {
            throw std::ios_base::failure("noncanonical canonical MN-diff update order");
        }
        SnapshotBoundedInput bounded{s, EVO_SNAPSHOT_MAX_MN_COMPACT_ITEMS};
        diff.updatedMNs.emplace(internal_id, CDeterministicMNStateDiff(deserialize, bounded));
        previous_id = internal_id;
        have_previous = true;
    }

    const size_t removed_count{ReadBoundedCompactSize(s, EVO_SNAPSHOT_MAX_MNS, "MN-diff removals")};
    if (removed_count > remaining_operations) throw std::ios_base::failure("historical MN-diff operation budget exceeded");
    remaining_operations -= removed_count;
    previous_id = 0;
    have_previous = false;
    for (size_t i{0}; i < removed_count; ++i) {
        const uint64_t internal_id{ReadVarInt<Stream, VarIntMode::DEFAULT, uint64_t>(s)};
        if (have_previous && previous_id >= internal_id) {
            throw std::ios_base::failure("noncanonical canonical MN-diff removal order");
        }
        diff.removedMns.emplace(internal_id);
        previous_id = internal_id;
        have_previous = true;
    }
    return diff;
}

template <typename Stream>
CDeterministicMNListDiff UnserializeCanonicalMNListDiff(Stream& s)
{
    size_t remaining_operations{EvoSnapshotMaxHistoricalMNOperations()};
    return UnserializeCanonicalMNListDiff(s, remaining_operations);
}

struct CMinedQuorumCommitment {
    uint256 quorum_base_block_hash;
    uint256 work_block_hash;
    llmq::CFinalCommitment commitment;
    uint256 mined_block_hash;

    SERIALIZE_METHODS(CMinedQuorumCommitment, obj)
    {
        READWRITE(obj.quorum_base_block_hash, obj.work_block_hash, obj.commitment, obj.mined_block_hash);
    }
};

template <typename Stream>
CMinedQuorumCommitment ReadMinedQuorumCommitment(Stream& s, const Consensus::LLMQParams& params)
{
    CMinedQuorumCommitment entry;
    auto& commitment{entry.commitment};
    s >> entry.quorum_base_block_hash >> entry.work_block_hash >> commitment.nVersion >> commitment.llmqType >> commitment.quorumHash;
    const bool indexed{commitment.nVersion == llmq::CFinalCommitment::LEGACY_BLS_INDEXED_QUORUM_VERSION ||
                       commitment.nVersion == llmq::CFinalCommitment::BASIC_BLS_INDEXED_QUORUM_VERSION};
    if (indexed) s >> commitment.quorumIndex;
    const size_t signers_size{ReadBoundedCompactSize(s, params.size, "commitment signers")};
    if (signers_size != static_cast<size_t>(params.size)) {
        throw std::ios_base::failure("invalid evo snapshot commitment signers size");
    }
    ReadFixedBitSet(s, commitment.signers, signers_size);
    const size_t valid_members_size{ReadBoundedCompactSize(s, params.size, "commitment valid members")};
    if (valid_members_size != static_cast<size_t>(params.size)) {
        throw std::ios_base::failure("invalid evo snapshot commitment valid-members size");
    }
    ReadFixedBitSet(s, commitment.validMembers, valid_members_size);
    const bool legacy{commitment.nVersion == llmq::CFinalCommitment::LEGACY_BLS_NON_INDEXED_QUORUM_VERSION ||
                      commitment.nVersion == llmq::CFinalCommitment::LEGACY_BLS_INDEXED_QUORUM_VERSION};
    s >> CBLSPublicKeyVersionWrapper(commitment.quorumPublicKey, legacy) >> commitment.quorumVvecHash >>
        CBLSSignatureVersionWrapper(commitment.quorumSig, legacy) >>
        CBLSSignatureVersionWrapper(commitment.membersSig, legacy);
    // The consensus/P2P serializer remains unchanged; this snapshot-local path
    // bounds both bitsets before allocation and verifies the decoded object.
    if (!entry.commitment.VerifySizes(params)) {
        throw std::ios_base::failure("invalid evo snapshot commitment bitset size");
    }
    s >> entry.mined_block_hash;
    return entry;
}

struct CQuorumSnapshotEntry {
    uint256 cycle_base_block_hash;
    uint256 work_block_hash;
    llmq::CQuorumSnapshot snapshot;
};

struct CHistoricalMNListDiff {
    uint256 previous_block_hash;
    uint256 block_hash;
    int height{-1};
    uint32_t total_registered_count{0};
    uint256 canonical_list_hash;
    CDeterministicMNListDiff diff;
};

struct CQuorumModifier {
    Consensus::LLMQType llmq_type{Consensus::LLMQType::LLMQ_NONE};
    uint256 work_block_hash;
    uint256 modifier;

    SERIALIZE_METHODS(CQuorumModifier, obj)
    {
        READWRITE(obj.llmq_type, obj.work_block_hash, obj.modifier);
    }
};

struct CQuorumSnapshotData {
    Consensus::LLMQType llmq_type{Consensus::LLMQType::LLMQ_NONE};
    bool rotation_enabled{false};
    std::vector<CMinedQuorumCommitment> active_commitments;
    std::vector<CMinedQuorumCommitment> safety_commitments;
    std::vector<CQuorumSnapshotEntry> rotation_snapshots;

    template <typename Stream> void Serialize(Stream& s) const;
    template <typename Stream> void Unserialize(Stream& s);
};

/** Canonical Dash-derived state attached to an assumeutxo snapshot. */
class CEvoSnapshot
{
public:
    uint16_t version{EVO_SNAPSHOT_VERSION};
    uint256 base_block_hash;
    CDeterministicMNList mn_list;
    std::vector<CQuorumSnapshotData> quorums;
    std::vector<CHistoricalMNListDiff> historical_mn_list_diffs;
    std::vector<CQuorumModifier> quorum_modifiers;
    CCreditPool credit_pool;
    AbstractEHFManager::Signals mnhf_signals;

    template <typename Stream> void Serialize(Stream& s) const;
    template <typename Stream> void Unserialize(Stream& s);

    /** Validate invariants not requiring chainstate or block-index lookup. */
    void Validate(bool require_canonical_order = false) const;
};

template <typename Stream, typename T, typename WriteOne>
void WriteSnapshotVector(Stream& s, const std::vector<T>& values, WriteOne&& write_one)
{
    WriteCompactSize(s, values.size());
    for (const auto& value : values) write_one(value);
}

template <typename Stream>
void WriteRotationSnapshot(Stream& s, const CQuorumSnapshotEntry& entry)
{
    s << entry.cycle_base_block_hash << entry.work_block_hash << entry.snapshot.mnSkipListMode;
    WriteCompactSize(s, entry.snapshot.activeQuorumMembers.size());
    WriteFixedBitSet(s, entry.snapshot.activeQuorumMembers, entry.snapshot.activeQuorumMembers.size());
    s << entry.snapshot.mnSkipList;
}

template <typename Stream>
CQuorumSnapshotEntry ReadRotationSnapshot(Stream& s, const Consensus::LLMQParams& params)
{
    CQuorumSnapshotEntry entry;
    s >> entry.cycle_base_block_hash >> entry.work_block_hash >> entry.snapshot.mnSkipListMode;
    // BuildQuorumSnapshot sizes this bitset to the complete work-block MN list,
    // not to the quorum size. The exact historical-list size is chain-aware and
    // is checked by ValidateEvoSnapshotAgainstChain.
    const size_t bit_count{ReadBoundedCompactSize(s, EVO_SNAPSHOT_MAX_MNS, "rotation bitset")};
    ReadFixedBitSet(s, entry.snapshot.activeQuorumMembers, bit_count);
    const size_t skip_count{ReadBoundedCompactSize(s, params.size, "rotation skip list")};
    entry.snapshot.mnSkipList.reserve(skip_count);
    for (size_t i{0}; i < skip_count; ++i) {
        int value;
        s >> value;
        entry.snapshot.mnSkipList.emplace_back(value);
    }
    return entry;
}

template <typename Stream>
void CQuorumSnapshotData::Serialize(Stream& s) const
{
    auto active{active_commitments};
    auto safety{safety_commitments};
    auto snapshots{rotation_snapshots};
    const auto commitment_less = [](const auto& a, const auto& b) {
        return std::tie(a.quorum_base_block_hash, a.mined_block_hash) <
               std::tie(b.quorum_base_block_hash, b.mined_block_hash);
    };
    std::sort(active.begin(), active.end(), commitment_less);
    std::sort(safety.begin(), safety.end(), commitment_less);
    std::sort(snapshots.begin(), snapshots.end(),
              [](const auto& a, const auto& b) { return a.cycle_base_block_hash < b.cycle_base_block_hash; });
    s << llmq_type << rotation_enabled << active << safety;
    WriteSnapshotVector(s, snapshots, [&](const auto& entry) { WriteRotationSnapshot(s, entry); });
}

template <typename Stream>
void CQuorumSnapshotData::Unserialize(Stream& s)
{
    s >> llmq_type >> rotation_enabled;
    const auto& params{SnapshotLLMQParams(llmq_type)};
    const size_t total_count{SnapshotCommitmentCount(params, rotation_enabled)};
    const size_t expected_active{static_cast<size_t>(params.signingActiveQuorumCount)};
    const size_t active_count{ReadBoundedCompactSize(s, expected_active, "active commitments")};
    active_commitments.reserve(active_count);
    for (size_t i{0}; i < active_count; ++i) {
        active_commitments.emplace_back(ReadMinedQuorumCommitment(s, params));
    }
    const size_t safety_count{ReadBoundedCompactSize(s, total_count - expected_active, "safety commitments")};
    safety_commitments.reserve(safety_count);
    for (size_t i{0}; i < safety_count; ++i) {
        safety_commitments.emplace_back(ReadMinedQuorumCommitment(s, params));
    }
    const size_t snapshot_count{ReadBoundedCompactSize(s, EVO_SNAPSHOT_ROTATION_CYCLES, "rotation snapshots")};
    rotation_snapshots.reserve(snapshot_count);
    for (size_t i{0}; i < snapshot_count; ++i) rotation_snapshots.emplace_back(ReadRotationSnapshot(s, params));
}

template <typename Stream>
void CEvoSnapshot::Serialize(Stream& s) const
{
    auto sorted_quorums{quorums};
    auto sorted_history{historical_mn_list_diffs};
    auto sorted_modifiers{quorum_modifiers};
    std::sort(sorted_quorums.begin(), sorted_quorums.end(),
              [](const auto& a, const auto& b) { return a.llmq_type < b.llmq_type; });
    std::sort(sorted_history.begin(), sorted_history.end(), [](const auto& a, const auto& b) {
        return std::tie(a.height, a.block_hash) > std::tie(b.height, b.block_hash);
    });
    std::sort(sorted_modifiers.begin(), sorted_modifiers.end(), [](const auto& a, const auto& b) {
        return std::tie(a.llmq_type, a.work_block_hash) < std::tie(b.llmq_type, b.work_block_hash);
    });
    s << version << base_block_hash;
    SerializeCanonicalMNList(s, mn_list);
    s << sorted_quorums;
    WriteCompactSize(s, sorted_history.size());
    for (const auto& entry : sorted_history) {
        s << entry.previous_block_hash << entry.block_hash << entry.height << entry.total_registered_count << entry.canonical_list_hash;
        SerializeCanonicalMNListDiff(s, entry.diff);
    }
    s << sorted_modifiers;
    s << credit_pool;
    WriteCompactSize(s, mnhf_signals.size());
    for (const auto& signal : mnhf_signals) s << signal;
}

template <typename Stream>
void CEvoSnapshot::Unserialize(Stream& s)
{
    s >> version;
    if (version != EVO_SNAPSHOT_VERSION) throw std::ios_base::failure("unsupported evo snapshot version");
    s >> base_block_hash;
    mn_list = UnserializeCanonicalMNList(s);
    const size_t quorum_count{ReadBoundedCompactSize(s, Consensus::available_llmqs.size(), "quorum-type count")};
    quorums.reserve(quorum_count);
    for (size_t i{0}; i < quorum_count; ++i) {
        CQuorumSnapshotData data;
        s >> data;
        quorums.emplace_back(std::move(data));
    }
    const size_t history_count{ReadBoundedCompactSize(s, EvoSnapshotMaxHistoricalMNLists(),
                                                      "historical MN-list count")};
    historical_mn_list_diffs.reserve(history_count);
    size_t remaining_history_operations{EvoSnapshotMaxHistoricalMNOperations()};
    for (size_t i{0}; i < history_count; ++i) {
        CHistoricalMNListDiff entry;
        s >> entry.previous_block_hash >> entry.block_hash >> entry.height >> entry.total_registered_count >> entry.canonical_list_hash;
        entry.diff = UnserializeCanonicalMNListDiff(s, remaining_history_operations);
        historical_mn_list_diffs.emplace_back(std::move(entry));
    }
    const size_t modifier_count{ReadBoundedCompactSize(s, EVO_SNAPSHOT_MAX_MODIFIERS, "quorum modifier count")};
    quorum_modifiers.reserve(modifier_count);
    for (size_t i{0}; i < modifier_count; ++i) {
        CQuorumModifier modifier;
        s >> modifier;
        quorum_modifiers.emplace_back(std::move(modifier));
    }
    s >> credit_pool.locked >> credit_pool.currentLimit >> credit_pool.latelyUnlocked;
    credit_pool.indexes.UnserializeBounded(s, EVO_SNAPSHOT_MAX_RANGES);
    const size_t signal_count{ReadBoundedCompactSize(s, Consensus::MAX_VERSION_BITS_DEPLOYMENTS, "MNHF signals")};
    for (size_t i{0}; i < signal_count; ++i) {
        std::pair<uint8_t, int> signal;
        s >> signal;
        if (!mnhf_signals.emplace(signal).second) throw std::ios_base::failure("duplicate MNHF signal bit");
    }
    Validate(/*require_canonical_order=*/true);
}

/** Single SHA256 of the canonical SER_DISK/CLIENT_VERSION encoding. */
uint256 GetEvoSnapshotHash(const CEvoSnapshot& snapshot);

bool BuildEvoSnapshot(const CChainParams& chainparams, const ChainstateManager& chainman,
                      CDeterministicMNManager& dmnman,
                      const llmq::CQuorumBlockProcessor& qblockman, llmq::CQuorumSnapshotManager& qsnapman,
                      CCreditPoolManager& cpoolman, CMNHFManager& mnhfman, const CBlockIndex* base_index,
                      CEvoSnapshot& snapshot, std::string& error) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

struct CQuorumReconstructionHeight {
    Consensus::LLMQType llmq_type;
    bool rotation;
    int quorum_height;
    int work_height;
};

/** Pure conservative reconstruction horizon for the supplied enabled types. */
std::vector<CQuorumReconstructionHeight> EvoSnapshotReconstructionHeights(
    int base_height, const std::vector<Consensus::LLMQParams>& enabled_llmqs);

/** Apply the complete diff chain and return lists keyed by target block hash. */
bool ReconstructHistoricalMNLists(const CEvoSnapshot& snapshot,
                                  std::map<uint256, CDeterministicMNList>& lists, std::string& error);

/** Validate all snapshot invariants requiring the block index or deployments. */
bool ValidateEvoSnapshotAgainstChain(const CEvoSnapshot& snapshot, const ChainstateManager& chainman,
                                     const CBlockIndex* base_index, std::string& error)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

/** Pure CbTx checks over already-built snapshot content. */
bool VerifyEvoSnapshotCbTx(const CEvoSnapshot& snapshot, const CCbTx& cbtx, std::string& error);

} // namespace evo

#endif // BITCOIN_EVO_SNAPSHOT_H
