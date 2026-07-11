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

BOOST_AUTO_TEST_CASE(active_chainstate_read_identity)
{
    CEvoDB db{util::DbWrapperParams{.path = m_args.GetDataDirBase() / "evodb_read_identity", .memory = true, .wipe = true}};
    const auto key = PayloadKey(2);

    WritePayload(db, EvoDbIdentity::SNAPSHOT, 2);

    Payload value;
    BOOST_CHECK(!db.Read(key, value));

    db.SetActiveChainstateIdentity(EvoDbIdentity::SNAPSHOT);
    BOOST_REQUIRE(db.Read(key, value));
    BOOST_CHECK(value == PayloadFor(2));

    {
        auto tx = db.BeginTransaction(EvoDbIdentity::NORMAL);
        BOOST_CHECK(!db.Read(key, value));
    }

    BOOST_REQUIRE(db.Read(key, value));
    BOOST_CHECK(value == PayloadFor(2));
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

BOOST_AUTO_TEST_SUITE_END()
