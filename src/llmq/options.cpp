// Copyright (c) 2018-2023 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/options.h>

#include <chainparams.h>
#include <consensus/params.h>
#include <deploymentstatus.h>
#include <spork.h>

namespace llmq
{

static bool EvalSpork(Consensus::LLMQType llmqType, int64_t spork_value)
{
    if (spork_value == 0) {
        return true;
    }
    if (spork_value == 1 && llmqType != Consensus::LLMQType::LLMQ_100_67 && llmqType != Consensus::LLMQType::LLMQ_400_60 && llmqType != Consensus::LLMQType::LLMQ_400_85) {
        return true;
    }
    return false;
}

bool IsAllMembersConnectedEnabled(Consensus::LLMQType llmqType)
{
    return EvalSpork(llmqType, sporkManager->GetSporkValue(SPORK_21_QUORUM_ALL_CONNECTED));
}

bool IsQuorumPoseEnabled(Consensus::LLMQType llmqType)
{
    return EvalSpork(llmqType, sporkManager->GetSporkValue(SPORK_23_QUORUM_POSE));
}


bool IsQuorumRotationEnabled(const Consensus::LLMQParams& llmqParams, gsl::not_null<const CBlockIndex*> pindex)
{
    if (!llmqParams.useRotation) {
        return false;
    }

    int cycleQuorumBaseHeight = pindex->nHeight - (pindex->nHeight % llmqParams.dkgInterval);
    if (cycleQuorumBaseHeight < 1) {
        return false;
    }
    // It should activate at least 1 block prior to the cycle start
    return DeploymentActiveAfter(pindex->GetAncestor(cycleQuorumBaseHeight - 1), Params().GetConsensus(), Consensus::DEPLOYMENT_DIP0024);
}

} // namespace llmq
