// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/evodb.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Payload = std::vector<uint8_t>;

uint256 BlockHash(uint32_t height)
{
    return uint256S(strprintf("%064x", height));
}

auto PayloadKey(uint32_t height)
{
    return std::make_pair(std::string{"test_evo_payload"}, BlockHash(height));
}

Payload PayloadFor(uint32_t height)
{
    return {static_cast<uint8_t>(height), static_cast<uint8_t>(height >> 8)};
}

void WritePayload(CEvoDB& db, EvoDbIdentity identity, uint32_t height)
{
    auto tx = db.BeginTransaction(identity);
    db.Write(PayloadKey(height), PayloadFor(height));
    tx->Commit();
}

void WriteMarker(CEvoDB& db, EvoDbIdentity identity, const uint256& hash)
{
    auto tx = db.BeginTransaction(identity);
    db.WriteBestBlock(identity, hash);
    tx->Commit();
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(evo_db_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(own_overlay_tombstone)
{
    CEvoDB db{util::DbWrapperParams{.path = m_args.GetDataDirBase() / "evodb_tombstone", .memory = true, .wipe = true}};
    const auto key = PayloadKey(1);

    WritePayload(db, EvoDbIdentity::NORMAL, 1);
    BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::NORMAL));

    {
        auto tx = db.BeginTransaction(EvoDbIdentity::NORMAL);
        db.Erase(key);
        tx->Commit();
    }
    {
        auto tx = db.BeginTransaction(EvoDbIdentity::NORMAL);
        Payload value;
        BOOST_CHECK(!db.Read(key, value));
    }
    {
        auto tx = db.BeginTransaction(EvoDbIdentity::SNAPSHOT);
        Payload value;
        BOOST_REQUIRE(db.Read(key, value));
        BOOST_CHECK(value == PayloadFor(1));
    }
}

BOOST_AUTO_TEST_CASE(transaction_less_access_uses_default_identity)
{
    CEvoDB db{util::DbWrapperParams{.path = m_args.GetDataDirBase() / "evodb_default_identity", .memory = true, .wipe = true}};
    const auto key = PayloadKey(7);

    // Committed to the SNAPSHOT overlay but not yet flushed to disk.
    WritePayload(db, EvoDbIdentity::SNAPSHOT, 7);

    // Transaction-less reads default to NORMAL and must not see it.
    Payload value;
    BOOST_CHECK(!db.Read(key, value));
    BOOST_CHECK(!db.Exists(key));

    // Once the snapshot chainstate is active, transaction-less consumers
    // resolve against the SNAPSHOT overlay.
    db.SetDefaultIdentity(EvoDbIdentity::SNAPSHOT);
    BOOST_REQUIRE(db.Read(key, value));
    BOOST_CHECK(value == PayloadFor(7));
    BOOST_CHECK(db.Exists(key));

    // Transaction-less writes land in the default identity's overlay and stay
    // invisible to the other identity.
    const auto key2 = PayloadKey(8);
    db.Write(key2, PayloadFor(8));
    db.SetDefaultIdentity(EvoDbIdentity::NORMAL);
    Payload value2;
    BOOST_CHECK(!db.Read(key2, value2));
    {
        auto tx = db.BeginTransaction(EvoDbIdentity::SNAPSHOT);
        BOOST_REQUIRE(db.Read(key2, value2));
        BOOST_CHECK(value2 == PayloadFor(8));
    }
}

BOOST_AUTO_TEST_CASE(write_derived_verifies_other_unflushed_overlay)
{
    const fs::path path = m_args.GetDataDirBase() / "evodb_derived_overlay";
    const auto key = PayloadKey(2);
    const auto payload = PayloadFor(2);
    Payload mismatch = payload;
    mismatch.push_back(0xff);

    {
        CEvoDB db{util::DbWrapperParams{.path = path, .memory = false, .wipe = true}};
        {
            auto tx = db.BeginTransaction(EvoDbIdentity::NORMAL);
            BOOST_REQUIRE(db.WriteDerived(key, payload));
            tx->Commit();
        }
        {
            auto tx = db.BeginTransaction(EvoDbIdentity::SNAPSHOT);
            BOOST_CHECK(!db.WriteDerived(key, mismatch));
            BOOST_REQUIRE(db.WriteDerived(key, payload));
            tx->Commit();
        }
        BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::SNAPSHOT));
        // Destroying db drops NORMAL's unflushed context. The reopened value
        // therefore proves SNAPSHOT's identical overlap did not suppress its write.
    }

    CEvoDB reloaded{util::DbWrapperParams{.path = path, .memory = false, .wipe = false}};
    Payload value;
    BOOST_REQUIRE(reloaded.Read(key, value));
    BOOST_CHECK(value == payload);
}

BOOST_AUTO_TEST_CASE(write_derived_rejects_disk_mismatch)
{
    CEvoDB db{util::DbWrapperParams{.path = m_args.GetDataDirBase() / "evodb_derived_mismatch", .memory = true, .wipe = true}};
    const auto key = PayloadKey(3);

    WritePayload(db, EvoDbIdentity::NORMAL, 3);
    BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::NORMAL));

    auto tx = db.BeginTransaction(EvoDbIdentity::SNAPSHOT);
    Payload mismatch = PayloadFor(3);
    mismatch.push_back(0xff);
    BOOST_CHECK(!db.WriteDerived(key, mismatch));
}

BOOST_AUTO_TEST_CASE(marker_flush_independence)
{
    const fs::path path = m_args.GetDataDirBase() / "evodb_markers";
    {
        CEvoDB db{util::DbWrapperParams{.path = path, .memory = false, .wipe = true}};
        WriteMarker(db, EvoDbIdentity::NORMAL, BlockHash(10));
        WriteMarker(db, EvoDbIdentity::SNAPSHOT, BlockHash(100));
        BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::NORMAL));
        BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::SNAPSHOT));

        WriteMarker(db, EvoDbIdentity::NORMAL, BlockHash(11));
        WriteMarker(db, EvoDbIdentity::SNAPSHOT, BlockHash(101));
        BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::NORMAL));
    }
    {
        CEvoDB db{util::DbWrapperParams{.path = path, .memory = false, .wipe = false}};
        BOOST_CHECK(db.VerifyBestBlock(EvoDbIdentity::NORMAL, BlockHash(11)));
        BOOST_CHECK(db.VerifyBestBlock(EvoDbIdentity::SNAPSHOT, BlockHash(100)));

        WriteMarker(db, EvoDbIdentity::NORMAL, BlockHash(12));
        WriteMarker(db, EvoDbIdentity::SNAPSHOT, BlockHash(102));
        BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::SNAPSHOT));
    }
    {
        CEvoDB db{util::DbWrapperParams{.path = path, .memory = false, .wipe = false}};
        BOOST_CHECK(db.VerifyBestBlock(EvoDbIdentity::NORMAL, BlockHash(11)));
        BOOST_CHECK(db.VerifyBestBlock(EvoDbIdentity::SNAPSHOT, BlockHash(102)));
    }
}

BOOST_AUTO_TEST_CASE(normal_marker_preserves_legacy_key_bytes)
{
    CEvoDB db{util::DbWrapperParams{.path = m_args.GetDataDirBase() / "evodb_legacy_key", .memory = true, .wipe = true}};
    WriteMarker(db, EvoDbIdentity::NORMAL, BlockHash(20));
    BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::NORMAL));

    CDataStream expected{SER_DISK, CLIENT_VERSION};
    expected << EVODB_BEST_BLOCK;
    std::unique_ptr<CDBIterator> it{db.GetRawDB().NewIterator()};
    it->SeekToFirst();
    BOOST_REQUIRE(it->Valid());
    const CDataStream actual = it->GetKey();
    BOOST_CHECK_EQUAL_COLLECTIONS(actual.begin(), actual.end(), expected.begin(), expected.end());
    it->Next();
    BOOST_CHECK(!it->Valid());
}

BOOST_AUTO_TEST_CASE(open_transaction_does_not_capture_other_threads)
{
    CEvoDB db{util::DbWrapperParams{.path = m_args.GetDataDirBase() / "evodb_tx_thread", .memory = true, .wipe = true}};
    db.SetDefaultIdentity(EvoDbIdentity::SNAPSHOT);
    WritePayload(db, EvoDbIdentity::SNAPSHOT, 30);

    // A transaction is one validation execution context, not a process-wide
    // mode: while the background chainstate holds a NORMAL transaction open,
    // concurrent transaction-less readers must still see the active snapshot's
    // overlay.
    auto tx = db.BeginTransaction(EvoDbIdentity::NORMAL);
    Payload same_thread;
    BOOST_CHECK(!db.Read(PayloadKey(30), same_thread));

    Payload other_thread;
    bool other_thread_read{false};
    std::thread reader{[&] { other_thread_read = db.Read(PayloadKey(30), other_thread); }};
    reader.join();
    tx->Commit();

    BOOST_REQUIRE(other_thread_read);
    BOOST_CHECK(other_thread == PayloadFor(30));
}

BOOST_AUTO_TEST_CASE(snapshot_markers_can_be_discarded)
{
    const fs::path path = m_args.GetDataDirBase() / "evodb_marker_rollback";
    {
        CEvoDB db{util::DbWrapperParams{.path = path, .memory = false, .wipe = true}};
        WriteMarker(db, EvoDbIdentity::SNAPSHOT, BlockHash(40));
        {
            auto tx = db.BeginTransaction(EvoDbIdentity::SNAPSHOT);
            db.WriteSnapshotBaseMNListHash(BlockHash(4));
            db.WriteBackgroundMNListHash(BlockHash(40), BlockHash(4));
            db.WriteDualChainstateMarker();
            tx->Commit();
        }
        BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::SNAPSHOT));
        BOOST_REQUIRE(db.HasDualChainstateMarker());

        // Abandoning snapshot activation after the markers were committed must
        // leave no trace: a stale dual-chainstate marker breaks legacy
        // bootstrapping and a stale best-block marker would revive a future
        // snapshot directory.
        {
            auto tx = db.BeginTransaction(EvoDbIdentity::SNAPSHOT);
            db.EraseSnapshotMarkers();
            tx->Commit();
        }
        BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::SNAPSHOT));
    }
    CEvoDB reopened{util::DbWrapperParams{.path = path, .memory = false, .wipe = false}};
    uint256 hash;
    uint256 hash2;
    BOOST_CHECK(!reopened.ReadBestBlock(EvoDbIdentity::SNAPSHOT, hash));
    BOOST_CHECK(!reopened.ReadSnapshotBaseMNListHash(hash));
    BOOST_CHECK(!reopened.ReadBackgroundMNListHash(hash, hash2));
    BOOST_CHECK(!reopened.HasDualChainstateMarker());
}

BOOST_AUTO_TEST_CASE(snapshot_marker_promotion_and_discard)
{
    CEvoDB db{util::DbWrapperParams{.path = m_args.GetDataDirBase() / "evodb_promotion", .memory = true, .wipe = true}};
    const uint256 normal_tip = BlockHash(30);
    const uint256 snapshot_tip = BlockHash(300);
    const uint256 mn_list_hash = BlockHash(3);
    const std::vector<uint256> required_work{BlockHash(10), BlockHash(20)};

    WriteMarker(db, EvoDbIdentity::NORMAL, normal_tip);
    {
        auto tx = db.BeginTransaction(EvoDbIdentity::SNAPSHOT);
        db.WriteBestBlock(EvoDbIdentity::SNAPSHOT, snapshot_tip);
        db.WriteSnapshotBaseMNListHash(mn_list_hash);
        db.WriteRequiredWorkMNListHashes(required_work);
        for (const auto& hash : required_work) db.WriteBackgroundWorkMNListHash(hash, BlockHash(40));
        db.WriteDualChainstateMarker();
        tx->Commit();
    }
    BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::NORMAL));
    BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::SNAPSHOT));

    BOOST_REQUIRE(db.PromoteSnapshotMarkers(snapshot_tip));
    // Promotion is idempotent across a restart after the synced batch lands.
    BOOST_REQUIRE(db.PromoteSnapshotMarkers(snapshot_tip));
    BOOST_CHECK(db.VerifyBestBlock(EvoDbIdentity::NORMAL, snapshot_tip));
    uint256 value;
    BOOST_CHECK(!db.ReadBestBlock(EvoDbIdentity::SNAPSHOT, value));
    BOOST_CHECK(!db.ReadSnapshotBaseMNListHash(value));
    std::vector<uint256> required_value;
    BOOST_CHECK(!db.ReadRequiredWorkMNListHashes(required_value));
    for (const auto& hash : required_work) BOOST_CHECK(!db.ReadBackgroundWorkMNListHash(hash, value));
    BOOST_CHECK(!db.HasDualChainstateMarker());

    WriteMarker(db, EvoDbIdentity::SNAPSHOT, BlockHash(301));
    {
        auto tx = db.BeginTransaction(EvoDbIdentity::SNAPSHOT);
        db.WriteSnapshotBaseMNListHash(BlockHash(4));
        db.WriteRequiredWorkMNListHashes(required_work);
        for (const auto& hash : required_work) db.WriteBackgroundWorkMNListHash(hash, BlockHash(41));
        db.WriteDualChainstateMarker();
        tx->Commit();
    }
    BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::SNAPSHOT));
    BOOST_REQUIRE(db.DiscardSnapshotMarkers());
    // Discard is likewise safe to retry.
    BOOST_REQUIRE(db.DiscardSnapshotMarkers());
    BOOST_CHECK(db.VerifyBestBlock(EvoDbIdentity::NORMAL, snapshot_tip));
    BOOST_CHECK(!db.ReadBestBlock(EvoDbIdentity::SNAPSHOT, value));
    BOOST_CHECK(!db.ReadSnapshotBaseMNListHash(value));
    BOOST_CHECK(!db.ReadRequiredWorkMNListHashes(required_value));
    for (const auto& hash : required_work) BOOST_CHECK(!db.ReadBackgroundWorkMNListHash(hash, value));
    BOOST_CHECK(!db.HasDualChainstateMarker());
}

BOOST_AUTO_TEST_SUITE_END()
