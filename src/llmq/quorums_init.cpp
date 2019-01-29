// Copyright (c) 2018-2019 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "quorums_init.h"

#include "quorums.h"
#include "quorums_blockprocessor.h"
#include "quorums_commitment.h"
#include "quorums_chainlocks.h"
#include "quorums_debug.h"
#include "quorums_dkgsessionmgr.h"
#include "quorums_signing.h"
#include "quorums_signing_shares.h"

#include "scheduler.h"

namespace llmq
{

static CBLSWorker blsWorker;

void InitLLMQSystem(CEvoDB& evoDb, CScheduler* scheduler, bool unitTests)
{
    quorumDKGDebugManager = new CDKGDebugManager(scheduler);
    quorumBlockProcessor = new CQuorumBlockProcessor(evoDb);
    quorumDKGSessionManager = new CDKGSessionManager(evoDb, blsWorker);
    quorumManager = new CQuorumManager(evoDb, blsWorker, *quorumDKGSessionManager);
    quorumSigSharesManager = new CSigSharesManager();
    quorumSigningManager = new CSigningManager(unitTests);
    chainLocksHandler = new CChainLocksHandler(scheduler);

    quorumSigSharesManager->StartWorkerThread();
}

void DestroyLLMQSystem()
{
    LogPrintf("DestroyLLMQSystem 1\n");
    if (quorumSigSharesManager) {
        quorumSigSharesManager->StopWorkerThread();
    }
    LogPrintf("DestroyLLMQSystem 2\n");

    delete chainLocksHandler;
    chainLocksHandler = nullptr;
    delete quorumSigningManager;
    quorumSigningManager = nullptr;
    LogPrintf("DestroyLLMQSystem 3\n");
    delete quorumSigSharesManager;
    quorumSigSharesManager = nullptr;
    LogPrintf("DestroyLLMQSystem 4\n");
    delete quorumManager;
    quorumManager = NULL;
    LogPrintf("DestroyLLMQSystem 5\n");
    delete quorumDKGSessionManager;
    quorumDKGSessionManager = NULL;
    LogPrintf("DestroyLLMQSystem 6\n");
    delete quorumBlockProcessor;
    quorumBlockProcessor = nullptr;
    LogPrintf("DestroyLLMQSystem 7\n");
    delete quorumDKGDebugManager;
    quorumDKGDebugManager = nullptr;
    LogPrintf("DestroyLLMQSystem 8\n");
}

}
