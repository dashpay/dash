// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/test/util.h>

#include <algorithm>
#include <chain.h>
#include <key.h>
#include <key_io.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/check.h>
#include <validation.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <memory>

namespace wallet {
const std::string ADDRESS_B58T_UNSPENDABLE = "yXXXXXXXXXXXXXXXXXXXXXXXXXXXVd2rXU";
const std::string ADDRESS_BCRT1_UNSPENDABLE = "bcrt1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq3xueyj";

std::unique_ptr<CWallet> CreateSyncedWallet(interfaces::Chain& chain, interfaces::CoinJoin::Loader& coinjoin_loader, ChainstateManager& chainman, ArgsManager& args, const CKey& key)
{
    struct ChainInfo {
        int height;
        uint256 tip_hash;
        uint256 genesis_hash;
    };
    const ChainInfo chain_info{WITH_LOCK(chainman.GetMutex(), return (ChainInfo{
        chainman.ActiveChain().Height(),
        chainman.ActiveChain().Tip()->GetBlockHash(),
        chainman.ActiveChain().Genesis()->GetBlockHash()}))};

    auto wallet = std::make_unique<CWallet>(&chain, &coinjoin_loader, "", args, CreateMockableWalletDatabase());
    {
        LOCK(wallet->cs_wallet);
        wallet->SetLastBlockProcessed(chain_info.height, chain_info.tip_hash);
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
    CWallet::ScanResult result = wallet->ScanForWalletTransactions(chain_info.genesis_hash, /*start_height=*/0, /*max_height=*/{}, reserver, /*fUpdate=*/false, /*save_progress=*/false);
    assert(result.status == CWallet::ScanResult::SUCCESS);
    assert(result.last_scanned_block == chain_info.tip_hash);
    assert(*result.last_scanned_height == chain_info.height);
    assert(result.last_failed_block.IsNull());
    return wallet;
}

std::string getnewaddress(CWallet& w)
{
    return EncodeDestination(getNewDestination(w));
}

CTxDestination getNewDestination(CWallet& w)
{
    return *Assert(w.GetNewDestination(""));
}

std::unique_ptr<WalletDatabase> DuplicateMockDatabase(WalletDatabase& database)
{
    return std::make_unique<MockableDatabase>(dynamic_cast<MockableDatabase&>(database).m_records);
}

bool MockableBatch::ReadKey(CDataStream&& key, CDataStream& value)
{
    if (!m_pass) return false;
    SerializeData key_data{key.begin(), key.end()};
    const auto& it = m_records.find(key_data);
    if (it == m_records.end()) return false;
    value.write(MakeByteSpan(it->second));
    return true;
}

bool MockableBatch::WriteKey(CDataStream&& key, CDataStream&& value, bool overwrite)
{
    if (!m_pass) return false;
    SerializeData key_data{key.begin(), key.end()};
    SerializeData value_data{value.begin(), value.end()};
    auto [it, inserted] = m_records.emplace(key_data, value_data);
    if (!inserted && overwrite) {
        it->second = value_data;
        inserted = true;
    }
    return inserted;
}

bool MockableBatch::EraseKey(CDataStream&& key)
{
    if (!m_pass) return false;
    SerializeData key_data{key.begin(), key.end()};
    m_records.erase(key_data);
    return true;
}

bool MockableBatch::HasKey(CDataStream&& key)
{
    if (!m_pass) return false;
    SerializeData key_data{key.begin(), key.end()};
    return m_records.count(key_data) > 0;
}

bool MockableBatch::ErasePrefix(Span<const std::byte> prefix)
{
    if (!m_pass) return false;
    auto it = m_records.begin();
    while (it != m_records.end()) {
        auto& key = it->first;
        if (key.size() < prefix.size() || std::search(key.begin(), key.end(), prefix.begin(), prefix.end()) != key.begin()) {
            ++it;
            continue;
        }
        it = m_records.erase(it);
    }
    return true;
}

bool MockableBatch::StartCursor()
{
    m_cursor = m_records.cbegin();
    return m_pass;
}

bool MockableBatch::ReadAtCursor(CDataStream& ssKey, CDataStream& ssValue, bool& complete)
{
    if (!m_pass) return false;
    complete = m_cursor == m_records.cend();
    if (complete) return true;
    ssKey.write(MakeByteSpan(m_cursor->first));
    ssValue.write(MakeByteSpan(m_cursor->second));
    ++m_cursor;
    return true;
}

std::unique_ptr<WalletDatabase> CreateMockableWalletDatabase(std::map<SerializeData, SerializeData> records)
{
    return std::make_unique<MockableDatabase>(std::move(records));
}

MockableDatabase& GetMockableDatabase(CWallet& wallet)
{
    return dynamic_cast<MockableDatabase&>(wallet.GetDatabase());
}
} // namespace wallet
