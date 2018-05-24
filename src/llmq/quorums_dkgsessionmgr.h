// Copyright (c) 2018 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DASH_QUORUMS_DKGSESSIONMGR_H
#define DASH_QUORUMS_DKGSESSIONMGR_H

#include "llmq/quorums_dkgsession.h"

#include "validation.h"

#include "ctpl.h"

class UniValue;

namespace llmq
{

enum QuorumPhase {
    QuorumPhase_Idle,
    QuorumPhase_Initialized,
    QuorumPhase_Contribute,
    QuorumPhase_Complain,
    QuorumPhase_Justify,
    QuorumPhase_Commit,
    QuorumPhase_Finalize,

    QuorumPhase_None=-1,
};

class CDKGPendingMessages
{
public:
    typedef std::pair<NodeId, std::shared_ptr<CDataStream>> BinaryMessage;

private:
    mutable CCriticalSection cs;
    size_t maxMessagesPerNode;
    std::list<BinaryMessage> pendingMessages;
    std::map<NodeId, size_t> messagesPerNode;
    std::set<uint256> seenMessages;

public:
    CDKGPendingMessages(size_t _maxMessagesPerNode);

    void PushPendingMessage(NodeId from, CDataStream& vRecv);
    std::list<BinaryMessage> PopPendingMessages(size_t maxCount);
    bool HasSeen(const uint256& hash) const;
    void Clear();

    // Might return nullptr messages, which indicates that deserialization failed for some reason
    template<typename Message>
    std::vector<std::pair<NodeId, std::shared_ptr<Message>>> PopAndDeserializeMessages(size_t maxCount)
    {
        auto binaryMessages = PopPendingMessages(maxCount);
        if (binaryMessages.empty()) {
            return {};
        }

        std::vector<std::pair<NodeId, std::shared_ptr<Message>>> ret;
        ret.reserve(binaryMessages.size());
        for (auto& bm : binaryMessages) {
            auto msg = std::make_shared<Message>();
            try {
                *bm.second >> *msg;
            } catch (...) {
                msg = nullptr;
            }
            ret.emplace_back(std::make_pair(bm.first, std::move(msg)));
        }

        return std::move(ret);
    }
};

// we have one handler per DKG type
class CDKGSessionHandler
{
private:
    friend class CDKGSessionManager;

private:
    mutable CCriticalSection cs;

    const Consensus::LLMQParams& params;
    CEvoDB& evoDb;
    ctpl::thread_pool& messageHandlerPool;
    CBLSWorker& blsWorker;
    CDKGSessionManager& dkgManager;

    QuorumPhase phase{QuorumPhase_Idle};
    int quorumHeight{-1};
    uint256 quorumHash;
    std::shared_ptr<CDKGSession> curSession;
    std::thread phaseHandlerThread;

    CDKGPendingMessages pendingContributions;
    CDKGPendingMessages pendingComplaints;
    CDKGPendingMessages pendingJustifications;
    CDKGPendingMessages pendingPrematureCommitments;

public:
    CDKGSessionHandler(const Consensus::LLMQParams& _params, CEvoDB& _evoDb, ctpl::thread_pool& _messageHandlerPool, CBLSWorker& blsWorker, CDKGSessionManager& _dkgManager);
    ~CDKGSessionHandler();

    void UpdatedBlockTip(const CBlockIndex *pindexNew, const CBlockIndex *pindexFork, bool fInitialDownload);
    void ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman& connman);

private:
    bool InitNewQuorum(int height, const uint256& quorumHash);

    std::pair<QuorumPhase, uint256> GetPhaseAndQuorumHash();

    typedef std::function<void()> StartPhaseFunc;
    typedef std::function<bool()> WhileWaitFunc;
    void WaitForNextPhase(QuorumPhase curPhase, QuorumPhase nextPhase, uint256& expectedQuorumHash, const WhileWaitFunc& runWhileWaiting);
    void WaitForNewQuorum(const uint256& oldQuorumHash);
    void RandomSleep(QuorumPhase curPhase, uint256& expectedQuorumHash, double randomSleepFactor, const WhileWaitFunc& runWhileWaiting);
    void HandlePhase(QuorumPhase curPhase, QuorumPhase nextPhase, uint256& expectedQuorumHash, double randomSleepFactor, const StartPhaseFunc& startPhaseFunc, const WhileWaitFunc& runWhileWaiting);
    void HandleDKGRound();
    void PhaseHandlerThread();
};

class CDKGSessionManager
{
private:
    CEvoDB& evoDb;
    CBLSWorker& blsWorker;
    ctpl::thread_pool messageHandlerPool;

    std::map<Consensus::LLMQType, CDKGSessionHandler> dkgSessionHandlers;

    // TODO cleanup
    CCriticalSection contributionsCacheCs;
    std::map<std::tuple<Consensus::LLMQType, uint256, uint256>, std::pair<BLSVerificationVectorPtr, CBLSSecretKey>> contributionsCache;

public:
    CDKGSessionManager(CEvoDB& _evoDb, CBLSWorker& _blsWorker);
    ~CDKGSessionManager();

    void UpdatedBlockTip(const CBlockIndex *pindexNew, const CBlockIndex *pindexFork, bool fInitialDownload);

    void ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman& connman);
    bool AlreadyHave(const CInv& inv) const;
    bool GetContribution(const uint256& hash, CDKGContribution& ret) const;
    bool GetComplaint(const uint256& hash, CDKGComplaint& ret) const;
    bool GetJustification(const uint256& hash, CDKGJustification& ret) const;
    bool GetPrematureCommitment(const uint256& hash, CDKGPrematureCommitment& ret) const;

    // Verified contributions are written while in the DKG
    void WriteVerifiedVvecContribution(Consensus::LLMQType llmqType, const uint256& quorumHash, const uint256& proTxHash, const BLSVerificationVectorPtr& vvec);
    void WriteVerifiedSkContribution(Consensus::LLMQType llmqType, const uint256& quorumHash, const uint256& proTxHash, const CBLSSecretKey& skContribution);
    bool GetVerifiedContributions(Consensus::LLMQType llmqType, const uint256& quorumHash, const std::vector<bool>& validMembers, std::vector<uint16_t>& memberIndexesRet, std::vector<BLSVerificationVectorPtr>& vvecsRet, BLSSecretKeyVector& skContributionsRet);
    bool GetVerifiedContribution(Consensus::LLMQType llmqType, const uint256& quorumHash, const uint256& proTxHash, BLSVerificationVectorPtr& vvecRet, CBLSSecretKey& skContributionRet);
};

extern CDKGSessionManager* quorumDKGSessionManager;

}

#endif //DASH_QUORUMS_DKGSESSIONMGR_H
