// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <evo/evodb.h>
#include <evo/snapshot_types.h>
#include <evo/viewcache.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
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

void WritePayload(CEvoDB& db, EvoDbIdentity identity, uint32_t height) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    auto tx = db.BeginTransaction(identity);
    db.Write(PayloadKey(height), PayloadFor(height));
    tx->Commit();
}

void WriteMarker(CEvoDB& db, EvoDbIdentity identity, const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    auto tx = db.BeginTransaction(identity);
    db.WriteBestBlock(identity, hash);
    tx->Commit();
}

const uint256 CONFIG_FINGERPRINT{uint256::ONE};

//! Bind the SNAPSHOT identity to a new published generation.
void PublishGeneration(CEvoDB& db, uint32_t base_height = 0)
{
    BOOST_REQUIRE(db.CreateSnapshotGeneration(BlockHash(base_height), base_height));
    BOOST_REQUIRE(db.PublishSnapshotGeneration(CONFIG_FINGERPRINT, std::nullopt));
}

//! Raw keys of the whole database, in order.
std::vector<std::vector<std::byte>> RawKeys(CEvoDB& db)
{
    std::vector<std::vector<std::byte>> keys;
    std::unique_ptr<CDBIterator> it{db.GetRawDB().NewIterator()};
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        const DataStream key{it->GetKey()};
        keys.emplace_back(key.begin(), key.end());
    }
    return keys;
}

//! Heights of the test payloads one view's iterator yields.
std::vector<uint32_t> IteratePayloads(CEvoDB& db, EvoDbIdentity identity) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    std::vector<uint32_t> heights;
    auto tx = db.BeginTransaction(identity);
    LOCK(db.cs);
    auto it{db.GetCurTransaction().NewIteratorUniquePtr()};
    for (it->Seek(PayloadKey(0)); it->Valid(); it->Next()) {
        std::pair<std::string, uint256> key;
        if (!it->GetKey(key) || key.first != PayloadKey(0).first) break;
        heights.push_back(static_cast<uint32_t>(UintToArith256(key.second).GetLow64()));
    }
    return heights;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(evo_db_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(own_overlay_tombstone)
{
    LOCK(::cs_main);
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
}

BOOST_AUTO_TEST_CASE(transaction_less_access_uses_default_identity)
{
    LOCK(::cs_main);
    CEvoDB db{util::DbWrapperParams{.path = m_args.GetDataDirBase() / "evodb_default_identity", .memory = true, .wipe = true}};
    const auto key = PayloadKey(7);
    PublishGeneration(db);

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

BOOST_AUTO_TEST_CASE(write_derived_compares_within_its_own_view)
{
    LOCK(::cs_main);
    CEvoDB db{util::DbWrapperParams{.path = m_args.GetDataDirBase() / "evodb_derived_view", .memory = true, .wipe = true}};
    PublishGeneration(db);
    const auto key = PayloadKey(2);
    const auto payload = PayloadFor(2);
    Payload other = payload;
    other.push_back(0xff);

    {
        auto tx = db.BeginTransaction(EvoDbIdentity::NORMAL);
        BOOST_REQUIRE(db.WriteDerived(key, payload));
        // An identical rewrite is accepted, a differing one refused.
        BOOST_CHECK(db.WriteDerived(key, payload));
        BOOST_CHECK(!db.WriteDerived(key, other));
        tx->Commit();
    }
    BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::NORMAL));
    {
        // The NORMAL record is invisible to the snapshot view: it neither
        // conflicts with nor supplies the snapshot's own record.
        auto tx = db.BeginTransaction(EvoDbIdentity::SNAPSHOT);
        BOOST_REQUIRE(db.WriteDerived(key, other));
        BOOST_CHECK(!db.WriteDerived(key, payload));
        tx->Commit();
    }
    BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::SNAPSHOT));
    for (const auto& [identity, expected] :
         {std::make_pair(EvoDbIdentity::NORMAL, payload), std::make_pair(EvoDbIdentity::SNAPSHOT, other)}) {
        auto tx = db.BeginTransaction(identity);
        Payload value;
        BOOST_REQUIRE(db.Read(key, value));
        BOOST_CHECK(value == expected);
    }
}

BOOST_AUTO_TEST_CASE(write_derived_rejects_disk_mismatch)
{
    LOCK(::cs_main);
    CEvoDB db{util::DbWrapperParams{.path = m_args.GetDataDirBase() / "evodb_derived_mismatch", .memory = true, .wipe = true}};
    PublishGeneration(db);
    const auto key = PayloadKey(3);

    WritePayload(db, EvoDbIdentity::SNAPSHOT, 3);
    BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::SNAPSHOT));

    auto tx = db.BeginTransaction(EvoDbIdentity::SNAPSHOT);
    Payload mismatch = PayloadFor(3);
    mismatch.push_back(0xff);
    BOOST_CHECK(!db.WriteDerived(key, mismatch));
}

BOOST_AUTO_TEST_CASE(generation_namespace_isolation)
{
    LOCK(::cs_main);
    const fs::path path = m_args.GetDataDirBase() / "evodb_isolation";
    const auto raw_keys_before_snapshot = [&]() EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        CEvoDB db{util::DbWrapperParams{.path = path, .memory = false, .wipe = true}};
        WritePayload(db, EvoDbIdentity::NORMAL, 1);
        WritePayload(db, EvoDbIdentity::NORMAL, 3);
        BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::NORMAL));
        return RawKeys(db);
    }();

    {
        CEvoDB db{util::DbWrapperParams{.path = path, .memory = false, .wipe = false}};
        BOOST_REQUIRE(db.LoadSnapshotGenerations(CONFIG_FINGERPRINT));
        PublishGeneration(db);
        WritePayload(db, EvoDbIdentity::SNAPSHOT, 2);
        WritePayload(db, EvoDbIdentity::SNAPSHOT, 3);
        BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::SNAPSHOT));
    }

    CEvoDB db{util::DbWrapperParams{.path = path, .memory = false, .wipe = false}};
    BOOST_REQUIRE(db.LoadSnapshotGenerations(CONFIG_FINGERPRINT));
    // Neither view reads the other's records, including a key both wrote.
    Payload value;
    {
        auto tx = db.BeginTransaction(EvoDbIdentity::NORMAL);
        BOOST_CHECK(db.Read(PayloadKey(1), value));
        BOOST_CHECK(!db.Read(PayloadKey(2), value));
    }
    {
        auto tx = db.BeginTransaction(EvoDbIdentity::SNAPSHOT);
        BOOST_CHECK(!db.Read(PayloadKey(1), value));
        BOOST_CHECK(db.Read(PayloadKey(2), value));
    }
    // Iterators stay inside their own keyspace.
    BOOST_CHECK(IteratePayloads(db, EvoDbIdentity::NORMAL) == (std::vector<uint32_t>{1, 3}));
    BOOST_CHECK(IteratePayloads(db, EvoDbIdentity::SNAPSHOT) == (std::vector<uint32_t>{2, 3}));

    // Rejection keeps the namespace until the coins directory is invalidated;
    // collecting it is idempotent.
    BOOST_REQUIRE(db.DiscardSnapshotMarkers());
    BOOST_CHECK(!db.GetSnapshotGeneration());
    BOOST_REQUIRE(db.LoadSnapshotGenerations(CONFIG_FINGERPRINT));
    BOOST_CHECK(db.HasRejectedGeneration());
    BOOST_REQUIRE(db.CollectRejectedGenerations());
    BOOST_REQUIRE(db.CollectRejectedGenerations());
    BOOST_CHECK(!db.HasRejectedGeneration());
    auto raw_keys{RawKeys(db)};
    // Only the generation counter remains of the snapshot.
    DataStream counter_key{};
    counter_key << EVODB_GENERATION_NEXT;
    std::erase(raw_keys, std::vector<std::byte>(counter_key.begin(), counter_key.end()));
    BOOST_CHECK(raw_keys == raw_keys_before_snapshot);
}

BOOST_AUTO_TEST_CASE(marker_flush_independence)
{
    LOCK(::cs_main);
    const fs::path path = m_args.GetDataDirBase() / "evodb_markers";
    {
        CEvoDB db{util::DbWrapperParams{.path = path, .memory = false, .wipe = true}};
        PublishGeneration(db);
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
        BOOST_REQUIRE(db.LoadSnapshotGenerations(CONFIG_FINGERPRINT));
        BOOST_CHECK(db.VerifyBestBlock(EvoDbIdentity::NORMAL, BlockHash(11)));
        BOOST_CHECK(db.VerifyBestBlock(EvoDbIdentity::SNAPSHOT, BlockHash(100)));

        WriteMarker(db, EvoDbIdentity::NORMAL, BlockHash(12));
        WriteMarker(db, EvoDbIdentity::SNAPSHOT, BlockHash(102));
        BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::SNAPSHOT));
    }
    {
        CEvoDB db{util::DbWrapperParams{.path = path, .memory = false, .wipe = false}};
        BOOST_REQUIRE(db.LoadSnapshotGenerations(CONFIG_FINGERPRINT));
        BOOST_CHECK(db.VerifyBestBlock(EvoDbIdentity::NORMAL, BlockHash(11)));
        BOOST_CHECK(db.VerifyBestBlock(EvoDbIdentity::SNAPSHOT, BlockHash(102)));
    }
}

BOOST_AUTO_TEST_CASE(normal_marker_preserves_legacy_key_bytes)
{
    LOCK(::cs_main);
    CEvoDB db{util::DbWrapperParams{.path = m_args.GetDataDirBase() / "evodb_legacy_key", .memory = true, .wipe = true}};
    WriteMarker(db, EvoDbIdentity::NORMAL, BlockHash(20));
    BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::NORMAL));

    DataStream expected{};
    expected << EVODB_BEST_BLOCK;
    std::unique_ptr<CDBIterator> it{db.GetRawDB().NewIterator()};
    it->SeekToFirst();
    BOOST_REQUIRE(it->Valid());
    const DataStream actual = it->GetKey();
    BOOST_CHECK_EQUAL_COLLECTIONS(actual.begin(), actual.end(), expected.begin(), expected.end());
    it->Next();
    BOOST_CHECK(!it->Valid());
}

BOOST_AUTO_TEST_CASE(open_transaction_does_not_capture_other_threads)
{
    LOCK(::cs_main);
    CEvoDB db{util::DbWrapperParams{.path = m_args.GetDataDirBase() / "evodb_tx_thread", .memory = true, .wipe = true}};
    PublishGeneration(db);
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

BOOST_AUTO_TEST_CASE(captured_tasks_release_their_claims_when_dropped)
{
    LOCK(::cs_main);
    CEvoDB db{util::DbWrapperParams{.path = m_args.GetDataDirBase() / "evodb_captured", .memory = true, .wipe = true}};
    PublishGeneration(db);
    EvoViewToken retired{db.ViewFor(EvoDbIdentity::SNAPSHOT)};
    ++retired.generation;

    // A task bound to a retired generation never runs; what its enqueuer
    // claimed for it is released instead.
    for (const auto& [view, live] :
         {std::make_pair(db.ViewFor(EvoDbIdentity::SNAPSHOT), true), std::make_pair(retired, false)}) {
        int ran{0};
        int released{0};
        auto task{[&] {
            EvoViewScope enqueuer{view};
            return WithCapturedEvoView([&] { ++ran; }, [&] { ++released; });
        }()};
        task();
        BOOST_CHECK_EQUAL(ran, live ? 1 : 0);
        BOOST_CHECK_EQUAL(released, live ? 0 : 1);
    }

    // So is one whose binding cannot be installed where it runs: here, a
    // thread holding a NORMAL transaction.
    {
        int ran{0};
        int released{0};
        auto task{[&] {
            EvoViewScope enqueuer{db.ViewFor(EvoDbIdentity::SNAPSHOT)};
            return WithCapturedEvoView([&] { ++ran; }, [&] { ++released; });
        }()};
        {
            const auto normal_tx{db.BeginTransaction(EvoDbIdentity::NORMAL)};
            task();
        }
        BOOST_CHECK_EQUAL(ran, 0);
        BOOST_CHECK_EQUAL(released, 1);
    }

    // A task that started and failed releases its claims itself, as the quorum
    // recovery workers do, so they are released exactly once.
    {
        int released{0};
        auto task{[&] {
            EvoViewScope enqueuer{db.ViewFor(EvoDbIdentity::SNAPSHOT)};
            return WithCapturedEvoView(
                [&] {
                    try {
                        throw BlockDataUnavailableError("unavailable");
                    } catch (const evo::LocalStateError&) {
                        ++released;
                        throw;
                    }
                },
                [&] { ++released; });
        }()};
        task();
        BOOST_CHECK_EQUAL(released, 1);
    }
}

BOOST_AUTO_TEST_CASE(view_scopes_nest_and_bind_transactions)
{
    LOCK(::cs_main);
    CEvoDB db{util::DbWrapperParams{.path = m_args.GetDataDirBase() / "evodb_view_scope", .memory = true, .wipe = true}};
    PublishGeneration(db);
    const EvoViewToken normal{db.ViewFor(EvoDbIdentity::NORMAL)};
    const EvoViewToken snapshot{db.ViewFor(EvoDbIdentity::SNAPSHOT)};
    WritePayload(db, EvoDbIdentity::SNAPSHOT, 50);

    // Unbound access keeps resolving to the default identity.
    BOOST_CHECK(db.CurrentView() == normal);
    Payload value;
    {
        EvoViewScope outer{snapshot};
        BOOST_CHECK(db.CurrentView() == snapshot);
        BOOST_CHECK(db.Read(PayloadKey(50), value));
        {
            EvoViewScope inner{normal};
            BOOST_CHECK(db.CurrentView() == normal);
            BOOST_CHECK(!db.Read(PayloadKey(50), value));
            // A transaction matching the innermost scope is accepted and
            // names the same view.
            auto tx = db.BeginTransaction(EvoDbIdentity::NORMAL);
            BOOST_CHECK(db.CurrentView() == normal);
            tx->Commit();
        }
        // Leaving the inner scope restores the outer binding.
        BOOST_CHECK(db.CurrentView() == snapshot);
        BOOST_CHECK(db.Read(PayloadKey(50), value));

        // A transaction for the other identity conflicts with the scope.
        BOOST_CHECK_THROW(db.BeginTransaction(EvoDbIdentity::NORMAL), EvoViewBindingError);
        BOOST_CHECK(!db.HasActiveTransaction());
    }
    BOOST_CHECK(db.CurrentView() == normal);

    // A scope opened inside a conflicting transaction is refused, and the
    // binding in force is left untouched.
    auto tx = db.BeginTransaction(EvoDbIdentity::SNAPSHOT);
    BOOST_CHECK_THROW(EvoViewScope{normal}, EvoViewBindingError);
    BOOST_CHECK(db.CurrentView() == snapshot);
    {
        EvoViewScope agreeing{snapshot};
        BOOST_CHECK(db.CurrentView() == snapshot);
    }
    tx->Commit();

    // A scope binds only the CEvoDB its token names.
    CEvoDB other{
        util::DbWrapperParams{.path = m_args.GetDataDirBase() / "evodb_view_scope_other", .memory = true, .wipe = true}};
    PublishGeneration(other);
    EvoViewScope other_scope{other.ViewFor(EvoDbIdentity::SNAPSHOT)};
    BOOST_CHECK(db.CurrentView() == normal);
    BOOST_CHECK(other.CurrentView() == other.ViewFor(EvoDbIdentity::SNAPSHOT));

    // A cache of state derived from one CEvoDB serves only that CEvoDB's live
    // views: neither another CEvoDB's view nor a retained token of a retired
    // generation gets a partition.
    EvoViewCache<int> cache;
    cache.SetOwner(db);
    cache.For(normal) = 1;
    cache.For(snapshot) = 2;
    BOOST_CHECK_THROW(cache.For(other.ViewFor(EvoDbIdentity::NORMAL)), EvoViewBindingError);
    BOOST_REQUIRE(db.AbandonSnapshotGeneration());
    BOOST_CHECK_THROW(cache.For(snapshot), EvoViewBindingError);
    // With no generation bound the snapshot identity names no view at all.
    BOOST_CHECK_THROW(cache.For(db.ViewFor(EvoDbIdentity::SNAPSHOT)), EvoViewBindingError);
    BOOST_CHECK_EQUAL(cache.For(normal), 0);
}

BOOST_AUTO_TEST_CASE(view_cache_lookup_excludes_retirement)
{
    CEvoDB db{
        util::DbWrapperParams{.path = m_args.GetDataDirBase() / "evodb_cache_retirement", .memory = true, .wipe = true}};
    PublishGeneration(db);
    const EvoViewToken snapshot{db.ViewFor(EvoDbIdentity::SNAPSHOT)};

    // A generation cannot retire between a cache's liveness check and its
    // partition lookup: a retirement started meanwhile waits for the lookup,
    // and the view is refused once it completes.
    std::atomic<bool> discarded{false};
    std::thread discard;
    EvoViewCache<int> cache{[&] {
        discard = std::thread{[&] { discarded = db.DiscardSnapshotMarkers(); }};
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        BOOST_CHECK(!discarded);
        return 1;
    }};
    cache.SetOwner(db);
    BOOST_CHECK_EQUAL(cache.For(snapshot), 1);
    discard.join();
    BOOST_CHECK(discarded);
    BOOST_CHECK_THROW(cache.For(snapshot), EvoViewBindingError);
}

BOOST_AUTO_TEST_CASE(snapshot_markers_can_be_discarded)
{
    LOCK(::cs_main);
    const fs::path path = m_args.GetDataDirBase() / "evodb_marker_rollback";
    {
        CEvoDB db{util::DbWrapperParams{.path = path, .memory = false, .wipe = true}};
        BOOST_REQUIRE(db.CreateSnapshotGeneration(BlockHash(40), 40));
        WriteMarker(db, EvoDbIdentity::SNAPSHOT, BlockHash(40));
        BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::SNAPSHOT));
        BOOST_REQUIRE(db.PublishSnapshotGeneration(CONFIG_FINGERPRINT, BlockHash(4)));
        {
            auto tx = db.BeginTransaction(EvoDbIdentity::NORMAL);
            db.WriteBackgroundMNListHash(BlockHash(40), BlockHash(4));
            tx->Commit();
        }
        BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::NORMAL));
        BOOST_REQUIRE(db.HasDualChainstateMarker());
        // Unflushed snapshot writes are dropped with the generation.
        WritePayload(db, EvoDbIdentity::SNAPSHOT, 41);

        // Abandoning snapshot activation after the generation was published
        // must leave no trace: a stale dual-chainstate marker breaks legacy
        // bootstrapping and a stale best-block marker would revive a future
        // snapshot directory.
        BOOST_REQUIRE(db.AbandonSnapshotGeneration());
    }
    CEvoDB reopened{util::DbWrapperParams{.path = path, .memory = false, .wipe = false}};
    BOOST_REQUIRE(reopened.LoadSnapshotGenerations(CONFIG_FINGERPRINT));
    uint256 hash;
    uint256 hash2;
    BOOST_CHECK(!reopened.GetSnapshotGeneration());
    BOOST_CHECK(!reopened.ReadBestBlock(EvoDbIdentity::SNAPSHOT, hash));
    BOOST_CHECK(!reopened.ReadSnapshotBaseMNListHash(hash));
    BOOST_CHECK(!reopened.ReadBackgroundMNListHash(hash, hash2));
    BOOST_CHECK(!reopened.HasDualChainstateMarker());
}

BOOST_AUTO_TEST_CASE(snapshot_marker_promotion_and_discard)
{
    LOCK(::cs_main);
    CEvoDB db{util::DbWrapperParams{.path = m_args.GetDataDirBase() / "evodb_promotion", .memory = true, .wipe = true}};
    const uint256 normal_tip = BlockHash(30);
    const uint256 snapshot_tip = BlockHash(300);
    const uint256 mn_list_hash = BlockHash(3);

    WriteMarker(db, EvoDbIdentity::NORMAL, normal_tip);
    WritePayload(db, EvoDbIdentity::NORMAL, 30);
    {
        auto tx = db.BeginTransaction(EvoDbIdentity::NORMAL);
        db.Write(EVODB_LLMQ_BEST_BLOCK, normal_tip);
        tx->Commit();
    }
    BOOST_REQUIRE(db.CreateSnapshotGeneration(normal_tip, 30));
    {
        auto tx = db.BeginTransaction(EvoDbIdentity::SNAPSHOT);
        db.WriteBestBlock(EvoDbIdentity::SNAPSHOT, snapshot_tip);
        // A record both views derived identically, one only the snapshot
        // chainstate wrote, and the mutable singleton both views advance.
        db.Write(PayloadKey(30), PayloadFor(30));
        db.Write(PayloadKey(300), PayloadFor(300));
        db.Write(EVODB_LLMQ_BEST_BLOCK, snapshot_tip);
        tx->Commit();
    }
    BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::NORMAL));
    BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::SNAPSHOT));
    BOOST_REQUIRE(db.PublishSnapshotGeneration(CONFIG_FINGERPRINT, mn_list_hash));

    // Promotion needs the journal written before the coins renames, at the tip
    // the generation's Evo state was flushed at.
    BOOST_CHECK(!db.PromoteSnapshotMarkers(snapshot_tip));
    BOOST_CHECK(!db.PrepareSnapshotPromotion(BlockHash(299)));
    BOOST_REQUIRE(db.PrepareSnapshotPromotion(snapshot_tip));
    BOOST_REQUIRE(db.PromoteSnapshotMarkers(snapshot_tip));
    // Promotion is idempotent across a restart after the synced batch lands.
    BOOST_REQUIRE(db.PromoteSnapshotMarkers(snapshot_tip));
    BOOST_CHECK(db.VerifyBestBlock(EvoDbIdentity::NORMAL, snapshot_tip));
    uint256 value;
    BOOST_CHECK(!db.ReadBestBlock(EvoDbIdentity::SNAPSHOT, value));
    BOOST_CHECK(!db.ReadSnapshotBaseMNListHash(value));
    BOOST_CHECK(!db.HasDualChainstateMarker());
    BOOST_CHECK(!db.ReadPromotionJournal());
    // Snapshot-written records are now NORMAL state; the singleton takes the
    // promoted value.
    BOOST_CHECK(IteratePayloads(db, EvoDbIdentity::NORMAL) == (std::vector<uint32_t>{30, 300}));
    uint256 llmq_best_block;
    BOOST_REQUIRE(db.Read(EVODB_LLMQ_BEST_BLOCK, llmq_best_block));
    BOOST_CHECK_EQUAL(llmq_best_block, snapshot_tip);

    BOOST_REQUIRE(db.CreateSnapshotGeneration(snapshot_tip, 300));
    WriteMarker(db, EvoDbIdentity::SNAPSHOT, BlockHash(301));
    WritePayload(db, EvoDbIdentity::SNAPSHOT, 301);
    BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::SNAPSHOT));
    BOOST_REQUIRE(db.PublishSnapshotGeneration(CONFIG_FINGERPRINT, BlockHash(4)));
    BOOST_REQUIRE(db.DiscardSnapshotMarkers());
    // Discard is likewise safe to retry.
    BOOST_REQUIRE(db.DiscardSnapshotMarkers());
    BOOST_CHECK(db.VerifyBestBlock(EvoDbIdentity::NORMAL, snapshot_tip));
    BOOST_CHECK(!db.ReadBestBlock(EvoDbIdentity::SNAPSHOT, value));
    BOOST_CHECK(!db.ReadSnapshotBaseMNListHash(value));
    BOOST_CHECK(!db.HasDualChainstateMarker());
    BOOST_CHECK(IteratePayloads(db, EvoDbIdentity::NORMAL) == (std::vector<uint32_t>{30, 300}));
}

BOOST_AUTO_TEST_CASE(promotion_refuses_conflicting_records)
{
    LOCK(::cs_main);
    CEvoDB db{
        util::DbWrapperParams{.path = m_args.GetDataDirBase() / "evodb_promotion_conflict", .memory = true, .wipe = true}};
    WritePayload(db, EvoDbIdentity::NORMAL, 5);
    BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::NORMAL));
    BOOST_REQUIRE(db.CreateSnapshotGeneration(BlockHash(5), 5));
    {
        auto tx = db.BeginTransaction(EvoDbIdentity::SNAPSHOT);
        db.WriteBestBlock(EvoDbIdentity::SNAPSHOT, BlockHash(6));
        db.Write(PayloadKey(5), PayloadFor(6));
        tx->Commit();
    }
    BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::SNAPSHOT));
    BOOST_REQUIRE(db.PublishSnapshotGeneration(CONFIG_FINGERPRINT, std::nullopt));
    BOOST_REQUIRE(db.PrepareSnapshotPromotion(BlockHash(6)));
    BOOST_CHECK(!db.PromoteSnapshotMarkers(BlockHash(6)));
    // The conflict is in the first merge batch, so nothing was merged:
    // NORMAL keeps its own record and tip. (Batches merged before a later
    // conflict stay merged; startup then keeps failing closed.)
    Payload value;
    BOOST_REQUIRE(db.Read(PayloadKey(5), value));
    BOOST_CHECK(value == PayloadFor(5));
    BOOST_CHECK(db.HasDualChainstateMarker());
}

BOOST_AUTO_TEST_CASE(generation_ids_never_wrap)
{
    LOCK(::cs_main);
    CEvoDB db{util::DbWrapperParams{.path = m_args.GetDataDirBase() / "evodb_generation_ids", .memory = true, .wipe = true}};
    // Generation 0 is the NORMAL keyspace: the last id is never handed out,
    // so the counter can never wrap into it.
    for (const uint32_t next : {std::numeric_limits<uint32_t>::max(), uint32_t{0}}) {
        BOOST_REQUIRE(db.GetRawDB().Write(EVODB_GENERATION_NEXT, next));
        BOOST_CHECK(!db.CreateSnapshotGeneration(BlockHash(1), 1));
        BOOST_CHECK(!db.GetSnapshotGeneration());
    }
    BOOST_REQUIRE(db.GetRawDB().Write(EVODB_GENERATION_NEXT, std::numeric_limits<uint32_t>::max() - 1));
    BOOST_CHECK(db.CreateSnapshotGeneration(BlockHash(1), 1));
}

BOOST_AUTO_TEST_SUITE_END()
