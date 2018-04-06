// Copyright (c) 2017 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "cbtx.h"
#include "specialtx.h"
#include "deterministicmns.h"

#include "validation.h"
#include "univalue.h"

bool CheckCbTx(const CTransaction& tx, const CBlockIndex* pindex, CValidationState& state)
{
    AssertLockHeld(cs_main);

    if (!tx.IsCoinBase())
        return state.DoS(100, false, REJECT_INVALID, "bad-cbtx-invalid");

    CCbTx cbTx;
    if (!GetTxPayload(tx, cbTx))
        return state.DoS(100, false, REJECT_INVALID, "bad-tx-payload");

    if (cbTx.nVersion != CCbTx::CURRENT_VERSION)
        return state.DoS(100, false, REJECT_INVALID, "bad-cbtx-version");

    if (pindex) {
        if (pindex->nHeight != cbTx.height)
            return state.DoS(100, false, REJECT_INVALID, "bad-cbtx-height");
    }

    return true;
}

std::string CCbTx::ToString() const
{
    return strprintf("CCbTx(height=%d, merkleRootMNList=%s)",
        height, merkleRootMNList.ToString());
}

void CCbTx::ToJson(UniValue& obj) const
{
    obj.clear();
    obj.setObject();
    obj.push_back(Pair("height", (int)height));
    obj.push_back(Pair("merkleRootMNList", merkleRootMNList.ToString()));
}
