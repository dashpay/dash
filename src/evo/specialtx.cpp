// Copyright (c) 2018-2023 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/specialtx.h>

#include <clientversion.h>
#include <hash.h>

uint256 CalcTxInputsHash(const CTransaction& tx)
{
    HashWriter hw{};
    for (const auto& in : tx.vin) {
        hw << in.prevout;
    }
    return hw.GetHash();
}

uint256 CalcTxOutputsHash(const CTransaction& tx)
{
    HashWriter hw{};
    for (const auto& out : tx.vout) {
        hw << out;
    }
    return hw.GetHash();
}

bool IsProviderTx(const CTransaction& tx)
{
    if (!tx.IsSpecialTxVersion()) return false;
    switch (tx.nType) {
    case TRANSACTION_PROVIDER_REGISTER:
    case TRANSACTION_PROVIDER_UPDATE_SERVICE:
    case TRANSACTION_PROVIDER_UPDATE_REGISTRAR:
    case TRANSACTION_PROVIDER_UPDATE_REVOKE:
    case TRANSACTION_PROVIDER_DISSOLVE:
    case TRANSACTION_PROVIDER_UPDATE_SHARE:
    case TRANSACTION_PROVIDER_UPDATE_SHARED_REGISTRAR:
        return true;
    default:
        return false;
    }
}
