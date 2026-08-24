// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_UTIL_MASTERNODE_H
#define BITCOIN_TEST_UTIL_MASTERNODE_H

#include <coins.h>
#include <primitives/transaction.h>

#include <map>
#include <vector>

class CBLSSecretKey;
class ChainstateManager;
class CKey;

using SimpleUTXOMap = std::map<COutPoint, Coin>;

SimpleUTXOMap BuildSimpleUtxoMap(const std::vector<CTransactionRef>& txs);
SimpleUTXOMap FundTransaction(const ChainstateManager& chainman, CMutableTransaction& tx, SimpleUTXOMap& utxos,
                              const CScript& script_payout, CAmount amount);
//! Overload for payout scripts that cannot receive the change, e.g. an OP_RETURN burn.
SimpleUTXOMap FundTransaction(const ChainstateManager& chainman, CMutableTransaction& tx, SimpleUTXOMap& utxos,
                              const CScript& script_payout, CAmount amount, const CScript& script_change);
void SignTransaction(CMutableTransaction& tx, const SimpleUTXOMap& coins, const CKey& coinbase_key);
CMutableTransaction CreateProRegTx(const ChainstateManager& chainman, SimpleUTXOMap& utxos, int port,
                                   const CScript& script_payout, const CKey& coinbase_key, CKey& owner_key_ret,
                                   CBLSSecretKey& operator_key_ret);
//! Spend `amount` to `script_payout`, e.g. to create a collateral output.
CMutableTransaction CreateSpendTx(const ChainstateManager& chainman, SimpleUTXOMap& utxos,
                                  const CScript& script_payout, CAmount amount, const CKey& coinbase_key);
//! The first output of `tx` holding a regular masternode collateral, or a null outpoint.
COutPoint GetCollateralOutpoint(const CMutableTransaction& tx);
//! ProRegTx that references a pre-existing collateral output instead of funding one inline.
CMutableTransaction CreateProRegTxExternalCollateral(const ChainstateManager& chainman, SimpleUTXOMap& utxos, int port,
                                                     const COutPoint& collateral_outpoint, const CScript& script_payout,
                                                     const CKey& owner_key, const CBLSSecretKey& operator_key,
                                                     const CKey& collateral_key, const CKey& coinbase_key);
CScript GenerateRandomAddress();

#endif // BITCOIN_TEST_UTIL_MASTERNODE_H
