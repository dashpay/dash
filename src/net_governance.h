// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NET_GOVERNANCE_H
#define BITCOIN_NET_GOVERNANCE_H

#include <net_processing.h>

class CGovernanceManager;
class CMasternodeSync;
class CNetFulfilledRequestManager;

class NetGovernance final : public NetHandler
{
public:
    NetGovernance(PeerManagerInternal* peer_manager, CGovernanceManager& gov_manager, CMasternodeSync& node_sync, CNetFulfilledRequestManager& netfulfilledman) :
        NetHandler(peer_manager),
        m_gov_manager(gov_manager),
        m_netfulfilledman(netfulfilledman)
    {
    }
    void Schedule(CScheduler& scheduler, CConnman& connman) override;
private:
    void ProcessTick(CConnman& connman);
    void SendGovernanceSyncRequest(CNode* pnode) const;

    CGovernanceManager& m_gov_manager;
    CMasternodeSync& m_node_sync;
    CNetFulfilledRequestManager& m_netfulfilledman;
};

#endif // BITCOIN_NET_GOVERNANCE_H
