// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NET_GOVERNANCE_H
#define BITCOIN_NET_GOVERNANCE_H

#include <net_processing.h>

//#include <util/threadinterrupt.h>

class CGovernanceManager;

class NetGovernance final : public NetHandler
{
public:
    NetGovernance(PeerManagerInternal* peer_manager, CGovernanceManager& gov_manager)
       : NetHandler(peer_manager), m_gov_manager(gov_manager)
    {
    }
    void Schedule(CScheduler& scheduler, CConnman& connman) override;
private:
    CGovernanceManager& m_gov_manager;
};

#endif // BITCOIN_NET_GOVERNANCE_H
