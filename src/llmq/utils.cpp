// Copyright (c) 2018-2021 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/utils.h>

#include <llmq/blockprocessor.h>
#include <llmq/commitment.h>
#include <llmq/quorums.h>
#include <llmq/snapshot.h>

#include <bls/bls.h>
#include <chainparams.h>
#include <evo/deterministicmns.h>
#include <evo/evodb.h>
#include <masternode/meta.h>
#include <net.h>
#include <random.h>
#include <spork.h>
#include <timedata.h>
#include <util/ranges.h>
#include <validation.h>
#include <versionbits.h>

#include <boost/range/adaptor/sliced.hpp>
#include <boost/range/irange.hpp>

namespace llmq
{

CCriticalSection cs_llmq_vbc;
VersionBitsCache llmq_versionbitscache;

std::vector<CDeterministicMNCPtr> CLLMQUtils::GetAllQuorumMembers(Consensus::LLMQType llmqType, const CBlockIndex* pQuorumBaseBlockIndex)
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

    if (CLLMQUtils::IsQuorumRotationEnabled(llmqType)) {
        {
            LOCK(cs_indexed_members);
            if (mapIndexedQuorumMembers.empty()) {
                InitQuorumsCache(mapIndexedQuorumMembers);
            }
        }
        /*
         * Quorums created with rotation are now created in a different way. All signingActiveQuorumCount are created during the period of dkgInterval.
         * But they are not created exactly in the same block, they are spreaded overtime: one quorum in each block until all signingActiveQuorumCount are created.
         * The new concept of quorumIndex is introduced in order to identify them.
         * In every dkgInterval blocks (also called CycleQuorumBaseBlock), the spreaded quorum creation starts like this:
         * For quorumIndex = 0 : signingActiveQuorumCount
         * Quorum Q with quorumIndex is created at height CycleQuorumBaseBlock + quorumIndex
         */

        int quorumIndex = pQuorumBaseBlockIndex->nHeight % GetLLMQParams(llmqType).dkgInterval;
        int cycleQuorumBaseHeight = pQuorumBaseBlockIndex->nHeight - quorumIndex;
        const CBlockIndex* pCycleQuorumBaseBlockIndex = pQuorumBaseBlockIndex->GetAncestor(cycleQuorumBaseHeight);

        /*
         * Since mapQuorumMembers stores Quorum members per block hash, and we don't know yet the block hashes of blocks for all quorumIndexes (since these blocks are not created yet)
         * We store them in a second cache mapIndexedQuorumMembers which stores them by {CycleQuorumBaseBlockHash, quorumIndex}
         */
        {
            LOCK(cs_indexed_members);
            if (mapIndexedQuorumMembers[llmqType].get(std::pair(pCycleQuorumBaseBlockIndex->GetBlockHash(), quorumIndex), quorumMembers)) {
                LOCK(cs_members);
                mapQuorumMembers[llmqType].insert(pQuorumBaseBlockIndex->GetBlockHash(), quorumMembers);

                /*
            * We also need to store which quorum block hash corresponds to which quorumIndex
            */
                quorumManager->SetQuorumIndexQuorumHash(llmqType, pQuorumBaseBlockIndex->GetBlockHash(), quorumIndex);
                return quorumMembers;
            }
        }

        auto q = ComputeQuorumMembersByQuarterRotation(llmqType, pCycleQuorumBaseBlockIndex);
        LOCK(cs_indexed_members);
        for (int i : boost::irange(0, static_cast<int>(q.size()))) {
            mapIndexedQuorumMembers[llmqType].insert(std::make_pair(pCycleQuorumBaseBlockIndex->GetBlockHash(), i), q[i]);
        }

        quorumMembers = q[0];
        LOCK(cs_members);
        mapQuorumMembers[llmqType].insert(pQuorumBaseBlockIndex->GetBlockHash(), quorumMembers);
        quorumManager->SetQuorumIndexQuorumHash(llmqType, pQuorumBaseBlockIndex->GetBlockHash(), 0);

        return quorumMembers;
    } else {
        quorumMembers = ComputeQuorumMembers(llmqType, pQuorumBaseBlockIndex);
        LOCK(cs_members);
        mapQuorumMembers[llmqType].insert(pQuorumBaseBlockIndex->GetBlockHash(), quorumMembers);
    }

    return quorumMembers;
}

std::vector<CDeterministicMNCPtr> CLLMQUtils::ComputeQuorumMembers(Consensus::LLMQType llmqType, const CBlockIndex* pQuorumBaseBlockIndex)
{
    auto allMns = deterministicMNManager->GetListForBlock(pQuorumBaseBlockIndex);
    auto modifier = ::SerializeHash(std::make_pair(llmqType, pQuorumBaseBlockIndex->GetBlockHash()));
    return allMns.CalculateQuorum(GetLLMQParams(llmqType).size, modifier);
}

std::vector<std::vector<CDeterministicMNCPtr>> CLLMQUtils::ComputeQuorumMembersByQuarterRotation(Consensus::LLMQType llmqType, const CBlockIndex* pQuorumBaseBlockIndex)
{
    const Consensus::LLMQParams& llmqParams = GetLLMQParams(llmqType);

    const int cycleLength = llmqParams.dkgInterval;

    const CBlockIndex* pBlockHMinusCIndex = pQuorumBaseBlockIndex->GetAncestor(pQuorumBaseBlockIndex->nHeight - cycleLength);
    const CBlockIndex* pBlockHMinus2CIndex = pQuorumBaseBlockIndex->GetAncestor(pQuorumBaseBlockIndex->nHeight - 2 * cycleLength);
    const CBlockIndex* pBlockHMinus3CIndex = pQuorumBaseBlockIndex->GetAncestor(pQuorumBaseBlockIndex->nHeight - 3 * cycleLength);

    PreviousQuorumQuarters previousQuarters = GetPreviousQuorumQuarterMembers(llmqParams, pBlockHMinusCIndex, pBlockHMinus2CIndex, pBlockHMinus3CIndex);
    /*
    for (int i : boost::irange(0, GetLLMQParams(llmqType).signingActiveQuorumCount)) {
        LogPrintf("GetPreviousQuorumQuarterMembers llmqType[%d], NOW[%d], quorumIndex[%d] H-C[%d]:%d, H-2C[%d]:%d, H-3C[%d]:%d\n", static_cast<int>(llmqType), pQuorumBaseBlockIndex->nHeight,  i,
                  pBlockHMinusCIndex->nHeight, previousQuarters.quarterHMinusC[i].size(),
                  pBlockHMinus2CIndex->nHeight, previousQuarters.quarterHMinus2C[i].size(),
                  pBlockHMinus3CIndex->nHeight, previousQuarters.quarterHMinus3C[i].size());
    }
*/
    //TODO Rewrite this part
    //Last quorum DKG has failed. Returning and caching the last quorum members
    /*
    if (!llmq::quorumBlockProcessor->HasMinedCommitment(llmqType, pBlockHMinusCIndex->GetBlockHash())) {
        //Only if they formed a full quorum
        auto mns = GetAllQuorumMembers(llmqType, pBlockHMinusCIndex);
        if(mns.size() == GetLLMQParams(llmqType).size) return mns;
    }
    */

    std::vector<std::vector<CDeterministicMNCPtr>> quorumMembers;
    quorumMembers.resize(llmqParams.signingActiveQuorumCount);

    auto newQuarterMembers = CLLMQUtils::BuildNewQuorumQuarterMembers(llmqParams, pQuorumBaseBlockIndex, previousQuarters);
    //TODO Check if it is triggered from outside (P2P, block validation). Throwing an exception is probably a wiser choice
    //assert (!newQuarterMembers.empty());

    for (auto i : boost::irange(0, llmqParams.signingActiveQuorumCount)) {
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
    }

    return quorumMembers;
}

PreviousQuorumQuarters CLLMQUtils::GetPreviousQuorumQuarterMembers(const Consensus::LLMQParams& llmqParams, const CBlockIndex* pBlockHMinusCIndex, const CBlockIndex* pBlockHMinus2CIndex, const CBlockIndex* pBlockHMinus3CIndex)
{
    PreviousQuorumQuarters quarters = {};

    quarters.quarterHMinusC.resize(llmqParams.signingActiveQuorumCount);
    quarters.quarterHMinus2C.resize(llmqParams.signingActiveQuorumCount);
    quarters.quarterHMinus3C.resize(llmqParams.signingActiveQuorumCount);

    std::optional<llmq::CQuorumSnapshot> quSnapshotHMinusC = quorumSnapshotManager->GetSnapshotForBlock(llmqParams.type, pBlockHMinusCIndex);
    if (quSnapshotHMinusC.has_value()) {
        quarters.quarterHMinusC = CLLMQUtils::GetQuorumQuarterMembersBySnapshot(llmqParams, pBlockHMinusCIndex, quSnapshotHMinusC.value());
        //TODO Check if it is triggered from outside (P2P, block validation). Throwing an exception is probably a wiser choice
        //assert (!quarterHMinusC.empty());

        std::optional<llmq::CQuorumSnapshot> quSnapshotHMinus2C = quorumSnapshotManager->GetSnapshotForBlock(llmqParams.type, pBlockHMinus2CIndex);
        if (quSnapshotHMinus2C.has_value()) {
            quarters.quarterHMinus2C = CLLMQUtils::GetQuorumQuarterMembersBySnapshot(llmqParams, pBlockHMinus2CIndex, quSnapshotHMinus2C.value());
            //TODO Check if it is triggered from outside (P2P, block validation). Throwing an exception is probably a wiser choice
            //assert (!quarterHMinusC.empty());

            std::optional<llmq::CQuorumSnapshot> quSnapshotHMinus3C = quorumSnapshotManager->GetSnapshotForBlock(llmqParams.type, pBlockHMinus3CIndex);
            if (quSnapshotHMinus3C.has_value()) {
                quarters.quarterHMinus3C = CLLMQUtils::GetQuorumQuarterMembersBySnapshot(llmqParams, pBlockHMinus3CIndex, quSnapshotHMinus3C.value());
                //TODO Check if it is triggered from outside (P2P, block validation). Throwing an exception is probably a wiser choice
                //assert (!quarterHMinusC.empty());
            }
        }
    }

    return quarters;
}

std::vector<std::vector<CDeterministicMNCPtr>> CLLMQUtils::BuildNewQuorumQuarterMembers(const Consensus::LLMQParams& llmqParams, const CBlockIndex* pQuorumBaseBlockIndex, const PreviousQuorumQuarters& previousQuarters)
{
    std::vector<std::vector<CDeterministicMNCPtr>> quarterQuorumMembers;

    auto nQuorums = static_cast<size_t>(llmqParams.signingActiveQuorumCount);

    auto quorumSize = static_cast<size_t>(llmqParams.size);
    auto quarterSize = quorumSize / 4;
    auto modifier = ::SerializeHash(std::make_pair(llmqParams.type, pQuorumBaseBlockIndex->GetBlockHash()));
    auto allMns = deterministicMNManager->GetListForBlock(pQuorumBaseBlockIndex);

    quarterQuorumMembers.resize(nQuorums);

    auto MnsUsedAtH = CDeterministicMNList();
    auto MnsNotUsedAtH = CDeterministicMNList();

    for (auto i : boost::irange(0, llmqParams.signingActiveQuorumCount)) {
        for (const auto& mn : previousQuarters.quarterHMinusC[i]) {
            try {
                MnsUsedAtH.AddMN(mn);
            } catch (std::runtime_error& e) {
            }
        }
        for (const auto& mn : previousQuarters.quarterHMinus2C[i]) {
            try {
                MnsUsedAtH.AddMN(mn);
            } catch (std::runtime_error& e) {
            }
        }
        for (const auto& mn : previousQuarters.quarterHMinus3C[i]) {
            try {
                MnsUsedAtH.AddMN(mn);
            } catch (std::runtime_error& e) {
            }
        }
    }

    allMns.ForEachMN(true, [&MnsUsedAtH, &MnsNotUsedAtH](const CDeterministicMNCPtr& dmn) {
        if (!MnsUsedAtH.ContainsMN(dmn->proTxHash)) {
            try {
                MnsUsedAtH.AddMN(mn);
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

    CQuorumSnapshot quorumSnapshot = {};

    CLLMQUtils::BuildQuorumSnapshot(llmqParams, allMns, MnsUsedAtH, sortedCombinedMnsList, quarterQuorumMembers, quorumSnapshot);

    quorumSnapshotManager->StoreSnapshotForBlock(llmqParams.type, pQuorumBaseBlockIndex, quorumSnapshot);

    return quarterQuorumMembers;
}

void CLLMQUtils::BuildQuorumSnapshot(const Consensus::LLMQParams& llmqParams, const CDeterministicMNList& mnAtH, const CDeterministicMNList& mnUsedAtH, std::vector<CDeterministicMNCPtr>& sortedCombinedMns, std::vector<std::vector<CDeterministicMNCPtr>>& quarterMembers, CQuorumSnapshot& quorumSnapshot)
{
    quorumSnapshot.activeQuorumMembers.resize(mnAtH.GetAllMNsCount());
    std::fill(quorumSnapshot.activeQuorumMembers.begin(),
              quorumSnapshot.activeQuorumMembers.end(),
              false);
    size_t index = {};
    mnAtH.ForEachMN(true, [&index, &quorumSnapshot, &mnUsedAtH](const CDeterministicMNCPtr& dmn) {
        if (mnUsedAtH.ContainsMN(dmn->proTxHash)) {
            quorumSnapshot.activeQuorumMembers[index] = true;
        }
        index++;
    });

    CLLMQUtils::BuildQuorumSnapshotSkipList(llmqParams, mnUsedAtH, sortedCombinedMns, quarterMembers, quorumSnapshot);
}

void CLLMQUtils::BuildQuorumSnapshotSkipList(const Consensus::LLMQParams& llmqParams, const CDeterministicMNList& mnUsedAtH, std::vector<CDeterministicMNCPtr>& sortedCombinedMns, std::vector<std::vector<CDeterministicMNCPtr>>& quarterMembers, CQuorumSnapshot& quorumSnapshot)
{
    auto quorumSize = static_cast<size_t>(llmqParams.size);
    auto quarterSize = quorumSize / 4;

    quarterMembers.resize(quarterSize);
    quarterMembers.clear();

    if (mnUsedAtH.GetAllMNsCount() == 0) {
        //Mode 0: No skipping
        quorumSnapshot.mnSkipListMode = SnapshotSkipMode::MODE_NO_SKIPPING;
        quorumSnapshot.mnSkipList.clear();

        //Iterate over the first quarterSize elements
        for (auto i : boost::irange(0, llmqParams.signingActiveQuorumCount)) {
            //Iterate over the first quarterSize elements
            for (auto&& m : sortedCombinedMns | boost::adaptors::sliced(0, quarterSize)) {
                quarterMembers[i].push_back(std::move(m));
            }
            sortedCombinedMns.erase(sortedCombinedMns.begin(), sortedCombinedMns.begin() + quarterSize);
        }
    } else if (mnUsedAtH.GetAllMNsCount() < sortedCombinedMns.size() / 2) {
        //Mode 1: Skipping entries
        quorumSnapshot.mnSkipListMode = SnapshotSkipMode::MODE_SKIPPING_ENTRIES;

        size_t first_entry_index = {};
        size_t index = {};
        for (auto i : boost::irange(0, llmqParams.signingActiveQuorumCount)) {
            while (quarterMembers[i].size() < quarterSize && index < sortedCombinedMns.size()) {
                if (mnUsedAtH.ContainsMN(sortedCombinedMns[index]->proTxHash)) {
                    if (first_entry_index == 0) {
                        first_entry_index = index;
                        quorumSnapshot.mnSkipList.push_back(static_cast<int>(index));
                    } else
                        quorumSnapshot.mnSkipList.push_back(static_cast<int>(first_entry_index - index));
                } else
                    quarterMembers[i].push_back(sortedCombinedMns[index]);
                index++;
            }
        }
    } else {
        //Mode 2: Non-Skipping entries
        quorumSnapshot.mnSkipListMode = SnapshotSkipMode::MODE_NO_SKIPPING_ENTRIES;

        size_t first_entry_index = {};
        size_t index = {};
        for (auto i : boost::irange(0, llmqParams.signingActiveQuorumCount)) {
            while (quarterMembers[i].size() < quarterSize && index < sortedCombinedMns.size()) {
                if (!mnUsedAtH.ContainsMN(sortedCombinedMns.at(index)->proTxHash)) {
                    if (first_entry_index == 0) {
                        first_entry_index = index;
                        quorumSnapshot.mnSkipList.push_back(static_cast<int>(index));
                    } else
                        quorumSnapshot.mnSkipList.push_back(static_cast<int>(first_entry_index - index));
                } else
                    quarterMembers[i].push_back(sortedCombinedMns[index]);
                index++;
            }
        }
    }

    /*if (quarterMembers.empty()) {
        quorumSnapshot.mnSkipListMode = SnapshotSkipMode::MODE_ALL_SKIPPED;
    }*/
}

std::vector<std::vector<CDeterministicMNCPtr>> CLLMQUtils::GetQuorumQuarterMembersBySnapshot(const Consensus::LLMQParams& llmqParams, const CBlockIndex* pQuorumBaseBlockIndex, const llmq::CQuorumSnapshot& snapshot)
{
    std::vector<std::vector<CDeterministicMNCPtr>> quarterQuorumMembers = {};

    auto numQuorums = static_cast<size_t>(llmqParams.signingActiveQuorumCount);
    auto quorumSize = static_cast<size_t>(llmqParams.size);
    auto quarterSize = quorumSize / 4;

    quarterQuorumMembers.resize(numQuorums);

    auto modifier = ::SerializeHash(std::make_pair(llmqParams.type, pQuorumBaseBlockIndex->GetBlockHash()));
    auto Mns = deterministicMNManager->GetListForBlock(pQuorumBaseBlockIndex);

    auto [MnsUsedAtH, MnsNotUsedAtH] = CLLMQUtils::GetMNUsageBySnapshot(llmqParams.type, pQuorumBaseBlockIndex, snapshot);

    auto sortedMnsUsedAtH = MnsUsedAtH.CalculateQuorum(MnsUsedAtH.GetAllMNsCount(), modifier);
    auto sortedMnsNotUsedAtH = MnsNotUsedAtH.CalculateQuorum(MnsNotUsedAtH.GetAllMNsCount(), modifier);
    auto sortedCombinedMnsList = std::move(sortedMnsNotUsedAtH);
    for (auto& m : sortedMnsUsedAtH) {
        sortedCombinedMnsList.push_back(std::move(m));
    }

    //Mode 0: No skipping
    if (snapshot.mnSkipListMode == SnapshotSkipMode::MODE_NO_SKIPPING) {
        for (auto i : boost::irange(0, llmqParams.signingActiveQuorumCount)) {
            //Iterate over the first quarterSize elements
            for (auto&& m : sortedCombinedMnsList | boost::adaptors::sliced(0, quarterSize)) {
                quarterQuorumMembers[i].push_back(std::move(m));
            }
            sortedCombinedMnsList.erase(sortedCombinedMnsList.begin(), sortedCombinedMnsList.begin() + quarterSize);
        }
    }
    //Mode 1: List holds entries to be skipped
    else if (snapshot.mnSkipListMode == SnapshotSkipMode::MODE_SKIPPING_ENTRIES) {
        std::set<uint256> mnProTxHashToRemove;
        size_t first_entry_index = {};
        for (const auto& s : snapshot.mnSkipList) {
            if (first_entry_index == 0) {
                first_entry_index = s;
                mnProTxHashToRemove.insert(sortedCombinedMnsList.at(s)->proTxHash);
            } else {
                mnProTxHashToRemove.insert(sortedCombinedMnsList.at(first_entry_index + s)->proTxHash);
            }
        }

        //In sortedCombinedMnsList, MNs found in mnProTxHashToRemove must be placed at the end while preserving original order
        //This is the reason we use std::stable_partition instead of std::partition
        std::stable_partition(sortedCombinedMnsList.begin(),
                              sortedCombinedMnsList.end(),
                              [&mnProTxHashToRemove](const CDeterministicMNCPtr& dmn) {
                                  return mnProTxHashToRemove.find(dmn->proTxHash) == mnProTxHashToRemove.end();
                              });

        for (auto i : boost::irange(0, llmqParams.signingActiveQuorumCount)) {
            //Iterate over the first quarterSize elements
            for (auto&& m : sortedCombinedMnsList | boost::adaptors::sliced(0, quarterSize)) {
                quarterQuorumMembers[i].push_back(std::move(m));
            }
            sortedCombinedMnsList.erase(sortedCombinedMnsList.begin(), sortedCombinedMnsList.begin() + quarterSize);
        }
    }
    //Mode 2: List holds entries to be kept
    else if (snapshot.mnSkipListMode == SnapshotSkipMode::MODE_NO_SKIPPING_ENTRIES) {
        std::set<uint256> mnProTxHashToKeep;
        size_t first_entry_index = {};
        for (const auto& s : snapshot.mnSkipList) {
            if (first_entry_index == 0) {
                first_entry_index = s;
                mnProTxHashToKeep.insert(sortedCombinedMnsList.at(s)->proTxHash);
            } else {
                mnProTxHashToKeep.insert(sortedCombinedMnsList.at(first_entry_index + s)->proTxHash);
            }
        }

        //In sortedCombinedMnsList, MNs not found in mnProTxHashToKeep must be placed at the end while preserving original order
        //This is the reason we use std::stable_partition instead of std::partition
        std::stable_partition(sortedCombinedMnsList.begin(),
                              sortedCombinedMnsList.end(),
                              [&mnProTxHashToKeep](const CDeterministicMNCPtr& dmn) {
                                  return mnProTxHashToKeep.find(dmn->proTxHash) != mnProTxHashToKeep.end();
                              });

        for (auto i : boost::irange(0, llmqParams.signingActiveQuorumCount)) {
            //Iterate over the first quarterSize elements
            for (auto&& m : sortedCombinedMnsList | boost::adaptors::sliced(0, quarterSize)) {
                quarterQuorumMembers[i].push_back(std::move(m));
            }
            sortedCombinedMnsList.erase(sortedCombinedMnsList.begin(), sortedCombinedMnsList.begin() + quarterSize);
        }
    }
    //Mode 3: Every node was skipped. Returning empty quarterQuorumMembers

    return quarterQuorumMembers;
}

std::pair<CDeterministicMNList, CDeterministicMNList> CLLMQUtils::GetMNUsageBySnapshot(Consensus::LLMQType llmqType, const CBlockIndex* pQuorumBaseBlockIndex, const llmq::CQuorumSnapshot& snapshot)
{
    CDeterministicMNList usedMNs;
    CDeterministicMNList nonUsedMNs;

    auto Mns = deterministicMNManager->GetListForBlock(pQuorumBaseBlockIndex);

    size_t i{0};
    Mns.ForEachMN(true, [&i, &snapshot, &usedMNs, &nonUsedMNs](const CDeterministicMNCPtr& dmn) {
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
    });

    return std::make_pair(usedMNs, nonUsedMNs);
}

uint256 CLLMQUtils::BuildCommitmentHash(Consensus::LLMQType llmqType, const uint256& blockHash, const std::vector<bool>& validMembers, const CBLSPublicKey& pubKey, const uint256& vvecHash)
{
    CHashWriter hw(SER_NETWORK, 0);
    hw << llmqType;
    hw << blockHash;
    hw << DYNBITSET(validMembers);
    hw << pubKey;
    hw << vvecHash;
    return hw.GetHash();
}

uint256 CLLMQUtils::BuildSignHash(Consensus::LLMQType llmqType, const uint256& quorumHash, const uint256& id, const uint256& msgHash)
{
    CHashWriter h(SER_GETHASH, 0);
    h << llmqType;
    h << quorumHash;
    h << id;
    h << msgHash;
    return h.GetHash();
}

static bool EvalSpork(Consensus::LLMQType llmqType, int64_t spork_value)
{
    if (spork_value == 0) {
        return true;
    }
    if (spork_value == 1 && llmqType != Consensus::LLMQType::LLMQ_100_67 && llmqType != Consensus::LLMQType::LLMQ_400_60 && llmqType != Consensus::LLMQType::LLMQ_400_85) {
        return true;
    }
    return false;
}

bool CLLMQUtils::IsAllMembersConnectedEnabled(Consensus::LLMQType llmqType)
{
    return EvalSpork(llmqType, sporkManager.GetSporkValue(SPORK_21_QUORUM_ALL_CONNECTED));
}

bool CLLMQUtils::IsQuorumPoseEnabled(Consensus::LLMQType llmqType)
{
    return EvalSpork(llmqType, sporkManager.GetSporkValue(SPORK_23_QUORUM_POSE));
}

bool CLLMQUtils::IsQuorumRotationEnabled(Consensus::LLMQType llmqType)
{
    LOCK(cs_main);
    bool fQuorumRotationActive = (VersionBitsTipState(Params().GetConsensus(), Consensus::DEPLOYMENT_DIP0024) == ThresholdState::ACTIVE);
    return llmqType == Params().GetConsensus().llmqTypeInstantSend && fQuorumRotationActive;
}

uint256 CLLMQUtils::DeterministicOutboundConnection(const uint256& proTxHash1, const uint256& proTxHash2)
{
    // We need to deterministically select who is going to initiate the connection. The naive way would be to simply
    // return the min(proTxHash1, proTxHash2), but this would create a bias towards MNs with a numerically low
    // hash. To fix this, we return the proTxHash that has the lowest value of:
    //   hash(min(proTxHash1, proTxHash2), max(proTxHash1, proTxHash2), proTxHashX)
    // where proTxHashX is the proTxHash to compare
    uint256 h1;
    uint256 h2;
    if (proTxHash1 < proTxHash2) {
        h1 = ::SerializeHash(std::make_tuple(proTxHash1, proTxHash2, proTxHash1));
        h2 = ::SerializeHash(std::make_tuple(proTxHash1, proTxHash2, proTxHash2));
    } else {
        h1 = ::SerializeHash(std::make_tuple(proTxHash2, proTxHash1, proTxHash1));
        h2 = ::SerializeHash(std::make_tuple(proTxHash2, proTxHash1, proTxHash2));
    }
    if (h1 < h2) {
        return proTxHash1;
    }
    return proTxHash2;
}

std::set<uint256> CLLMQUtils::GetQuorumConnections(const Consensus::LLMQParams& llmqParams, const CBlockIndex* pQuorumBaseBlockIndex, const uint256& forMember, bool onlyOutbound)
{
    if (IsAllMembersConnectedEnabled(llmqParams.type)) {
        auto mns = GetAllQuorumMembers(llmqParams.type, pQuorumBaseBlockIndex);
        std::set<uint256> result;

        for (const auto& dmn : mns) {
            if (dmn->proTxHash == forMember) {
                continue;
            }
            // Determine which of the two MNs (forMember vs dmn) should initiate the outbound connection and which
            // one should wait for the inbound connection. We do this in a deterministic way, so that even when we
            // end up with both connecting to each other, we know which one to disconnect
            uint256 deterministicOutbound = DeterministicOutboundConnection(forMember, dmn->proTxHash);
            if (!onlyOutbound || deterministicOutbound == dmn->proTxHash) {
                result.emplace(dmn->proTxHash);
            }
        }
        return result;
    } else {
        return GetQuorumRelayMembers(llmqParams, pQuorumBaseBlockIndex, forMember, onlyOutbound);
    }
}

std::set<uint256> CLLMQUtils::GetQuorumRelayMembers(const Consensus::LLMQParams& llmqParams, const CBlockIndex* pQuorumBaseBlockIndex, const uint256& forMember, bool onlyOutbound)
{
    auto mns = GetAllQuorumMembers(llmqParams.type, pQuorumBaseBlockIndex);
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

std::set<size_t> CLLMQUtils::CalcDeterministicWatchConnections(Consensus::LLMQType llmqType, const CBlockIndex* pQuorumBaseBlockIndex, size_t memberCount, size_t connectionCount)
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

bool CLLMQUtils::EnsureQuorumConnections(const Consensus::LLMQParams& llmqParams, const CBlockIndex* pQuorumBaseBlockIndex, const uint256& myProTxHash)
{
    auto members = GetAllQuorumMembers(llmqParams, pQuorumBaseBlockIndex);
    bool isMember = std::find_if(members.begin(), members.end(), [&](const auto& dmn) { return dmn->proTxHash == myProTxHash; }) != members.end();

    if (!isMember && !CLLMQUtils::IsWatchQuorumsEnabled()) {
        return false;
    }

    std::set<uint256> connections;
    std::set<uint256> relayMembers;
    if (isMember) {
        connections = CLLMQUtils::GetQuorumConnections(llmqParams, pQuorumBaseBlockIndex, myProTxHash, true);
        relayMembers = CLLMQUtils::GetQuorumRelayMembers(llmqParams, pQuorumBaseBlockIndex, myProTxHash, true);
    } else {
        auto cindexes = CLLMQUtils::CalcDeterministicWatchConnections(llmqParams.type, pQuorumBaseBlockIndex, members.size(), 1);
        for (auto idx : cindexes) {
            connections.emplace(members[idx]->proTxHash);
        }
        relayMembers = connections;
    }
    if (!connections.empty()) {
        if (!g_connman->HasMasternodeQuorumNodes(llmqParams.type, pQuorumBaseBlockIndex->GetBlockHash()) && LogAcceptCategory(BCLog::LLMQ)) {
            auto mnList = deterministicMNManager->GetListAtChainTip();
            std::string debugMsg = strprintf("CLLMQUtils::%s -- adding masternodes quorum connections for quorum %s:\n", __func__, pQuorumBaseBlockIndex->GetBlockHash().ToString());
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
        g_connman->SetMasternodeQuorumNodes(llmqParams.type, pQuorumBaseBlockIndex->GetBlockHash(), connections);
    }
    if (!relayMembers.empty()) {
        g_connman->SetMasternodeQuorumRelayMembers(llmqParams.type, pQuorumBaseBlockIndex->GetBlockHash(), relayMembers);
    }
    return true;
}

void CLLMQUtils::AddQuorumProbeConnections(const Consensus::LLMQParams& llmqParams, const CBlockIndex *pQuorumBaseBlockIndex, const uint256 &myProTxHash)
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
            std::string debugMsg = strprintf("CLLMQUtils::%s -- adding masternodes probes for quorum %s:\n", __func__, pQuorumBaseBlockIndex->GetBlockHash().ToString());
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
        g_connman->AddPendingProbeConnections(probeConnections);
    }
}

bool CLLMQUtils::IsQuorumActive(Consensus::LLMQType llmqType, const uint256& quorumHash)
{
    // sig shares and recovered sigs are only accepted from recent/active quorums
    // we allow one more active quorum as specified in consensus, as otherwise there is a small window where things could
    // fail while we are on the brink of a new quorum
    auto quorums = quorumManager->ScanQuorums(llmqType, GetLLMQParams(llmqType).signingActiveQuorumCount + 1);
    return ranges::any_of(quorums, [&quorumHash](const auto& q){ return q->qc->quorumHash == quorumHash; });
}

bool CLLMQUtils::IsQuorumTypeEnabled(Consensus::LLMQType llmqType, const CBlockIndex* pindex)
{
    const Consensus::Params& consensusParams = Params().GetConsensus();

    switch (llmqType)
    {
        case Consensus::LLMQType::LLMQ_50_60:
        case Consensus::LLMQType::LLMQ_400_60:
        case Consensus::LLMQType::LLMQ_400_85:
            break;
        case Consensus::LLMQType::LLMQ_100_67:
        case Consensus::LLMQType::LLMQ_TEST_V17:
            if (LOCK(cs_llmq_vbc); VersionBitsState(pindex, consensusParams, Consensus::DEPLOYMENT_DIP0020, llmq_versionbitscache) != ThresholdState::ACTIVE) {
                return false;
            }
            break;
        case Consensus::LLMQType::LLMQ_TEST:
        case Consensus::LLMQType::LLMQ_DEVNET:
            break;
        default:
            throw std::runtime_error(strprintf("%s: Unknown LLMQ type %d", __func__, static_cast<uint8_t>(llmqType)));
    }

    return true;
}

std::vector<Consensus::LLMQType> CLLMQUtils::GetEnabledQuorumTypes(const CBlockIndex* pindex)
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

std::vector<std::reference_wrapper<const Consensus::LLMQParams>> CLLMQUtils::GetEnabledQuorumParams(const CBlockIndex* pindex)
{
    std::vector<std::reference_wrapper<const Consensus::LLMQParams>> ret;
    ret.reserve(Params().GetConsensus().llmqs.size());

    std::copy_if(Params().GetConsensus().llmqs.begin(), Params().GetConsensus().llmqs.end(), std::back_inserter(ret),
                 [&pindex](const auto& params){return IsQuorumTypeEnabled(params.type, pindex);});

    return ret;
}

bool CLLMQUtils::QuorumDataRecoveryEnabled()
{
    return gArgs.GetBoolArg("-llmq-data-recovery", DEFAULT_ENABLE_QUORUM_DATA_RECOVERY);
}

bool CLLMQUtils::IsWatchQuorumsEnabled()
{
    static bool fIsWatchQuroumsEnabled = gArgs.GetBoolArg("-watchquorums", DEFAULT_WATCH_QUORUMS);
    return fIsWatchQuroumsEnabled;
}

std::map<Consensus::LLMQType, QvvecSyncMode> CLLMQUtils::GetEnabledQuorumVvecSyncEntries()
{
    std::map<Consensus::LLMQType, QvvecSyncMode> mapQuorumVvecSyncEntries;
    for (const auto& strEntry : gArgs.GetArgs("-llmq-qvvec-sync")) {
        Consensus::LLMQType llmqType = Consensus::LLMQType::LLMQ_NONE;
        QvvecSyncMode mode{QvvecSyncMode::Invalid};
        std::istringstream ssEntry(strEntry);
        std::string strLLMQType, strMode, strTest;
        const bool fLLMQTypePresent = std::getline(ssEntry, strLLMQType, ':') && strLLMQType != "";
        const bool fModePresent = std::getline(ssEntry, strMode, ':') && strMode != "";
        const bool fTooManyEntries = static_cast<bool>(std::getline(ssEntry, strTest, ':'));
        if (!fLLMQTypePresent || !fModePresent || fTooManyEntries) {
            throw std::invalid_argument(strprintf("Invalid format in -llmq-qvvec-sync: %s", strEntry));
        }

        if (auto optLLMQParams = ranges::find_if_opt(Params().GetConsensus().llmqs,
                                                     [&strLLMQType](const auto& params){return params.name == strLLMQType;})) {
            llmqType = optLLMQParams->type;
        } else {
            throw std::invalid_argument(strprintf("Invalid llmqType in -llmq-qvvec-sync: %s", strEntry));
        }
        if (mapQuorumVvecSyncEntries.count(llmqType) > 0) {
            throw std::invalid_argument(strprintf("Duplicated llmqType in -llmq-qvvec-sync: %s", strEntry));
        }

        int32_t nMode;
        if (ParseInt32(strMode, &nMode)) {
            switch (nMode) {
            case (int32_t)QvvecSyncMode::Always:
                mode = QvvecSyncMode::Always;
                break;
            case (int32_t)QvvecSyncMode::OnlyIfTypeMember:
                mode = QvvecSyncMode::OnlyIfTypeMember;
                break;
            default:
                mode = QvvecSyncMode::Invalid;
                break;
            }
        }
        if (mode == QvvecSyncMode::Invalid) {
            throw std::invalid_argument(strprintf("Invalid mode in -llmq-qvvec-sync: %s", strEntry));
        }
        mapQuorumVvecSyncEntries.emplace(llmqType, mode);
    }
    return mapQuorumVvecSyncEntries;
}

template <typename CacheType>
void CLLMQUtils::InitQuorumsCache(CacheType& cache)
{
    for (auto& llmq : Params().GetConsensus().llmqs) {
        cache.emplace(std::piecewise_construct, std::forward_as_tuple(llmq.type),
                      std::forward_as_tuple(llmq.signingActiveQuorumCount + 1));
    }
}
template void CLLMQUtils::InitQuorumsCache<std::map<Consensus::LLMQType, unordered_lru_cache<uint256, bool, StaticSaltedHasher>>>(std::map<Consensus::LLMQType, unordered_lru_cache<uint256, bool, StaticSaltedHasher>>& cache);
template void CLLMQUtils::InitQuorumsCache<std::map<Consensus::LLMQType, unordered_lru_cache<uint256, std::vector<CQuorumCPtr>, StaticSaltedHasher>>>(std::map<Consensus::LLMQType, unordered_lru_cache<uint256, std::vector<CQuorumCPtr>, StaticSaltedHasher>>& cache);
template void CLLMQUtils::InitQuorumsCache<std::map<Consensus::LLMQType, unordered_lru_cache<uint256, std::shared_ptr<llmq::CQuorum>, StaticSaltedHasher, 0ul, 0ul>, std::less<Consensus::LLMQType>, std::allocator<std::pair<Consensus::LLMQType const, unordered_lru_cache<uint256, std::shared_ptr<llmq::CQuorum>, StaticSaltedHasher, 0ul, 0ul>>>>>(std::map<Consensus::LLMQType, unordered_lru_cache<uint256, std::shared_ptr<llmq::CQuorum>, StaticSaltedHasher, 0ul, 0ul>, std::less<Consensus::LLMQType>, std::allocator<std::pair<Consensus::LLMQType const, unordered_lru_cache<uint256, std::shared_ptr<llmq::CQuorum>, StaticSaltedHasher, 0ul, 0ul>>>>&);
template void CLLMQUtils::InitQuorumsCache<std::map<Consensus::LLMQType, unordered_lru_cache<uint256, int, StaticSaltedHasher>>>(std::map<Consensus::LLMQType, unordered_lru_cache<uint256, int, StaticSaltedHasher>>& cache);

const Consensus::LLMQParams& GetLLMQParams(Consensus::LLMQType llmqType)
{
    return Params().GetLLMQ(llmqType);
}

} // namespace llmq
