// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LLMQ_NET_SIGNING_H
#define BITCOIN_LLMQ_NET_SIGNING_H

#include <ctpl_stl.h>
#include <net_processing.h>
#include <util/threadinterrupt.h>
#include <util/time.h>
#include <validationinterface.h>

#include <thread>

#include <memory>

class CSporkManager;

namespace llmq {
class CSigSharesManager;
class CSigningManager;

class NetSigning final : public NetHandler, public CValidationInterface
{
public:
    NetSigning(PeerManagerInternal* peer_manager, CSigningManager& sig_manager, CSigSharesManager* shares_manager,
               const CSporkManager& sporkman) :
        NetHandler(peer_manager),
        m_sig_manager{sig_manager},
        m_shares_manager{shares_manager},
        m_sporkman{sporkman}
    {
        workInterrupt.reset();
    }
    void ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv) override;

    [[nodiscard]] bool ProcessPendingRecoveredSigs();
    void ProcessRecoveredSig(std::shared_ptr<const CRecoveredSig> recoveredSig, bool consider_proactive_relay);

protected:
    // CValidationInterface
    void NotifyRecoveredSig(const std::shared_ptr<const CRecoveredSig>& sig, bool proactive_relay) override;

    // NetSigning
    void Start() override;
    void Stop() override;
    void Interrupt() override { workInterrupt(); };

private:
    void WorkThreadSigning();
    void WorkThreadCleaning();
    void WorkThreadDispatcher();

    bool ProcessPendingSigShares();

private:
    CSigningManager& m_sig_manager;
    CSigSharesManager* m_shares_manager;
    const CSporkManager& m_sporkman;

    CleanupThrottler<NodeClock> cleanupThrottler;

    std::thread signing_thread;
    std::thread shares_cleaning_thread;
    std::thread shares_dispatcher_thread;
    mutable ctpl::thread_pool worker_pool;

    CThreadInterrupt workInterrupt;
};

} // namespace llmq

#endif // BITCOIN_LLMQ_NET_SIGNING_H
