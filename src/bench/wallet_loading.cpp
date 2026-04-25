// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <interfaces/chain.h>
#include <node/context.h>
#include <test/util/mining.h>
#include <test/util/setup_common.h>
#include <test/util/wallet.h>
#include <wallet/test/util.h>
#include <util/translation.h>
#include <validationinterface.h>
#include <wallet/context.h>
#include <wallet/receive.h>
#include <wallet/wallet.h>

#include <optional>

namespace wallet {
static void AddTx(CWallet& wallet)
{
    CMutableTransaction mtx;
    mtx.vout.push_back({COIN, GetScriptForDestination(*Assert(wallet.GetNewDestination("")))});
    mtx.vin.push_back(CTxIn());

    wallet.AddToWallet(MakeTransactionRef(mtx), TxStateInactive{});
}

static std::unique_ptr<WalletDatabase> DuplicateMockDatabase(WalletDatabase& database, DatabaseOptions& options)
{
    auto new_database = CreateMockWalletDatabase(options);

    // Get a cursor to the original database
    auto batch = database.MakeBatch();
    batch->StartCursor();

    // Get a batch for the new database
    auto new_batch = new_database->MakeBatch();

    // Read all records from the original database and write them to the new one
    while (true) {
        CDataStream key(SER_DISK, CLIENT_VERSION);
        CDataStream value(SER_DISK, CLIENT_VERSION);
        bool complete;
        batch->ReadAtCursor(key, value, complete);
        if (complete) break;
        new_batch->Write(key, value);
    }

    return new_database;
}

static void WalletLoading(benchmark::Bench& bench, bool legacy_wallet)
{
    const auto test_setup = MakeNoLogFileContext<TestingSetup>();
    test_setup->m_args.ForceSetArg("-unsafesqlitesync", "1");

    WalletContext context;
    context.args = &test_setup->m_args;
    context.chain = test_setup->m_node.chain.get();

    // Setup the wallet
    // Loading the wallet will also create it
    DatabaseOptions options;
    if (legacy_wallet) {
        options.require_format = DatabaseFormat::BERKELEY;
    } else {
        options.create_flags = WALLET_FLAG_DESCRIPTORS;
        options.require_format = DatabaseFormat::SQLITE;
    }
    auto database = CreateMockWalletDatabase(options);
    auto wallet = TestLoadWallet(std::move(database), context, options.create_flags);

    // Generate a bunch of transactions and addresses to put into the wallet
    for (int i = 0; i < 1000; ++i) {
        AddTx(*wallet);
    }

    database = DuplicateMockDatabase(wallet->GetDatabase(), options);

    // reload the wallet for the actual benchmark
    TestUnloadWallet(context, std::move(wallet));

    bench.epochs(5).run([&] {
        wallet = TestLoadWallet(std::move(database), context, options.create_flags);

        // Cleanup
        database = DuplicateMockDatabase(wallet->GetDatabase(), options);
        TestUnloadWallet(context, std::move(wallet));
    });
}

#ifdef USE_BDB
static void WalletLoadingLegacy(benchmark::Bench& bench) { WalletLoading(bench, /*legacy_wallet=*/true); }
BENCHMARK(WalletLoadingLegacy, benchmark::PriorityLevel::HIGH);
#endif

#ifdef USE_SQLITE
static void WalletLoadingDescriptors(benchmark::Bench& bench) { WalletLoading(bench, /*legacy_wallet=*/false); }
BENCHMARK(WalletLoadingDescriptors, benchmark::PriorityLevel::HIGH);
#endif
} // namespace wallet
