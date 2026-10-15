// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <evo/deterministicmns.h>
#include <evo/evodb.h>
#include <evo/snapshot_types.h>
#include <evo/viewcache.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/fs.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
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

// Flips a byte of the first occurrence of `needle` in a table file of the closed LevelDB at
// `db_dir`, so that the table block holding it fails its checksum.
bool CorruptTableBlock(const fs::path& db_dir, const std::vector<uint8_t>& needle)
{
    for (const auto& entry : fs::directory_iterator(db_dir)) {
        if (entry.path().extension() != ".ldb") continue;
        std::vector<char> bytes;
        {
            std::ifstream in{entry.path(), std::ios::binary};
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        const auto pos{std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end(),
                                   [](char a, uint8_t b) { return static_cast<uint8_t>(a) == b; })};
        if (pos == bytes.end()) continue;
        *pos ^= 1;
        std::ofstream out{entry.path(), std::ios::binary | std::ios::trunc};
        out.write(bytes.data(), bytes.size());
        out.close();
        return !out.fail();
    }
    return false;
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

//! Raw keys and values of the whole database, in order.
std::vector<std::pair<std::string, std::string>> RawEntries(CEvoDB& db)
{
    std::vector<std::pair<std::string, std::string>> entries;
    std::unique_ptr<CDBIterator> it{db.GetRawDB().NewIterator()};
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        const DataStream key{it->GetKey()};
        CDataStream value{SER_DISK, CLIENT_VERSION};
        BOOST_REQUIRE(db.GetRawDB().ReadDataStream(key, value));
        entries.emplace_back(key.str(), value.str());
    }
    return entries;
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
    // Retiring a generation drops its partition and leaves NORMAL's.
    BOOST_CHECK_EQUAL(cache.For(normal), 1);
    size_t partitions{0};
    cache.ForEach([&](int) { ++partitions; });
    BOOST_CHECK_EQUAL(partitions, 1U);
}

BOOST_AUTO_TEST_CASE(turn_overtaken_by_publication_keeps_its_view)
{
    LOCK(::cs_main);
    CEvoDB db{util::DbWrapperParams{.path = m_args.GetDataDirBase() / "evodb_turn_publication", .memory = true, .wipe = true}};
    WritePayload(db, EvoDbIdentity::NORMAL, 1);
    BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::NORMAL));

    const CBlockIndex active_tip;
    const ActiveEvoViewScope turn{&db, [&]() EXCLUSIVE_LOCKS_REQUIRED(::cs_main) { return &active_tip; }};
    BOOST_CHECK_EQUAL(turn.Tip(), &active_tip);
    BOOST_CHECK(db.CurrentView() == db.ViewFor(EvoDbIdentity::NORMAL));

    // A snapshot is published and activated while the turn runs: the turn
    // keeps reading the view it started in.
    PublishGeneration(db);
    db.SetDefaultIdentity(EvoDbIdentity::SNAPSHOT);
    Payload value;
    BOOST_CHECK_NO_THROW(BOOST_CHECK(db.Read(PayloadKey(1), value)));
    BOOST_CHECK(value == PayloadFor(1));
    BOOST_CHECK(db.CurrentView() == db.ViewFor(EvoDbIdentity::NORMAL));
}

// With a single view a dispatch turn binds without waiting for cs_main; while
// a snapshot generation is live it binds under cs_main.
BOOST_AUTO_TEST_CASE(single_view_turn_does_not_wait_for_cs_main)
{
    CEvoDB db{util::DbWrapperParams{.path = m_args.GetDataDirBase() / "evodb_turn_cs_main", .memory = true, .wipe = true}};
    const auto bind_while_cs_main_held{[&](std::chrono::milliseconds wait) {
        std::promise<EvoViewToken> bound;
        auto bound_view{bound.get_future()};
        std::thread turn_thread;
        bool bound_while_held;
        {
            LOCK(::cs_main);
            turn_thread = std::thread{[&] {
                const ActiveEvoViewScope turn{&db};
                bound.set_value(db.CurrentView());
            }};
            bound_while_held = bound_view.wait_for(wait) == std::future_status::ready;
        }
        turn_thread.join();
        return std::make_pair(bound_while_held, bound_view.get());
    }};

    const auto [single_bound_while_held, single_view]{bind_while_cs_main_held(30s)};
    BOOST_CHECK(single_bound_while_held);
    BOOST_CHECK(single_view == db.ViewFor(EvoDbIdentity::NORMAL));

    PublishGeneration(db);
    db.SetDefaultIdentity(EvoDbIdentity::SNAPSHOT);
    const auto [dual_bound_while_held, dual_view]{bind_while_cs_main_held(200ms)};
    BOOST_CHECK(!dual_bound_while_held);
    BOOST_CHECK(dual_view == db.ViewFor(EvoDbIdentity::SNAPSHOT));
}

// A service round that keeps failing is logged once per view generation and
// reason, and its recovery once.
BOOST_AUTO_TEST_CASE(failing_service_rounds_log_once_per_cause)
{
    CEvoDB db{util::DbWrapperParams{.path = m_args.GetDataDirBase() / "evodb_round_logs", .memory = true, .wipe = true}};
    std::vector<std::string> logged;
    const auto callback{LogInstance().PushBackCallback([&](const std::string& line) {
        if (line.find("test-round") != std::string::npos) logged.push_back(line);
    })};
    const auto run{[&](std::optional<std::string> failure) {
        RunActiveEvoRound(&db, "test-round", [&] {
            if (failure) throw BlockDataUnavailableError(*failure);
        });
    }};
    SetMockTime(GetTime<std::chrono::seconds>());
    // Two tasks that share an operation name and fail in turn.
    for (int i{0}; i < 3; ++i) {
        run("list unavailable");
        run("quorum unavailable");
    }
    PublishGeneration(db);
    db.SetDefaultIdentity(EvoDbIdentity::SNAPSHOT);
    run("quorum unavailable");
    // A success between failures is no recovery yet: rounds succeed again
    // once they have stopped failing for a minute.
    run(std::nullopt);
    run("quorum unavailable");
    SetMockTime(GetMockTime() + 59s);
    run(std::nullopt);
    SetMockTime(GetMockTime() + 1s);
    run(std::nullopt);
    run(std::nullopt);
    run("quorum unavailable");
    LogInstance().DeleteCallback(callback);
    SetMockTime(0s);

    BOOST_REQUIRE_EQUAL(logged.size(), 5U);
    BOOST_CHECK(logged[0].find("Evo state unavailable: list unavailable") != std::string::npos);
    BOOST_CHECK(logged[1].find("Evo state unavailable: quorum unavailable") != std::string::npos);
    BOOST_CHECK(logged[2].find("Evo state unavailable: quorum unavailable") != std::string::npos);
    BOOST_CHECK(logged[3].find("Evo state available again") != std::string::npos);
    BOOST_CHECK(logged[4].find("Evo state unavailable: quorum unavailable") != std::string::npos);
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

BOOST_AUTO_TEST_CASE(retiring_a_generation_keeps_the_active_tip_list)
{
    CDeterministicMNManager& dmnman{*Assert(m_node.dmnman)};
    CEvoDB& evodb{*Assert(m_node.evodb)};
    const uint256 tip_hash{BlockHash(500)};
    CBlockIndex tip;
    tip.nHeight = Params().GetConsensus().DIP0003Height;
    tip.phashBlock = &tip_hash;
    const auto tip_list_hash{[&] {
        const EvoViewScope normal{evodb.ViewFor(EvoDbIdentity::NORMAL)};
        return dmnman.GetListAtChainTip().GetBlockHash();
    }};
    {
        const EvoViewScope normal{evodb.ViewFor(EvoDbIdentity::NORMAL)};
        dmnman.UpdatedBlockTip(&tip);
    }
    BOOST_CHECK_EQUAL(tip_list_hash(), tip_hash);

    // A snapshot load that fails after creating its generation.
    {
        LOCK(::cs_main);
        BOOST_REQUIRE(evodb.CreateSnapshotGeneration(BlockHash(600), 600));
        BOOST_REQUIRE(evodb.AbandonSnapshotGeneration());
    }
    BOOST_CHECK_EQUAL(tip_list_hash(), tip_hash);

    // So does a published one that is rejected.
    BOOST_REQUIRE(evodb.CreateSnapshotGeneration(BlockHash(600), 600));
    BOOST_REQUIRE(evodb.PublishSnapshotGeneration(CONFIG_FINGERPRINT, std::nullopt));
    BOOST_REQUIRE(evodb.DiscardSnapshotMarkers());
    BOOST_CHECK_EQUAL(tip_list_hash(), tip_hash);
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
        // A seeded record NORMAL derived identically, one only the snapshot
        // chainstate wrote, and the mutable singleton both views advance.
        db.WriteSeed(PayloadKey(30), PayloadFor(30), EvoSeedPolicy::BYTE_DETERMINISTIC);
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
    // An unseeded record NORMAL also holds was derived from a block below the
    // base, which only NORMAL connects.
    BOOST_CHECK(!db.PromoteSnapshotMarkers(BlockHash(6)));
    // The conflict is in the first merge batch, so nothing was merged:
    // NORMAL keeps its own record and tip. (Batches merged before a later
    // conflict stay merged; startup then keeps failing closed.)
    Payload value;
    BOOST_REQUIRE(db.Read(PayloadKey(5), value));
    BOOST_CHECK(value == PayloadFor(5));
    BOOST_CHECK(db.HasDualChainstateMarker());
}

BOOST_AUTO_TEST_CASE(promotion_applies_each_seeded_key_policy)
{
    LOCK(::cs_main);
    enum class Outcome { PROMOTED, FAILED };
    struct Case {
        std::string name;
        std::function<void(CEvoDB&)> seed;
        Outcome outcome;
    };
    // NORMAL holds payloads 10 and 11; the snapshot tip is 300.
    const std::vector<Case> cases{
        {"seeds agreeing with NORMAL are dropped, the rest merged",
         [](CEvoDB& db) {
             db.WriteSeed(PayloadKey(10), PayloadFor(10), EvoSeedPolicy::BYTE_DETERMINISTIC);
             db.WriteSeed(PayloadKey(20), PayloadFor(20), EvoSeedPolicy::BYTE_DETERMINISTIC);
             db.WriteSeed(PayloadKey(11), PayloadFor(99), EvoSeedPolicy::SCAFFOLDING);
             db.WriteSeed(PayloadKey(21), PayloadFor(21), EvoSeedPolicy::SCAFFOLDING);
             db.Write(PayloadKey(300), PayloadFor(300));
         },
         Outcome::PROMOTED},
        {"a byte-deterministic seed NORMAL derived differently",
         [](CEvoDB& db) { db.WriteSeed(PayloadKey(10), PayloadFor(99), EvoSeedPolicy::BYTE_DETERMINISTIC); },
         Outcome::FAILED},
        {"a masternode-list seed with no way to compare it",
         [](CEvoDB& db) { db.WriteSeed(PayloadKey(10), PayloadFor(10), EvoSeedPolicy::MN_LIST); },
         Outcome::FAILED},
        {"a seed of an unknown family",
         [](CEvoDB& db) { db.WriteSeed(PayloadKey(20), PayloadFor(20), static_cast<EvoSeedPolicy>(0x7f)); },
         Outcome::FAILED},
        {"an unseeded record NORMAL holds with the same bytes",
         [](CEvoDB& db) { db.Write(PayloadKey(10), PayloadFor(10)); },
         Outcome::FAILED},
    };
    using Failpoint = CEvoDB::PromotionFailpoint;
    for (const auto& test : cases) {
        for (const Failpoint failpoint : {Failpoint::NONE, Failpoint::MID_MERGE, Failpoint::AFTER_MERGE,
                                          Failpoint::AFTER_COLLECT}) {
            BOOST_TEST_MESSAGE(test.name);
            CEvoDB db{util::DbWrapperParams{.path = m_args.GetDataDirBase() / "evodb_seed_policy", .memory = true, .wipe = true}};
            WriteMarker(db, EvoDbIdentity::NORMAL, BlockHash(30));
            WritePayload(db, EvoDbIdentity::NORMAL, 10);
            WritePayload(db, EvoDbIdentity::NORMAL, 11);
            BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::NORMAL));
            BOOST_REQUIRE(db.CreateSnapshotGeneration(BlockHash(30), 30));
            {
                auto tx = db.BeginTransaction(EvoDbIdentity::SNAPSHOT);
                db.WriteBestBlock(EvoDbIdentity::SNAPSHOT, BlockHash(300));
                test.seed(db);
                tx->Commit();
            }
            BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::SNAPSHOT));
            BOOST_REQUIRE(db.PublishSnapshotGeneration(CONFIG_FINGERPRINT, std::nullopt));
            BOOST_REQUIRE(db.PrepareSnapshotPromotion(BlockHash(300)));
            // One record per merge batch, so a failpoint can stop between any two.
            db.SetMergeBatchSizeForTesting(1);
            db.SetPromotionFailpointForTesting(failpoint);
            bool promoted{db.PromoteSnapshotMarkers(BlockHash(300))};
            if (failpoint != Failpoint::NONE && test.outcome == Outcome::PROMOTED) {
                BOOST_CHECK(!promoted);
                db.SetPromotionFailpointForTesting(Failpoint::NONE);
                promoted = db.PromoteSnapshotMarkers(BlockHash(300));
            }
            BOOST_CHECK_EQUAL(promoted, test.outcome == Outcome::PROMOTED);
            if (!promoted) {
                BOOST_CHECK(db.HasDualChainstateMarker());
                continue;
            }
            BOOST_CHECK(IteratePayloads(db, EvoDbIdentity::NORMAL) == (std::vector<uint32_t>{10, 11, 300}));
            const EvoViewScope normal{db.ViewFor(EvoDbIdentity::NORMAL)};
            Payload value;
            BOOST_REQUIRE(db.Read(PayloadKey(11), value));
            BOOST_CHECK(value == PayloadFor(11));
            DataStream manifest_tag{};
            manifest_tag << EVODB_SEED_MANIFEST;
            for (const auto& key : RawKeys(db)) {
                BOOST_CHECK(!std::equal(manifest_tag.begin(), manifest_tag.end(), key.begin(),
                                        key.begin() + std::min(key.size(), manifest_tag.size())));
            }
        }
    }
}

// A table block that fails its checksum is skipped by the iterator, which
// stays valid at the next one: promotion and collection must stop there
// rather than finish without the records it held.
BOOST_AUTO_TEST_CASE(generation_lifecycle_stops_at_an_unreadable_table_block)
{
    // Each payload outgrows a LevelDB table block, so the corrupted block
    // holds exactly one of them.
    const auto large_payload{[](uint32_t height) { return Payload(8192, static_cast<uint8_t>(height)); }};
    const uint256 tip{BlockHash(300)};
    const auto record_key{std::make_pair(EVODB_GENERATION_RECORD, uint32_t{1})};
    for (const bool rejected : {false, true}) {
        const fs::path path{m_args.GetDataDirBase() / "evodb_unreadable_generation"};
        {
            LOCK(::cs_main);
            CEvoDB db{util::DbWrapperParams{.path = path, .memory = false, .wipe = true}};
            WriteMarker(db, EvoDbIdentity::NORMAL, BlockHash(30));
            BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::NORMAL));
            BOOST_REQUIRE(db.CreateSnapshotGeneration(BlockHash(30), 30));
            {
                auto tx = db.BeginTransaction(EvoDbIdentity::SNAPSHOT);
                db.WriteBestBlock(EvoDbIdentity::SNAPSHOT, tip);
                for (const uint32_t height : {101U, 102U, 103U}) db.Write(PayloadKey(height), large_payload(height));
                tx->Commit();
            }
            BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::SNAPSHOT, /*sync=*/true));
            BOOST_REQUIRE(db.PublishSnapshotGeneration(CONFIG_FINGERPRINT, std::nullopt));
            BOOST_REQUIRE(rejected ? db.DiscardSnapshotMarkers() : db.PrepareSnapshotPromotion(tip));
            db.GetRawDB().CompactFull();
        }
        BOOST_REQUIRE(CorruptTableBlock(path / "evodb", Payload(512, 102)));

        LOCK(::cs_main);
        CEvoDB db{util::DbWrapperParams{.path = path, .memory = false, .wipe = false}};
        BOOST_REQUIRE(db.LoadSnapshotGenerations(CONFIG_FINGERPRINT));
        if (rejected) {
            BOOST_CHECK(!db.CollectRejectedGenerations());
            BOOST_CHECK(db.HasRejectedGeneration());
            BOOST_CHECK(db.GetRawDB().Exists(record_key));
            continue;
        }
        BOOST_CHECK(!db.PromoteSnapshotMarkers(tip));
        const auto journal{db.ReadPromotionJournal()};
        BOOST_REQUIRE(journal);
        BOOST_CHECK(journal->phase == EvoPromotionPhase::MERGING);
        BOOST_CHECK(db.GetRawDB().Exists(record_key));
        BOOST_CHECK(db.HasDualChainstateMarker());
        BOOST_CHECK(!db.VerifyBestBlock(EvoDbIdentity::NORMAL, tip));
    }
}

// A promotion interrupted after every merge batch, with the database reopened
// each time, ends with what an uninterrupted one would have: merged payloads,
// seeds settled by their policy, or a refusal at a late conflicting record.
BOOST_AUTO_TEST_CASE(promotion_resumes_after_restarts_between_merge_batches)
{
    using Failpoint = CEvoDB::PromotionFailpoint;
    const uint256 tip{BlockHash(300)};
    const std::vector<uint32_t> merged{101, 102, 103, 104, 105, 106};
    for (const bool late_conflict : {false, true}) {
        for (const size_t batch_size : {1U, 2U, 3U}) {
            BOOST_TEST_MESSAGE("batch size " << batch_size << (late_conflict ? ", late conflict" : ""));
            const fs::path path{m_args.GetDataDirBase() / "evodb_promotion_restarts"};
            {
                LOCK(::cs_main);
                CEvoDB db{util::DbWrapperParams{.path = path, .memory = false, .wipe = true}};
                WriteMarker(db, EvoDbIdentity::NORMAL, BlockHash(30));
                for (const uint32_t height : {10U, 11U, 200U}) WritePayload(db, EvoDbIdentity::NORMAL, height);
                BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::NORMAL));
                BOOST_REQUIRE(db.CreateSnapshotGeneration(BlockHash(30), 30));
                {
                    auto tx = db.BeginTransaction(EvoDbIdentity::SNAPSHOT);
                    db.WriteBestBlock(EvoDbIdentity::SNAPSHOT, tip);
                    db.WriteSeed(PayloadKey(10), PayloadFor(10), EvoSeedPolicy::BYTE_DETERMINISTIC);
                    db.WriteSeed(PayloadKey(20), PayloadFor(20), EvoSeedPolicy::BYTE_DETERMINISTIC);
                    db.WriteSeed(PayloadKey(21), PayloadFor(21), EvoSeedPolicy::SCAFFOLDING);
                    for (const uint32_t height : merged) db.Write(PayloadKey(height), PayloadFor(height));
                    // Ordered after every payload above.
                    if (late_conflict) db.Write(PayloadKey(200), PayloadFor(201));
                    tx->Commit();
                }
                BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::SNAPSHOT, /*sync=*/true));
                BOOST_REQUIRE(db.PublishSnapshotGeneration(CONFIG_FINGERPRINT, std::nullopt));
                BOOST_REQUIRE(db.PrepareSnapshotPromotion(tip));
            }

            bool promoted{false};
            bool resumed_after_a_merged_payload{false};
            std::vector<uint8_t> last_cursor;
            for (int run{0}; run < 32 && !promoted; ++run) {
                LOCK(::cs_main);
                CEvoDB db{util::DbWrapperParams{.path = path, .memory = false, .wipe = false}};
                BOOST_REQUIRE(db.LoadSnapshotGenerations(CONFIG_FINGERPRINT));
                db.SetMergeBatchSizeForTesting(batch_size);
                db.SetPromotionFailpointForTesting(Failpoint::MID_MERGE);
                promoted = db.PromoteSnapshotMarkers(tip);
                if (promoted) break;
                const auto journal{db.ReadPromotionJournal()};
                BOOST_REQUIRE(journal);
                // A refused merge makes no progress: it stays refused.
                if (journal->cursor == last_cursor) break;
                last_cursor = journal->cursor;
                if (db.GetRawDB().Exists(PayloadKey(merged.front())) && !db.GetRawDB().Exists(PayloadKey(merged.back()))) {
                    resumed_after_a_merged_payload = true;
                }
            }
            BOOST_CHECK(resumed_after_a_merged_payload);
            BOOST_CHECK_EQUAL(promoted, !late_conflict);

            LOCK(::cs_main);
            CEvoDB db{util::DbWrapperParams{.path = path, .memory = false, .wipe = false}};
            BOOST_REQUIRE(db.LoadSnapshotGenerations(CONFIG_FINGERPRINT));
            if (late_conflict) {
                BOOST_CHECK(db.HasDualChainstateMarker());
                BOOST_CHECK(db.ReadPromotionJournal());
                BOOST_CHECK(!db.VerifyBestBlock(EvoDbIdentity::NORMAL, tip));
                const EvoViewScope normal{db.ViewFor(EvoDbIdentity::NORMAL)};
                Payload value;
                BOOST_REQUIRE(db.Read(PayloadKey(200), value));
                BOOST_CHECK(value == PayloadFor(200));
                continue;
            }
            BOOST_CHECK(db.VerifyBestBlock(EvoDbIdentity::NORMAL, tip));
            BOOST_CHECK(!db.ReadPromotionJournal());
            std::vector<uint32_t> expected{10, 11};
            expected.insert(expected.end(), merged.begin(), merged.end());
            expected.push_back(200);
            BOOST_CHECK(IteratePayloads(db, EvoDbIdentity::NORMAL) == expected);
            DataStream manifest_tag{};
            manifest_tag << EVODB_SEED_MANIFEST;
            for (const auto& key : RawKeys(db)) {
                BOOST_CHECK(!std::equal(manifest_tag.begin(), manifest_tag.end(), key.begin(),
                                        key.begin() + std::min(key.size(), manifest_tag.size())));
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(only_an_unpublished_generation_records_seeds)
{
    LOCK(::cs_main);
    CEvoDB db{util::DbWrapperParams{.path = m_args.GetDataDirBase() / "evodb_seed_binding", .memory = true, .wipe = true}};
    DataStream manifest_tag{};
    manifest_tag << EVODB_SEED_MANIFEST;
    const auto manifest_keys{[&] {
        return std::ranges::count_if(RawKeys(db), [&](const auto& key) {
            return key.size() >= manifest_tag.size() && std::equal(manifest_tag.begin(), manifest_tag.end(), key.begin());
        });
    }};
    {
        auto tx = db.BeginTransaction(EvoDbIdentity::NORMAL);
        BOOST_CHECK(db.WriteSeed(PayloadKey(1), PayloadFor(1), EvoSeedPolicy::BYTE_DETERMINISTIC));
        BOOST_CHECK(!db.WriteSeed(PayloadKey(1), PayloadFor(2), EvoSeedPolicy::BYTE_DETERMINISTIC));
        tx->Commit();
    }
    BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::NORMAL));
    BOOST_CHECK_EQUAL(manifest_keys(), 0);
    PublishGeneration(db);
    auto tx = db.BeginTransaction(EvoDbIdentity::SNAPSHOT);
    BOOST_CHECK_THROW(db.WriteSeed(PayloadKey(1), PayloadFor(1), EvoSeedPolicy::BYTE_DETERMINISTIC),
                      EvoViewBindingError);
}

BOOST_AUTO_TEST_CASE(fingerprint_refusal_names_the_generation_and_changes_nothing)
{
    using Failpoint = CEvoDB::PromotionFailpoint;
    for (const bool promoting : {false, true}) {
        const fs::path path{m_args.GetDataDirBase() / "evodb_fingerprint_refusal"};
        {
            LOCK(::cs_main);
            CEvoDB db{util::DbWrapperParams{.path = path, .memory = false, .wipe = true}};
            WritePayload(db, EvoDbIdentity::NORMAL, 10);
            BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::NORMAL));
            BOOST_REQUIRE(db.CreateSnapshotGeneration(BlockHash(40), 40));
            {
                auto tx = db.BeginTransaction(EvoDbIdentity::SNAPSHOT);
                db.WriteBestBlock(EvoDbIdentity::SNAPSHOT, BlockHash(41));
                db.Write(PayloadKey(41), PayloadFor(41));
                db.Write(PayloadKey(42), PayloadFor(42));
                tx->Commit();
            }
            BOOST_REQUIRE(db.CommitRootTransaction(EvoDbIdentity::SNAPSHOT, /*sync=*/true));
            BOOST_REQUIRE(db.PublishSnapshotGeneration(CONFIG_FINGERPRINT, std::nullopt));
            if (promoting) {
                BOOST_REQUIRE(db.PrepareSnapshotPromotion(BlockHash(41)));
                db.SetMergeBatchSizeForTesting(1);
                db.SetPromotionFailpointForTesting(Failpoint::MID_MERGE);
                BOOST_REQUIRE(!db.PromoteSnapshotMarkers(BlockHash(41)));
            }
            // A generation left unpublished by an interrupted load, which an
            // accepted startup collects.
            EvoGenerationRecord stale;
            stale.generation = 7;
            stale.base = BlockHash(50);
            stale.base_height = 50;
            BOOST_REQUIRE(db.GetRawDB().Write(std::make_pair(EVODB_GENERATION_RECORD, uint32_t{7}), stale));
            DataStream stale_key{CEvoDBStore::GenerationPrefix(7)};
            stale_key << PayloadKey(50);
            CDBBatch batch{db.GetRawDB()};
            batch.Write(stale_key, PayloadFor(50));
            BOOST_REQUIRE(db.GetRawDB().WriteBatch(batch, /*fSync=*/true));
        }

        CEvoDB reopened{util::DbWrapperParams{.path = path, .memory = false, .wipe = false}};
        const auto before{RawEntries(reopened)};
        const auto result{reopened.LoadSnapshotGenerations(uint256::ZERO)};
        BOOST_REQUIRE(!result);
        const std::string error{util::ErrorString(result).original};
        BOOST_TEST_MESSAGE(error);
        for (const std::string& part : {std::string{"generation 1"}, BlockHash(40).ToString(), std::string{"height 40"},
                                        std::string{promoting ? "promotion merging" : "published, not promoted"},
                                        std::string{"Changed component unknown (only the aggregate fingerprint was "
                                                    "recorded)"},
                                        std::string{"previous version and configuration"},
                                        std::string{"-reindex-chainstate"}}) {
            BOOST_CHECK_MESSAGE(error.find(part) != std::string::npos, "refusal does not name: " + part);
        }
        BOOST_CHECK(RawEntries(reopened) == before);
        // The configuration it was recorded under still resumes it.
        BOOST_CHECK(reopened.LoadSnapshotGenerations(CONFIG_FINGERPRINT));
        BOOST_REQUIRE(reopened.GetSnapshotGeneration());
        BOOST_CHECK_EQUAL(reopened.GetSnapshotGeneration()->generation, 1U);
    }
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
