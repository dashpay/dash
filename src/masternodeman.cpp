// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activemasternode.h"
#include "addrman.h"
#include "alert.h"
#include "clientversion.h"
#include "init.h"
#include "governance.h"
#include "masternode-payments.h"
#include "masternode-sync.h"
#include "masternodeman.h"
#include "messagesigner.h"
#include "netfulfilledman.h"
#include "netmessagemaker.h"
#ifdef ENABLE_WALLET
#include "privatesend-client.h"
#endif // ENABLE_WALLET
#include "script/standard.h"
#include "ui_interface.h"
#include "util.h"
#include "warnings.h"

#include "evo/deterministicmns.h"
#include "evo/providertx.h"

/** Masternode manager */
CMasternodeMan mnodeman;

const std::string CMasternodeMan::SERIALIZATION_VERSION_STRING = "CMasternodeMan-Version-13";
const int CMasternodeMan::LAST_PAID_SCAN_BLOCKS = 100;

struct CompareLastPaidBlock
{
    bool operator()(const std::pair<int, const CMasternode*>& t1,
                    const std::pair<int, const CMasternode*>& t2) const
    {
        return (t1.first != t2.first) ? (t1.first < t2.first) : (t1.second->outpoint < t2.second->outpoint);
    }
};

struct CompareScoreMN
{
    bool operator()(const std::pair<arith_uint256, const CMasternode*>& t1,
                    const std::pair<arith_uint256, const CMasternode*>& t2) const
    {
        return (t1.first != t2.first) ? (t1.first < t2.first) : (t1.second->outpoint < t2.second->outpoint);
    }
};

struct CompareByAddr

{
    bool operator()(const CMasternode* t1,
                    const CMasternode* t2) const
    {
        return t1->addr < t2->addr;
    }
};

CMasternodeMan::CMasternodeMan():
    cs(),
    mapMasternodes(),
    fMasternodesAdded(false),
    fMasternodesRemoved(false),
    vecDirtyGovernanceObjectHashes(),
    nDsqCount(0)
{}

bool CMasternodeMan::IsValidForMixingTxes(const COutPoint& outpoint)
{
    LOCK(cs);
    CMasternode* pmn = Find(outpoint);
    if (!pmn) {
        return false;
    }
    return pmn->IsValidForMixingTxes();
}

bool CMasternodeMan::AllowMixing(const COutPoint &outpoint)
{
    LOCK(cs);
    CMasternode* pmn = Find(outpoint);
    if (!pmn) {
        return false;
    }
    nDsqCount++;
    pmn->nLastDsq = nDsqCount;
    pmn->nMixingTxCount = 0;

    return true;
}

bool CMasternodeMan::DisallowMixing(const COutPoint &outpoint)
{
    LOCK(cs);
    CMasternode* pmn = Find(outpoint);
    if (!pmn) {
        return false;
    }
    pmn->nMixingTxCount++;

    return true;
}

void CMasternodeMan::AddDeterministicMasternodes()
{
    bool added = false;
    {
        LOCK(cs);
        unsigned int oldMnCount = mapMasternodes.size();

        auto mnList = deterministicMNManager->GetListAtChainTip();
        mnList.ForEachMN(true, [this](const CDeterministicMNCPtr& dmn) {
            // call Find() on each deterministic MN to force creation of CMasternode object
            auto mn = Find(dmn->collateralOutpoint);
            assert(mn);

            // make sure we use the splitted keys from now on
            mn->keyIDOwner = dmn->pdmnState->keyIDOwner;
            mn->blsPubKeyOperator = dmn->pdmnState->pubKeyOperator;
            mn->keyIDVoting = dmn->pdmnState->keyIDVoting;
            mn->addr = dmn->pdmnState->addr;
            mn->nProtocolVersion = DMN_PROTO_VERSION;

            // If it appeared in the valid list, it is enabled no matter what
            mn->nActiveState = CMasternode::MASTERNODE_ENABLED;
        });

        added = oldMnCount != mapMasternodes.size();
    }

    if (added) {
        NotifyMasternodeUpdates(*g_connman, true, false);
    }
}

void CMasternodeMan::RemoveNonDeterministicMasternodes()
{
    bool erased = false;
    {
        LOCK(cs);
        std::set<COutPoint> mnSet;
        auto mnList = deterministicMNManager->GetListAtChainTip();
        mnList.ForEachMN(true, [&](const CDeterministicMNCPtr& dmn) {
            mnSet.insert(dmn->collateralOutpoint);
        });
        auto it = mapMasternodes.begin();
        while (it != mapMasternodes.end()) {
            if (!mnSet.count(it->second.outpoint)) {
                mapMasternodes.erase(it++);
                erased = true;
            } else {
                ++it;
            }
        }
    }
    if (erased) {
        NotifyMasternodeUpdates(*g_connman, false, true);
    }
}

void CMasternodeMan::Clear()
{
    LOCK(cs);
    mapMasternodes.clear();
    nDsqCount = 0;
}

int CMasternodeMan::CountMasternodes()
{
    LOCK(cs);

    auto mnList = deterministicMNManager->GetListAtChainTip();
    return (int)mnList.GetAllMNsCount();
}

int CMasternodeMan::CountEnabled()
{
    LOCK(cs);

    auto mnList = deterministicMNManager->GetListAtChainTip();
    return (int)mnList.GetValidMNsCount();
}

/* Only IPv4 masternodes are allowed in 12.1, saving this for later
int CMasternodeMan::CountByIP(int nNetworkType)
{
    LOCK(cs);
    int nNodeCount = 0;

    for (const auto& mnpair : mapMasternodes)
        if ((nNetworkType == NET_IPV4 && mnpair.second.addr.IsIPv4()) ||
            (nNetworkType == NET_TOR  && mnpair.second.addr.IsTor())  ||
            (nNetworkType == NET_IPV6 && mnpair.second.addr.IsIPv6())) {
                nNodeCount++;
        }

    return nNodeCount;
}
*/

CMasternode* CMasternodeMan::Find(const COutPoint &outpoint)
{
    LOCK(cs);

    // This code keeps compatibility to old code depending on the non-deterministic MN lists
    // When deterministic MN lists get activated, we stop relying on the MNs we encountered due to MNBs and start
    // using the MNs found in the deterministic MN manager. To keep compatibility, we create CMasternode entries
    // for these and return them here. This is needed because we also need to track some data per MN that is not
    // on-chain, like vote counts

    auto mnList = deterministicMNManager->GetListAtChainTip();
    auto dmn = mnList.GetMNByCollateral(outpoint);
    if (!dmn || !mnList.IsMNValid(dmn)) {
        return nullptr;
    }

    auto it = mapMasternodes.find(outpoint);
    if (it != mapMasternodes.end()) {
        return &(it->second);
    } else {
        // MN is not in mapMasternodes but in the deterministic list. Create an entry in mapMasternodes for compatibility with legacy code
        CMasternode mn(outpoint.hash, dmn);
        it = mapMasternodes.emplace(outpoint, mn).first;
        return &(it->second);
    }
}

bool CMasternodeMan::GetMasternodeInfo(const uint256& proTxHash, masternode_info_t& mnInfoRet)
{
    auto dmn = deterministicMNManager->GetListAtChainTip().GetValidMN(proTxHash);
    if (!dmn)
        return false;
    return GetMasternodeInfo(dmn->collateralOutpoint, mnInfoRet);
}

bool CMasternodeMan::GetMasternodeInfo(const COutPoint& outpoint, masternode_info_t& mnInfoRet)
{
    LOCK(cs);
    CMasternode* mn = Find(outpoint);
    if (!mn)
        return false;
    mnInfoRet = mn->GetInfo();
    return true;
}

masternode_info_t CMasternodeMan::FindRandomNotInVec(const std::vector<COutPoint> &vecToExclude, int nProtocolVersion)
{
    LOCK(cs);

    int nCountEnabled = CountEnabled();
    int nCountNotExcluded = nCountEnabled - vecToExclude.size();

    LogPrintf("CMasternodeMan::FindRandomNotInVec -- %d enabled masternodes, %d masternodes to choose from\n", nCountEnabled, nCountNotExcluded);
    if(nCountNotExcluded < 1) return masternode_info_t();

    // fill a vector of pointers
    std::vector<const CMasternode*> vpMasternodesShuffled;
    for (const auto& mnpair : mapMasternodes) {
        vpMasternodesShuffled.push_back(&mnpair.second);
    }

    FastRandomContext insecure_rand;
    // shuffle pointers
    std::random_shuffle(vpMasternodesShuffled.begin(), vpMasternodesShuffled.end(), insecure_rand);
    bool fExclude;

    // loop through
    for (const auto& pmn : vpMasternodesShuffled) {
        if(pmn->nProtocolVersion < nProtocolVersion || !pmn->IsEnabled()) continue;
        fExclude = false;
        for (const auto& outpointToExclude : vecToExclude) {
            if(pmn->outpoint == outpointToExclude) {
                fExclude = true;
                break;
            }
        }
        if(fExclude) continue;
        if (!deterministicMNManager->HasValidMNCollateralAtChainTip(pmn->outpoint))
            continue;
        // found the one not in vecToExclude
        LogPrint("masternode", "CMasternodeMan::FindRandomNotInVec -- found, masternode=%s\n", pmn->outpoint.ToStringShort());
        return pmn->GetInfo();
    }

    LogPrint("masternode", "CMasternodeMan::FindRandomNotInVec -- failed\n");
    return masternode_info_t();
}

std::map<COutPoint, CMasternode> CMasternodeMan::GetFullMasternodeMap()
{
    LOCK(cs);

    std::map<COutPoint, CMasternode> result;
    auto mnList = deterministicMNManager->GetListAtChainTip();
    for (const auto &p : mapMasternodes) {
        auto dmn = mnList.GetMNByCollateral(p.first);
        if (dmn && mnList.IsMNValid(dmn)) {
            result.emplace(p.first, p.second);
        }
    }
    return result;
}

bool CMasternodeMan::GetMasternodeScores(const uint256& nBlockHash, CMasternodeMan::score_pair_vec_t& vecMasternodeScoresRet, int nMinProtocol)
{
    AssertLockHeld(cs);

    vecMasternodeScoresRet.clear();

    auto mnList = deterministicMNManager->GetListAtChainTip();
    auto scores = mnList.CalculateScores(nBlockHash);
    for (const auto& p : scores) {
        auto* mn = Find(p.second->collateralOutpoint);
        vecMasternodeScoresRet.emplace_back(p.first, mn);
    }

    sort(vecMasternodeScoresRet.rbegin(), vecMasternodeScoresRet.rend(), CompareScoreMN());
    return !vecMasternodeScoresRet.empty();
}

bool CMasternodeMan::GetMasternodeRank(const COutPoint& outpoint, int& nRankRet, int nBlockHeight, int nMinProtocol)
{
    uint256 tmp;
    return GetMasternodeRank(outpoint, nRankRet, tmp, nBlockHeight, nMinProtocol);
}

bool CMasternodeMan::GetMasternodeRank(const COutPoint& outpoint, int& nRankRet, uint256& blockHashRet, int nBlockHeight, int nMinProtocol)
{
    nRankRet = -1;

    if (!masternodeSync.IsBlockchainSynced())
        return false;

    // make sure we know about this block
    blockHashRet = uint256();
    if (!GetBlockHash(blockHashRet, nBlockHeight)) {
        LogPrintf("CMasternodeMan::%s -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", __func__, nBlockHeight);
        return false;
    }

    LOCK(cs);

    score_pair_vec_t vecMasternodeScores;
    if (!GetMasternodeScores(blockHashRet, vecMasternodeScores, nMinProtocol))
        return false;

    int nRank = 0;
    for (const auto& scorePair : vecMasternodeScores) {
        nRank++;
        if(scorePair.second->outpoint == outpoint) {
            nRankRet = nRank;
            return true;
        }
    }

    return false;
}

bool CMasternodeMan::GetMasternodeRanks(CMasternodeMan::rank_pair_vec_t& vecMasternodeRanksRet, int nBlockHeight, int nMinProtocol)
{
    vecMasternodeRanksRet.clear();

    if (!masternodeSync.IsBlockchainSynced())
        return false;

    // make sure we know about this block
    uint256 nBlockHash = uint256();
    if (!GetBlockHash(nBlockHash, nBlockHeight)) {
        LogPrintf("CMasternodeMan::%s -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", __func__, nBlockHeight);
        return false;
    }

    LOCK(cs);

    score_pair_vec_t vecMasternodeScores;
    if (!GetMasternodeScores(nBlockHash, vecMasternodeScores, nMinProtocol))
        return false;

    int nRank = 0;
    for (const auto& scorePair : vecMasternodeScores) {
        nRank++;
        vecMasternodeRanksRet.push_back(std::make_pair(nRank, *scorePair.second));
    }

    return true;
}

void CMasternodeMan::ProcessMasternodeConnections(CConnman& connman)
{
    std::vector<masternode_info_t> vecMnInfo; // will be empty when no wallet
#ifdef ENABLE_WALLET
    privateSendClient.GetMixingMasternodesInfo(vecMnInfo);
#endif // ENABLE_WALLET

    connman.ForEachNode(CConnman::AllNodes, [&vecMnInfo](CNode* pnode) {
        if (pnode->fMasternode) {
#ifdef ENABLE_WALLET
            bool fFound = false;
            for (const auto& mnInfo : vecMnInfo) {
                if (pnode->addr == mnInfo.addr) {
                    fFound = true;
                    break;
                }
            }
            if (fFound) return; // do NOT disconnect mixing masternodes
#endif // ENABLE_WALLET
            LogPrintf("Closing Masternode connection: peer=%d, addr=%s\n", pnode->id, pnode->addr.ToString());
            pnode->fDisconnect = true;
        }
    });
}

std::string CMasternodeMan::ToString() const
{
    std::ostringstream info;

    info << "Masternodes: masternode object count: " << (int)mapMasternodes.size() <<
            ", deterministic masternode count: " << deterministicMNManager->GetListAtChainTip().GetAllMNsCount() <<
            ", nDsqCount: " << (int)nDsqCount;
    return info.str();
}

bool CMasternodeMan::AddGovernanceVote(const COutPoint& outpoint, uint256 nGovernanceObjectHash)
{
    LOCK(cs);
    CMasternode* pmn = Find(outpoint);
    if(!pmn) {
        return false;
    }
    pmn->AddGovernanceVote(nGovernanceObjectHash);
    return true;
}

void CMasternodeMan::RemoveGovernanceObject(uint256 nGovernanceObjectHash)
{
    LOCK(cs);
    for(auto& mnpair : mapMasternodes) {
        mnpair.second.RemoveGovernanceObject(nGovernanceObjectHash);
    }
}

void CMasternodeMan::UpdatedBlockTip(const CBlockIndex *pindex)
{
    nCachedBlockHeight = pindex->nHeight;
    LogPrint("masternode", "CMasternodeMan::UpdatedBlockTip -- nCachedBlockHeight=%d\n", nCachedBlockHeight);

    AddDeterministicMasternodes();
    RemoveNonDeterministicMasternodes();
}

void CMasternodeMan::NotifyMasternodeUpdates(CConnman& connman, bool forceAddedChecks, bool forceRemovedChecks)
{
    // Avoid double locking
    bool fMasternodesAddedLocal = false;
    bool fMasternodesRemovedLocal = false;
    {
        LOCK(cs);
        fMasternodesAddedLocal = fMasternodesAdded;
        fMasternodesRemovedLocal = fMasternodesRemoved;
    }

    if(fMasternodesAddedLocal || forceAddedChecks) {
        governance.CheckMasternodeOrphanObjects(connman);
        governance.CheckMasternodeOrphanVotes(connman);
    }
    if(fMasternodesRemovedLocal || forceRemovedChecks) {
        governance.UpdateCachesAndClean();
    }

    LOCK(cs);
    fMasternodesAdded = false;
    fMasternodesRemoved = false;
}

void CMasternodeMan::DoMaintenance(CConnman& connman)
{
    if(fLiteMode) return; // disable all Dash specific functionality

    if(!masternodeSync.IsBlockchainSynced() || ShutdownRequested())
        return;

    static unsigned int nTick = 0;

    nTick++;

    if(nTick % 60 == 0) {
        mnodeman.ProcessMasternodeConnections(connman);
    }
}
