// Copyright (c) 2017-2021 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/cbtx.h>
#include <evo/deterministicmns.h>
#include <evo/simplifiedmns.h>
#include <llmq/blockprocessor.h>
#include <llmq/commitment.h>
#include <llmq/quorums.h>
#include <llmq/snapshot.h>

#include <evo/specialtx.h>

#include <serialize.h>
#include <version.h>

#include <base58.h>
#include <chainparams.h>
#include <univalue.h>
#include <validation.h>


static const std::string DB_QUORUM_SNAPSHOT = "llmq_S";

std::unique_ptr<CQuorumSnapshotManager> quorumSnapshotManager;

void CQuorumSnapshot::ToJson(UniValue& obj) const
{
    obj.setObject();
    UniValue activeQ(UniValue::VARR);
    for (const auto& h : activeQuorumMembers) {
        activeQ.push_back(h);
    }
    obj.pushKV("activeQuorumMembers", activeQ);
    obj.pushKV("mnSkipListMode", mnSkipListMode);
    UniValue skipList(UniValue::VARR);
    for (const auto& h : mnSkipList) {
        skipList.push_back(h);
    }
    obj.pushKV("mnSkipList", skipList);
}

void CQuorumRotationInfo::ToJson(UniValue& obj) const
{
    obj.setObject();
    obj.pushKV("creationHeight", creationHeight);
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << quorumSnaphotAtHMinusC;
    obj.pushKV("quorumSnaphotAtHMinusC", HexStr(ss));
    ss.clear();
    ss << quorumSnaphotAtHMinus2C;
    obj.pushKV("quorumSnaphotAtHMinus2C", HexStr(ss));
    ss.clear();
    ss << quorumSnaphotAtHMinus3C;
    obj.pushKV("quorumSnaphotAtHMinus3C", HexStr(ss));
    ss.clear();
    ss << mnListDiffTip;
    obj.pushKV("mnListDiffTip", HexStr(ss));
    ss.clear();
    ss << mnListDiffAtH;
    obj.pushKV("mnListDiffAtH", HexStr(ss));
    ss.clear();
    ss << mnListDiffAtHMinusC;
    obj.pushKV("mnListDiffAtHMinusC", HexStr(ss));
    ss.clear();
    ss << mnListDiffAtHMinus2C;
    obj.pushKV("mnListDiffAtHMinus2C", HexStr(ss));
    ss.clear();
    ss << mnListDiffAtHMinus3C;
    obj.pushKV("mnListDiffAtHMinus3C", HexStr(ss));
    ss.clear();
}

bool BuildQuorumRotationInfo(const CGetQuorumRotationInfo& request, CQuorumRotationInfo& response, std::string& errorRet)
{
    AssertLockHeld(cs_main);

    if (request.heightsNb > 4) {
        errorRet = strprintf("invalid requested heightsNb");
        return false;
    }

    if (request.heightsNb != request.knownHeights.size()) {
        errorRet = strprintf("missmatch requested heightsNb and size(knownHeights)");
        return false;
    }

    LOCK(deterministicMNManager->cs);

    //TODO Handle request if client knows some heights
    if (request.heightsNb == 0) {
        const CBlockIndex* baseBlockIndex = chainActive.Genesis();
        if (!baseBlockIndex) {
            errorRet = strprintf("genesis block not found");
            return false;
        }

        const CBlockIndex* tipBlockIndex = chainActive.Tip();
        if (!tipBlockIndex) {
            errorRet = strprintf("tip block not found");
            return false;
        }

        auto baseDmnList = deterministicMNManager->GetListForBlock(baseBlockIndex);
        auto tipMnList = deterministicMNManager->GetListForBlock(tipBlockIndex);

        response.mnListDiffTip = baseDmnList.BuildSimplifiedDiff(tipMnList);

        auto quorums = llmq::quorumBlockProcessor->GetMinedAndActiveCommitmentsUntilBlock(tipBlockIndex);

        auto instantSendQuorum = quorums.find(Params().GetConsensus().llmqTypeInstantSend);
        if (instantSendQuorum == quorums.end()) {
            errorRet = strprintf("No InstantSend quorum found");
            return false;
        }

        if (instantSendQuorum->second.empty()) {
            errorRet = strprintf("Empty list for InstantSend quorum");
            return false;
        }

        // Since the returned quorums are in reversed order, the most recent one is at index 0
        const CBlockIndex* hBlockIndex = LookupBlockIndex(instantSendQuorum->second.at(0)->GetBlockHash());
        if (!hBlockIndex) {
            errorRet = strprintf("Can not find block H");
            return false;
        }
        auto HDmnList = deterministicMNManager->GetListForBlock(hBlockIndex);
        response.mnListDiffAtH = baseDmnList.BuildSimplifiedDiff(HDmnList);
        response.creationHeight = hBlockIndex->nHeight;

        // H-C
        const CBlockIndex* hcBlockIndex = LookupBlockIndex(instantSendQuorum->second.at(1)->GetBlockHash());
        if (!hcBlockIndex) {
            errorRet = strprintf("Can not find block H-C");
            return false;
        }
        auto HCDmnList = deterministicMNManager->GetListForBlock(hcBlockIndex);
        response.mnListDiffAtHMinusC = baseDmnList.BuildSimplifiedDiff(HCDmnList);
        response.quorumSnaphotAtHMinusC = quorumSnapshotManager->GetSnapshotForBlock(Params().GetConsensus().llmqTypeInstantSend, hcBlockIndex);

        // H-2C
        const CBlockIndex* h2cBlockIndex = LookupBlockIndex(instantSendQuorum->second.at(2)->GetBlockHash());
        if (!h2cBlockIndex) {
            errorRet = strprintf("Can not find block H-2C");
            return false;
        }
        auto H2CDmnList = deterministicMNManager->GetListForBlock(h2cBlockIndex);
        response.mnListDiffAtHMinus2C = baseDmnList.BuildSimplifiedDiff(H2CDmnList);
        response.quorumSnaphotAtHMinus2C = quorumSnapshotManager->GetSnapshotForBlock(Params().GetConsensus().llmqTypeInstantSend, h2cBlockIndex);

        // H-3C
        const CBlockIndex* h3cBlockIndex = LookupBlockIndex(instantSendQuorum->second.at(3)->GetBlockHash());
        if (!h3cBlockIndex) {
            errorRet = strprintf("Can not find block H-3C");
            return false;
        }
        auto H3CDmnList = deterministicMNManager->GetListForBlock(h3cBlockIndex);
        response.mnListDiffAtHMinus3C = baseDmnList.BuildSimplifiedDiff(H3CDmnList);
        response.quorumSnaphotAtHMinus3C = quorumSnapshotManager->GetSnapshotForBlock(Params().GetConsensus().llmqTypeInstantSend, h3cBlockIndex);
    }
    //TODO Handle request if client knows some heights
    else {
    }

    return true;
}

CQuorumSnapshotManager::CQuorumSnapshotManager(CEvoDB& _evoDb) :
    evoDb(_evoDb)
{
}

CQuorumSnapshot CQuorumSnapshotManager::GetSnapshotForBlock(const Consensus::LLMQType llmqType, const CBlockIndex* pindex)
{
    LOCK(cs);

    CQuorumSnapshot snapshot;

    auto snapshotHash = ::SerializeHash(std::make_pair(llmqType, pindex->GetBlockHash()));

    // try using cache before reading from disk
    auto it = quorumSnapshotCache.find(snapshotHash);
    if (it != quorumSnapshotCache.end()) {
        snapshot = it->second;
        return snapshot;
    }

    if (evoDb.Read(std::make_pair(DB_QUORUM_SNAPSHOT, snapshotHash), snapshot)) {
        quorumSnapshotCache.emplace(snapshotHash, snapshot);
        return snapshot;
    }

    return snapshot;
}

void CQuorumSnapshotManager::StoreSnapshotForBlock(const Consensus::LLMQType llmqType, const CBlockIndex* pindex, CQuorumSnapshot& snapshot)
{
    LOCK(cs);

    auto snapshotHash = ::SerializeHash(std::make_pair(llmqType, pindex->GetBlockHash()));

    evoDb.Write(std::make_pair(DB_QUORUM_SNAPSHOT, snapshotHash), snapshot);
    quorumSnapshotCache.emplace(snapshotHash, snapshot);
}
