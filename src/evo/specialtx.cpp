// Copyright (c) 2018-2023 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/specialtx.h>

#include <clientversion.h>
#include <consensus/validation.h>
#include <hash.h>

#include <evo/assetlocktx.h>
#include <evo/cbtx.h>
#include <evo/deterministicmns.h>
#include <evo/mnhftx.h>
#include <llmq/commitment.h>

bool CheckSpecialTxBasic(const CTransaction& tx, TxValidationState& state)
{
    // Context-free basic validation for special transactions - no chain state required
    if (!tx.HasExtraPayloadField()) {
        // Not a special transaction, nothing to check
        return true;
    }

    switch (tx.nType) {
    case TRANSACTION_PROVIDER_REGISTER:
        return CheckProRegTxBasic(tx, state);
    case TRANSACTION_PROVIDER_UPDATE_SERVICE:
        return CheckProUpServTxBasic(tx, state);
    case TRANSACTION_PROVIDER_UPDATE_REGISTRAR:
        return CheckProUpRegTxBasic(tx, state);
    case TRANSACTION_PROVIDER_UPDATE_REVOKE:
        return CheckProUpRevTxBasic(tx, state);
    case TRANSACTION_COINBASE:
        return CheckCbTxBasic(tx, state);
    case TRANSACTION_QUORUM_COMMITMENT:
        return llmq::CheckLLMQCommitmentBasic(tx, state);
    case TRANSACTION_MNHF_SIGNAL:
        return CheckMNHFTxBasic(tx, state);
    case TRANSACTION_ASSET_LOCK:
        return CheckAssetLockTx(tx, state);  // Already context-free
    case TRANSACTION_ASSET_UNLOCK:
        return CheckAssetUnlockTxBasic(tx, state);
    default:
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-tx-type");
    }
}

uint256 CalcTxInputsHash(const CTransaction& tx)
{
    CHashWriter hw(SER_GETHASH, CLIENT_VERSION);
    for (const auto& in : tx.vin) {
        hw << in.prevout;
    }
    return hw.GetHash();
}
