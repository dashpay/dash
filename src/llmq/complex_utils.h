// Copyright (c) 2018-2021 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LLMQ_COMPLEX_UTILS_H
#define BITCOIN_LLMQ_COMPLEX_UTILS_H

#include <consensus/params.h>

#include <random.h>
#include <versionbits.h>

#include <vector>
#include <memory>
#include <set>
#include <optional>

class CConnman;
class CBlockIndex;
class CDeterministicMN;
class CDeterministicMNList;
using CDeterministicMNCPtr = std::shared_ptr<const CDeterministicMN>;
class CBLSPublicKey;

namespace llmq
{

class CQuorumSnapshot;

//QuorumMembers per quorumIndex at heights H-Cycle, H-2Cycles, H-3Cycles
struct PreviousQuorumQuarters {
    std::vector<std::vector<CDeterministicMNCPtr>> quarterHMinusC;
    std::vector<std::vector<CDeterministicMNCPtr>> quarterHMinus2C;
    std::vector<std::vector<CDeterministicMNCPtr>> quarterHMinus3C;
    explicit PreviousQuorumQuarters(size_t s) :
            quarterHMinusC(s), quarterHMinus2C(s), quarterHMinus3C(s) {}
};

class CLLMQComplexUtils
{
public:
    // includes members which failed DKG
    static std::vector<CDeterministicMNCPtr> GetAllQuorumMembers(Consensus::LLMQType llmqType, const CBlockIndex* pQuorumBaseBlockIndex);

    static void PreComputeQuorumMembers(const CBlockIndex* pQuorumBaseBlockIndex);

    static std::vector<CDeterministicMNCPtr> ComputeQuorumMembers(Consensus::LLMQType llmqType, const CBlockIndex* pQuorumBaseBlockIndex);
    static std::vector<std::vector<CDeterministicMNCPtr>> ComputeQuorumMembersByQuarterRotation(Consensus::LLMQType llmqType, const CBlockIndex* pCycleQuorumBaseBlockIndex);

    static std::vector<std::vector<CDeterministicMNCPtr>> BuildNewQuorumQuarterMembers(const Consensus::LLMQParams& llmqParams, const CBlockIndex* pQuorumBaseBlockIndex, const PreviousQuorumQuarters& quarters);

    static PreviousQuorumQuarters GetPreviousQuorumQuarterMembers(const Consensus::LLMQParams& llmqParams, const CBlockIndex* pBlockHMinusCIndex, const CBlockIndex* pBlockHMinus2CIndex, const CBlockIndex* pBlockHMinus3CIndex, int nHeight);
    static std::vector<std::vector<CDeterministicMNCPtr>> GetQuorumQuarterMembersBySnapshot(const Consensus::LLMQParams& llmqParams, const CBlockIndex* pQuorumBaseBlockIndex, const llmq::CQuorumSnapshot& snapshot, int nHeights);
    static std::pair<CDeterministicMNList, CDeterministicMNList> GetMNUsageBySnapshot(Consensus::LLMQType llmqType, const CBlockIndex* pQuorumBaseBlockIndex, const llmq::CQuorumSnapshot& snapshot, int nHeight);

    static void BuildQuorumSnapshot(const Consensus::LLMQParams& llmqParams, const CDeterministicMNList& mnAtH, const CDeterministicMNList& mnUsedAtH, std::vector<CDeterministicMNCPtr>& sortedCombinedMns, CQuorumSnapshot& quorumSnapshot, int nHeight, std::vector<int>& skipList, const CBlockIndex* pQuorumBaseBlockIndex);

    static std::set<uint256> GetQuorumConnections(const Consensus::LLMQParams& llmqParams, const CBlockIndex* pQuorumBaseBlockIndex, const uint256& forMember, bool onlyOutbound);
    static std::set<uint256> GetQuorumRelayMembers(const Consensus::LLMQParams& llmqParams, const CBlockIndex* pQuorumBaseBlockIndex, const uint256& forMember, bool onlyOutbound);

    static bool EnsureQuorumConnections(const Consensus::LLMQParams& llmqParams, const CBlockIndex* pQuorumBaseBlockIndex, CConnman& connman, const uint256& myProTxHash);
    static void AddQuorumProbeConnections(const Consensus::LLMQParams& llmqParams, const CBlockIndex* pQuorumBaseBlockIndex, CConnman& connman, const uint256& myProTxHash);

    static bool IsQuorumActive(Consensus::LLMQType llmqType, const uint256& quorumHash);
    static bool IsQuorumTypeEnabled(Consensus::LLMQType llmqType, const CBlockIndex* pindex);
    static bool IsQuorumTypeEnabledInternal(Consensus::LLMQType llmqType, const CBlockIndex* pindex, std::optional<bool> optDIP0024IsActive, std::optional<bool> optHaveDIP0024Quorums);

    static Consensus::LLMQType GetInstantSendLLMQType(const CBlockIndex* pindex);

    static std::vector<Consensus::LLMQType> GetEnabledQuorumTypes(const CBlockIndex* pindex);
    static std::vector<std::reference_wrapper<const Consensus::LLMQParams>> GetEnabledQuorumParams(const CBlockIndex* pindex);

    template <typename CacheType>
    static void InitQuorumsCache(CacheType& cache);


    static std::set<size_t> CalcDeterministicWatchConnections(Consensus::LLMQType llmqType, const CBlockIndex* pQuorumBaseBlockIndex, size_t memberCount, size_t connectionCount);

};

} // namespace llmq

#endif // BITCOIN_LLMQ_COMPLEX_UTILS_H
