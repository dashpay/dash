// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_TEST_UTIL_H
#define BITCOIN_WALLET_TEST_UTIL_H

#include <script/standard.h>
#include <memory>

class ArgsManager;
class ChainstateManager;
class CKey;
enum class OutputType;
namespace interfaces {
class Chain;
namespace CoinJoin {
class Loader;
} // namespace CoinJoin
} // namespace interfaces

namespace wallet {
class CWallet;
struct DatabaseOptions;
class WalletDatabase;

std::unique_ptr<CWallet> CreateSyncedWallet(interfaces::Chain& chain, interfaces::CoinJoin::Loader& coinjoin_loader, ChainstateManager& chainman, ArgsManager& args, const CKey& key);

// Creates a copy of the provided database
std::unique_ptr<WalletDatabase> DuplicateMockDatabase(WalletDatabase& database, DatabaseOptions& options);

/** Returns a new encoded destination from the wallet */
std::string getnewaddress(wallet::CWallet& w);
/** Returns a new destination, of an specific type, from the wallet */
CTxDestination getNewDestination(wallet::CWallet& w);

} // namespace wallet

#endif // BITCOIN_WALLET_TEST_UTIL_H
