// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.


#ifndef BITCOIN_NET_INSTANTSEND_H
#define BITCOIN_NET_INSTANTSEND_H

#include <net_processing.h>

#include <util/threadinterrupt.h>
namespace llmq {
class CInstantSendManager;
}

class NetInstantSend : public NetHandler
{
public:
    NetInstantSend(PeerManagerInternal* peer_manager, llmq::CInstantSendManager& is_manager)
       : NetHandler(peer_manager), m_is_manager{is_manager}
    {
        workInterrupt.reset();
    }

    void ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv) override;

    void Start();
    void Stop();
    void InterruptWorkerThread() { workInterrupt(); };

    void WorkThreadMain();
private:
    void ProcessPendingISLocks(const Uint256HashMap<std::pair<NodeId, instantsend::InstantSendLockPtr>>& locks_to_process);

    llmq::CInstantSendManager& m_is_manager;

    std::thread workThread;
    CThreadInterrupt workInterrupt;

};

#endif // BITCOIN_NET_INSTANTSEND_H
