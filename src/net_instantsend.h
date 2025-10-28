// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NET_INSTANTSEND_H
#define BITCOIN_NET_INSTANTSEND_H

#include <net_processing.h>

#include <util/threadinterrupt.h>

namespace instantsend
{
struct InstantSendLock;
using InstantSendLockPtr = std::shared_ptr<InstantSendLock>;
} // namespace instantsend
namespace llmq {
class CInstantSendManager;
class CQuorumManager;
} // namespace llmq

class NetInstantSend final : public NetHandler
{
public:
    NetInstantSend(PeerManagerInternal* peer_manager, llmq::CInstantSendManager& is_manager, llmq::CQuorumManager& qman)
       : NetHandler(peer_manager), m_is_manager{is_manager}, m_qman(qman)
    {
        workInterrupt.reset();
    }
    void ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv) override;

    void Start() override;
    void Stop() override;
    void Interrupt() override { workInterrupt(); };

    void WorkThreadMain();
private:
    void ProcessPendingISLocks(Uint256HashMap<std::pair<NodeId, instantsend::InstantSendLockPtr>>&& locks_to_process);

    Uint256HashSet ProcessPendingInstantSendLocks(const Consensus::LLMQParams& llmq_params, int signOffset, bool ban,
                                                  const Uint256HashMap<std::pair<NodeId, instantsend::InstantSendLockPtr>>& pend);
    //    EXCLUSIVE_LOCKS_REQUIRED(!cs_nonLocked, !cs_pendingLocks, !cs_pendingRetry);
    llmq::CInstantSendManager& m_is_manager;
    llmq::CQuorumManager& m_qman;

    std::thread workThread;
    CThreadInterrupt workInterrupt;

};

#endif // BITCOIN_NET_INSTANTSEND_H
