// Copyright (c) 2018-2021 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/complex_utils.h>
#include <llmq/utils.h>

#include <llmq/quorums.h>

#include <llmq/commitment.h>
#include <llmq/snapshot.h>

#include <chainparams.h>
#include <evo/deterministicmns.h>
#include <masternode/meta.h>
#include <net.h>
#include <timedata.h>
#include <util/ranges.h>
#include <versionbits.h>
#include <chain.h>
#include <uint256.h>
#include <hash.h>
#include <logging.h>

#include <vector>
#include <set>
#include <optional>

namespace llmq
{

std::vector<CDeterministicMNCPtr> CLLMQComplexUtils::ComputeQuorumMembers(Consensus::LLMQType llmqType, const CBlockIndex* pQuorumBaseBlockIndex)
{
    auto allMns = deterministicMNManager->GetListForBlock(pQuorumBaseBlockIndex);
    auto modifier = ::SerializeHash(std::make_pair(llmqType, pQuorumBaseBlockIndex->GetBlockHash()));
    return allMns.CalculateQuorum(GetLLMQParams(llmqType).size, modifier);
}

std::vector<std::vector<CDeterministicMNCPtr>> CLLMQComplexUtils::ComputeQuorumMembersByQuarterRotation(Consensus::LLMQType llmqType, const CBlockIndex* pCycleQuorumBaseBlockIndex)
{
    const Consensus::LLMQParams& llmqParams = GetLLMQParams(llmqType);

    const int cycleLength = llmqParams.dkgInterval;
    assert(pCycleQuorumBaseBlockIndex->nHeight % cycleLength == 0);

    const CBlockIndex* pBlockHMinusCIndex = pCycleQuorumBaseBlockIndex->GetAncestor(pCycleQuorumBaseBlockIndex->nHeight - cycleLength);
    const CBlockIndex* pBlockHMinus2CIndex = pCycleQuorumBaseBlockIndex->GetAncestor(pCycleQuorumBaseBlockIndex->nHeight - 2 * cycleLength);
    const CBlockIndex* pBlockHMinus3CIndex = pCycleQuorumBaseBlockIndex->GetAncestor(pCycleQuorumBaseBlockIndex->nHeight - 3 * cycleLength);
    LOCK(deterministicMNManager->cs);
    const CBlockIndex* pWorkBlockIndex = pCycleQuorumBaseBlockIndex->GetAncestor(pCycleQuorumBaseBlockIndex->nHeight - 8);
    auto allMns = deterministicMNManager->GetListForBlock(pWorkBlockIndex);
    LogPrint(BCLog::LLMQ, "CLLMQComplexUtils::ComputeQuorumMembersByQuarterRotation llmqType[%d] nHeight[%d] allMns[%d]\n", static_cast<int>(llmqType), pCycleQuorumBaseBlockIndex->nHeight, allMns.GetValidMNsCount());

    PreviousQuorumQuarters previousQuarters = GetPreviousQuorumQuarterMembers(llmqParams, pBlockHMinusCIndex, pBlockHMinus2CIndex, pBlockHMinus3CIndex, pCycleQuorumBaseBlockIndex->nHeight);

    auto nQuorums = size_t(llmqParams.signingActiveQuorumCount);
    std::vector<std::vector<CDeterministicMNCPtr>> quorumMembers(nQuorums);

    auto newQuarterMembers = CLLMQComplexUtils::BuildNewQuorumQuarterMembers(llmqParams, pCycleQuorumBaseBlockIndex, previousQuarters);
    //TODO Check if it is triggered from outside (P2P, block validation). Throwing an exception is probably a wiser choice
    //assert (!newQuarterMembers.empty());

    for (auto i = 0; i < nQuorums; ++i) {
        std::stringstream ss;

        ss << " 3Cmns[";
        for (auto& m : previousQuarters.quarterHMinus3C[i]) {
            ss << m->proTxHash.ToString().substr(0, 4) << " | ";
        }
        ss << " ] 2Cmns[";
        for (auto& m : previousQuarters.quarterHMinus2C[i]) {
            ss << m->proTxHash.ToString().substr(0, 4) << " | ";
        }
        ss << " ] Cmns[";
        for (auto& m : previousQuarters.quarterHMinusC[i]) {
            ss << m->proTxHash.ToString().substr(0, 4) << " | ";
        }
        ss << " ] new[";
        for (auto& m : newQuarterMembers[i]) {
            ss << m->proTxHash.ToString().substr(0, 4) << " | ";
        }
        ss << " ]";
        LogPrint(BCLog::LLMQ, "QuarterComposition h[%d] i[%d]:%s\n", pCycleQuorumBaseBlockIndex->nHeight, i, ss.str());
    }

    for (auto i = 0; i < nQuorums; ++i) {
        for (auto& m : previousQuarters.quarterHMinus3C[i]) {
            quorumMembers[i].push_back(std::move(m));
        }
        for (auto& m : previousQuarters.quarterHMinus2C[i]) {
            quorumMembers[i].push_back(std::move(m));
        }
        for (auto& m : previousQuarters.quarterHMinusC[i]) {
            quorumMembers[i].push_back(std::move(m));
        }
        for (auto& m : newQuarterMembers[i]) {
            quorumMembers[i].push_back(std::move(m));
        }

        std::stringstream ss;
        ss << " [";
        for (auto& m : quorumMembers[i]) {
            ss << m->proTxHash.ToString().substr(0, 4) << " | ";
        }
        ss << "]";
        LogPrint(BCLog::LLMQ, "QuorumComposition h[%d] i[%d]:%s\n", pCycleQuorumBaseBlockIndex->nHeight, i, ss.str());
    }

    return quorumMembers;
}

PreviousQuorumQuarters CLLMQComplexUtils::GetPreviousQuorumQuarterMembers(const Consensus::LLMQParams& llmqParams, const CBlockIndex* pBlockHMinusCIndex, const CBlockIndex* pBlockHMinus2CIndex, const CBlockIndex* pBlockHMinus3CIndex, int nHeight)
{
    auto nQuorums = size_t(llmqParams.signingActiveQuorumCount);
    PreviousQuorumQuarters quarters(nQuorums);

    std::optional<llmq::CQuorumSnapshot> quSnapshotHMinusC = quorumSnapshotManager->GetSnapshotForBlock(llmqParams.type, pBlockHMinusCIndex);
    if (quSnapshotHMinusC.has_value()) {
        quarters.quarterHMinusC = CLLMQComplexUtils::GetQuorumQuarterMembersBySnapshot(llmqParams, pBlockHMinusCIndex, quSnapshotHMinusC.value(), nHeight);
        //TODO Check if it is triggered from outside (P2P, block validation). Throwing an exception is probably a wiser choice
        //assert (!quarterHMinusC.empty());

        std::optional<llmq::CQuorumSnapshot> quSnapshotHMinus2C = quorumSnapshotManager->GetSnapshotForBlock(llmqParams.type, pBlockHMinus2CIndex);
        if (quSnapshotHMinus2C.has_value()) {
            quarters.quarterHMinus2C = CLLMQComplexUtils::GetQuorumQuarterMembersBySnapshot(llmqParams, pBlockHMinus2CIndex, quSnapshotHMinus2C.value(), nHeight);
            //TODO Check if it is triggered from outside (P2P, block validation). Throwing an exception is probably a wiser choice
            //assert (!quarterHMinusC.empty());

            std::optional<llmq::CQuorumSnapshot> quSnapshotHMinus3C = quorumSnapshotManager->GetSnapshotForBlock(llmqParams.type, pBlockHMinus3CIndex);
            if (quSnapshotHMinus3C.has_value()) {
                quarters.quarterHMinus3C = CLLMQComplexUtils::GetQuorumQuarterMembersBySnapshot(llmqParams, pBlockHMinus3CIndex, quSnapshotHMinus3C.value(), nHeight);
                //TODO Check if it is triggered from outside (P2P, block validation). Throwing an exception is probably a wiser choice
                //assert (!quarterHMinusC.empty());
            }
        }
    }

    return quarters;
}

std::vector<std::vector<CDeterministicMNCPtr>> CLLMQComplexUtils::BuildNewQuorumQuarterMembers(const Consensus::LLMQParams& llmqParams, const CBlockIndex* pQuorumBaseBlockIndex, const PreviousQuorumQuarters& previousQuarters)
{
    auto nQuorums = size_t(llmqParams.signingActiveQuorumCount);
    std::vector<std::vector<CDeterministicMNCPtr>> quarterQuorumMembers(nQuorums);

    auto quorumSize = size_t(llmqParams.size);
    auto quarterSize = quorumSize / 4;
    const CBlockIndex* pWorkBlockIndex = pQuorumBaseBlockIndex->GetAncestor(pQuorumBaseBlockIndex->nHeight - 8);
    auto modifier = ::SerializeHash(std::make_pair(llmqParams.type, pWorkBlockIndex->GetBlockHash()));

    LOCK(deterministicMNManager->cs);
    auto allMns = deterministicMNManager->GetListForBlock(pWorkBlockIndex);

    if (allMns.GetValidMNsCount() < quarterSize) return quarterQuorumMembers;

    auto MnsUsedAtH = CDeterministicMNList();
    auto MnsNotUsedAtH = CDeterministicMNList();
    std::vector<CDeterministicMNList> MnsUsedAtHIndexed(nQuorums);

    for (auto i = 0; i < nQuorums; ++i) {
        for (const auto& mn : previousQuarters.quarterHMinusC[i]) {
            try {
                MnsUsedAtH.AddMN(mn);
            } catch (std::runtime_error& e) {
            }
            try {
                MnsUsedAtHIndexed[i].AddMN(mn);
            } catch (std::runtime_error& e) {
            }
        }
        for (const auto& mn : previousQuarters.quarterHMinus2C[i]) {
            try {
                MnsUsedAtH.AddMN(mn);
            } catch (std::runtime_error& e) {
            }
            try {
                MnsUsedAtHIndexed[i].AddMN(mn);
            } catch (std::runtime_error& e) {
            }
        }
        for (const auto& mn : previousQuarters.quarterHMinus3C[i]) {
            try {
                MnsUsedAtH.AddMN(mn);
            } catch (std::runtime_error& e) {
            }
            try {
                MnsUsedAtHIndexed[i].AddMN(mn);
            } catch (std::runtime_error& e) {
            }
        }
    }

    allMns.ForEachMNShared(true, [&MnsUsedAtH, &MnsNotUsedAtH](const CDeterministicMNCPtr& dmn) {
        if (!MnsUsedAtH.HasMN(dmn->proTxHash)) {
            try {
                MnsNotUsedAtH.AddMN(dmn);
            } catch (std::runtime_error& e) {
            }
        }
    });

    auto sortedMnsUsedAtHM = MnsUsedAtH.CalculateQuorum(MnsUsedAtH.GetAllMNsCount(), modifier);
    auto sortedMnsNotUsedAtH = MnsNotUsedAtH.CalculateQuorum(MnsNotUsedAtH.GetAllMNsCount(), modifier);
    auto sortedCombinedMnsList = std::move(sortedMnsNotUsedAtH);
    for (auto& m : sortedMnsUsedAtHM) {
        sortedCombinedMnsList.push_back(std::move(m));
    }

    std::stringstream ss;
    ss << " [";
    for (auto& m : sortedCombinedMnsList) {
        ss << m->proTxHash.ToString().substr(0, 4) << " | ";
    }
    ss << "]";
    LogPrint(BCLog::LLMQ, "BuildNewQuorumQuarterMembers h[%d] sortedCombinedMnsList:%s\n", pQuorumBaseBlockIndex->nHeight, ss.str());

    std::vector<int> skipList;
    int firstSkippedIndex = 0;
    auto idx = 0;
    for (auto i = 0; i < nQuorums; ++i) {
        auto usedMNsCount = MnsUsedAtHIndexed[i].GetAllMNsCount();
        while (quarterQuorumMembers[i].size() < quarterSize && (usedMNsCount + quarterQuorumMembers[i].size() < sortedCombinedMnsList.size())) {
            if (!MnsUsedAtHIndexed[i].HasMN(sortedCombinedMnsList[idx]->proTxHash)) {
                quarterQuorumMembers[i].push_back(sortedCombinedMnsList[idx]);
            } else {
                if (firstSkippedIndex == 0) {
                    firstSkippedIndex = idx;
                    skipList.push_back(idx);
                } else {
                    skipList.push_back(idx - firstSkippedIndex);
                }
            }
            if (++idx == sortedCombinedMnsList.size()) {
                idx = 0;
            }
        }
    }

    CQuorumSnapshot quorumSnapshot = {};

    CLLMQComplexUtils::BuildQuorumSnapshot(llmqParams, allMns, MnsUsedAtH, sortedCombinedMnsList, quorumSnapshot, pQuorumBaseBlockIndex->nHeight, skipList, pQuorumBaseBlockIndex);

    quorumSnapshotManager->StoreSnapshotForBlock(llmqParams.type, pQuorumBaseBlockIndex, quorumSnapshot);

    return quarterQuorumMembers;
}

void CLLMQComplexUtils::BuildQuorumSnapshot(const Consensus::LLMQParams& llmqParams, const CDeterministicMNList& mnAtH, const CDeterministicMNList& mnUsedAtH, std::vector<CDeterministicMNCPtr>& sortedCombinedMns, CQuorumSnapshot& quorumSnapshot, int nHeight, std::vector<int>& skipList, const CBlockIndex* pQuorumBaseBlockIndex)
{
    quorumSnapshot.activeQuorumMembers.resize(mnAtH.GetAllMNsCount());
    const CBlockIndex* pWorkBlockIndex = pQuorumBaseBlockIndex->GetAncestor(pQuorumBaseBlockIndex->nHeight - 8);
    auto modifier = ::SerializeHash(std::make_pair(llmqParams.type, pWorkBlockIndex->GetBlockHash()));
    auto sortedAllMns = mnAtH.CalculateQuorum(mnAtH.GetAllMNsCount(), modifier);

    std::fill(quorumSnapshot.activeQuorumMembers.begin(),
              quorumSnapshot.activeQuorumMembers.end(),
              false);
    size_t index = {};
    for (const auto& dmn : sortedAllMns) {
        if (mnUsedAtH.HasMN(dmn->proTxHash)) {
            quorumSnapshot.activeQuorumMembers[index] = true;
        }
        index++;
    }

    if (skipList.empty()) {
        quorumSnapshot.mnSkipListMode = SnapshotSkipMode::MODE_NO_SKIPPING;
        quorumSnapshot.mnSkipList.clear();
    } else {
        quorumSnapshot.mnSkipListMode = SnapshotSkipMode::MODE_SKIPPING_ENTRIES;
        quorumSnapshot.mnSkipList = std::move(skipList);
    }
}

std::vector<std::vector<CDeterministicMNCPtr>> CLLMQComplexUtils::GetQuorumQuarterMembersBySnapshot(const Consensus::LLMQParams& llmqParams, const CBlockIndex* pQuorumBaseBlockIndex, const llmq::CQuorumSnapshot& snapshot, int nHeight)
{
    std::vector<CDeterministicMNCPtr> sortedCombinedMns;
    {
        const CBlockIndex* pWorkBlockIndex = pQuorumBaseBlockIndex->GetAncestor(pQuorumBaseBlockIndex->nHeight - 8);
        const auto modifier = ::SerializeHash(std::make_pair(llmqParams.type, pWorkBlockIndex->GetBlockHash()));
        const auto [MnsUsedAtH, MnsNotUsedAtH] = CLLMQComplexUtils::GetMNUsageBySnapshot(llmqParams.type, pQuorumBaseBlockIndex, snapshot, nHeight);
        // the list begins with all the unused MNs
        auto sortedMnsNotUsedAtH = MnsNotUsedAtH.CalculateQuorum(MnsNotUsedAtH.GetAllMNsCount(), modifier);
        sortedCombinedMns = std::move(sortedMnsNotUsedAtH);
        // Now add the already used MNs to the end of the list
        auto sortedMnsUsedAtH = MnsUsedAtH.CalculateQuorum(MnsUsedAtH.GetAllMNsCount(), modifier);
        std::move(sortedMnsUsedAtH.begin(), sortedMnsUsedAtH.end(), std::back_inserter(sortedCombinedMns));
    }

    if (sortedCombinedMns.empty()) return {};

    auto numQuorums = size_t(llmqParams.signingActiveQuorumCount);
    auto quorumSize = size_t(llmqParams.size);
    auto quarterSize = quorumSize / 4;

    std::vector<std::vector<CDeterministicMNCPtr>> quarterQuorumMembers(numQuorums);

    switch (snapshot.mnSkipListMode) {
        case SnapshotSkipMode::MODE_NO_SKIPPING:
        {
            auto itm = sortedCombinedMns.begin();
            for (auto i = 0; i < llmqParams.signingActiveQuorumCount; ++i) {
                while (quarterQuorumMembers[i].size() < quarterSize) {
                    quarterQuorumMembers[i].push_back(*itm);
                    itm++;
                    if (itm == sortedCombinedMns.end()) {
                        itm = sortedCombinedMns.begin();
                    }
                }
            }
            return quarterQuorumMembers;
        }
        case SnapshotSkipMode::MODE_SKIPPING_ENTRIES: // List holds entries to be skipped
        {
            size_t first_entry_index{0};
            std::vector<int> processesdSkipList;
            for (const auto& s : snapshot.mnSkipList) {
                if (first_entry_index == 0) {
                    first_entry_index = s;
                    processesdSkipList.push_back(s);
                } else {
                    processesdSkipList.push_back(first_entry_index + s);
                }
            }

            auto idx = 0;
            auto itsk = processesdSkipList.begin();
            for (auto i = 0; i < llmqParams.signingActiveQuorumCount; ++i) {
                while (quarterQuorumMembers[i].size() < quarterSize) {
                    if (itsk != processesdSkipList.end() && idx == *itsk) {
                        itsk++;
                    } else {
                        quarterQuorumMembers[i].push_back(sortedCombinedMns[idx]);
                    }
                    idx++;
                    if (idx == sortedCombinedMns.size()) {
                        idx = 0;
                    }
                }
            }
            return quarterQuorumMembers;
        }
        case SnapshotSkipMode::MODE_NO_SKIPPING_ENTRIES: // List holds entries to be kept
        case SnapshotSkipMode::MODE_ALL_SKIPPED: // Every node was skipped. Returning empty quarterQuorumMembers
        default:
            return {};
    }
}

std::pair<CDeterministicMNList, CDeterministicMNList> CLLMQComplexUtils::GetMNUsageBySnapshot(Consensus::LLMQType llmqType, const CBlockIndex* pQuorumBaseBlockIndex, const llmq::CQuorumSnapshot& snapshot, int nHeight)
{
    CDeterministicMNList usedMNs;
    CDeterministicMNList nonUsedMNs;
    LOCK(deterministicMNManager->cs);

    const CBlockIndex* pWorkBlockIndex = pQuorumBaseBlockIndex->GetAncestor(pQuorumBaseBlockIndex->nHeight - 8);
    auto modifier = ::SerializeHash(std::make_pair(llmqType, pWorkBlockIndex->GetBlockHash()));

    auto Mns = deterministicMNManager->GetListForBlock(pWorkBlockIndex);
    auto sortedAllMns = Mns.CalculateQuorum(Mns.GetAllMNsCount(), modifier);

    size_t i{0};
    for (const auto& dmn : sortedAllMns) {
        if (snapshot.activeQuorumMembers[i]) {
            try {
                usedMNs.AddMN(dmn);
            } catch (std::runtime_error& e) {
            }
        } else {
            try {
                nonUsedMNs.AddMN(dmn);
            } catch (std::runtime_error& e) {
            }
        }
        i++;
    }

    return std::make_pair(usedMNs, nonUsedMNs);
}

Consensus::LLMQType CLLMQComplexUtils::GetInstantSendLLMQType(const CBlockIndex* pindex)
{
    if (CLLMQUtils::IsDIP0024Active(pindex) && !quorumManager->ScanQuorums(Params().GetConsensus().llmqTypeDIP0024InstantSend, pindex, 1).empty()) {
        return Params().GetConsensus().llmqTypeDIP0024InstantSend;
    }
    return Params().GetConsensus().llmqTypeInstantSend;
}

std::set<uint256> CLLMQComplexUtils::GetQuorumConnections(const Consensus::LLMQParams& llmqParams, const CBlockIndex* pQuorumBaseBlockIndex, const uint256& forMember, bool onlyOutbound)
{
    if (CLLMQUtils::IsAllMembersConnectedEnabled(llmqParams.type)) {
        auto mns = GetAllQuorumMembers(llmqParams.type, pQuorumBaseBlockIndex);
        std::set<uint256> result;

        for (const auto& dmn : mns) {
            if (dmn->proTxHash == forMember) {
                continue;
            }
            // Determine which of the two MNs (forMember vs dmn) should initiate the outbound connection and which
            // one should wait for the inbound connection. We do this in a deterministic way, so that even when we
            // end up with both connecting to each other, we know which one to disconnect
            uint256 deterministicOutbound = CLLMQUtils::DeterministicOutboundConnection(forMember, dmn->proTxHash);
            if (!onlyOutbound || deterministicOutbound == dmn->proTxHash) {
                result.emplace(dmn->proTxHash);
            }
        }
        return result;
    } else {
        return GetQuorumRelayMembers(llmqParams, pQuorumBaseBlockIndex, forMember, onlyOutbound);
    }
}

std::set<uint256> CLLMQComplexUtils::GetQuorumRelayMembers(const Consensus::LLMQParams& llmqParams, const CBlockIndex* pQuorumBaseBlockIndex, const uint256& forMember, bool onlyOutbound)
{
    auto mns = CLLMQComplexUtils::GetAllQuorumMembers(llmqParams.type, pQuorumBaseBlockIndex);
    std::set<uint256> result;

    auto calcOutbound = [&](size_t i, const uint256& proTxHash) {
        // Relay to nodes at indexes (i+2^k)%n, where
        //   k: 0..max(1, floor(log2(n-1))-1)
        //   n: size of the quorum/ring
        std::set<uint256> r;
        int gap = 1;
        int gap_max = (int)mns.size() - 1;
        int k = 0;
        while ((gap_max >>= 1) || k <= 1) {
            size_t idx = (i + gap) % mns.size();
            const auto& otherDmn = mns[idx];
            if (otherDmn->proTxHash == proTxHash) {
                continue;
            }
            r.emplace(otherDmn->proTxHash);
            gap <<= 1;
            k++;
        }
        return r;
    };

    for (size_t i = 0; i < mns.size(); i++) {
        const auto& dmn = mns[i];
        if (dmn->proTxHash == forMember) {
            auto r = calcOutbound(i, dmn->proTxHash);
            result.insert(r.begin(), r.end());
        } else if (!onlyOutbound) {
            auto r = calcOutbound(i, dmn->proTxHash);
            if (r.count(forMember)) {
                result.emplace(dmn->proTxHash);
            }
        }
    }

    return result;
}

bool CLLMQComplexUtils::EnsureQuorumConnections(const Consensus::LLMQParams& llmqParams, const CBlockIndex* pQuorumBaseBlockIndex, CConnman& connman, const uint256& myProTxHash)
{
    auto members = GetAllQuorumMembers(llmqParams.type, pQuorumBaseBlockIndex);
    bool isMember = std::find_if(members.begin(), members.end(), [&](const auto& dmn) { return dmn->proTxHash == myProTxHash; }) != members.end();

    if (!isMember && !CLLMQUtils::IsWatchQuorumsEnabled()) {
        return false;
    }

    std::set<uint256> connections;
    std::set<uint256> relayMembers;
    if (isMember) {
        connections = CLLMQComplexUtils::GetQuorumConnections(llmqParams, pQuorumBaseBlockIndex, myProTxHash, true);
        relayMembers = CLLMQComplexUtils::GetQuorumRelayMembers(llmqParams, pQuorumBaseBlockIndex, myProTxHash, true);
    } else {
        auto cindexes = CalcDeterministicWatchConnections(llmqParams.type, pQuorumBaseBlockIndex, members.size(), 1);
        for (auto idx : cindexes) {
            connections.emplace(members[idx]->proTxHash);
        }
        relayMembers = connections;
    }
    if (!connections.empty()) {
        if (!connman.HasMasternodeQuorumNodes(llmqParams.type, pQuorumBaseBlockIndex->GetBlockHash()) && LogAcceptCategory(BCLog::LLMQ)) {
            auto mnList = deterministicMNManager->GetListAtChainTip();
            std::string debugMsg = strprintf("CLLMQComplexUtils::%s -- adding masternodes quorum connections for quorum %s:\n", __func__, pQuorumBaseBlockIndex->GetBlockHash().ToString());
            for (auto& c : connections) {
                auto dmn = mnList.GetValidMN(c);
                if (!dmn) {
                    debugMsg += strprintf("  %s (not in valid MN set anymore)\n", c.ToString());
                } else {
                    debugMsg += strprintf("  %s (%s)\n", c.ToString(), dmn->pdmnState->addr.ToString(false));
                }
            }
            LogPrint(BCLog::NET_NETCONN, debugMsg.c_str()); /* Continued */
        }
        connman.SetMasternodeQuorumNodes(llmqParams.type, pQuorumBaseBlockIndex->GetBlockHash(), connections);
    }
    if (!relayMembers.empty()) {
        connman.SetMasternodeQuorumRelayMembers(llmqParams.type, pQuorumBaseBlockIndex->GetBlockHash(), relayMembers);
    }
    return true;
}

void CLLMQComplexUtils::AddQuorumProbeConnections(const Consensus::LLMQParams& llmqParams, const CBlockIndex *pQuorumBaseBlockIndex, CConnman& connman, const uint256 &myProTxHash)
{
    if (!CLLMQUtils::IsQuorumPoseEnabled(llmqParams.type)) {
        return;
    }

    auto members = GetAllQuorumMembers(llmqParams.type, pQuorumBaseBlockIndex);
    auto curTime = GetAdjustedTime();

    std::set<uint256> probeConnections;
    for (const auto& dmn : members) {
        if (dmn->proTxHash == myProTxHash) {
            continue;
        }
        auto lastOutbound = mmetaman.GetMetaInfo(dmn->proTxHash)->GetLastOutboundSuccess();
        // re-probe after 50 minutes so that the "good connection" check in the DKG doesn't fail just because we're on
        // the brink of timeout
        if (curTime - lastOutbound > 50 * 60) {
            probeConnections.emplace(dmn->proTxHash);
        }
    }

    if (!probeConnections.empty()) {
        if (LogAcceptCategory(BCLog::LLMQ)) {
            auto mnList = deterministicMNManager->GetListAtChainTip();
            std::string debugMsg = strprintf("CLLMQComplexUtils::%s -- adding masternodes probes for quorum %s:\n", __func__, pQuorumBaseBlockIndex->GetBlockHash().ToString());
            for (auto& c : probeConnections) {
                auto dmn = mnList.GetValidMN(c);
                if (!dmn) {
                    debugMsg += strprintf("  %s (not in valid MN set anymore)\n", c.ToString());
                } else {
                    debugMsg += strprintf("  %s (%s)\n", c.ToString(), dmn->pdmnState->addr.ToString(false));
                }
            }
            LogPrint(BCLog::NET_NETCONN, debugMsg.c_str()); /* Continued */
        }
        connman.AddPendingProbeConnections(probeConnections);
    }
}

bool CLLMQComplexUtils::IsQuorumActive(Consensus::LLMQType llmqType, const uint256& quorumHash)
{
    // sig shares and recovered sigs are only accepted from recent/active quorums
    // we allow one more active quorum as specified in consensus, as otherwise there is a small window where things could
    // fail while we are on the brink of a new quorum
    auto quorums = quorumManager->ScanQuorums(llmqType, GetLLMQParams(llmqType).signingActiveQuorumCount + 1);
    return ranges::any_of(quorums, [&quorumHash](const auto& q){ return q->qc->quorumHash == quorumHash; });
}

bool CLLMQComplexUtils::IsQuorumTypeEnabled(Consensus::LLMQType llmqType, const CBlockIndex* pindex)
{
    return IsQuorumTypeEnabledInternal(llmqType, pindex, std::nullopt, std::nullopt);
}

bool CLLMQComplexUtils::IsQuorumTypeEnabledInternal(Consensus::LLMQType llmqType, const CBlockIndex* pindex, std::optional<bool> optDIP0024IsActive, std::optional<bool> optHaveDIP0024Quorums)
{
    const Consensus::Params& consensusParams = Params().GetConsensus();

    switch (llmqType)
    {
        case Consensus::LLMQType::LLMQ_TEST_INSTANTSEND:
        case Consensus::LLMQType::LLMQ_50_60: {
            bool fDIP0024IsActive = optDIP0024IsActive.has_value() ? *optDIP0024IsActive : CLLMQUtils::IsDIP0024Active(pindex);
            if (fDIP0024IsActive) {
                bool fHaveDIP0024Quorums = optHaveDIP0024Quorums.has_value() ? *optHaveDIP0024Quorums
                                                                             : !quorumManager->ScanQuorums(
                                consensusParams.llmqTypeDIP0024InstantSend, pindex, 1).empty();
                if (fHaveDIP0024Quorums) {
                    return false;
                }
            }
            break;
        }
        case Consensus::LLMQType::LLMQ_TEST:
        case Consensus::LLMQType::LLMQ_400_60:
        case Consensus::LLMQType::LLMQ_400_85:
            break;
        case Consensus::LLMQType::LLMQ_100_67:
        case Consensus::LLMQType::LLMQ_TEST_V17:
            if (LOCK(cs_llmq_vbc); VersionBitsState(pindex, consensusParams, Consensus::DEPLOYMENT_DIP0020, llmq_versionbitscache) != ThresholdState::ACTIVE) {
                return false;
            }
            break;
        case Consensus::LLMQType::LLMQ_60_75:
        case Consensus::LLMQType::LLMQ_TEST_DIP0024: {
            bool fDIP0024IsActive = optDIP0024IsActive.has_value() ? *optDIP0024IsActive : CLLMQUtils::IsDIP0024Active(pindex);
            if (!fDIP0024IsActive) {
                return false;
            }
            break;
        }
        case Consensus::LLMQType::LLMQ_DEVNET:
            break;
        default:
            throw std::runtime_error(strprintf("%s: Unknown LLMQ type %d", __func__, static_cast<uint8_t>(llmqType)));
    }

    return true;
}

std::vector<Consensus::LLMQType> CLLMQComplexUtils::GetEnabledQuorumTypes(const CBlockIndex* pindex)
{
    std::vector<Consensus::LLMQType> ret;
    ret.reserve(Params().GetConsensus().llmqs.size());
    for (const auto& params : Params().GetConsensus().llmqs) {
        if (IsQuorumTypeEnabled(params.type, pindex)) {
            ret.push_back(params.type);
        }
    }
    return ret;
}

std::vector<std::reference_wrapper<const Consensus::LLMQParams>> CLLMQComplexUtils::GetEnabledQuorumParams(const CBlockIndex* pindex)
{
    std::vector<std::reference_wrapper<const Consensus::LLMQParams>> ret;
    ret.reserve(Params().GetConsensus().llmqs.size());

    std::copy_if(Params().GetConsensus().llmqs.begin(), Params().GetConsensus().llmqs.end(), std::back_inserter(ret),
                 [&pindex](const auto& params){return IsQuorumTypeEnabled(params.type, pindex);});

    return ret;
}

std::vector<CDeterministicMNCPtr> CLLMQComplexUtils::GetAllQuorumMembers(Consensus::LLMQType llmqType, const CBlockIndex* pQuorumBaseBlockIndex)
{
    static CCriticalSection cs_members;
    static std::map<Consensus::LLMQType, unordered_lru_cache<uint256, std::vector<CDeterministicMNCPtr>, StaticSaltedHasher>> mapQuorumMembers GUARDED_BY(cs_members);
    static CCriticalSection cs_indexed_members;
    static std::map<Consensus::LLMQType, unordered_lru_cache<std::pair<uint256, int>, std::vector<CDeterministicMNCPtr>, StaticSaltedHasher>> mapIndexedQuorumMembers GUARDED_BY(cs_indexed_members);
    if (!IsQuorumTypeEnabled(llmqType, pQuorumBaseBlockIndex->pprev)) {
        return {};
    }
    std::vector<CDeterministicMNCPtr> quorumMembers;
    {
        LOCK(cs_members);
        if (mapQuorumMembers.empty()) {
            InitQuorumsCache(mapQuorumMembers);
        }
        if (mapQuorumMembers[llmqType].get(pQuorumBaseBlockIndex->GetBlockHash(), quorumMembers)) {
            return quorumMembers;
        }
    }

    if (CLLMQUtils::IsQuorumRotationEnabled(llmqType, pQuorumBaseBlockIndex)) {
        if (LOCK(cs_indexed_members); mapIndexedQuorumMembers.empty()) {
            InitQuorumsCache(mapIndexedQuorumMembers);
        }
        /*
         * Quorums created with rotation are now created in a different way. All signingActiveQuorumCount are created during the period of dkgInterval.
         * But they are not created exactly in the same block, they are spreaded overtime: one quorum in each block until all signingActiveQuorumCount are created.
         * The new concept of quorumIndex is introduced in order to identify them.
         * In every dkgInterval blocks (also called CycleQuorumBaseBlock), the spreaded quorum creation starts like this:
         * For quorumIndex = 0 : signingActiveQuorumCount
         * Quorum Q with quorumIndex is created at height CycleQuorumBaseBlock + quorumIndex
         */

        const Consensus::LLMQParams& llmqParams = GetLLMQParams(llmqType);
        int quorumIndex = pQuorumBaseBlockIndex->nHeight % llmqParams.dkgInterval;
        if (quorumIndex >= llmqParams.signingActiveQuorumCount) {
            return {};
        }
        int cycleQuorumBaseHeight = pQuorumBaseBlockIndex->nHeight - quorumIndex;
        const CBlockIndex* pCycleQuorumBaseBlockIndex = pQuorumBaseBlockIndex->GetAncestor(cycleQuorumBaseHeight);

        /*
         * Since mapQuorumMembers stores Quorum members per block hash, and we don't know yet the block hashes of blocks for all quorumIndexes (since these blocks are not created yet)
         * We store them in a second cache mapIndexedQuorumMembers which stores them by {CycleQuorumBaseBlockHash, quorumIndex}
         */
        if (LOCK(cs_indexed_members); mapIndexedQuorumMembers[llmqType].get(std::pair(pCycleQuorumBaseBlockIndex->GetBlockHash(), quorumIndex), quorumMembers)) {
            LOCK(cs_members);
            mapQuorumMembers[llmqType].insert(pQuorumBaseBlockIndex->GetBlockHash(), quorumMembers);
            return quorumMembers;
        }

        auto q = ComputeQuorumMembersByQuarterRotation(llmqType, pCycleQuorumBaseBlockIndex);
        LOCK(cs_indexed_members);
        for (int i = 0; i < static_cast<int>(q.size()); ++i) {
            mapIndexedQuorumMembers[llmqType].insert(std::make_pair(pCycleQuorumBaseBlockIndex->GetBlockHash(), i), q[i]);
        }

        quorumMembers = q[quorumIndex];
    } else {
        quorumMembers = ComputeQuorumMembers(llmqType, pQuorumBaseBlockIndex);
    }

    LOCK(cs_members);
    mapQuorumMembers[llmqType].insert(pQuorumBaseBlockIndex->GetBlockHash(), quorumMembers);
    return quorumMembers;
}

void CLLMQComplexUtils::PreComputeQuorumMembers(const CBlockIndex* pQuorumBaseBlockIndex)
{
    for (const Consensus::LLMQParams& params : GetEnabledQuorumParams(pQuorumBaseBlockIndex->pprev)) {
        if (llmq::CLLMQUtils::IsQuorumRotationEnabled(params.type, pQuorumBaseBlockIndex) && (pQuorumBaseBlockIndex->nHeight % params.dkgInterval == 0)) {
            GetAllQuorumMembers(params.type, pQuorumBaseBlockIndex);
        }
    }
}

template <typename CacheType>
void CLLMQComplexUtils::InitQuorumsCache(CacheType& cache)
{
    for (auto& llmq : Params().GetConsensus().llmqs) {
        cache.emplace(std::piecewise_construct, std::forward_as_tuple(llmq.type),
                      std::forward_as_tuple(llmq.signingActiveQuorumCount + 1));
    }
}

template void CLLMQComplexUtils::InitQuorumsCache<std::map<Consensus::LLMQType, unordered_lru_cache<uint256, bool, StaticSaltedHasher>>>(std::map<Consensus::LLMQType, unordered_lru_cache<uint256, bool, StaticSaltedHasher>>& cache);
template void CLLMQComplexUtils::InitQuorumsCache<std::map<Consensus::LLMQType, unordered_lru_cache<uint256, std::vector<CQuorumCPtr>, StaticSaltedHasher>>>(std::map<Consensus::LLMQType, unordered_lru_cache<uint256, std::vector<CQuorumCPtr>, StaticSaltedHasher>>& cache);
template void CLLMQComplexUtils::InitQuorumsCache<std::map<Consensus::LLMQType, unordered_lru_cache<uint256, std::shared_ptr<llmq::CQuorum>, StaticSaltedHasher, 0ul, 0ul>, std::less<Consensus::LLMQType>, std::allocator<std::pair<Consensus::LLMQType const, unordered_lru_cache<uint256, std::shared_ptr<llmq::CQuorum>, StaticSaltedHasher, 0ul, 0ul>>>>>(std::map<Consensus::LLMQType, unordered_lru_cache<uint256, std::shared_ptr<llmq::CQuorum>, StaticSaltedHasher, 0ul, 0ul>, std::less<Consensus::LLMQType>, std::allocator<std::pair<Consensus::LLMQType const, unordered_lru_cache<uint256, std::shared_ptr<llmq::CQuorum>, StaticSaltedHasher, 0ul, 0ul>>>>&);
template void CLLMQComplexUtils::InitQuorumsCache<std::map<Consensus::LLMQType, unordered_lru_cache<uint256, int, StaticSaltedHasher>>>(std::map<Consensus::LLMQType, unordered_lru_cache<uint256, int, StaticSaltedHasher>>& cache);


std::set<size_t> CLLMQComplexUtils::CalcDeterministicWatchConnections(Consensus::LLMQType llmqType, const CBlockIndex* pQuorumBaseBlockIndex, size_t memberCount, size_t connectionCount)
{
    static uint256 qwatchConnectionSeed;
    static std::atomic<bool> qwatchConnectionSeedGenerated{false};
    static CCriticalSection qwatchConnectionSeedCs;
    if (!qwatchConnectionSeedGenerated) {
        LOCK(qwatchConnectionSeedCs);
        qwatchConnectionSeed = GetRandHash();
        qwatchConnectionSeedGenerated = true;
    }

    std::set<size_t> result;
    uint256 rnd = qwatchConnectionSeed;
    for (size_t i = 0; i < connectionCount; i++) {
        rnd = ::SerializeHash(std::make_pair(rnd, std::make_pair(llmqType, pQuorumBaseBlockIndex->GetBlockHash())));
        result.emplace(rnd.GetUint64(0) % memberCount);
    }
    return result;
}


} // namespace llmq
