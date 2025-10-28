// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <net_instantsend.h>
#include <instantsend/instantsend.h>
#include <util/thread.h>

void NetInstantSend::ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv)
{
    if (msg_type != NetMsgType::ISDLOCK) {
        return;
    }

    if (!m_is_manager.IsInstantSendEnabled()) return;

    // TODO: consider removing shared_ptr from this islock
    const auto islock = std::make_shared<instantsend::InstantSendLock>();
    vRecv >> *islock;

    uint256 hash = ::SerializeHash(*islock);

    WITH_LOCK(::cs_main, m_peer_manager->PeerEraseObjectRequest(pfrom.GetId(), CInv{MSG_ISDLOCK, hash}));

    if (!islock->TriviallyValid()) {
        m_peer_manager->PeerMisbehaving(pfrom.GetId(), 100);
        return;
    }

    int block_height = m_is_manager.GetCycleBlockHeight(islock->cycleHash);
    if (block_height < 0) {
        // Maybe we don't have the block yet or maybe some peer spams invalid values for cycleHash
        m_peer_manager->PeerMisbehaving(pfrom.GetId(), 1);
        return;
    }

    // Deterministic islocks MUST use rotation based llmq
    auto llmqType = Params().GetConsensus().llmqTypeDIP0024InstantSend;
    const auto& llmq_params_opt = Params().GetLLMQ(llmqType);
    assert(llmq_params_opt);
    if (block_height % llmq_params_opt->dkgInterval != 0) {
        m_peer_manager->PeerMisbehaving(pfrom.GetId(), 100);
        return;
    }

    if (!m_is_manager.IsKnownInstantSend(hash)) {
        LogPrint(BCLog::INSTANTSEND, "CInstantSendManager::%s -- txid=%s, islock=%s: received islock, peer=%d\n", __func__,
                 islock->txid.ToString(), hash.ToString(), pfrom.GetId());

        m_is_manager.EnqueueInstantSendLock(pfrom.GetId(), hash, islock);
    }
}

void NetInstantSend::Start()
{
    // can't start new thread if we have one running already
    if (workThread.joinable()) {
        assert(false);
    }

    workThread = std::thread(&util::TraceThread, "isman", [this] { WorkThreadMain(); });

    // TODO: doubing that it works at all reliable even in previous implementation
    if (auto signer = m_is_manager.Signer(); signer) {
        signer->Start();
    }
}

void NetInstantSend::Stop()
{
    if (auto signer = m_is_manager.Signer(); signer) {
        signer->Stop();
    }

    // make sure to call InterruptWorkerThread() first
    if (!workInterrupt) {
        assert(false);
    }

    if (workThread.joinable()) {
        workThread.join();
    }
}


void NetInstantSend::ProcessPendingISLocks(const Uint256HashMap<std::pair<NodeId, instantsend::InstantSendLockPtr>>& locks_to_process)
{
    // TODO Investigate if leaving this is ok
    auto llmqType = Params().GetConsensus().llmqTypeDIP0024InstantSend;
    const auto& llmq_params_opt = Params().GetLLMQ(llmqType);
    assert(llmq_params_opt);
    const auto& llmq_params = llmq_params_opt.value();
    auto dkgInterval = llmq_params.dkgInterval;

    // First check against the current active set and don't ban
    auto bad_is_locks = ProcessPendingInstantSendLocks(llmq_params, /*signOffset=*/0, /*ban=*/false, locks_to_process, ret.m_peer_activity);
    if (!bad_is_locks.empty()) {
        LogPrint(BCLog::INSTANTSEND, "CInstantSendManager::%s -- doing verification on old active set\n", __func__);

        // filter out valid IS locks from "locks_to_process"
        for (auto it = locks_to_process.begin(); it != locks_to_process.end();) {
            if (bad_is_locks.find(it->first) == bad_is_locks.end()) {
                it = locks_to_process.erase(it);
            } else {
                ++it;
            }
        }
        // Now check against the previous active set and perform banning if this fails
        ProcessPendingInstantSendLocks(llmq_params, dkgInterval, /*ban=*/true, locks_to_process, ret.m_peer_activity);
    }
}

void NetInstantSend::WorkThreadMain()
{
    while (!workInterrupt) {
        bool fMoreWork = [&]() -> bool {
            if (!m_is_manager.IsInstantSendEnabled()) return false;

            auto [more_work, locks] = m_is_manager.GetPendingLocks();
            ProcessPendingISLocks(locks);
            /*
            for (auto& [node_id, mpr] : peer_activity) {
                m_peer_manager.PostProcessMessage(std::move(mpr), node_id);
            }
            */
            auto signer = m_is_manager.Signer();
            if (!signer) return more_work;

            signer->ProcessPendingRetryLockTxs(m_is_manager.PrepareTxToRetry());
            return more_work;
        }();

        if (!fMoreWork && !workInterrupt.sleep_for(std::chrono::milliseconds(100))) {
            return;
        }
    }
}

