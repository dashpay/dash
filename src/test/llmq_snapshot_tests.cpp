// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/llmq_tests.h>
#include <test/util/setup_common.h>

#include <chain.h>
#include <chainparams.h>
#include <streams.h>
#include <univalue.h>

#include <llmq/params.h>
#include <llmq/snapshot.h>

#include <boost/test/unit_test.hpp>

#include <vector>

using namespace llmq;
using namespace llmq::testutils;

BOOST_FIXTURE_TEST_SUITE(llmq_snapshot_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(quorum_snapshot_serialization_test)
{
    std::vector<bool> activeMembers = CreateBitVector(10, {0, 2, 4, 6, 8});
    const auto skipMode = SnapshotSkipMode::MODE_SKIPPING_ENTRIES;
    const std::vector<int> skipList = {10, 20, 30};

    CQuorumSnapshot snapshot(activeMembers, skipMode, skipList);
    BOOST_CHECK(TestSerializationRoundtrip(snapshot));

    DataStream ss{};
    ss << snapshot;
    CQuorumSnapshot deserialized;
    ss >> deserialized;
    BOOST_CHECK(deserialized.activeQuorumMembers == snapshot.activeQuorumMembers);
    BOOST_CHECK_EQUAL(deserialized.mnSkipListMode, snapshot.mnSkipListMode);
    BOOST_CHECK(deserialized.mnSkipList == snapshot.mnSkipList);

    for (const auto mode : {SnapshotSkipMode::MODE_NO_SKIPPING, SnapshotSkipMode::MODE_SKIPPING_ENTRIES,
                            SnapshotSkipMode::MODE_NO_SKIPPING_ENTRIES, SnapshotSkipMode::MODE_ALL_SKIPPED}) {
        CQuorumSnapshot mode_snapshot({true, false, true}, mode, {1, 2, 3});
        DataStream mode_stream{};
        mode_stream << mode_snapshot;
        CQuorumSnapshot mode_out;
        mode_stream >> mode_out;
        BOOST_CHECK_EQUAL(mode_out.mnSkipListMode, mode);
    }

    std::vector<bool> large_members(400);
    for (size_t i = 0; i < large_members.size(); ++i) {
        large_members[i] = (i % 3 != 0);
    }
    std::vector<int> large_skip;
    large_skip.reserve(100);
    for (int i = 0; i < 100; ++i) {
        large_skip.push_back(i * 4);
    }
    const CQuorumSnapshot large(large_members, SnapshotSkipMode::MODE_SKIPPING_ENTRIES, large_skip);
    DataStream large_stream{};
    large_stream << large;
    CQuorumSnapshot large_out;
    large_stream >> large_out;
    BOOST_CHECK(large_out.activeQuorumMembers == large_members);
    BOOST_CHECK(large_out.mnSkipList == large_skip);

    BOOST_CHECK(TestSerializationRoundtrip(CQuorumSnapshot({}, SnapshotSkipMode::MODE_NO_SKIPPING, {})));
    BOOST_CHECK(TestSerializationRoundtrip(CQuorumSnapshot({}, SnapshotSkipMode::MODE_SKIPPING_ENTRIES, {1, 2, 3})));
    BOOST_CHECK(TestSerializationRoundtrip(CQuorumSnapshot({true, false, true}, SnapshotSkipMode::MODE_NO_SKIPPING, {})));
    BOOST_CHECK(TestSerializationRoundtrip(CQuorumSnapshot({true}, SnapshotSkipMode::MODE_NO_SKIPPING, {})));
    BOOST_CHECK(TestSerializationRoundtrip(CQuorumSnapshot(std::vector<bool>(8, true), SnapshotSkipMode::MODE_NO_SKIPPING, {})));
    std::vector<bool> nine(9, false);
    nine[8] = true;
    BOOST_CHECK(TestSerializationRoundtrip(CQuorumSnapshot(nine, SnapshotSkipMode::MODE_NO_SKIPPING, {})));
    std::vector<bool> alternating(16);
    for (size_t i = 0; i < alternating.size(); ++i) {
        alternating[i] = (i % 2 == 0);
    }
    BOOST_CHECK(TestSerializationRoundtrip(CQuorumSnapshot(alternating, SnapshotSkipMode::MODE_NO_SKIPPING, {})));
}

BOOST_AUTO_TEST_CASE(get_last_base_block_hash_repeated_base_blocks_test)
{
    std::vector<CBlockIndex> blocks(4);
    std::vector<uint256> hashes{
        GetTestBlockHash(10),
        GetTestBlockHash(20),
        GetTestBlockHash(30),
        GetTestBlockHash(40),
    };
    for (size_t i{0}; i < blocks.size(); ++i) {
        blocks[i].nHeight = static_cast<int>((i + 1) * 10);
        blocks[i].phashBlock = &hashes[i];
    }

    // GetLastBaseBlockHash() requires sorted input and tolerates duplicates as a no-op.
    // BuildQuorumRotationInfo deliberately does NOT deduplicate in the legacy path so
    // the wire response to older peers stays bit-for-bit identical; these checks
    // demonstrate that the duplicate is harmless to GetLastBaseBlockHash's output.
    std::vector<const CBlockIndex*> sorted_repeated_base_blocks{
        &blocks[0],
        &blocks[1],
        &blocks[1],
        &blocks[2],
    };
    std::vector<const CBlockIndex*> sorted_unique_base_blocks{
        &blocks[0],
        &blocks[1],
        &blocks[2],
    };
    BOOST_CHECK(GetLastBaseBlockHash(sorted_repeated_base_blocks, &blocks[3]) == hashes[2]);
    BOOST_CHECK(GetLastBaseBlockHash(sorted_repeated_base_blocks, &blocks[1]) == hashes[1]);
    // Duplicate vs unique input produces the same hash.
    BOOST_CHECK(GetLastBaseBlockHash(sorted_repeated_base_blocks, &blocks[3]) ==
                GetLastBaseBlockHash(sorted_unique_base_blocks, &blocks[3]));
    BOOST_CHECK(GetLastBaseBlockHash(sorted_repeated_base_blocks, &blocks[1]) ==
                GetLastBaseBlockHash(sorted_unique_base_blocks, &blocks[1]));

    // No base at or below the target falls back to the genesis hash.
    std::vector<const CBlockIndex*> above_target{&blocks[2], &blocks[3]};
    BOOST_CHECK(GetLastBaseBlockHash(above_target, &blocks[0]) == Params().GetConsensus().hashGenesisBlock);
    BOOST_CHECK(GetLastBaseBlockHash({}, &blocks[3]) == Params().GetConsensus().hashGenesisBlock);
}

BOOST_AUTO_TEST_CASE(get_quorum_rotation_info_serialization_test)
{
    CGetQuorumRotationInfo getInfo;

    // Test with multiple base block hashes
    getInfo.baseBlockHashes = {GetTestBlockHash(1), GetTestBlockHash(2), GetTestBlockHash(3)};
    getInfo.blockRequestHash = GetTestBlockHash(100);
    getInfo.extraShare = true;

    // Test serialization
    BOOST_CHECK(TestSerializationRoundtrip(getInfo));

    // Test with empty base block hashes
    CGetQuorumRotationInfo emptyInfo;
    emptyInfo.blockRequestHash = GetTestBlockHash(200);
    emptyInfo.extraShare = false;

    BOOST_CHECK(TestSerializationRoundtrip(emptyInfo));
}

BOOST_AUTO_TEST_CASE(get_quorum_rotation_info_base_block_hashes_limit_test)
{
    const auto serialize_with_count{[](size_t count) {
        CGetQuorumRotationInfo info;
        info.blockRequestHash = GetTestBlockHash(100);
        info.baseBlockHashes.assign(count, GetTestBlockHash(1));
        DataStream ss{};
        ss << info;
        return ss;
    }};

    // A request at the limit still round-trips.
    {
        DataStream ss{serialize_with_count(MAX_BASE_BLOCK_HASHES)};
        CGetQuorumRotationInfo parsed;
        BOOST_CHECK_NO_THROW(ss >> parsed);
        BOOST_CHECK_EQUAL(parsed.baseBlockHashes.size(), MAX_BASE_BLOCK_HASHES);
    }

    // One past it is rejected before any element is decoded, so an unauthenticated peer
    // cannot make the server hold a list sized by MAX_PROTOCOL_MESSAGE_LENGTH.
    {
        DataStream ss{serialize_with_count(MAX_BASE_BLOCK_HASHES + 1)};
        const size_t payload_after_count{ss.size() - 3}; // CompactSize for 4097 is 3 bytes
        CGetQuorumRotationInfo parsed;
        BOOST_CHECK_THROW(ss >> parsed, std::ios_base::failure);
        BOOST_CHECK(parsed.baseBlockHashes.empty());
        BOOST_CHECK_EQUAL(ss.size(), payload_after_count);
    }
}

BOOST_AUTO_TEST_CASE(quorum_rotation_info_serialization_test)
{
    // Note: mnListDiff{smth} testing requires proper CSimplifiedMNListDiff setup
    // which is complex and better tested in functional tests

    // Test CQuorumRotationInfo serialization with various optional field combinations
    CQuorumRotationInfo rotInfo;

    // Set up basic required fields
    rotInfo.cycleHMinusC.m_snap = CQuorumSnapshot({true, false, true}, SnapshotSkipMode::MODE_SKIPPING_ENTRIES, {1, 2});
    rotInfo.cycleHMinus2C.m_snap = CQuorumSnapshot({false, true, false}, SnapshotSkipMode::MODE_NO_SKIPPING, {});
    rotInfo.cycleHMinus3C.m_snap = CQuorumSnapshot({true, true, false}, SnapshotSkipMode::MODE_ALL_SKIPPED, {3});

    // Test without extraShare
    rotInfo.extraShare = false;
    BOOST_CHECK(TestSerializationRoundtrip(rotInfo));

    // Test with extraShare but uninitialized optional fields
    rotInfo.extraShare = true;
    BOOST_CHECK(TestSerializationRoundtrip(rotInfo));

    // Test with extraShare and initialized snapshot
    llmq::CycleData extra_cycle;
    extra_cycle.m_snap = CQuorumSnapshot({false, false, true}, SnapshotSkipMode::MODE_SKIPPING_ENTRIES, {4, 5, 6});
    rotInfo.cycleHMinus4C = extra_cycle;
    BOOST_CHECK(TestSerializationRoundtrip(rotInfo));

    CFinalCommitment commitment{GetLLMQParams(Consensus::LLMQType::LLMQ_TEST), uint256::ONE};
    rotInfo.lastCommitmentPerIndex.push_back(commitment);
    rotInfo.quorumSnapshotList.push_back(
        CQuorumSnapshot({false, false, true}, SnapshotSkipMode::MODE_SKIPPING_ENTRIES, {7, 8}));
    BOOST_CHECK(TestSerializationRoundtrip(rotInfo));
}

BOOST_AUTO_TEST_CASE(quorum_snapshot_json_values)
{
    const std::vector<bool> activeMembers = {true, false, true};
    const std::vector<int> skipList = {10, 20};
    CQuorumSnapshot snapshot(activeMembers, SnapshotSkipMode::MODE_SKIPPING_ENTRIES, skipList);

    const UniValue json = snapshot.ToJson();
    BOOST_CHECK_EQUAL(json["mnSkipListMode"].getInt<int>(),
                      static_cast<int>(SnapshotSkipMode::MODE_SKIPPING_ENTRIES));
    BOOST_REQUIRE_EQUAL(json["activeQuorumMembers"].size(), activeMembers.size());
    BOOST_CHECK_EQUAL(json["activeQuorumMembers"][0].get_bool(), true);
    BOOST_CHECK_EQUAL(json["activeQuorumMembers"][1].get_bool(), false);
    BOOST_CHECK_EQUAL(json["activeQuorumMembers"][2].get_bool(), true);
    BOOST_REQUIRE_EQUAL(json["mnSkipList"].size(), skipList.size());
    BOOST_CHECK_EQUAL(json["mnSkipList"][0].getInt<int>(), 10);
    BOOST_CHECK_EQUAL(json["mnSkipList"][1].getInt<int>(), 20);
}

BOOST_AUTO_TEST_CASE(quorum_snapshot_truncated_wire_is_rejected)
{
    // Every proper prefix of a populated snapshot is shorter than the length the
    // compact-size fields claim, so Unserialize must fail rather than return a
    // partial object. One-byte steps, not a stride, so a prefix the stride skipped
    // cannot be accepted.
    const CQuorumSnapshot snapshot({true, false, true}, SnapshotSkipMode::MODE_SKIPPING_ENTRIES, {1, 2, 3});
    DataStream full{};
    full << snapshot;
    const std::string data = full.str();
    for (size_t truncateAt = 1; truncateAt < data.size(); ++truncateAt) {
        DataStream truncated{std::vector<unsigned char>(data.begin(), data.begin() + truncateAt)};
        CQuorumSnapshot deserialized;
        BOOST_CHECK_THROW(truncated >> deserialized, std::ios_base::failure);
    }
}

BOOST_AUTO_TEST_SUITE_END()
