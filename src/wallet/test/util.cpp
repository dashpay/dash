// Copyright (c) 2021-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/test/util.h>

#include <chain.h>
#include <key.h>
#include <key_io.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <validation.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <cassert>
#include <memory>

namespace wallet {
std::unique_ptr<CWallet> CreateSyncedWallet(interfaces::Chain& chain, ChainstateManager& chainman, const CKey& key)
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

    auto wallet = std::make_unique<CWallet>(&chain, "", CreateMockWalletDatabase());
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

std::unique_ptr<WalletDatabase> DuplicateMockDatabase(WalletDatabase& database, DatabaseOptions& options)
{
    auto new_database = CreateMockWalletDatabase(options);

    // Get a cursor to the original database
    auto batch = database.MakeBatch();
    std::unique_ptr<wallet::DatabaseCursor> cursor = batch->GetNewCursor();

    // Get a batch for the new database
    auto new_batch = new_database->MakeBatch();

    // Read all records from the original database and write them to the new one
    while (true) {
        DataStream key{};
        DataStream value{};
        DatabaseCursor::Status status = cursor->Next(key, value);
        assert(status != DatabaseCursor::Status::FAIL);
        if (status == DatabaseCursor::Status::DONE) break;
        new_batch->Write(Span{key}, Span{value});
    }

    return new_database;
}

std::string getnewaddress(CWallet& w)
{
    constexpr auto output_type = OutputType::LEGACY;
    return EncodeDestination(getNewDestination(w, output_type));
}

CTxDestination getNewDestination(CWallet& w, OutputType output_type)
{
    assert(output_type == OutputType::LEGACY);
    return *Assert(w.GetNewDestination(""));
}

} // namespace wallet
