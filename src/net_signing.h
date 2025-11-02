// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.


#ifndef BITCOIN_NET_SIGNING_H
#define BITCOIN_NET_SIGNING_H

#include <net_processing.h>
#include <validationinterface.h>
#include <util/threadinterrupt.h>

#include <memory>

namespace llmq {
class CSigSharesManager;
class CSigningManager;
} // namespace llmq
class NetSigning final : public NetHandler, public CValidationInterface
{
public:
    NetSigning(PeerManagerInternal* peer_manager, llmq::CSigningManager& sig_manager, llmq::CSigSharesManager* shares_manager)
       : NetHandler(peer_manager), m_sig_manager(sig_manager), m_shares_manager(shares_manager)
    {
        workInterrupt.reset();
    }
    void ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv) override;

protected:
    // CValidationInterface
    void NotifyRecoveredSig(const std::shared_ptr<const llmq::CRecoveredSig>& sig) override;

    // NetSigning
    void Start() override;
    void Stop() override;
    void Interrupt() override { workInterrupt(); };

    void WorkThreadSigning();
    void WorkThreadShares();

private:
    bool ProcessPendingSigShares();
    bool ProcessPendingRecoveredSigs();
    void ProcessRecoveredSig(const std::shared_ptr<const llmq::CRecoveredSig>& recoveredSig);
    void BanNode(NodeId id);
    void RemoveBannedNodeStates();

private:
    llmq::CSigningManager& m_sig_manager;
    llmq::CSigSharesManager* m_shares_manager;

    std::thread signing_thread;
    std::thread shares_thread;

    CThreadInterrupt workInterrupt;
};

#endif // BITCOIN_NET_SIGNING_H
