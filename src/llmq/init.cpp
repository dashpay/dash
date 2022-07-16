// Copyright (c) 2018-2022 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/init.h>

#include <llmq/quorums.h>
#include <llmq/blockprocessor.h>
#include <llmq/commitment.h>
#include <llmq/chainlocks.h>
#include <llmq/debug.h>
#include <llmq/dkgsessionmgr.h>
#include <llmq/instantsend.h>
#include <llmq/signing.h>
#include <llmq/signing_shares.h>
#include <llmq/utils.h>
#include <consensus/validation.h>

#include <node/context.h>

#include <dbwrapper.h>

namespace llmq
{

CBLSWorker* blsWorker;

void InitLLMQSystem(NodeContext& node, CEvoDB& evoDb, CTxMemPool& mempool, CConnman& connman, bool unitTests, bool fWipe)
{
    blsWorker = new CBLSWorker();

    node.quorumDKGDebugManager = std::make_unique<CDKGDebugManager>();
    quorumBlockProcessor = new CQuorumBlockProcessor(evoDb, connman);
    quorumDKGSessionManager = new CDKGSessionManager(connman, *blsWorker, *node.quorumDKGDebugManager, unitTests, fWipe);
    quorumManager = new CQuorumManager(evoDb, connman, *blsWorker, *quorumDKGSessionManager);
    quorumSigSharesManager = new CSigSharesManager(connman, *quorumManager);
    quorumSigningManager = new CSigningManager(connman, *quorumManager, *quorumSigSharesManager, unitTests, fWipe);
    chainLocksHandler = new CChainLocksHandler(mempool, connman, *quorumSigningManager);
    quorumInstantSendManager = new CInstantSendManager(mempool, connman, *chainLocksHandler, *quorumSigningManager, unitTests, fWipe);

    // NOTE: we use this only to wipe the old db, do NOT use it for anything else
    // TODO: remove it in some future version
    auto llmqDbTmp = std::make_unique<CDBWrapper>(unitTests ? "" : (GetDataDir() / "llmq"), 1 << 20, unitTests, true);
}

void DestroyLLMQSystem()
{
    delete quorumInstantSendManager;
    quorumInstantSendManager = nullptr;
    delete chainLocksHandler;
    chainLocksHandler = nullptr;
    delete quorumSigningManager;
    quorumSigningManager = nullptr;
    delete quorumSigSharesManager;
    quorumSigSharesManager = nullptr;
    delete quorumManager;
    quorumManager = nullptr;
    delete quorumDKGSessionManager;
    quorumDKGSessionManager = nullptr;
    delete quorumBlockProcessor;
    quorumBlockProcessor = nullptr;
    delete blsWorker;
    blsWorker = nullptr;
    LOCK(cs_llmq_vbc);
    llmq_versionbitscache.Clear();
}

void StartLLMQSystem()
{
    if (blsWorker) {
        blsWorker->Start();
    }
    if (quorumDKGSessionManager) {
        quorumDKGSessionManager->StartThreads();
    }
    if (quorumManager) {
        quorumManager->Start();
    }
    if (quorumSigSharesManager != nullptr && quorumSigningManager != nullptr) {
        quorumSigningManager->RegisterRecoveredSigsListener(quorumSigSharesManager);
        quorumSigSharesManager->StartWorkerThread();
    }
    if (chainLocksHandler) {
        chainLocksHandler->Start();
    }
    if (quorumInstantSendManager) {
        quorumInstantSendManager->Start();
    }
}

void StopLLMQSystem()
{
    if (quorumInstantSendManager) {
        quorumInstantSendManager->Stop();
    }
    if (chainLocksHandler) {
        chainLocksHandler->Stop();
    }
    if (quorumSigSharesManager != nullptr && quorumSigningManager != nullptr) {
        quorumSigSharesManager->StopWorkerThread();
        quorumSigningManager->UnregisterRecoveredSigsListener(quorumSigSharesManager);
    }
    if (quorumManager) {
        quorumManager->Stop();
    }
    if (quorumDKGSessionManager) {
        quorumDKGSessionManager->StopThreads();
    }
    if (blsWorker) {
        blsWorker->Stop();
    }
}

void InterruptLLMQSystem()
{
    if (quorumSigSharesManager) {
        quorumSigSharesManager->InterruptWorkerThread();
    }
    if (quorumInstantSendManager) {
        quorumInstantSendManager->InterruptWorkerThread();
    }
}

} // namespace llmq
