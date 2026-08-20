// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <clientversion.h>
#include <consensus/merkle.h>
#include <evo/cbtx.h>
#include <evo/chainhelper.h>
#include <evo/creditpool.h>
#include <evo/evodb.h>
#include <evo/mnhftx.h>
#include <evo/netinfo.h>
#include <evo/snapshot.h>
#include <hash.h>
#include <llmq/blockprocessor.h>
#include <llmq/context.h>
#include <llmq/quorumsman.h>
#include <llmq/signhash.h>
#include <llmq/snapshot.h>
#include <llmq/utils.h>
#include <masternode/meta.h>
#include <node/context.h>
#include <streams.h>
#include <validation.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/test/unit_test.hpp>

namespace {

uint256 H(uint8_t value)
{
    uint256 hash;
    hash.begin()[0] = value;
    return hash;
}

uint256 CollidingH(uint8_t suffix)
{
    uint256 hash;
    std::fill_n(hash.begin(), 8, 0xa5);
    hash.begin()[8] = suffix;
    return hash;
}

uint160 H160(uint8_t value)
{
    uint160 hash;
    hash.begin()[0] = value;
    return hash;
}

CDeterministicMNCPtr MN(uint64_t internal_id, uint8_t hash_suffix, MnType type, int version, uint8_t address_tag)
{
    auto state{std::make_shared<CDeterministicMNState>()};
    state->nVersion = version;
    state->nRegisteredHeight = 10 + internal_id;
    state->nLastPaidHeight = 20 + internal_id;
    state->nPoSePenalty = internal_id;
    state->keyIDOwner = CKeyID{H160(address_tag)};
    state->keyIDVoting = CKeyID{H160(address_tag + 20)};
    state->scriptPayout = CScript{} << OP_RETURN << std::vector<unsigned char>{address_tag, 1};
    state->scriptOperatorPayout = CScript{} << OP_RETURN << std::vector<unsigned char>{address_tag, 2};
    state->netInfo = NetInfoInterface::MakeNetInfo(version);
    BOOST_REQUIRE_EQUAL(state->netInfo->AddEntry(NetInfoPurpose::CORE_P2P,
                                                strprintf("1.1.1.%d:%d", address_tag, Params().GetDefaultPort())),
                        NetInfoStatus::Success);
    if (type == MnType::Evo) {
        state->platformNodeID = H160(address_tag + 40);
        BOOST_REQUIRE_EQUAL(state->netInfo->AddEntry(NetInfoPurpose::PLATFORM_P2P,
                                                    strprintf("2.2.2.%d:26657", address_tag)),
                            NetInfoStatus::Success);
        BOOST_REQUIRE_EQUAL(state->netInfo->AddEntry(NetInfoPurpose::PLATFORM_HTTPS,
                                                    strprintf("evo%d.example.org:443", address_tag)),
                            NetInfoStatus::Success);
    }

    auto dmn{std::make_shared<CDeterministicMN>(internal_id, type)};
    dmn->proTxHash = CollidingH(hash_suffix);
    dmn->collateralOutpoint = COutPoint(H(address_tag + 80), internal_id);
    dmn->nOperatorReward = address_tag * 10;
    state->UpdateConfirmedHash(dmn->proTxHash, H(address_tag + 100));
    dmn->pdmnState = std::move(state);
    return dmn;
}

CDeterministicMNList MNList(const uint256& block_hash, int height, bool reverse)
{
    CDeterministicMNList list{block_hash, height, 10};
    std::vector<CDeterministicMNCPtr> mns{
        MN(2, 3, MnType::Regular, ProTxVersion::LegacyBLS, 3),
        MN(5, 1, MnType::Evo, ProTxVersion::ExtAddr, 5),
        MN(7, 2, MnType::Regular, ProTxVersion::LegacyBLS, 7),
    };
    if (reverse) std::reverse(mns.begin(), mns.end());
    for (const auto& dmn : mns) list.AddMN(dmn, /*fBumpTotalCount=*/false);
    return list;
}

evo::CMinedQuorumCommitment Commitment(Consensus::LLMQType type, uint8_t quorum, uint8_t mined, bool rotated,
                                       int16_t index = 0)
{
    llmq::CFinalCommitment commitment;
    commitment.nVersion = rotated ? llmq::CFinalCommitment::BASIC_BLS_INDEXED_QUORUM_VERSION
                                  : llmq::CFinalCommitment::BASIC_BLS_NON_INDEXED_QUORUM_VERSION;
    commitment.llmqType = type;
    commitment.quorumHash = H(quorum);
    commitment.quorumIndex = index;
    const auto& params{evo::SnapshotLLMQParams(type)};
    commitment.signers.resize(params.size);
    commitment.validMembers.resize(params.size);
    return {H(quorum), H(quorum + 120), std::move(commitment), H(mined)};
}

evo::CEvoSnapshot SyntheticSnapshot(bool reverse_representation = false)
{
    evo::CEvoSnapshot snapshot;
    snapshot.base_block_hash = H(42);
    snapshot.mn_list = MNList(snapshot.base_block_hash, 500, reverse_representation);
    snapshot.credit_pool.locked = 123456;
    snapshot.credit_pool.currentLimit = 700;
    snapshot.credit_pool.latelyUnlocked = 11;
    if (reverse_representation) {
        BOOST_REQUIRE(snapshot.credit_pool.indexes.Add(15));
        BOOST_REQUIRE(snapshot.credit_pool.indexes.Add(8));
        BOOST_REQUIRE(snapshot.credit_pool.indexes.Add(7));
        BOOST_REQUIRE(snapshot.credit_pool.indexes.Add(9));
        BOOST_REQUIRE(snapshot.credit_pool.indexes.Remove(9));
        snapshot.mnhf_signals.emplace(9, 30);
        snapshot.mnhf_signals.emplace(2, 12);
    } else {
        BOOST_REQUIRE(snapshot.credit_pool.indexes.Add(7));
        BOOST_REQUIRE(snapshot.credit_pool.indexes.Add(8));
        BOOST_REQUIRE(snapshot.credit_pool.indexes.Add(15));
        snapshot.mnhf_signals.emplace(2, 12);
        snapshot.mnhf_signals.emplace(9, 30);
    }

    evo::CQuorumSnapshotData plain;
    plain.llmq_type = Consensus::LLMQType::LLMQ_TEST;
    plain.active_commitments = {Commitment(plain.llmq_type, 11, 51, false), Commitment(plain.llmq_type, 12, 52, false)};
    plain.safety_commitments = {Commitment(plain.llmq_type, 10, 50, false)};

    evo::CQuorumSnapshotData rotated;
    rotated.llmq_type = Consensus::LLMQType::LLMQ_TEST_DIP0024;
    rotated.rotation_enabled = true;
    rotated.active_commitments = {Commitment(rotated.llmq_type, 31, 71, true, 0),
                                  Commitment(rotated.llmq_type, 32, 72, true, 1)};
    rotated.safety_commitments = {Commitment(rotated.llmq_type, 21, 61, true, 0),
                                  Commitment(rotated.llmq_type, 22, 62, true, 1)};
    for (uint8_t i{1}; i <= evo::EVO_SNAPSHOT_ROTATION_CYCLES; ++i) {
        const auto mode{i == 2 ? SnapshotSkipMode::MODE_SKIPPING_ENTRIES : SnapshotSkipMode::MODE_NO_SKIPPING};
        rotated.rotation_snapshots.push_back(
            {H(40 + i), H(100 + i), llmq::CQuorumSnapshot{{true, false, true, false}, mode, i == 2 ? std::vector<int>{1} : std::vector<int>{}}});
    }

    snapshot.quorums = {std::move(plain), std::move(rotated)};
    std::set<uint256> work_hashes;
    std::set<std::pair<Consensus::LLMQType, uint256>> modifier_keys;
    for (const auto& data : snapshot.quorums) {
        for (const auto* commitments : {&data.active_commitments, &data.safety_commitments}) {
            for (const auto& entry : *commitments) {
                work_hashes.insert(entry.work_block_hash);
                modifier_keys.emplace(data.llmq_type, entry.work_block_hash);
            }
        }
        for (const auto& entry : data.rotation_snapshots) {
            work_hashes.insert(entry.work_block_hash);
            modifier_keys.emplace(data.llmq_type, entry.work_block_hash);
        }
    }
    CDeterministicMNList previous{snapshot.mn_list};
    uint256 previous_hash{snapshot.base_block_hash};
    int height{499};
    for (const auto& work_hash : work_hashes) {
        auto list{MNList(work_hash, height--, reverse_representation)};
        snapshot.historical_mn_list_diffs.push_back({previous_hash, work_hash, list.GetHeightForSnapshotCodec(),
                                                     list.GetTotalRegisteredCount(), evo::CanonicalMNListHash(list),
                                                     previous.BuildDiff(list)});
        previous_hash = work_hash;
        previous = std::move(list);
    }
    for (const auto& [type, work_hash] : modifier_keys) {
        snapshot.quorum_modifiers.push_back({type, work_hash, H(static_cast<uint8_t>(150 + snapshot.quorum_modifiers.size()))});
    }
    if (reverse_representation) {
        std::reverse(snapshot.quorums.begin(), snapshot.quorums.end());
        std::reverse(snapshot.historical_mn_list_diffs.begin(), snapshot.historical_mn_list_diffs.end());
        std::reverse(snapshot.quorum_modifiers.begin(), snapshot.quorum_modifiers.end());
        for (auto& data : snapshot.quorums) {
            std::reverse(data.active_commitments.begin(), data.active_commitments.end());
            std::reverse(data.safety_commitments.begin(), data.safety_commitments.end());
            std::reverse(data.rotation_snapshots.begin(), data.rotation_snapshots.end());
        }
    }
    return snapshot;
}

CDataStream SerializeSnapshot(const evo::CEvoSnapshot& snapshot)
{
    CDataStream stream{SER_DISK, CLIENT_VERSION};
    stream << snapshot;
    return stream;
}

void CheckInvalid(evo::CEvoSnapshot snapshot)
{
    BOOST_CHECK_THROW(snapshot.Validate(), std::ios_base::failure);
}

} // namespace

BOOST_AUTO_TEST_SUITE(evo_snapshot_tests)

BOOST_FIXTURE_TEST_CASE(populated_roundtrip_and_representation_independence, BasicTestingSetup)
{
    const auto forward{SyntheticSnapshot()};
    const auto reverse{SyntheticSnapshot(/*reverse_representation=*/true)};
    const auto forward_bytes{SerializeSnapshot(forward)};
    const auto reverse_bytes{SerializeSnapshot(reverse)};
    BOOST_CHECK_EQUAL_COLLECTIONS(forward_bytes.begin(), forward_bytes.end(), reverse_bytes.begin(), reverse_bytes.end());
    BOOST_CHECK(evo::CanonicalMNListHash(forward.mn_list) == evo::CanonicalMNListHash(reverse.mn_list));
    BOOST_CHECK(GetEvoSnapshotHash(forward) == GetEvoSnapshotHash(reverse));

    CDataStream input{forward_bytes};
    evo::CEvoSnapshot decoded;
    input >> decoded;
    BOOST_CHECK(input.empty());
    const auto decoded_bytes{SerializeSnapshot(decoded)};
    BOOST_CHECK_EQUAL_COLLECTIONS(forward_bytes.begin(), forward_bytes.end(), decoded_bytes.begin(), decoded_bytes.end());
    BOOST_CHECK(evo::CanonicalMNListHash(decoded.mn_list) == evo::CanonicalMNListHash(forward.mn_list));
    BOOST_CHECK_EQUAL(decoded.mn_list.GetCounts().total(), 3U);
    BOOST_CHECK_EQUAL(decoded.historical_mn_list_diffs.size(), forward.historical_mn_list_diffs.size());
    BOOST_CHECK(decoded.credit_pool.indexes.Contains(7));
    BOOST_CHECK(decoded.credit_pool.indexes.Contains(8));
    BOOST_CHECK(decoded.credit_pool.indexes.Contains(15));
    BOOST_CHECK(decoded.mnhf_signals == forward.mnhf_signals);

    for (const auto internal_id : {2U, 5U, 7U}) {
        const auto original{forward.mn_list.GetMNByInternalId(internal_id)};
        BOOST_REQUIRE(original);
        const auto by_hash{decoded.mn_list.GetMN(original->proTxHash)};
        const auto by_id{decoded.mn_list.GetMNByInternalId(internal_id)};
        const auto by_collateral{decoded.mn_list.GetUniquePropertyMN(original->collateralOutpoint)};
        const auto by_owner{decoded.mn_list.GetUniquePropertyMN(original->pdmnState->keyIDOwner)};
        const auto by_service{decoded.mn_list.GetMNByService(original->pdmnState->netInfo->GetPrimary())};
        BOOST_REQUIRE(by_hash);
        BOOST_REQUIRE(by_id);
        BOOST_REQUIRE(by_collateral);
        BOOST_REQUIRE(by_owner);
        BOOST_REQUIRE(by_service);
        BOOST_CHECK(by_hash->proTxHash == original->proTxHash);
        BOOST_CHECK(by_id->proTxHash == original->proTxHash);
        BOOST_CHECK(by_collateral->proTxHash == original->proTxHash);
        BOOST_CHECK(by_owner->proTxHash == original->proTxHash);
        BOOST_CHECK(by_service->proTxHash == original->proTxHash);
    }
}

BOOST_AUTO_TEST_CASE(reconstruction_horizon_height_enumeration)
{
    const auto rotated{evo::SnapshotLLMQParams(Consensus::LLMQType::LLMQ_TEST_DIP0024)};
    const auto plain{evo::SnapshotLLMQParams(Consensus::LLMQType::LLMQ_TEST)};
    const int base_height{20 * rotated.dkgInterval + 7};
    const auto heights{evo::EvoSnapshotReconstructionHeights(base_height, {rotated, plain})};
    BOOST_REQUIRE_EQUAL(heights.size(), evo::EVO_SNAPSHOT_ROTATION_CYCLES +
                                             evo::SnapshotCommitmentCount(plain, false));
    const int rotated_h{base_height - base_height % rotated.dkgInterval};
    for (size_t i{0}; i < evo::EVO_SNAPSHOT_ROTATION_CYCLES; ++i) {
        const int expected_cycle{rotated_h - static_cast<int>(i + 1) * rotated.dkgInterval};
        BOOST_CHECK(heights[i].rotation);
        BOOST_CHECK_EQUAL(heights[i].quorum_height, expected_cycle);
        BOOST_CHECK_EQUAL(heights[i].work_height, expected_cycle - llmq::WORK_DIFF_DEPTH);
    }
    const int plain_h{base_height - base_height % plain.dkgInterval};
    for (size_t i{0}; i < evo::SnapshotCommitmentCount(plain, false); ++i) {
        const auto& height{heights[evo::EVO_SNAPSHOT_ROTATION_CYCLES + i]};
        BOOST_CHECK(!height.rotation);
        BOOST_CHECK_EQUAL(height.quorum_height, plain_h - static_cast<int>(i) * plain.dkgInterval);
        BOOST_CHECK_EQUAL(height.work_height, height.quorum_height - llmq::WORK_DIFF_DEPTH);
    }
}

BOOST_AUTO_TEST_CASE(rotation_bitset_larger_than_quorum_roundtrips)
{
    const auto& params{evo::SnapshotLLMQParams(Consensus::LLMQType::LLMQ_TEST_DIP0024)};
    evo::CQuorumSnapshotEntry entry;
    entry.cycle_base_block_hash = H(1);
    entry.work_block_hash = H(2);
    entry.snapshot.activeQuorumMembers.resize(params.size + 3);
    entry.snapshot.activeQuorumMembers[params.size + 1] = true;
    entry.snapshot.mnSkipListMode = SnapshotSkipMode::MODE_NO_SKIPPING;

    CDataStream stream{SER_DISK, CLIENT_VERSION};
    evo::WriteRotationSnapshot(stream, entry);
    const auto decoded{evo::ReadRotationSnapshot(stream, params)};
    BOOST_CHECK(stream.empty());
    BOOST_CHECK_EQUAL(decoded.snapshot.activeQuorumMembers.size(), params.size + 3U);
    BOOST_CHECK(decoded.snapshot.activeQuorumMembers[params.size + 1]);
}

BOOST_FIXTURE_TEST_CASE(populated_v3_golden_value, BasicTestingSetup)
{
    BOOST_CHECK_EQUAL(GetEvoSnapshotHash(SyntheticSnapshot()).ToString(),
                      "bb1985a651ed3110218a3c8d65d77c85facdc544d6b9203f0815d5785c1f01ff");
}

BOOST_FIXTURE_TEST_CASE(canonical_mn_reader_rejects_order_and_counter, BasicTestingSetup)
{
    BOOST_CHECK(evo::CanonicalMNListHash(CDeterministicMNList{}) ==
                evo::CanonicalMNListHash(CDeterministicMNList{}));
    const auto write_raw = [](uint32_t total, std::vector<CDeterministicMNCPtr> mns) {
        CDataStream stream{SER_DISK, CLIENT_VERSION};
        stream << H(42) << 42 << total;
        WriteCompactSize(stream, mns.size());
        for (const auto& dmn : mns) stream << *dmn;
        return stream;
    };
    auto unsorted{write_raw(10, {MN(2, 2, MnType::Regular, ProTxVersion::LegacyBLS, 2),
                                 MN(1, 1, MnType::Regular, ProTxVersion::LegacyBLS, 1)})};
    BOOST_CHECK_THROW(evo::UnserializeCanonicalMNList(unsorted), std::ios_base::failure);
    auto bad_counter{write_raw(2, {MN(2, 1, MnType::Regular, ProTxVersion::LegacyBLS, 1)})};
    BOOST_CHECK_THROW(evo::UnserializeCanonicalMNList(bad_counter), std::ios_base::failure);
}

BOOST_FIXTURE_TEST_CASE(diff_chain_roundtrip_and_canonical_determinism, BasicTestingSetup)
{
    const auto base{MNList(H(10), 100, false)};
    auto target{base};
    target.RemoveMN(base.GetMNByInternalId(2)->proTxHash);
    target.AddMN(MN(8, 8, MnType::Regular, ProTxVersion::LegacyBLS, 8));
    for (const uint64_t id : {5, 7}) {
        const auto dmn{target.GetMNByInternalId(id)};
        auto state{std::make_shared<CDeterministicMNState>(*dmn->pdmnState)};
        state->nLastPaidHeight += static_cast<int>(id);
        target.UpdateMN(*dmn, state);
    }
    const auto diff{base.BuildDiff(target)};
    auto permuted{diff};
    std::reverse(permuted.addedMNs.begin(), permuted.addedMNs.end());
    std::vector<std::pair<uint64_t, CDeterministicMNStateDiff>> updates(permuted.updatedMNs.begin(),
                                                                       permuted.updatedMNs.end());
    std::reverse(updates.begin(), updates.end());
    permuted.updatedMNs.clear();
    for (auto& update : updates) permuted.updatedMNs.emplace(std::move(update));

    CDataStream canonical{SER_DISK, CLIENT_VERSION};
    CDataStream reordered{SER_DISK, CLIENT_VERSION};
    evo::SerializeCanonicalMNListDiff(canonical, diff);
    evo::SerializeCanonicalMNListDiff(reordered, permuted);
    BOOST_CHECK_EQUAL_COLLECTIONS(canonical.begin(), canonical.end(), reordered.begin(), reordered.end());

    auto decoded{evo::UnserializeCanonicalMNListDiff(canonical)};
    auto reconstructed{base};
    reconstructed.ApplyDiffForSnapshot(H(11), 99, target.GetTotalRegisteredCount(), decoded);
    target.ApplyDiffForSnapshot(H(11), 99, target.GetTotalRegisteredCount(), CDeterministicMNListDiff{});
    BOOST_CHECK(evo::CanonicalMNListHash(reconstructed) == evo::CanonicalMNListHash(target));
    BOOST_CHECK(canonical.empty());
}

BOOST_FIXTURE_TEST_CASE(historical_diff_decode_has_cumulative_operation_budget, BasicTestingSetup)
{
    CDeterministicMNListDiff one_removal;
    one_removal.removedMns.emplace(1);
    CDataStream first{SER_DISK, CLIENT_VERSION};
    CDataStream second{SER_DISK, CLIENT_VERSION};
    evo::SerializeCanonicalMNListDiff(first, one_removal);
    evo::SerializeCanonicalMNListDiff(second, one_removal);

    size_t remaining_operations{1};
    const auto decoded{evo::UnserializeCanonicalMNListDiff(first, remaining_operations)};
    BOOST_CHECK_EQUAL(decoded.removedMns.size(), 1U);
    BOOST_CHECK_EQUAL(remaining_operations, 0U);
    BOOST_CHECK_THROW(evo::UnserializeCanonicalMNListDiff(second, remaining_operations),
                      std::ios_base::failure);
    BOOST_CHECK(evo::EvoSnapshotMaxHistoricalMNLists() < 2'048U);
}

BOOST_FIXTURE_TEST_CASE(context_free_validation_matrix, BasicTestingSetup)
{
    auto snapshot{SyntheticSnapshot()};
    snapshot.version++;
    CheckInvalid(snapshot);
    snapshot = SyntheticSnapshot();
    snapshot.base_block_hash = H(1);
    CheckInvalid(snapshot);
    snapshot = SyntheticSnapshot();
    std::reverse(snapshot.quorums.begin(), snapshot.quorums.end());
    BOOST_CHECK_THROW(snapshot.Validate(/*require_canonical_order=*/true), std::ios_base::failure);
    snapshot = SyntheticSnapshot();
    std::reverse(snapshot.historical_mn_list_diffs.begin(), snapshot.historical_mn_list_diffs.end());
    BOOST_CHECK_THROW(snapshot.Validate(/*require_canonical_order=*/true), std::ios_base::failure);
    snapshot = SyntheticSnapshot();
    std::reverse(snapshot.quorums[0].active_commitments.begin(), snapshot.quorums[0].active_commitments.end());
    BOOST_CHECK_THROW(snapshot.Validate(/*require_canonical_order=*/true), std::ios_base::failure);
    snapshot = SyntheticSnapshot();
    std::reverse(snapshot.quorums[1].rotation_snapshots.begin(), snapshot.quorums[1].rotation_snapshots.end());
    BOOST_CHECK_THROW(snapshot.Validate(/*require_canonical_order=*/true), std::ios_base::failure);
    snapshot = SyntheticSnapshot();
    snapshot.historical_mn_list_diffs[0].block_hash = H(1);
    CheckInvalid(snapshot);
    snapshot = SyntheticSnapshot();
    snapshot.quorums[1].rotation_snapshots[0].work_block_hash = H(1);
    CheckInvalid(snapshot);
    snapshot = SyntheticSnapshot();
    snapshot.quorums[0].llmq_type = Consensus::LLMQType::LLMQ_NONE;
    CheckInvalid(snapshot);
    snapshot = SyntheticSnapshot();
    snapshot.quorums[0].rotation_enabled = true;
    CheckInvalid(snapshot);
    snapshot = SyntheticSnapshot();
    snapshot.quorums[0].active_commitments.pop_back();
    CheckInvalid(snapshot);
    snapshot = SyntheticSnapshot();
    snapshot.quorums[0].safety_commitments.clear();
    CheckInvalid(snapshot);
    snapshot = SyntheticSnapshot();
    snapshot.quorums[1].rotation_snapshots.pop_back();
    CheckInvalid(snapshot);

    // Parameter-derived history counts are ceilings. Young chains and newly
    // activated quorum types legitimately carry fewer commitments and cycles;
    // chain-aware validation and the base CbTx establish completeness later.
    snapshot = SyntheticSnapshot();
    snapshot.quorums.clear();
    snapshot.historical_mn_list_diffs.clear();
    snapshot.quorum_modifiers.clear();
    evo::CQuorumSnapshotData partial;
    partial.llmq_type = Consensus::LLMQType::LLMQ_TEST;
    snapshot.quorums.emplace_back(std::move(partial));
    BOOST_CHECK_NO_THROW(snapshot.Validate());

    const auto mutate_commitment = [](auto mutation) {
        auto value{SyntheticSnapshot()};
        mutation(value.quorums[0].active_commitments[0]);
        CheckInvalid(std::move(value));
    };
    mutate_commitment([](auto& e) { e.commitment.validMembers.resize(e.commitment.signers.size() + 1); });
    mutate_commitment([](auto& e) {
        e.commitment.signers.clear();
        e.commitment.validMembers.clear();
    });
    mutate_commitment([](auto& e) { e.quorum_base_block_hash.SetNull(); });
    mutate_commitment([](auto& e) { e.mined_block_hash.SetNull(); });
    mutate_commitment([](auto& e) { e.commitment.llmqType = Consensus::LLMQType::LLMQ_TEST_PLATFORM; });
    mutate_commitment([](auto& e) { e.commitment.quorumHash = H(99); });
    mutate_commitment([](auto& e) { e.commitment.nVersion = llmq::CFinalCommitment::BASIC_BLS_INDEXED_QUORUM_VERSION; });
    mutate_commitment([](auto& e) { e.commitment.nVersion = 99; });
    snapshot = SyntheticSnapshot();
    snapshot.quorums[1].active_commitments[1].commitment.quorumIndex = 0;
    CheckInvalid(snapshot);
    snapshot = SyntheticSnapshot();
    snapshot.quorums[1].active_commitments[1].commitment.quorumIndex = 2;
    CheckInvalid(snapshot);

    const auto mutate_rotation = [](auto mutation) {
        auto value{SyntheticSnapshot()};
        mutation(value.quorums[1].rotation_snapshots[0]);
        CheckInvalid(std::move(value));
    };
    mutate_rotation([](auto& e) { e.cycle_base_block_hash.SetNull(); });
    mutate_rotation([](auto& e) { e.work_block_hash.SetNull(); });
    mutate_rotation([](auto& e) { e.snapshot.mnSkipListMode = static_cast<SnapshotSkipMode>(9); });
    mutate_rotation([](auto& e) { e.snapshot.activeQuorumMembers.resize(evo::EVO_SNAPSHOT_MAX_MNS + 1); });
    mutate_rotation([](auto& e) { e.snapshot.mnSkipList = {-1}; });

    // A cycle's skip list accumulates across every quorum index, so lengths
    // beyond a single quorum's size and negative wraparound deltas after the
    // first (absolute) entry are legitimate.
    auto aggregate_skips{SyntheticSnapshot()};
    auto& rotation_entry{aggregate_skips.quorums[1].rotation_snapshots[0]};
    const auto& rotation_params{evo::SnapshotLLMQParams(aggregate_skips.quorums[1].llmq_type)};
    rotation_entry.snapshot.mnSkipListMode = SnapshotSkipMode::MODE_SKIPPING_ENTRIES;
    rotation_entry.snapshot.mnSkipList.assign(static_cast<size_t>(rotation_params.size) + 2, 1);
    rotation_entry.snapshot.mnSkipList.front() = 3;
    rotation_entry.snapshot.mnSkipList.back() = -2;
    BOOST_CHECK_NO_THROW(aggregate_skips.Validate());
}

BOOST_FIXTURE_TEST_CASE(bounded_readers_reject_claimed_sizes_first, BasicTestingSetup)
{
    CDataStream mn_stream{SER_DISK, CLIENT_VERSION};
    mn_stream << H(1) << 1 << uint32_t{0};
    WriteCompactSize(mn_stream, evo::EVO_SNAPSHOT_MAX_MNS + 1);
    BOOST_CHECK_THROW(evo::UnserializeCanonicalMNList(mn_stream), std::ios_base::failure);
    BOOST_CHECK(mn_stream.empty());

    const auto decode_quorum = [](CDataStream stream) {
        evo::CQuorumSnapshotData data;
        stream >> data;
    };
    const auto& plain_params{evo::SnapshotLLMQParams(Consensus::LLMQType::LLMQ_TEST)};
    const size_t commitment_limit{evo::SnapshotCommitmentCount(plain_params, false)};
    CDataStream active{SER_DISK, CLIENT_VERSION};
    active << Consensus::LLMQType::LLMQ_TEST << false;
    WriteCompactSize(active, plain_params.signingActiveQuorumCount + 1);
    BOOST_CHECK_THROW(decode_quorum(active), std::ios_base::failure);
    CDataStream safety{SER_DISK, CLIENT_VERSION};
    safety << Consensus::LLMQType::LLMQ_TEST << false;
    WriteCompactSize(safety, 0);
    WriteCompactSize(safety, commitment_limit - plain_params.signingActiveQuorumCount + 1);
    BOOST_CHECK_THROW(decode_quorum(safety), std::ios_base::failure);
    CDataStream rotations{SER_DISK, CLIENT_VERSION};
    rotations << Consensus::LLMQType::LLMQ_TEST_DIP0024 << true;
    WriteCompactSize(rotations, 0);
    WriteCompactSize(rotations, 0);
    WriteCompactSize(rotations, evo::EVO_SNAPSHOT_ROTATION_CYCLES + 1);
    BOOST_CHECK_THROW(decode_quorum(rotations), std::ios_base::failure);

    const auto& rotated_params{evo::SnapshotLLMQParams(Consensus::LLMQType::LLMQ_TEST_DIP0024)};
    CDataStream bitset{SER_DISK, CLIENT_VERSION};
    bitset << H(1) << H(2) << SnapshotSkipMode::MODE_NO_SKIPPING;
    WriteCompactSize(bitset, evo::EVO_SNAPSHOT_MAX_MNS + 1);
    BOOST_CHECK_THROW(evo::ReadRotationSnapshot(bitset, rotated_params), std::ios_base::failure);
    CDataStream skip_list{SER_DISK, CLIENT_VERSION};
    skip_list << H(1) << H(2) << SnapshotSkipMode::MODE_NO_SKIPPING;
    WriteCompactSize(skip_list, 0);
    WriteCompactSize(skip_list, rotated_params.size + 1);
    BOOST_CHECK_THROW(evo::ReadRotationSnapshot(skip_list, rotated_params), std::ios_base::failure);

    CDataStream commitment_bits{SER_DISK, CLIENT_VERSION};
    auto oversized_commitment{Commitment(Consensus::LLMQType::LLMQ_TEST, 1, 2, false)};
    oversized_commitment.commitment.signers.resize(plain_params.size + 1);
    commitment_bits << Consensus::LLMQType::LLMQ_TEST << false;
    WriteCompactSize(commitment_bits, 1);
    commitment_bits << oversized_commitment;
    evo::CQuorumSnapshotData oversized_data;
    BOOST_CHECK_EXCEPTION(commitment_bits >> oversized_data, std::ios_base::failure, [](const auto& e) {
        return std::string{e.what()}.find("inconsistent evo snapshot commitment bitset sizes") != std::string::npos;
    });
    // The valid-members bitset payload, BLS material, and mined-block hash
    // remain unread: rejection occurs at the mismatched claimed size.
    BOOST_CHECK_GT(commitment_bits.size(), uint256::size());

    const auto commitment_prefix = [](CDataStream& s) {
        s << Consensus::LLMQType::LLMQ_TEST << false;
        WriteCompactSize(s, 1);
        s << H(1) << H(2) << uint16_t{llmq::CFinalCommitment::BASIC_BLS_NON_INDEXED_QUORUM_VERSION}
          << Consensus::LLMQType::LLMQ_TEST << H(1);
    };
    CDataStream over_ceiling{SER_DISK, CLIENT_VERSION};
    commitment_prefix(over_ceiling);
    WriteCompactSize(over_ceiling, evo::EVO_SNAPSHOT_MAX_QUORUM_SIZE + 1);
    BOOST_CHECK_THROW(decode_quorum(over_ceiling), std::ios_base::failure);
    CDataStream empty_bits{SER_DISK, CLIENT_VERSION};
    commitment_prefix(empty_bits);
    WriteCompactSize(empty_bits, 0);
    BOOST_CHECK_THROW(decode_quorum(empty_bits), std::ios_base::failure);

    auto oversized_payout_mn{std::const_pointer_cast<CDeterministicMN>(
        MN(8, 8, MnType::Regular, ProTxVersion::ExtAddr, 8))};
    auto payout_state{std::make_shared<CDeterministicMNState>(*oversized_payout_mn->pdmnState)};
    payout_state->payouts.resize(evo::EVO_SNAPSHOT_MAX_PAYOUT_SHARES + 1);
    oversized_payout_mn->pdmnState = std::move(payout_state);
    CDataStream payouts{SER_DISK, CLIENT_VERSION};
    payouts << H(42) << 42 << uint32_t{10};
    WriteCompactSize(payouts, 1);
    payouts << *oversized_payout_mn;
    BOOST_CHECK_THROW(evo::UnserializeCanonicalMNList(payouts), std::ios_base::failure);

    CDataStream wrapped_string{SER_DISK, CLIENT_VERSION};
    WriteCompactSize(wrapped_string, evo::EVO_SNAPSHOT_MAX_MN_COMPACT_ITEMS + 1);
    wrapped_string << uint8_t{0x42};
    evo::SnapshotBoundedInput bounded_string{wrapped_string, evo::EVO_SNAPSHOT_MAX_MN_COMPACT_ITEMS};
    OverrideStream bounded_override{&bounded_string, SER_DISK, CLIENT_VERSION};
    std::string decoded_string;
    BOOST_CHECK_EXCEPTION(bounded_override >> decoded_string, std::ios_base::failure,
                          [](const auto& e) { return std::string{e.what()}.find("CompactSize budget exceeded") != std::string::npos; });
    BOOST_CHECK(decoded_string.empty());
    BOOST_REQUIRE_EQUAL(wrapped_string.size(), 1U);
    BOOST_CHECK_EQUAL(std::to_integer<uint8_t>(wrapped_string.data()[0]), 0x42);

    const auto snapshot_prefix = [](CDataStream& stream) {
        stream << evo::EVO_SNAPSHOT_VERSION << H(42);
        evo::SerializeCanonicalMNList(stream, CDeterministicMNList{H(42), 1, 0});
    };
    CDataStream quorum_types{SER_DISK, CLIENT_VERSION};
    snapshot_prefix(quorum_types);
    WriteCompactSize(quorum_types, Consensus::available_llmqs.size() + 1);
    evo::CEvoSnapshot decoded;
    BOOST_CHECK_THROW(quorum_types >> decoded, std::ios_base::failure);

    CDataStream history{SER_DISK, CLIENT_VERSION};
    snapshot_prefix(history);
    WriteCompactSize(history, 0);
    WriteCompactSize(history, evo::EvoSnapshotMaxHistoricalMNLists() + 1);
    BOOST_CHECK_THROW(history >> decoded, std::ios_base::failure);

    CDataStream signals{SER_DISK, CLIENT_VERSION};
    snapshot_prefix(signals);
    WriteCompactSize(signals, 0);
    WriteCompactSize(signals, 0);
    WriteCompactSize(signals, 0);
    signals << CCreditPool{};
    WriteCompactSize(signals, Consensus::MAX_VERSION_BITS_DEPLOYMENTS + 1);
    BOOST_CHECK_THROW(signals >> decoded, std::ios_base::failure);

    CDataStream ranges{SER_DISK, CLIENT_VERSION};
    snapshot_prefix(ranges);
    WriteCompactSize(ranges, 0);
    WriteCompactSize(ranges, 0);
    WriteCompactSize(ranges, 0);
    ranges << CAmount{0} << CAmount{0} << CAmount{0};
    WriteCompactSize(ranges, evo::EVO_SNAPSHOT_MAX_RANGES + 1);
    BOOST_CHECK_THROW(ranges >> decoded, std::ios_base::failure);
    BOOST_CHECK(ranges.empty());

    const auto snapshot_bytes{SerializeSnapshot(SyntheticSnapshot())};
    DomainPort domain;
    BOOST_REQUIRE_EQUAL(domain.Set("evo5.example.org", 443), DomainPort::Status::Success);
    CDataStream encoded_domain{SER_DISK, CLIENT_VERSION};
    encoded_domain << domain;
    const auto domain_pos{std::search(snapshot_bytes.begin(), snapshot_bytes.end(),
                                      encoded_domain.begin(), encoded_domain.end())};
    BOOST_REQUIRE(domain_pos != snapshot_bytes.end());

    CDataStream oversized_domain{SER_DISK, CLIENT_VERSION};
    const size_t domain_offset{static_cast<size_t>(std::distance(snapshot_bytes.begin(), domain_pos))};
    oversized_domain.write(Span{snapshot_bytes}.first(domain_offset));
    constexpr size_t MAX_DOMAIN_LENGTH{253};
    WriteCompactSize(oversized_domain, MAX_DOMAIN_LENGTH + 1);
    const std::string oversized_addr(MAX_DOMAIN_LENGTH + 1, 'a');
    oversized_domain.write(MakeByteSpan(oversized_addr));
    const size_t serialized_addr_size{encoded_domain.size() - sizeof(uint16_t)};
    oversized_domain.write(Span{snapshot_bytes}.subspan(domain_offset + serialized_addr_size));
    BOOST_CHECK_EXCEPTION(oversized_domain >> decoded, std::ios_base::failure,
                          [](const auto& e) { return std::string{e.what()}.find("String length limit exceeded") != std::string::npos; });
}

BOOST_FIXTURE_TEST_CASE(commitment_sizes_are_format_bounded_not_param_exact, BasicTestingSetup)
{
    // -llmqtestparams and -llmqdevnetparams change the effective quorum size at
    // runtime, so commitments whose bitsets differ from the static default must
    // pass this layer; exact sizing is established by chain-aware validation.
    auto snapshot{SyntheticSnapshot()};
    const auto& params{evo::SnapshotLLMQParams(snapshot.quorums[0].llmq_type)};
    for (auto& entry : snapshot.quorums[0].active_commitments) {
        entry.commitment.signers.assign(params.size + 5, false);
        entry.commitment.validMembers.assign(params.size + 5, true);
    }
    BOOST_CHECK_NO_THROW(snapshot.Validate(/*require_canonical_order=*/true));
    const auto bytes{SerializeSnapshot(snapshot)};
    CDataStream input{bytes};
    evo::CEvoSnapshot decoded;
    BOOST_CHECK_NO_THROW(input >> decoded);
    BOOST_CHECK(input.empty());
    const auto reencoded{SerializeSnapshot(decoded)};
    BOOST_CHECK_EQUAL_COLLECTIONS(bytes.begin(), bytes.end(), reencoded.begin(), reencoded.end());
}

BOOST_FIXTURE_TEST_CASE(mnhf_signal_wire_order_is_canonical, BasicTestingSetup)
{
    const auto bytes{SerializeSnapshot(SyntheticSnapshot())};
    // The MNHF signal section is the encoding's tail: a count followed by
    // (bit, height) pairs. SyntheticSnapshot carries (2, 12) and (9, 30).
    const size_t tail_size{1 + 2 * (sizeof(uint8_t) + sizeof(int32_t))};
    const auto with_signals = [&](const std::vector<std::pair<uint8_t, int>>& signals) {
        CDataStream stream{SER_DISK, CLIENT_VERSION};
        stream.write(Span{bytes}.first(bytes.size() - tail_size));
        WriteCompactSize(stream, signals.size());
        for (const auto& signal : signals) stream << signal;
        return stream;
    };
    const auto expect_noncanonical = [](CDataStream stream) {
        evo::CEvoSnapshot decoded;
        BOOST_CHECK_EXCEPTION(stream >> decoded, std::ios_base::failure, [](const auto& e) {
            return std::string{e.what()}.find("noncanonical MNHF signal order") != std::string::npos;
        });
    };
    auto canonical{with_signals({{2, 12}, {9, 30}})};
    evo::CEvoSnapshot decoded;
    BOOST_CHECK_NO_THROW(canonical >> decoded);
    BOOST_CHECK(canonical.empty());
    BOOST_CHECK_EQUAL(decoded.mnhf_signals.size(), 2U);
    expect_noncanonical(with_signals({{9, 30}, {2, 12}}));
    expect_noncanonical(with_signals({{2, 12}, {2, 30}}));
}

BOOST_FIXTURE_TEST_CASE(cbtx_cross_checks, BasicTestingSetup)
{
    const auto snapshot{SyntheticSnapshot()};
    CCbTx cbtx;
    cbtx.nVersion = CCbTx::Version::CLSIG_AND_BALANCE;
    cbtx.merkleRootMNList = snapshot.mn_list.to_sml()->CalcMerkleRoot();
    std::vector<uint256> quorum_hashes;
    for (const auto& data : snapshot.quorums) {
        for (const auto& entry : data.active_commitments) quorum_hashes.emplace_back(SerializeHash(entry.commitment));
    }
    std::sort(quorum_hashes.begin(), quorum_hashes.end());
    cbtx.merkleRootQuorums = ComputeMerkleRoot(quorum_hashes);
    cbtx.creditPoolBalance = snapshot.credit_pool.locked;

    std::string error;
    BOOST_CHECK(evo::VerifyEvoSnapshotCbTx(snapshot, cbtx, error));
    cbtx.merkleRootMNList = H(1);
    BOOST_CHECK(!evo::VerifyEvoSnapshotCbTx(snapshot, cbtx, error));
    cbtx.merkleRootMNList = snapshot.mn_list.to_sml()->CalcMerkleRoot();
    cbtx.merkleRootQuorums = H(2);
    BOOST_CHECK(!evo::VerifyEvoSnapshotCbTx(snapshot, cbtx, error));
    cbtx.merkleRootQuorums = ComputeMerkleRoot(quorum_hashes);
    cbtx.creditPoolBalance++;
    BOOST_CHECK(!evo::VerifyEvoSnapshotCbTx(snapshot, cbtx, error));
}

BOOST_FIXTURE_TEST_CASE(rejects_unknown_wire_version, BasicTestingSetup)
{
    auto bytes{SerializeSnapshot(SyntheticSnapshot())};
    bytes.data()[0] = std::byte{4};
    evo::CEvoSnapshot decoded;
    BOOST_CHECK_THROW(bytes >> decoded, std::ios_base::failure);
}

BOOST_AUTO_TEST_SUITE_END()
