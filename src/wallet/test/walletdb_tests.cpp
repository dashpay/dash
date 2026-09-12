// Copyright (c) 2012-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>
#include <clientversion.h>
#include <streams.h>
#include <uint256.h>
#include <wallet/hdchain.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/walletdb.h>

#include <boost/test/unit_test.hpp>

namespace wallet {
BOOST_FIXTURE_TEST_SUITE(walletdb_tests, WalletTestingSetup)

BOOST_AUTO_TEST_CASE(walletdb_readkeyvalue)
{
    /**
     * When ReadKeyValue() reads from either a "key" or "wkey" it first reads the CDataStream steam into a
     * CPrivKey or CWalletKey respectively and then reads a hash of the pubkey and privkey into a uint256.
     * Wallets from 0.8 or before do not store the pubkey/privkey hash, trying to read the hash from old
     * wallets throws an exception, for backwards compatibility this read is wrapped in a try block to
     * silently fail. The test here makes sure the type of exception thrown from CDataStream::read()
     * matches the type we expect, otherwise we need to update the "key"/"wkey" exception type caught.
     */
    CDataStream ssValue(SER_DISK, CLIENT_VERSION);
    uint256 dummy;
    BOOST_CHECK_THROW(ssValue >> dummy, std::ios_base::failure);
}

// Helper: build a key/value stream pair for an HD chain DB record and run ReadKeyValue.
static bool TryReadHDChainRecord(CWallet& wallet, const std::string& dbKey, bool fCrypted, std::string& strErrOut)
{
    CHDChain chain;
    chain.SetCrypted(fCrypted);

    CDataStream ssKey(SER_DISK, CLIENT_VERSION);
    CDataStream ssValue(SER_DISK, CLIENT_VERSION);
    ssKey << dbKey;
    ssValue << chain;

    std::string strType;
    LOCK(wallet.cs_wallet);
    return ReadKeyValue(&wallet, ssKey, ssValue, strType, strErrOut);
}

BOOST_AUTO_TEST_CASE(walletdb_hdchain_type_mismatch)
{
    // Regression: a wallet record claiming HDCHAIN but carrying a crypted CHDChain
    // (or vice versa) used to trigger an assert and abort the process. It must now
    // surface as a graceful load error.
    std::string strErr;

    BOOST_CHECK(!TryReadHDChainRecord(m_wallet, DBKeys::HDCHAIN, /*fCrypted=*/true, strErr));
    BOOST_CHECK_EQUAL(strErr, "Error reading wallet database: HD chain type mismatch");

    strErr.clear();
    BOOST_CHECK(!TryReadHDChainRecord(m_wallet, DBKeys::CRYPTED_HDCHAIN, /*fCrypted=*/false, strErr));
    BOOST_CHECK_EQUAL(strErr, "Error reading wallet database: HD chain type mismatch");
}

BOOST_AUTO_TEST_CASE(walletdb_platform_data_records)
{
    LOCK(m_wallet.cs_wallet);

    // Round-trip through the in-memory map: write, prefix query, erase.
    const std::vector<unsigned char> value_a{0x01, 0x02};
    const std::vector<unsigned char> value_b{0x03};
    BOOST_CHECK(m_wallet.WritePlatformData("platform/identity/0", value_a));
    BOOST_CHECK(m_wallet.WritePlatformData("platform/identity/1", value_b));
    BOOST_CHECK(m_wallet.WritePlatformData("platform/metadata", value_b));

    auto records{m_wallet.GetPlatformData("platform/identity/")};
    BOOST_CHECK_EQUAL(records.size(), 2U);
    BOOST_CHECK(records.at("platform/identity/0") == value_a);
    BOOST_CHECK(records.at("platform/identity/1") == value_b);
    BOOST_CHECK_EQUAL(m_wallet.GetPlatformData("").size(), 3U);
    BOOST_CHECK(m_wallet.GetPlatformData("platform/idem").empty());

    // An empty value erases the record.
    BOOST_CHECK(m_wallet.WritePlatformData("platform/identity/0", {}));
    records = m_wallet.GetPlatformData("platform/identity/");
    BOOST_CHECK_EQUAL(records.size(), 1U);
    BOOST_CHECK_EQUAL(records.count("platform/identity/0"), 0U);

    // The wallet-load path (ReadKeyValue) must populate the same map.
    CDataStream ssKey(SER_DISK, CLIENT_VERSION);
    CDataStream ssValue(SER_DISK, CLIENT_VERSION);
    ssKey << std::make_pair(DBKeys::PLATFORM_DATA, std::string{"platform/loaded"});
    ssValue << value_a;
    std::string strType, strErr;
    BOOST_CHECK(ReadKeyValue(&m_wallet, ssKey, ssValue, strType, strErr));
    BOOST_CHECK_EQUAL(strType, DBKeys::PLATFORM_DATA);
    BOOST_CHECK(m_wallet.GetPlatformData("platform/loaded").at("platform/loaded") == value_a);

    // A truncated value must fail the read and identify PLATFORM_DATA as the
    // failing type so LoadWallet() can classify the damage as noncritical.
    CDataStream ssGood(SER_DISK, CLIENT_VERSION);
    ssGood << value_a;
    CDataStream ssBadKey(SER_DISK, CLIENT_VERSION);
    CDataStream ssBadValue(SER_DISK, CLIENT_VERSION);
    ssBadKey << std::make_pair(DBKeys::PLATFORM_DATA, std::string{"platform/corrupt"});
    ssBadValue.write({ssGood.data(), ssGood.size() - 1});
    strType.clear();
    strErr.clear();
    BOOST_CHECK(!ReadKeyValue(&m_wallet, ssBadKey, ssBadValue, strType, strErr));
    BOOST_CHECK_EQUAL(strType, DBKeys::PLATFORM_DATA);
    BOOST_CHECK(m_wallet.GetPlatformData("platform/corrupt").empty());
}

//! Load a wallet whose database holds a PLATFORM_DATA record under
//! `record_key` with a truncated value (claims five bytes, carries none),
//! next to an intact record, and report the load result and the Platform
//! data that made it into the wallet.
static DBErrors LoadWithCorruptPlatformRecord(const node::NodeContext& node, const ArgsManager& args,
                                              const std::string& record_key,
                                              std::map<std::string, std::vector<unsigned char>>& platform_data_out)
{
    CWallet wallet(node.chain.get(), node.coinjoin_loader.get(), "", args, CreateMockWalletDatabase());
    const std::vector<unsigned char> truncated{0x05};
    const std::vector<unsigned char> intact_value{0x01, 0x02};
    {
        const std::unique_ptr<DatabaseBatch> batch{wallet.GetDatabase().MakeBatch()};
        BOOST_REQUIRE(batch->Write(std::make_pair(DBKeys::PLATFORM_DATA, record_key), Span{truncated}));
        BOOST_REQUIRE(batch->Write(std::make_pair(DBKeys::PLATFORM_DATA, std::string{"platform/intact"}), intact_value));
    }
    const DBErrors result{WalletBatch(wallet.GetDatabase()).LoadWallet(&wallet)};
    platform_data_out = WITH_LOCK(wallet.cs_wallet, return wallet.GetPlatformData(""));
    return result;
}

BOOST_AUTO_TEST_CASE(walletdb_platform_data_corruption_policy)
{
    // Platform records are opaque cache/metadata. Damage is noncritical, the
    // wallet still loads, and intact records survive. There is no reserved
    // Platform seed-selection record.
    std::map<std::string, std::vector<unsigned char>> platform_data;
    BOOST_CHECK(LoadWithCorruptPlatformRecord(m_node, m_args, "platform/metadata", platform_data) ==
                DBErrors::NONCRITICAL_ERROR);
    BOOST_CHECK_EQUAL(platform_data.count("platform/metadata"), 0U);
    BOOST_CHECK(platform_data.at("platform/intact") == std::vector<unsigned char>({0x01, 0x02}));

    platform_data.clear();
    BOOST_CHECK(LoadWithCorruptPlatformRecord(m_node, m_args, "platform/identity/0", platform_data) ==
                DBErrors::NONCRITICAL_ERROR);
    BOOST_CHECK_EQUAL(platform_data.count("platform/identity/0"), 0U);
    BOOST_CHECK(platform_data.at("platform/intact") == std::vector<unsigned char>({0x01, 0x02}));
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
