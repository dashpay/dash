// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/test/util.h>

#include <sync.h>
#include <validationinterface.h>
#include <wallet/context.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <memory>
#include <optional>
#include <vector>

namespace wallet {
std::shared_ptr<CWallet> TestLoadWallet(std::unique_ptr<WalletDatabase> database, WalletContext& context, uint64_t create_flags)
{
    bilingual_str error;
    std::vector<bilingual_str> warnings;
    auto wallet = CWallet::Create(context, "", std::move(database), create_flags, error, warnings);
    if (context.coinjoin_loader) {
        // TODO: see CreateWalletWithoutChain
        AddWallet(context, wallet);
    }
    NotifyWalletLoaded(context, wallet);
    if (context.chain) {
        wallet->postInitProcess();
    }
    return wallet;
}

std::shared_ptr<CWallet> TestLoadWallet(WalletContext& context)
{
    DatabaseOptions options;
    options.create_flags = WALLET_FLAG_DESCRIPTORS;
    DatabaseStatus status;
    bilingual_str error;
    auto database = MakeWalletDatabase("", options, status, error);
    return TestLoadWallet(std::move(database), context, options.create_flags);
}

void TestUnloadWallet(WalletContext& context, std::shared_ptr<CWallet>&& wallet)
{
    std::vector<bilingual_str> warnings;
    SyncWithValidationInterfaceQueue();
    wallet->m_chain_notifications_handler.reset();
    if (context.coinjoin_loader) {
        RemoveWallet(context, wallet, /*load_on_start=*/std::nullopt, warnings);
    }
    UnloadWallet(std::move(wallet));
}
} // namespace wallet
