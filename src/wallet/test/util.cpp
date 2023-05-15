// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/test/util.h>

#include <chain.h>
#include <key.h>
#include <key_io.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <boost/test/unit_test.hpp>

#include <memory>

namespace wallet {
std::unique_ptr<WalletDatabase> CreateMockableWalletDatabase(std::map<SerializeData, SerializeData> records)
{
    return std::make_unique<MockableDatabase>(records);
}

MockableDatabase& GetMockableDatabase(CWallet& wallet)
{
    return dynamic_cast<MockableDatabase&>(wallet.GetDatabase());
}
std::unique_ptr<CWallet> CreateSyncedWallet(interfaces::Chain& chain, interfaces::CoinJoin::Loader& coinjoin_loader, CChain& cchain, ArgsManager& args, const CKey& key)
{
    auto wallet = std::make_unique<CWallet>(&chain, &coinjoin_loader, "", args, CreateMockableWalletDatabase());
    {
        LOCK(wallet->cs_wallet);
        wallet->SetLastBlockProcessed(cchain.Height(), cchain.Tip()->GetBlockHash());
    }
    wallet->LoadWallet();
    {
        LOCK(wallet->cs_wallet);
        wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        wallet->SetupDescriptorScriptPubKeyMans("", "");

        FlatSigningProvider provider;
        std::string error;
        std::unique_ptr<Descriptor> desc = Parse("combo(" + EncodeSecret(key) + ")", provider, error, /* require_checksum=*/ false);
        assert(desc);
        WalletDescriptor w_desc(std::move(desc), 0, 0, 1, 1);
        if (!wallet->AddWalletDescriptor(w_desc, provider, "", false)) assert(false);
    }
    WalletRescanReserver reserver(*wallet);
    reserver.reserve();
    CWallet::ScanResult result = wallet->ScanForWalletTransactions(cchain.Genesis()->GetBlockHash(), /*start_height=*/0, /*max_height=*/{}, reserver, /*fUpdate=*/false, /*save_progress=*/false);
    BOOST_CHECK_EQUAL(result.status, CWallet::ScanResult::SUCCESS);
    BOOST_CHECK_EQUAL(result.last_scanned_block, cchain.Tip()->GetBlockHash());
    BOOST_CHECK_EQUAL(*result.last_scanned_height, cchain.Height());
    BOOST_CHECK(result.last_failed_block.IsNull());
    return wallet;
}
MockableBatch::MockableBatch(Record& records, bool pass)
    : m_records(records), m_pass(pass), m_cursor_active(false)
{
}

MockableBatch::~MockableBatch() {}

bool MockableBatch::DeserializeKey(CDataStream&& key, SerializeData& out)
{
    try { key >> out; return true; }
    catch (...) { return false; }
}

bool MockableBatch::ReadKey(CDataStream&& key, CDataStream& value)
{
    SerializeData skey;
    if (!DeserializeKey(std::move(key), skey)) return false;
    auto it = m_records.find(skey);
    if (it == m_records.end()) return false;
    value << it->second;
    return true;
}

bool MockableBatch::WriteKey(CDataStream&& key, CDataStream&& value, bool overwrite)
{
    SerializeData skey, svalue;
    if (!DeserializeKey(std::move(key), skey)) return false;
    try { value >> svalue; } catch (...) { return false; }
    if (!overwrite && m_records.count(skey)) return false;
    m_records[skey] = svalue;
    return true;
}

bool MockableBatch::EraseKey(CDataStream&& key)
{
    SerializeData skey;
    if (!DeserializeKey(std::move(key), skey)) return false;
    return m_records.erase(skey) > 0;
}

bool MockableBatch::HasKey(CDataStream&& key)
{
    SerializeData skey;
    if (!DeserializeKey(std::move(key), skey)) return false;
    return m_records.count(skey) != 0;
}

bool MockableBatch::ErasePrefix(Span<const std::byte>)
{
    return true;
}

void MockableBatch::Flush() {}
void MockableBatch::Close() {}

bool MockableBatch::StartCursor()
{
    m_cursor_it = m_records.begin();
    m_cursor_active = true;
    return true;
}

bool MockableBatch::ReadAtCursor(CDataStream& ssKey, CDataStream& ssValue, bool& complete)
{
    if (!m_cursor_active) { complete = true; return false; }
    if (m_cursor_it == m_records.end()) { complete = true; return true; }
    ssKey << m_cursor_it->first;
    ssValue << m_cursor_it->second;
    ++m_cursor_it;
    complete = (m_cursor_it == m_records.end());
    return true;
}

void MockableBatch::CloseCursor()
{
    m_cursor_active = false;
}

bool MockableBatch::TxnBegin() { return m_pass; }
bool MockableBatch::TxnCommit() { return m_pass; }
bool MockableBatch::TxnAbort() { return m_pass; }
} // namespace wallet
