// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_GOVERNANCE_NET_GOVERNANCE_H
#define BITCOIN_GOVERNANCE_NET_GOVERNANCE_H

#include <net_processing.h>

class CGovernanceManager;
class CMasternodeSync;
class CNetFulfilledRequestManager;

class NetGovernance final : public NetHandler
{
public:
    NetGovernance(PeerManagerInternal* peer_manager, CGovernanceManager& gov_manager, CMasternodeSync& node_sync, CNetFulfilledRequestManager& netfulfilledman,
                  CConnman& connman) :
        NetHandler(peer_manager),
        m_gov_manager(gov_manager),
        m_node_sync(node_sync),
        m_netfulfilledman(netfulfilledman),
        m_connman(connman)
    {
    }
    //! How many parents of orphan votes RequestOrphanParents() asks for at a time
    static constexpr size_t MAX_ORPHAN_PARENT_REQUESTS{10};

    void Schedule(CScheduler& scheduler) override;
    /** Ask one random peer for a few of the parents that orphan votes wait for, in case no peer that
     *  relayed those votes is left to fetch them from */
    void RequestOrphanParents();

    void ProcessMessage(CNode& peer, const std::string& msg_type, DataStream& vRecv) override;

    bool AlreadyHave(const CInv& inv) override;
    bool DeferAnnouncement(const CInv& inv) override;
    bool ProcessGetData(CNode& pfrom, const CInv& inv, const CNetMsgMaker& msgMaker) override;

private:
    CGovernanceManager& m_gov_manager;
    CMasternodeSync& m_node_sync;
    CNetFulfilledRequestManager& m_netfulfilledman;
    CConnman& m_connman;
};

#endif // BITCOIN_GOVERNANCE_NET_GOVERNANCE_H
