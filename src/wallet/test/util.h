// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_TEST_UTIL_H
#define BITCOIN_WALLET_TEST_UTIL_H

#include <script/standard.h>

#include <memory>
#include <string>

class ArgsManager;
class ChainstateManager;
class CKey;
namespace interfaces {
class Chain;
namespace CoinJoin {
class Loader;
} // namespace CoinJoin
} // namespace interfaces

namespace wallet {
class CWallet;

extern const std::string ADDRESS_B58T_UNSPENDABLE;
extern const std::string ADDRESS_BCRT1_UNSPENDABLE;

std::unique_ptr<CWallet> CreateSyncedWallet(interfaces::Chain& chain, interfaces::CoinJoin::Loader& coinjoin_loader, ChainstateManager& chainman, ArgsManager& args, const CKey& key);

/** Returns a new encoded destination from the wallet */
std::string getnewaddress(CWallet& w);
/** Returns a new destination from the wallet. Dash only supports OutputType::LEGACY. */
CTxDestination getNewDestination(CWallet& w);
} // namespace wallet

#endif // BITCOIN_WALLET_TEST_UTIL_H
