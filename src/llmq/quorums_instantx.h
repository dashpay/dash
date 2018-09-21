// Copyright (c) 2017 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DASH_QUORUMS_INSTANTX_H
#define DASH_QUORUMS_INSTANTX_H

#include "quorums_signing.h"

#include "primitives/transaction.h"

namespace llmq
{

class CInstantXManager
{
private:
    CCriticalSection cs;

public:
    void ProcessTx(CNode* pfrom, const CTransaction& tx, CConnman& connman, const Consensus::Params& params);
    bool IsLocked(const CTransaction& tx, const Consensus::Params& params);
    bool IsConflicting(const CTransaction& tx, const Consensus::Params& params);
};

extern CInstantXManager quorumInstantXManager;

}

#endif//DASH_QUORUMS_INSTANTX_H
