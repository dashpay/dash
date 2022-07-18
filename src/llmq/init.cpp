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

std::shared_ptr<CBLSWorker> blsWorker; // shared between quorumDKGSessionManager and quorumManager

void InitLLMQSystem(NodeContext& node, CEvoDB& evoDb, CTxMemPool& mempool, CConnman& connman, bool unitTests, bool fWipe)
{
    blsWorker = std::make_shared<CBLSWorker>();

    node.quorumDKGDebugManager = std::make_unique<CDKGDebugManager>();
    node.quorumBlockProcessor = std::make_unique<CQuorumBlockProcessor>(evoDb, connman);
    node.quorumDKGSessionManager = std::make_unique<CDKGSessionManager>(connman, blsWorker, *node.quorumDKGDebugManager, unitTests, fWipe);
    node.quorumManager = std::make_unique<CQuorumManager>(evoDb, connman, blsWorker, *node.quorumBlockProcessor, *node.quorumDKGSessionManager);
    node.quorumSigSharesManager = std::make_unique<CSigSharesManager>(connman, node);
    node.quorumSigningManager = std::make_unique<CSigningManager>(connman, *node.quorumManager, *node.quorumSigSharesManager, unitTests, fWipe);
    node.chainLocksHandler = std::make_unique<CChainLocksHandler>(mempool, connman, node);
    node.quorumInstantSendManager = std::make_unique<CInstantSendManager>(mempool, connman, node, unitTests, fWipe);

    // NOTE: we use this only to wipe the old db, do NOT use it for anything else
    // TODO: remove it in some future version
    auto llmqDbTmp = std::make_unique<CDBWrapper>(unitTests ? "" : (GetDataDir() / "llmq"), 1 << 20, unitTests, true);
}

void DestroyLLMQSystem()
{
    LOCK(cs_llmq_vbc);
    llmq_versionbitscache.Clear();
}

void StartLLMQSystem(NodeContext& node)
{
    if (blsWorker) {
        blsWorker->Start();
    }
    if (node.quorumDKGSessionManager) {
        node.quorumDKGSessionManager->StartThreads();
    }
    if (node.quorumManager) {
        node.quorumManager->Start();
    }
    if (node.quorumSigSharesManager != nullptr && node.quorumSigningManager != nullptr) {
        node.quorumSigningManager->RegisterRecoveredSigsListener(node.quorumSigSharesManager.get());
        node.quorumSigSharesManager->StartWorkerThread();
    }
    if (node.chainLocksHandler) {
        node.chainLocksHandler->Start();
    }
    if (node.quorumInstantSendManager) {
        node.quorumInstantSendManager->Start();
    }
}

void StopLLMQSystem(NodeContext& node)
{
    if (node.quorumInstantSendManager) {
        node.quorumInstantSendManager->Stop();
    }
    if (node.chainLocksHandler) {
        node.chainLocksHandler->Stop();
    }
    if (node.quorumSigSharesManager != nullptr && node.quorumSigningManager != nullptr) {
        node.quorumSigSharesManager->StopWorkerThread();
        node.quorumSigningManager->UnregisterRecoveredSigsListener(node.quorumSigSharesManager.get());
    }
    if (node.quorumManager) {
        node.quorumManager->Stop();
    }
    if (node.quorumDKGSessionManager) {
        node.quorumDKGSessionManager->StopThreads();
    }
    if (blsWorker) {
        blsWorker->Stop();
    }
}

void InterruptLLMQSystem(NodeContext& node)
{
    if (node.quorumSigSharesManager) {
        node.quorumSigSharesManager->InterruptWorkerThread();
    }
    if (node.quorumInstantSendManager) {
        node.quorumInstantSendManager->InterruptWorkerThread();
    }
}

} // namespace llmq
