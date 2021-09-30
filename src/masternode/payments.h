// Copyright (c) 2014-2021 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MASTERNODE_PAYMENTS_H
#define BITCOIN_MASTERNODE_PAYMENTS_H

#include <amount.h>

#include <string>
#include <vector>

class CMasternodePayments;
class CBlock;
class CTransaction;
class CMutableTransaction;
class CTxOut;

/// TODO: all 4 functions do not belong here really, they should be refactored/moved somewhere (main.cpp ?)
std::optional<std::string> IsOldBudgetBlockValueValid(const CBlock& block, int nBlockHeight, CAmount blockReward);
std::optional<std::string> IsBlockValueValid(const CBlock& block, int nBlockHeight, CAmount blockReward);
bool IsBlockPayeeValid(const CTransaction& txNew, int nBlockHeight, CAmount blockReward);

struct masternode_superblock_payments {std::vector<CTxOut> vMasternodePayments; std::vector<CTxOut> vSuperblockPayments;};
masternode_superblock_payments FillBlockPayments(CMutableTransaction& txNew, int nBlockHeight, CAmount blockReward);

extern CMasternodePayments mnpayments;

//
// Masternode Payments Class
// Keeps track of who should get paid for which blocks
//

class CMasternodePayments
{
public:
    static std::optional<std::vector<CTxOut>> GetBlockTxOuts(int nBlockHeight, CAmount blockReward);
    static bool IsTransactionValid(const CTransaction& txNew, int nBlockHeight, CAmount blockReward);

    static std::optional<std::vector<CTxOut>> GetMasternodeTxOuts(int nBlockHeight, CAmount blockReward);
};

#endif // BITCOIN_MASTERNODE_PAYMENTS_H
