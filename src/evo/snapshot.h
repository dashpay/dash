// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_SNAPSHOT_H
#define BITCOIN_EVO_SNAPSHOT_H

#include <consensus/params.h>
#include <crypto/common.h>
#include <evo/creditpool.h>
#include <evo/deterministicmns.h>
#include <kernel/cs_main.h>
#include <llmq/commitment.h>
#include <llmq/params.h>
#include <llmq/snapshot.h>
#include <primitives/transaction.h>
#include <versionbits.h>

#include <hash.h>
#include <serialize.h>
#include <streams.h>
#include <uint256.h>
#include <util/check.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

class CBlockIndex;
class CChainParams;
class ChainstateManager;
class CCbTx;
class CBlock;
class CCreditPoolManager;
class CMNHFManager;
class CSpecialTxProcessor;
struct EvoViewToken;

namespace llmq {
class CQuorumBlockProcessor;
class CQuorumSnapshotManager;
struct ScanCoverageRecord;
} // namespace llmq
namespace node {
class BlockManager;
} // namespace node

namespace evo {

static constexpr uint16_t EVO_SNAPSHOT_VERSION{4};
/** Serialized little-endian bytes are "DASHEVO\0". */
static constexpr uint64_t EVO_SNAPSHOT_MARKER{0x004f564548534144ULL};
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
// A cycle's skip list accumulates across every quorum index and the build can
// wrap the combined MN list more than once, so a single quorum's size does not
// bound its legitimate length. This is a decode ceiling on claimed sizes only,
// far above any state the aggregate rotation build reaches on real chains.
static constexpr size_t EVO_SNAPSHOT_MAX_SKIPLIST_ENTRIES{1'000'000};
// Commitment bitsets are sized by the effective chain parameters, which
// -llmqtestparams/-llmqdevnetparams may override at runtime. This context-free
// layer only enforces an allocation ceiling (far above the largest defined
// quorum, size 400) plus internal consistency; exact sizing against the
// effective parameters belongs to the chain-aware validation.
static constexpr size_t EVO_SNAPSHOT_MAX_QUORUM_SIZE{10'000};
// CDeterministicMNList's HAMT hashes proTxHash by its first 8 bytes, so a
// snapshot supplying many distinct hashes that share one 64-bit prefix would
// make every insertion copy the whole collision node (quadratic decode work).
// Real proTxHashes are uniform txids: among 100,000 of them even a single
// shared prefix has probability ~3e-10, so a run of 8 is unreachable outside
// crafted input. Enforced on the base list and every reconstructed list.
static constexpr size_t EVO_SNAPSHOT_MAX_HASH_PREFIX_RUN{8};
// Every historical entry costs a full traversal, sort, and canonical hash of
// the reconstructed list, so a few hundred bytes of zero-operation diffs could
// otherwise drag a maximum-size list across every planned work block: the
// table-wide bound is 436 lists (330 for the mainnet table), ~44M record visits
// at the 100,000-MN ceiling, so this budget refuses such inputs. An honest base
// plans only the work blocks its commitments and h/N name: tens of lists at
// today's populations of a few thousand MNs, and even the worst case of every
// LLMQ_60_75 index failing its DKG in a different cycle (262 lists) stays below
// a million visits at 3,000 MNs.
static constexpr size_t EVO_SNAPSHOT_MAX_RECONSTRUCTION_RECORDS{8'000'000};
// The base coinbase is length-delimited and bounded before allocation; its
// merkle branch proves it against the base header, so it is at most as deep as
// a tree over 2^32 transactions.
static constexpr size_t EVO_SNAPSHOT_MAX_COINBASE_BYTES{1'000'000};
static constexpr size_t EVO_SNAPSHOT_MAX_COINBASE_BRANCH{32};
// The credit-pool window is CreditPoolPeriodBlocks() long: 576 on main, test
// and devnet, 100 on regtest. A test pins every network to this bound.
static constexpr size_t EVO_SNAPSHOT_MAX_CREDIT_HISTORY{576};
// At most two bootstrap work entries per type: the cycle containing the base
// and the next one.
static constexpr size_t EVO_SNAPSHOT_MAX_BOOTSTRAP_WORK{2};
static_assert(std::ranges::all_of(Consensus::available_llmqs, [](const auto& params) {
    return !params.useRotation || params.keepOldConnections <= 2 * params.signingActiveQuorumCount;
}), "rotated LLMQ retention exceeds the two serialized cycles");
// Bootstrap work entries are ordered by cycle height, and a cycle's work block
// precedes it. Both need every cycle to be longer than the work depth.
static_assert(std::ranges::all_of(Consensus::available_llmqs,
                                  [](const auto& params) { return params.dkgInterval > llmq::WORK_DIFF_DEPTH; }),
              "an LLMQ cycle must be longer than the quorum work depth");

/** Commitments carried for one type: max(a + 1, keepOld) without rotation, 2a with it. */
constexpr size_t SnapshotCommitmentCountFor(const Consensus::LLMQParams& params, bool rotation_enabled)
{
    return rotation_enabled
               ? 2 * static_cast<size_t>(params.signingActiveQuorumCount)
               : static_cast<size_t>(std::max(params.signingActiveQuorumCount + 1, params.keepOldConnections));
}

/** Rotation quarters a rotating type can need: three per commitment cycle, plus h..h-3C. */
constexpr size_t EvoSnapshotMaxRotationSnapshots(const Consensus::LLMQParams& params)
{
    return 3 * SnapshotCommitmentCountFor(params, /*rotation_enabled=*/true) + 4;
}

/** Work blocks one type can need, over every mode the type can run in. */
constexpr size_t EvoSnapshotMaxWorkBlocks(const Consensus::LLMQParams& params)
{
    const size_t plain{SnapshotCommitmentCountFor(params, /*rotation_enabled=*/false) + 2};
    if (!params.useRotation) return plain;
    return std::max(plain, 4 * SnapshotCommitmentCountFor(params, /*rotation_enabled=*/true) + 6);
}

/**
 * Maximum number of distinct historical work-block lists a snapshot can need.
 * The serialized set is deduplicated, so summing every type's bound over the
 * static table is conservative.
 */
constexpr size_t EvoSnapshotMaxHistoricalMNLists()
{
    size_t count{0};
    for (const auto& params : Consensus::available_llmqs)
        count += EvoSnapshotMaxWorkBlocks(params);
    return count;
}
static constexpr size_t EVO_SNAPSHOT_MAX_MODIFIERS{4'096};
// Modifiers are one per (type, work block), so they number at most the sum of
// the per-type work-block bounds.
static_assert(EvoSnapshotMaxHistoricalMNLists() <= EVO_SNAPSHOT_MAX_MODIFIERS,
              "one modifier per (type, work block) must fit the modifier bound");

template <typename Stream>
size_t ReadBoundedCompactSize(Stream& s, size_t limit, const char* field)
{
    const uint64_t size{ReadCompactSize(s)};
    if (size > limit) throw std::ios_base::failure(std::string{"oversized evo snapshot "} + field);
    return static_cast<size_t>(size);
}

/**
 * The static table is intentional for this context-free layer: the runtime
 * overrides (-llmqtestparams, -llmqdevnetparams) mutate only size/threshold
 * fields on the CChainParams copy, so the count, rotation, and interval fields
 * consumed here are reliable. Nothing here may depend on LLMQParams::size;
 * size checks are format-level bounds with exact sizing done chain-aware.
 */
inline const Consensus::LLMQParams& SnapshotLLMQParams(Consensus::LLMQType type)
{
    const auto it{std::ranges::find_if(Consensus::available_llmqs,
                                       [type](const auto& params) { return params.type == type; })};
    if (it == Consensus::available_llmqs.end()) throw std::ios_base::failure("unknown evo snapshot LLMQ type");
    return *it;
}

inline size_t SnapshotCommitmentCount(const Consensus::LLMQParams& params, bool rotation_enabled)
{
    // Rotation seeding promises the active and previous complete cycles. A
    // future parameter set retaining more must extend the serialized cycles.
    if (rotation_enabled && params.keepOldConnections > 2 * params.signingActiveQuorumCount) {
        throw std::ios_base::failure("rotated LLMQ retention exceeds two cycles");
    }
    return SnapshotCommitmentCountFor(params, rotation_enabled);
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

/**
 * Lazy BLS wrappers keep the consumed bytes unparsed and silently replace an
 * undecodable key with the empty (all-zero) encoding on first Get(). Force
 * that before comparing the canonical reencoding, so the snapshot hash cannot
 * change depending on whether a consumer has touched the key.
 */
inline void MaterializeLazyBLSFields(const CDeterministicMN& dmn) { dmn.pdmnState->pubKeyOperator.Get(); }
inline void MaterializeLazyBLSFields(const CDeterministicMNStateDiff& state_diff)
{
    if (state_diff.fields & CDeterministicMNStateDiff::Field_pubKeyOperator) state_diff.state.pubKeyOperator.Get();
}

template <typename Stream>
class SnapshotBoundedInput
{
private:
    Stream& m_source;
    HashVerifier<Stream> m_stream;
    uint64_t m_compact_budget;

public:
    SnapshotBoundedInput(Stream& stream, uint64_t compact_budget) :
        m_source{stream}, m_stream{stream}, m_compact_budget{compact_budget} {}

    int GetType() const { return m_source.GetType(); }
    int GetVersion() const { return m_source.GetVersion(); }
    void read(Span<std::byte> dst) { m_stream.read(dst); }
    void ignore(size_t size) { m_stream.ignore(size); }

    uint64_t ReadBudgetedCompactSize()
    {
        const uint64_t size{::ReadCompactSize(m_stream)};
        if (size > m_compact_budget) throw std::ios_base::failure("canonical MN nested CompactSize budget exceeded");
        m_compact_budget -= size;
        return size;
    }

    // Reused serializers can normalize nested maps or other encodings. Hash
    // the consumed bytes so canonicality can be checked without buffering them.
    template <typename T>
    void CheckCanonicalEncoding(const T& obj)
    {
        MaterializeLazyBLSFields(obj);
        CHashWriter canonical{GetType(), GetVersion()};
        canonical << obj;
        if (m_stream.GetHash() != canonical.GetHash()) {
            throw std::ios_base::failure("noncanonical MN object encoding");
        }
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
    size_t prefix_run{0};
    for (size_t i{0}; i < count; ++i) {
        SnapshotBoundedInput bounded{s, EVO_SNAPSHOT_MAX_MN_COMPACT_ITEMS};
        auto dmn{std::make_shared<CDeterministicMN>(deserialize, bounded)};
        bounded.CheckCanonicalEncoding(*dmn);
        if (!dmn->HasInternalId()) throw std::ios_base::failure("invalid canonical MN internalId");
        if (dmn->pdmnState->payouts.size() > EVO_SNAPSHOT_MAX_PAYOUT_SHARES) {
            throw std::ios_base::failure("oversized canonical MN payout list");
        }
        if (!dmn->pdmnState->netInfo->IsEmpty() && dmn->pdmnState->netInfo->Validate() != NetInfoStatus::Success) {
            throw std::ios_base::failure("invalid canonical MN network info");
        }
        if (have_previous && !(previous < dmn->proTxHash)) {
            throw std::ios_base::failure("noncanonical canonical MN-list order");
        }
        // Entries arrive sorted by the full hash, so equal 64-bit prefixes are
        // adjacent. Reject collision runs before AddMN performs the inserts.
        prefix_run = (have_previous && ReadLE64(previous.begin()) == ReadLE64(dmn->proTxHash.begin()))
                         ? prefix_run + 1
                         : 1;
        if (prefix_run > EVO_SNAPSHOT_MAX_HASH_PREFIX_RUN) {
            throw std::ios_base::failure("canonical MN-list hash-prefix run exceeds collision bound");
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
        bounded.CheckCanonicalEncoding(*dmn);
        if (!dmn->HasInternalId()) throw std::ios_base::failure("invalid canonical MN internalId");
        if ((have_previous && previous_id >= dmn->GetInternalId()) ||
            dmn->pdmnState->payouts.size() > EVO_SNAPSHOT_MAX_PAYOUT_SHARES ||
            (!dmn->pdmnState->netInfo->IsEmpty() && dmn->pdmnState->netInfo->Validate() != NetInfoStatus::Success)) {
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
        CDeterministicMNStateDiff state_diff{deserialize, bounded};
        bounded.CheckCanonicalEncoding(state_diff);
        diff.updatedMNs.emplace(internal_id, std::move(state_diff));
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

struct MinedQuorumCommitment {
    uint256 quorum_base_block_hash;
    uint256 work_block_hash;
    llmq::CFinalCommitment commitment;
    uint256 mined_block_hash;

    SERIALIZE_METHODS(MinedQuorumCommitment, obj)
    {
        READWRITE(obj.quorum_base_block_hash, obj.work_block_hash, obj.commitment, obj.mined_block_hash);
    }
};

template <typename Stream>
MinedQuorumCommitment ReadMinedQuorumCommitment(Stream& s)
{
    MinedQuorumCommitment entry;
    auto& commitment{entry.commitment};
    s >> entry.quorum_base_block_hash >> entry.work_block_hash >> commitment.nVersion >> commitment.llmqType >> commitment.quorumHash;
    const bool indexed{commitment.nVersion == llmq::CFinalCommitment::LEGACY_BLS_INDEXED_QUORUM_VERSION ||
                       commitment.nVersion == llmq::CFinalCommitment::BASIC_BLS_INDEXED_QUORUM_VERSION};
    if (indexed) s >> commitment.quorumIndex;
    // The consensus/P2P serializer remains unchanged; this snapshot-local path
    // bounds both claimed bitset sizes before allocation. The effective quorum
    // size is runtime-configurable, so only internal consistency is enforced
    // here; exact sizing is established by the chain-aware validation.
    const size_t signers_size{ReadBoundedCompactSize(s, EVO_SNAPSHOT_MAX_QUORUM_SIZE, "commitment signers")};
    if (signers_size == 0) {
        throw std::ios_base::failure("empty evo snapshot commitment signers");
    }
    ReadFixedBitSet(s, commitment.signers, signers_size);
    const size_t valid_members_size{ReadBoundedCompactSize(s, EVO_SNAPSHOT_MAX_QUORUM_SIZE, "commitment valid members")};
    if (valid_members_size != signers_size) {
        throw std::ios_base::failure("inconsistent evo snapshot commitment bitset sizes");
    }
    ReadFixedBitSet(s, commitment.validMembers, valid_members_size);
    const bool legacy{commitment.nVersion == llmq::CFinalCommitment::LEGACY_BLS_NON_INDEXED_QUORUM_VERSION ||
                      commitment.nVersion == llmq::CFinalCommitment::LEGACY_BLS_INDEXED_QUORUM_VERSION};
    HashVerifier<Stream> bls_input{s};
    bls_input >> CBLSPublicKeyVersionWrapper(commitment.quorumPublicKey, legacy) >> commitment.quorumVvecHash >>
        CBLSSignatureVersionWrapper(commitment.quorumSig, legacy) >>
        CBLSSignatureVersionWrapper(commitment.membersSig, legacy);
    s >> entry.mined_block_hash;
    // BLS decoding may normalize an opposite-scheme encoding to an invalid
    // object. Snapshot input must match the requested scheme's canonical bytes.
    CHashWriter canonical{s.GetType(), s.GetVersion()};
    canonical << CBLSPublicKeyVersionWrapper(commitment.quorumPublicKey, legacy) << commitment.quorumVvecHash
              << CBLSSignatureVersionWrapper(commitment.quorumSig, legacy)
              << CBLSSignatureVersionWrapper(commitment.membersSig, legacy);
    if (bls_input.GetHash() != canonical.GetHash()) {
        throw std::ios_base::failure("noncanonical evo snapshot commitment encoding");
    }
    return entry;
}

struct QuorumSnapshotEntry {
    uint256 cycle_base_block_hash;
    uint256 work_block_hash;
    llmq::CQuorumSnapshot snapshot;
};

struct HistoricalMNListDiff {
    uint256 previous_block_hash;
    uint256 block_hash;
    int height{-1};
    uint32_t total_registered_count{0};
    uint256 canonical_list_hash;
    CDeterministicMNListDiff diff;
};

/**
 * Cumulative add/update/remove operations a diff chain costs the decoder.
 * UnserializeCanonicalMNListDiff() charges these same amounts incrementally
 * against EvoSnapshotMaxHistoricalMNOperations() while streaming, so a chain
 * above that ceiling cannot be decoded back from its own canonical bytes.
 */
inline size_t EvoSnapshotHistoricalMNOperations(const std::vector<HistoricalMNListDiff>& history)
{
    size_t operations{0};
    for (const auto& entry : history) {
        operations += entry.diff.addedMNs.size() + entry.diff.updatedMNs.size() + entry.diff.removedMns.size();
    }
    return operations;
}

struct QuorumModifier {
    Consensus::LLMQType llmq_type{Consensus::LLMQType::LLMQ_NONE};
    uint256 work_block_hash;
    uint256 modifier;

    SERIALIZE_METHODS(QuorumModifier, obj) { READWRITE(obj.llmq_type, obj.work_block_hash, obj.modifier); }
};

/**
 * A work block the snapshot carries for the cycle containing the base (h) or
 * the next one (N) that no carried commitment or rotation quarter references.
 * The cycle need not be mined: only its height and the work block are named.
 */
struct BootstrapWork {
    int32_t cycle_base_height{-1};
    uint256 work_block_hash;

    bool operator==(const BootstrapWork&) const = default;
    SERIALIZE_METHODS(BootstrapWork, obj) { READWRITE(obj.cycle_base_height, obj.work_block_hash); }
};

struct QuorumSnapshotData {
    Consensus::LLMQType llmq_type{Consensus::LLMQType::LLMQ_NONE};
    bool rotation_enabled{false};
    std::vector<MinedQuorumCommitment> active_commitments;
    std::vector<MinedQuorumCommitment> safety_commitments;
    std::vector<QuorumSnapshotEntry> rotation_snapshots;
    //! Strictly ascending by cycle height.
    std::vector<BootstrapWork> bootstrap_work;

    template <typename Stream> void Serialize(Stream& s) const;
    template <typename Stream> void Unserialize(Stream& s);
};

/** Canonical Dash-derived state attached to an assumeutxo snapshot. */
class EvoSnapshot
{
public:
    uint16_t version{EVO_SNAPSHOT_VERSION};
    uint256 base_block_hash;
    CDeterministicMNList mn_list;
    //! The base block's coinbase, null exactly when DIP3 is inactive at the base.
    CTransactionRef base_coinbase;
    //! Merkle branch of the coinbase (index 0) to the base header, leaf to root.
    std::vector<uint256> base_coinbase_branch;
    std::vector<QuorumSnapshotData> quorums;
    std::vector<HistoricalMNListDiff> historical_mn_list_diffs;
    std::vector<QuorumModifier> quorum_modifiers;
    CCreditPool credit_pool;
    //! The window entries of the blocks ending at the base, oldest first.
    std::vector<CreditHistoryEntry> credit_history;
    AbstractEHFManager::Signals mnhf_signals;

    template <typename Stream> void Serialize(Stream& s) const;
    template <typename Stream> void Unserialize(Stream& s);

    /** Validate invariants not requiring chainstate or block-index lookup. */
    void Validate(bool require_canonical_order = false) const;
};

/**
 * Canonical wire order, one overload per snapshot collection element. The
 * serializer, the decode-time order check, and the object-level order check
 * all derive from these, so a type without an overload is a compile error
 * rather than a silently accepted order.
 */
inline bool IsCanonicallyBefore(const MinedQuorumCommitment& a, const MinedQuorumCommitment& b)
{
    return std::tie(a.quorum_base_block_hash, a.mined_block_hash) < std::tie(b.quorum_base_block_hash, b.mined_block_hash);
}

inline bool IsCanonicallyBefore(const QuorumSnapshotEntry& a, const QuorumSnapshotEntry& b)
{
    return a.cycle_base_block_hash < b.cycle_base_block_hash;
}

/** Historical diffs descend by height: the chain is replayed newest first. */
inline bool IsCanonicallyBefore(const HistoricalMNListDiff& a, const HistoricalMNListDiff& b)
{
    return std::tie(a.height, a.block_hash) > std::tie(b.height, b.block_hash);
}

inline bool IsCanonicallyBefore(const QuorumModifier& a, const QuorumModifier& b)
{
    return std::tie(a.llmq_type, a.work_block_hash) < std::tie(b.llmq_type, b.work_block_hash);
}

inline bool IsCanonicallyBefore(const BootstrapWork& a, const BootstrapWork& b)
{
    return a.cycle_base_height < b.cycle_base_height;
}

inline bool IsCanonicallyBefore(const QuorumSnapshotData& a, const QuorumSnapshotData& b)
{
    return a.llmq_type < b.llmq_type;
}

template <typename T>
std::vector<T> CanonicallySortedCopy(std::vector<T> values)
{
    std::sort(values.begin(), values.end(), [](const T& a, const T& b) { return IsCanonicallyBefore(a, b); });
    return values;
}

template <typename T>
bool IsCanonicallySorted(const std::vector<T>& values)
{
    return std::adjacent_find(values.begin(), values.end(),
                              [](const T& a, const T& b) { return !IsCanonicallyBefore(a, b); }) == values.end();
}

template <typename Stream, typename T, typename WriteOne>
void WriteSnapshotVector(Stream& s, const std::vector<T>& values, WriteOne&& write_one)
{
    WriteCompactSize(s, values.size());
    for (const auto& value : values) write_one(value);
}

template <typename Stream>
void WriteRotationSnapshot(Stream& s, const QuorumSnapshotEntry& entry)
{
    s << entry.cycle_base_block_hash << entry.work_block_hash << entry.snapshot.mnSkipListMode;
    WriteCompactSize(s, entry.snapshot.activeQuorumMembers.size());
    WriteFixedBitSet(s, entry.snapshot.activeQuorumMembers, entry.snapshot.activeQuorumMembers.size());
    s << entry.snapshot.mnSkipList;
}

template <typename Stream>
QuorumSnapshotEntry ReadRotationSnapshot(Stream& s, const Consensus::LLMQParams& params)
{
    QuorumSnapshotEntry entry;
    s >> entry.cycle_base_block_hash >> entry.work_block_hash >> entry.snapshot.mnSkipListMode;
    // BuildQuorumSnapshot sizes this bitset to the complete work-block MN list,
    // not to the quorum size. The exact historical-list size is chain-aware and
    // is checked by the chain-aware validation layered on later in the series.
    const size_t bit_count{ReadBoundedCompactSize(s, EVO_SNAPSHOT_MAX_MNS, "rotation bitset")};
    ReadFixedBitSet(s, entry.snapshot.activeQuorumMembers, bit_count);
    const size_t skip_count{ReadBoundedCompactSize(s, EVO_SNAPSHOT_MAX_SKIPLIST_ENTRIES, "rotation skip list")};
    // Clamp the upfront allocation: a hostile claimed count must pay with its
    // own serialized bytes, not with a proportional reserve.
    entry.snapshot.mnSkipList.reserve(std::min<size_t>(skip_count, params.size));
    for (size_t i{0}; i < skip_count; ++i) {
        int value;
        s >> value;
        entry.snapshot.mnSkipList.emplace_back(value);
    }
    return entry;
}

template <typename Stream>
void QuorumSnapshotData::Serialize(Stream& s) const
{
    const auto active{CanonicallySortedCopy(active_commitments)};
    const auto safety{CanonicallySortedCopy(safety_commitments)};
    const auto snapshots{CanonicallySortedCopy(rotation_snapshots)};
    s << llmq_type << rotation_enabled << active << safety;
    WriteSnapshotVector(s, snapshots, [&](const auto& entry) { WriteRotationSnapshot(s, entry); });
    s << CanonicallySortedCopy(bootstrap_work);
}

template <typename Stream>
void QuorumSnapshotData::Unserialize(Stream& s)
{
    uint8_t rotation_flag;
    s >> llmq_type >> rotation_flag;
    if (rotation_flag > 1) throw std::ios_base::failure("noncanonical evo quorum rotation flag");
    rotation_enabled = rotation_flag != 0;
    // Same replacement semantics as EvoSnapshot::Unserialize: decoding into a
    // reused object must not retain (or exceed the count bounds through)
    // previously held entries.
    active_commitments.clear();
    safety_commitments.clear();
    rotation_snapshots.clear();
    bootstrap_work.clear();
    const auto& params{SnapshotLLMQParams(llmq_type)};
    const size_t total_count{SnapshotCommitmentCount(params, rotation_enabled)};
    const size_t expected_active{static_cast<size_t>(params.signingActiveQuorumCount)};
    const size_t active_count{ReadBoundedCompactSize(s, expected_active, "active commitments")};
    active_commitments.reserve(active_count);
    for (size_t i{0}; i < active_count; ++i) {
        active_commitments.emplace_back(ReadMinedQuorumCommitment(s));
    }
    const size_t safety_count{ReadBoundedCompactSize(s, total_count - expected_active, "safety commitments")};
    safety_commitments.reserve(safety_count);
    for (size_t i{0}; i < safety_count; ++i) {
        safety_commitments.emplace_back(ReadMinedQuorumCommitment(s));
    }
    const size_t snapshot_count{
        ReadBoundedCompactSize(s, rotation_enabled ? EvoSnapshotMaxRotationSnapshots(params) : size_t{0},
                               "rotation snapshots")};
    rotation_snapshots.reserve(snapshot_count);
    for (size_t i{0}; i < snapshot_count; ++i) rotation_snapshots.emplace_back(ReadRotationSnapshot(s, params));
    const size_t bootstrap_count{ReadBoundedCompactSize(s, EVO_SNAPSHOT_MAX_BOOTSTRAP_WORK, "bootstrap work")};
    for (size_t i{0}; i < bootstrap_count; ++i) {
        BootstrapWork entry;
        s >> entry;
        if (!bootstrap_work.empty() && bootstrap_work.back().cycle_base_height >= entry.cycle_base_height) {
            throw std::ios_base::failure("noncanonical evo bootstrap work order");
        }
        bootstrap_work.emplace_back(entry);
    }
}

template <typename Stream>
void EvoSnapshot::Serialize(Stream& s) const
{
    const auto sorted_quorums{CanonicallySortedCopy(quorums)};
    const auto sorted_history{CanonicallySortedCopy(historical_mn_list_diffs)};
    const auto sorted_modifiers{CanonicallySortedCopy(quorum_modifiers)};
    s << version << base_block_hash;
    SerializeCanonicalMNList(s, mn_list);
    if (base_coinbase) {
        CDataStream coinbase{SER_NETWORK, PROTOCOL_VERSION};
        coinbase << *base_coinbase;
        WriteCompactSize(s, coinbase.size());
        s.write(MakeByteSpan(coinbase));
    } else {
        WriteCompactSize(s, 0);
    }
    s << base_coinbase_branch;
    s << sorted_quorums;
    WriteCompactSize(s, sorted_history.size());
    for (const auto& entry : sorted_history) {
        s << entry.previous_block_hash << entry.block_hash << entry.height << entry.total_registered_count << entry.canonical_list_hash;
        SerializeCanonicalMNListDiff(s, entry.diff);
    }
    s << sorted_modifiers;
    s << credit_pool.locked << credit_pool.currentLimit << credit_pool.latelyUnlocked << credit_pool.indexes;
    s << credit_history;
    WriteCompactSize(s, mnhf_signals.size());
    for (const auto& signal : mnhf_signals) s << signal;
}

template <typename Stream>
void EvoSnapshot::Unserialize(Stream& s)
{
    s >> version;
    if (version != EVO_SNAPSHOT_VERSION) throw std::ios_base::failure("unsupported evo snapshot version");
    s >> base_block_hash;
    // Decoding must replace any previous contents: the collections below are
    // appended to (and the signal map merged into), so a reused object would
    // otherwise accumulate state that the consumed bytes never contained.
    quorums.clear();
    historical_mn_list_diffs.clear();
    quorum_modifiers.clear();
    credit_history.clear();
    mnhf_signals.clear();
    mn_list = UnserializeCanonicalMNList(s);
    base_coinbase.reset();
    if (const size_t coinbase_size{ReadBoundedCompactSize(s, EVO_SNAPSHOT_MAX_COINBASE_BYTES, "base coinbase")}) {
        std::vector<std::byte> bytes(coinbase_size);
        s.read(bytes);
        CDataStream coinbase{bytes, SER_NETWORK, PROTOCOL_VERSION};
        CMutableTransaction tx;
        coinbase >> tx;
        if (!coinbase.empty()) throw std::ios_base::failure("trailing bytes after the evo snapshot base coinbase");
        base_coinbase = MakeTransactionRef(std::move(tx));
        CDataStream reencoded{SER_NETWORK, PROTOCOL_VERSION};
        reencoded << *base_coinbase;
        if (reencoded.size() != bytes.size() || !std::equal(bytes.begin(), bytes.end(), reencoded.begin())) {
            throw std::ios_base::failure("noncanonical evo snapshot base coinbase");
        }
    }
    const size_t branch_size{ReadBoundedCompactSize(s, EVO_SNAPSHOT_MAX_COINBASE_BRANCH, "coinbase branch")};
    base_coinbase_branch.resize(branch_size);
    for (auto& node : base_coinbase_branch)
        s >> node;
    const size_t quorum_count{ReadBoundedCompactSize(s, Consensus::available_llmqs.size(), "quorum-type count")};
    quorums.reserve(quorum_count);
    for (size_t i{0}; i < quorum_count; ++i) {
        QuorumSnapshotData data;
        s >> data;
        quorums.emplace_back(std::move(data));
    }
    const size_t history_count{ReadBoundedCompactSize(s, EvoSnapshotMaxHistoricalMNLists(),
                                                      "historical MN-list count")};
    historical_mn_list_diffs.reserve(history_count);
    size_t remaining_history_operations{EvoSnapshotMaxHistoricalMNOperations()};
    for (size_t i{0}; i < history_count; ++i) {
        HistoricalMNListDiff entry;
        s >> entry.previous_block_hash >> entry.block_hash >> entry.height >> entry.total_registered_count >> entry.canonical_list_hash;
        entry.diff = UnserializeCanonicalMNListDiff(s, remaining_history_operations);
        historical_mn_list_diffs.emplace_back(std::move(entry));
    }
    const size_t modifier_count{ReadBoundedCompactSize(s, EVO_SNAPSHOT_MAX_MODIFIERS, "quorum modifier count")};
    quorum_modifiers.reserve(modifier_count);
    for (size_t i{0}; i < modifier_count; ++i) {
        QuorumModifier modifier;
        s >> modifier;
        quorum_modifiers.emplace_back(std::move(modifier));
    }
    s >> credit_pool.locked >> credit_pool.currentLimit >> credit_pool.latelyUnlocked;
    credit_pool.indexes.UnserializeBounded(s, EVO_SNAPSHOT_MAX_RANGES);
    const size_t history_entries{ReadBoundedCompactSize(s, EVO_SNAPSHOT_MAX_CREDIT_HISTORY, "credit history")};
    credit_history.resize(history_entries);
    for (auto& entry : credit_history)
        s >> entry;
    const size_t signal_count{ReadBoundedCompactSize(s, VERSIONBITS_NUM_BITS, "MNHF signals")};
    // The signal map normalizes iteration order, so wire order is observable
    // only here: require the strictly ascending bit order the serializer
    // emits, which also rejects duplicate bits.
    std::optional<uint8_t> previous_bit;
    for (size_t i{0}; i < signal_count; ++i) {
        std::pair<uint8_t, int> signal;
        s >> signal;
        if (previous_bit && *previous_bit >= signal.first) {
            throw std::ios_base::failure("noncanonical MNHF signal order");
        }
        previous_bit = signal.first;
        mnhf_signals.emplace(signal);
    }
    Validate(/*require_canonical_order=*/true);
}

/** Single SHA256 of the canonical SER_DISK/CLIENT_VERSION encoding. */
uint256 GetEvoSnapshotHash(const EvoSnapshot& snapshot);

/** A work block the snapshot carries for one type, and the cycle (or, without
 *  rotation, the quorum) whose score modifier it carries. */
struct EvoSnapshotWorkBlock {
    const CBlockIndex* work_index{nullptr};
    int cycle_height{-1};
};

/** The Evo state one LLMQ type needs at a snapshot base: every quorum member
 *  computation the snapshot chainstate makes at or below the base reads only
 *  these, plus the carried commitments. */
struct EvoSnapshotDependencyPlan {
    Consensus::LLMQType llmq_type{Consensus::LLMQType::LLMQ_NONE};
    bool rotation_enabled{false};
    //! S_T: cycles whose rotation quarters are carried, ascending.
    std::vector<const CBlockIndex*> rotation_cycles;
    //! W_T: work blocks whose lists and modifiers are carried, ascending.
    std::vector<EvoSnapshotWorkBlock> work_blocks;
    //! The entries of W_T for h and N that nothing else references.
    std::vector<BootstrapWork> bootstrap_work;
};

/** The work block a quorum's members are computed from: the cycle's work block
 *  with rotation, otherwise the quorum's own block before v20 and its work
 *  block after. */
int EvoSnapshotWorkHeight(const Consensus::Params& consensus, const Consensus::LLMQParams& params,
                          bool rotation_enabled, int quorum_height);

/** Derive the exact dependency sets of `llmq_type` at `base_index` from the
 *  carried commitments' quorum blocks. The builder, the chain validator and
 *  the completion rebuild all use it. */
bool PlanEvoSnapshotDependencies(const ChainstateManager& chainman, const CBlockIndex* base_index,
                                 Consensus::LLMQType llmq_type, const std::vector<const CBlockIndex*>& commitments,
                                 EvoSnapshotDependencyPlan& plan, std::string& error) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

/** Build the Evo section at `base_index` from the calling thread's Evo view. */
bool BuildEvoSnapshot(const CChainParams& chainparams, const ChainstateManager& chainman, CDeterministicMNManager& dmnman,
                      const llmq::CQuorumBlockProcessor& qblockman, llmq::CQuorumSnapshotManager& qsnapman,
                      CCreditPoolManager& cpoolman, CMNHFManager& mnhfman, const CBlockIndex* base_index,
                      EvoSnapshot& snapshot, std::string& error) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

/** Apply the complete diff chain and return lists keyed by target block hash. */
bool ReconstructHistoricalMNLists(const EvoSnapshot& snapshot, std::map<uint256, CDeterministicMNList>& lists,
                                  std::string& error, size_t max_records = EVO_SNAPSHOT_MAX_RECONSTRUCTION_RECORDS);

/** Validate all snapshot invariants requiring the block index or deployments,
 *  including exact equality with the planned dependency sets. */
bool ValidateEvoSnapshotAgainstChain(const EvoSnapshot& snapshot, const ChainstateManager& chainman,
                                     const CBlockIndex* base_index, std::string& error)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

/** Pure CbTx checks over already-built snapshot content. */
bool VerifyEvoSnapshotCbTx(const EvoSnapshot& snapshot, const CCbTx& cbtx, std::string& error);

/** The merkle root a leaf at `index` reaches through `branch` (leaf to root). */
uint256 ComputeMerkleRootFromBranch(const uint256& leaf, const std::vector<uint256>& branch, uint32_t index);
/** The merkle branch of the transaction at `index` in `block`, leaf to root. */
std::vector<uint256> BlockMerkleBranch(const CBlock& block, uint32_t index);

/** Load-time anchor of the section to the base header: the carried coinbase
 *  must hash through its branch to the header's merkle root, and its CbTx,
 *  decoded with the BLS scheme in force at the base, must commit to the
 *  carried MN list, active quorums and credit pool. */
bool VerifyEvoSnapshotBaseBlock(const EvoSnapshot& snapshot, const CBlockIndex& base_index,
                                const Consensus::Params& consensus, std::string& error);

/** The scan-coverage gate of ValidateEvoSnapshotAgainstChain on its own:
 *  every ChainLock verification scan B + 1 may make and the asset-unlock scan
 *  of B + 1 must be proven from the section. */
bool CheckEvoSnapshotScanCoverage(const EvoSnapshot& snapshot, const ChainstateManager& chainman,
                                  const CBlockIndex* base_index, std::string& error) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

/** How the scan sequences of `params`' type end in the section: END where the
 *  canonical commitment selection carries fewer records than it takes, so no
 *  older one exists; UNKNOWN otherwise. Local-only, recorded at seeding.
 *
 *  END rests on the carried counts alone. The CbTx quorum root binds only the
 *  active set and headers say nothing about how many older commitments exist,
 *  so validation bounds the safety commitments from above only: a section
 *  omitting one would turn UNKNOWN into END and a short scan into a wrong
 *  answer. Only the mandatory evo-hash pin binds the counts, so no section may
 *  be seeded without it. */
llmq::ScanCoverageRecord EvoSnapshotScanCoverage(const EvoSnapshot& snapshot, const Consensus::LLMQParams& params);

/** Seed the decoded snapshot's state into `view`, inside the caller's open
 *  EvoDB transaction for that view. */
bool SeedEvoSnapshotState(const EvoViewToken& view, const EvoSnapshot& snapshot, CSpecialTxProcessor& special_tx,
                          const node::BlockManager& blockman, const CBlockIndex* base_index, std::string& error)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

} // namespace evo

#endif // BITCOIN_EVO_SNAPSHOT_H
