// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <net_governance.h>

#include <chainparams.h>
#include <evo/deterministicmns.h>
#include <governance/governance.h>
#include <logging.h>
#include <net.h>
#include <netfulfilledman.h>
#include <netmessagemaker.h>
#include <node/interface_ui.h>
#include <node/sync.h>
#include <scheduler.h>
#include <shutdown.h>

class CConnman;

void NetGovernance::Schedule(CScheduler& scheduler, CConnman& connman)
{
    if (!m_gov_manager.IsValid()) return;

    scheduler.scheduleEvery(
            [this, &connman]() -> void {
                if (ShutdownRequested()) return;
                ProcessTick(connman);
            },
         std::chrono::seconds{1});

    scheduler.scheduleEvery(
        [this, &connman]() -> void {
            if (!m_node_sync.IsSynced()) return;

            // CHECK OBJECTS WE'VE ASKED FOR, REMOVE OLD ENTRIES
            m_gov_manager.CleanOrphanObjects();
            m_gov_manager.RequestOrphanObjects(connman);

            // CHECK AND REMOVE - REPROCESS GOVERNANCE OBJECTS
            m_gov_manager.CheckAndRemove();
        },
        std::chrono::minutes{5});

    scheduler.scheduleEvery(
        [this]() -> void {
            auto relay_invs = m_gov_manager.FetchRelayInventory();
            for (const auto& inv : relay_invs) {
                m_peer_manager->PeerRelayInv(inv);
            }
        },
        // Tests need tighter timings to avoid timeouts, use more relaxed pacing otherwise
        Params().IsMockableChain() ? std::chrono::seconds{1} : std::chrono::seconds{5});
}

void NetGovernance::SendGovernanceSyncRequest(CNode* pnode, CConnman& connman) const
{
    CNetMsgMaker msgMaker(pnode->GetCommonVersion());
    CBloomFilter filter;
    connman.PushMessage(pnode, msgMaker.Make(NetMsgType::MNGOVERNANCESYNC, uint256(), filter));
}

int NetGovernance::RequestGovernanceObjectVotes(const std::vector<CNode*>& vNodesCopy, CConnman& connman) const
{
    if (vNodesCopy.empty()) return -1;

    int64_t now = GetTime();

    // Maximum number of nodes to request votes from for the same object hash on real networks
    // (mainnet, testnet, devnets). Keep this low to avoid unnecessary bandwidth usage.
    static constexpr size_t REALNET_PEERS_PER_HASH{3};
    // Maximum number of nodes to request votes from for the same object hash on regtest.
    // During testing, nodes are isolated to create conflicting triggers. Using the real
    // networks limit of 3 nodes often results in querying only "non-isolated" nodes, missing the
    // isolated ones we need to test. This high limit ensures all available nodes are queried.
    static constexpr size_t REGTEST_PEERS_PER_HASH{std::numeric_limits<size_t>::max()};

    size_t peers_per_hash_max = Params().IsMockableChain() ? REGTEST_PEERS_PER_HASH : REALNET_PEERS_PER_HASH;

    // TODO: add a LOCK to guard mapAskedRecently. It's unprotected now and has been for awhile
    static std::map<uint256, std::map<CService, int64_t> > mapAskedRecently;
    auto [vTriggerObjHashes, vOtherObjHashes] = m_gov_manager.PrepareVotesToRequest(vNodesCopy, mapAskedRecently, now, peers_per_hash_max);

    if (vTriggerObjHashes.empty() && vOtherObjHashes.empty()) return -2;

    // This should help us to get some idea about an impact this can bring once deployed on mainnet.
    // Testnet is ~40 times smaller in masternode count, but only ~1000 masternodes usually vote,
    // so 1 obj on mainnet == ~10 objs or ~1000 votes on testnet. However we want to test a higher
    // number of votes to make sure it's robust enough, so aim at 2000 votes per masternode per request.
    // On mainnet nMaxObjRequestsPerNode is always set to 1.
    int nMaxObjRequestsPerNode = 1;
    size_t nProjectedVotes = 2000;
    if (Params().NetworkIDString() != CBaseChainParams::MAIN) {
        nMaxObjRequestsPerNode = std::max(1, int(nProjectedVotes / std::max(1, (int)m_gov_manager.GetMNManager().GetListAtChainTip().GetValidMNsCount())));
    }

    int timeout = 60 * 60;
    for (int i = 0; i < nMaxObjRequestsPerNode; ++i) {
        uint256 nHashGovobj;

        // ask for triggers first
        if (!vTriggerObjHashes.empty()) {
            nHashGovobj = vTriggerObjHashes.back();
        } else {
            if (vOtherObjHashes.empty()) break;
            nHashGovobj = vOtherObjHashes.back();
        }
        bool fAsked = false;
        for (const auto& pnode : vNodesCopy) {
            // Don't try to sync any data from outbound non-relay "masternode" connections.
            // Inbound connection this early is most likely a "masternode" connection
            // initiated from another node, so skip it too.
            if (!pnode->CanRelay() || (connman.IsActiveMasternode() && pnode->IsInboundConn())) continue;
            // stop early to prevent setAskFor overflow
            {
                LOCK(::cs_main);
                size_t nProjectedSize = m_peer_manager->PeerGetRequestedObjectCount(pnode->GetId()) + nProjectedVotes;
                if (nProjectedSize > MAX_INV_SZ) continue;
                // to early to ask the same node
                if (mapAskedRecently[nHashGovobj].count(pnode->addr)) continue;
            }

            m_gov_manager.RequestGovernanceObject(pnode, nHashGovobj, connman, true);
            mapAskedRecently[nHashGovobj][pnode->addr] = now + timeout;
            fAsked = true;
            // stop loop if max number of peers per obj was asked
            if (mapAskedRecently[nHashGovobj].size() >= peers_per_hash_max) break;
        }
        // NOTE: this should match `if` above (the one before `while`)
        if (!vTriggerObjHashes.empty()) {
            vTriggerObjHashes.pop_back();
        } else {
            vOtherObjHashes.pop_back();
        }
        if (!fAsked) i--;
    }
    LogPrint(BCLog::GOBJECT, "NetGovernance::RequestGovernanceObjectVotes -- end: vTriggerObjHashes %d vOtherObjHashes %d mapAskedRecently %d\n",
        vTriggerObjHashes.size(), vOtherObjHashes.size(), mapAskedRecently.size());

    return int(vTriggerObjHashes.size() + vOtherObjHashes.size());
}

void NetGovernance::ProcessTick(CConnman& connman)
{
    assert(m_netfulfilledman.IsValid());

    static int nTick = 0;
    nTick++;

    const static int64_t nSyncStart = TicksSinceEpoch<std::chrono::milliseconds>(SystemClock::now());
    const static std::string strAllow = strprintf("allow-sync-%lld", nSyncStart);

    // reset the sync process if the last call to this function was more than 60 minutes ago (client was in sleep mode)
    static int64_t nTimeLastProcess = GetTime();
    if (!Params().IsMockableChain() && GetTime() - nTimeLastProcess > 60 * 60 && !connman.IsActiveMasternode()) {
        LogPrintf("NetGovernance::ProcessTick -- WARNING: no actions for too long, restarting sync...\n");
        m_node_sync.Reset(true);
        nTimeLastProcess = GetTime();
        return;
    }

    if(GetTime() - nTimeLastProcess < NODE_SYNC_TICK_SECONDS) {
        // too early, nothing to do here
        return;
    }

    nTimeLastProcess = GetTime();
    const CConnman::NodesSnapshot snap{connman, /* cond = */ CConnman::FullyConnectedOnly};

    // gradually request the rest of the votes after sync finished
    if(m_node_sync.IsSynced()) {
        RequestGovernanceObjectVotes(snap.Nodes(), connman);
        return;
    }

    // Calculate "progress" for LOG reporting / GUI notification
    int attempt = m_node_sync.GetAttempt();
    int asset_id = m_node_sync.GetAssetID();
    double nSyncProgress = double(attempt + (asset_id - 1) * 8) / (8*4);
    LogPrint(BCLog::MNSYNC, "CMasternodeSync::ProcessTick -- nTick %d asset_id %d nTriedPeerCount %d nSyncProgress %f\n", nTick, asset_id, attempt, nSyncProgress);
    uiInterface.NotifyAdditionalDataSyncProgressChanged(nSyncProgress);

    for (auto& pnode : snap.Nodes())
    {
        CNetMsgMaker msgMaker(pnode->GetCommonVersion());

        // Don't try to sync any data from outbound non-relay "masternode" connections.
        // Inbound connection this early is most likely a "masternode" connection
        // initiated from another node, so skip it too.
        if (!pnode->CanRelay() || (connman.IsActiveMasternode() && pnode->IsInboundConn())) continue;

        {
            if ((pnode->HasPermission(NetPermissionFlags::NoBan) || pnode->IsManualConn()) && !m_netfulfilledman.HasFulfilledRequest(pnode->addr, strAllow)) {
                m_netfulfilledman.RemoveAllFulfilledRequests(pnode->addr);
                m_netfulfilledman.AddFulfilledRequest(pnode->addr, strAllow);
                LogPrintf("CMasternodeSync::ProcessTick -- skipping mnsync restrictions for peer=%d\n", pnode->GetId());
            }

            if(m_netfulfilledman.HasFulfilledRequest(pnode->addr, "full-sync")) {
                // We already fully synced from this node recently,
                // disconnect to free this connection slot for another peer.
                pnode->fDisconnect = true;
                LogPrintf("CMasternodeSync::ProcessTick -- disconnecting from recently synced peer=%d\n", pnode->GetId());
                continue;
            }

            // SPORK : ALWAYS ASK FOR SPORKS AS WE SYNC

            if(!m_netfulfilledman.HasFulfilledRequest(pnode->addr, "spork-sync")) {
                // always get sporks first, only request once from each peer
                m_netfulfilledman.AddFulfilledRequest(pnode->addr, "spork-sync");
                // get current network sporks
                connman.PushMessage(pnode, msgMaker.Make(NetMsgType::GETSPORKS));
                LogPrint(BCLog::MNSYNC, "CMasternodeSync::ProcessTick -- nTick %d asset_id %d -- requesting sporks from peer=%d\n", nTick, asset_id, pnode->GetId());
            }

            if (asset_id == NODE_SYNC_BLOCKCHAIN) {
                int64_t nTimeSyncTimeout = snap.Nodes().size() > 3 ? NODE_SYNC_TICK_SECONDS : NODE_SYNC_TIMEOUT_SECONDS;
                if (m_node_sync.IsReachedBestHeader() && (GetTime() - m_node_sync.GetLastBump() > nTimeSyncTimeout)) {
                    // At this point we know that:
                    // a) there are peers (because we are looping on at least one of them);
                    // b) we waited for at least NODE_SYNC_TICK_SECONDS/NODE_SYNC_TIMEOUT_SECONDS
                    //    (depending on the number of connected peers) since we reached the headers tip the last
                    //    time (i.e. since fReachedBestHeader has been set to true);
                    // c) there were no blocks (UpdatedBlockTip, NotifyHeaderTip) or headers (AcceptedBlockHeader)
                    //    for at least NODE_SYNC_TICK_SECONDS/NODE_SYNC_TIMEOUT_SECONDS (depending on
                    //    the number of connected peers).
                    // We must be at the tip already, let's move to the next asset.
                    m_node_sync.SwitchToNextAsset();
                    uiInterface.NotifyAdditionalDataSyncProgressChanged(nSyncProgress);

                    if (gArgs.GetBoolArg("-syncmempool", DEFAULT_SYNC_MEMPOOL)) {
                        // Now that the blockchain is synced request the mempool from the connected outbound nodes if possible
                        for (auto pNodeTmp : snap.Nodes()) {
                            bool fRequestedEarlier = m_netfulfilledman.HasFulfilledRequest(pNodeTmp->addr, "mempool-sync");
                            if (!pNodeTmp->IsInboundConn() && !fRequestedEarlier && !pNodeTmp->IsBlockRelayOnly()) {
                                m_netfulfilledman.AddFulfilledRequest(pNodeTmp->addr, "mempool-sync");
                                connman.PushMessage(pNodeTmp, msgMaker.Make(NetMsgType::MEMPOOL));
                                LogPrint(BCLog::MNSYNC, "CMasternodeSync::ProcessTick -- nTick %d asset_id %d -- syncing mempool from peer=%d\n", nTick, asset_id, pNodeTmp->GetId());
                            }
                        }
                    }
                }
            }

            // GOVOBJ : SYNC GOVERNANCE ITEMS FROM OUR PEERS

            if(asset_id == NODE_SYNC_GOVERNANCE) {
                if (!m_gov_manager.IsValid()) {
                    m_node_sync.SwitchToNextAsset();
                    return;
                }
                LogPrint(BCLog::GOBJECT, "CMasternodeSync::ProcessTick -- nTick %d asset_id %d last_bump %lld GetTime() %lld diff %lld\n", nTick, asset_id, m_node_sync.GetLastBump(), GetTime(), GetTime() - m_node_sync.GetLastBump());

                // check for timeout first
                if(GetTime() - m_node_sync.GetLastBump() > NODE_SYNC_TIMEOUT_SECONDS) {
                    LogPrint(BCLog::MNSYNC, "CMasternodeSync::ProcessTick -- nTick %d asset_id %d -- timeout\n", nTick, asset_id);
                    if(attempt == 0) {
                        LogPrintf("CMasternodeSync::ProcessTick -- WARNING: failed to sync %s\n", m_node_sync.GetAssetName());
                        // it's kind of ok to skip this for now, hopefully we'll catch up later?
                    }
                    m_node_sync.SwitchToNextAsset();
                    return;
                }

                // only request obj sync once from each peer
                if(m_netfulfilledman.HasFulfilledRequest(pnode->addr, "governance-sync")) {
                    // will request votes on per-obj basis from each node in a separate loop below
                    // to avoid deadlocks here
                    continue;
                }
                m_netfulfilledman.AddFulfilledRequest(pnode->addr, "governance-sync");

                m_node_sync.BumpAttempt();

                SendGovernanceSyncRequest(pnode, connman);

                break; //this will cause each peer to get one request each six seconds for the various assets we need
            }
        }
    }


    if (asset_id != NODE_SYNC_GOVERNANCE) {
        // looped through all nodes and not syncing governance yet/already, release them
        return;
    }

    // request votes on per-obj basis from each node
    for (const auto& pnode : snap.Nodes()) {
        if(!m_netfulfilledman.HasFulfilledRequest(pnode->addr, "governance-sync")) {
            continue; // to early for this node
        }
        const std::vector<CNode*> vNodeCopy{pnode};
        int nObjsLeftToAsk = RequestGovernanceObjectVotes(vNodeCopy, connman);
        // check for data
        if(nObjsLeftToAsk == 0) {
            static int64_t nTimeNoObjectsLeft = 0;
            static int nLastTick = 0;
            static int nLastVotes = 0;
            if(nTimeNoObjectsLeft == 0) {
                // asked all objects for votes for the first time
                nTimeNoObjectsLeft = GetTime();
            }
            // make sure the condition below is checked only once per tick
            if(nLastTick == nTick) continue;
            if (GetTime() - nTimeNoObjectsLeft > NODE_SYNC_TIMEOUT_SECONDS &&
                m_gov_manager.GetVoteCount() - nLastVotes < std::max(int(0.0001 * nLastVotes), NODE_SYNC_TICK_SECONDS)) {
                // We already asked for all objects, waited for NODE_SYNC_TIMEOUT_SECONDS
                // after that and less then 0.01% or NODE_SYNC_TICK_SECONDS
                // (i.e. 1 per second) votes were received during the last tick.
                // We can be pretty sure that we are done syncing.
                LogPrintf("CMasternodeSync::ProcessTick -- nTick %d asset_id %d -- asked for all objects, nothing to do\n", nTick, NODE_SYNC_GOVERNANCE);
                // reset nTimeNoObjectsLeft to be able to use the same condition on resync
                nTimeNoObjectsLeft = 0;
                m_node_sync.SwitchToNextAsset();
                return;
            }
            nLastTick = nTick;
            nLastVotes = m_gov_manager.GetVoteCount();
        }
    }
}

