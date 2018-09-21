// Copyright (c) 2017 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "quorums_instantx.h"

#include "chainparams.h"

namespace llmq
{

CInstantXManager quorumInstantXManager;

void CInstantXManager::ProcessTx(CNode* pfrom, const CTransaction& tx, CConnman& connman, const Consensus::Params& params)
{
    auto llmqType = params.llmqForInstantSend;
    if (llmqType == Consensus::LLMQ_NONE) {
        return;
    }

    if (fMasternodeMode) {
        auto quorum = quorumManager->GetNewestQuorum(llmqType);
        if (quorum != nullptr) {
            for (const auto& in : tx.vin) {
                uint256 id = ::SerializeHash(in.prevout);
                quorumsSigningManager->AsyncSignIfMember(llmqType, id, tx.GetHash());
            }
        }
    }
}

bool CInstantXManager::IsLocked(const CTransaction& tx, const Consensus::Params& params)
{
    auto llmqType = params.llmqForInstantSend;
    if (llmqType == Consensus::LLMQ_NONE) {
        return false;
    }

    for (const auto& in : tx.vin) {
        uint256 id = ::SerializeHash(in.prevout);
        if (!quorumsSigningManager->HasRecoveredSig(llmqType, id, tx.GetHash())) {
            return false;
        }
    }
    return true;
}

bool CInstantXManager::IsConflicting(const CTransaction& tx, const Consensus::Params& params)
{
    auto llmqType = params.llmqForInstantSend;
    if (llmqType == Consensus::LLMQ_NONE) {
        return false;
    }

    for (const auto& in : tx.vin) {
        uint256 id = ::SerializeHash(in.prevout);
        if (quorumsSigningManager->IsConflicting(llmqType, id, tx.GetHash())) {
            return true;
        }
    }
    return false;
}

}
