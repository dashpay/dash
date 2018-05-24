// Copyright (c) 2018 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DASH_QUORUMS_H
#define DASH_QUORUMS_H

#include "evo/evodb.h"
#include "evo/deterministicmns.h"

#include "validationinterface.h"
#include "consensus/params.h"

#include "bls/bls.h"
#include "bls/bls_worker.h"

namespace llmq
{

// If true, we will connect to all new quorums and watch their communication
static const bool DEFAULT_WATCH_QUORUMS = false;

class CDKGSessionManager;

class CQuorum
{
public:
    const Consensus::LLMQParams& params;
    uint256 quorumHash;
    int height;
    std::vector<CDeterministicMNCPtr> members;
    std::vector<bool> validMembers;
    CBLSPublicKey quorumPublicKey;

    // These are only valid when we either participated in the DKG or fully watched it
    BLSVerificationVectorPtr quorumVvec;
    CBLSSecretKey skShare;

private:
    mutable CBLSWorkerCache blsCache;
    std::atomic<bool> stopCachePopulatorThread;
    std::thread cachePopulatorThread;

public:
    CQuorum(const Consensus::LLMQParams& _params, CBLSWorker& _blsWorker) : params(_params), blsCache(_blsWorker), stopCachePopulatorThread(false) {}
    ~CQuorum();
    void Init(const uint256& quorumHash, int height, const std::vector<CDeterministicMNCPtr>& members, const std::vector<bool>& validMembers, const CBLSPublicKey& quorumPublicKey);

    bool IsMember(const uint256& proTxHash) const;
    bool IsValidMember(const uint256& proTxHash) const;
    int GetMemberIndex(const uint256& proTxHash) const;

    CBLSPublicKey GetPubKeyShare(size_t memberIdx) const;
    CBLSSecretKey GetSkShare() const;

    bool WriteContributions(CEvoDB& evoDb);
    bool ReadContributions(CEvoDB& evoDb);
    static void StartCachePopulatorThread(std::shared_ptr<CQuorum> _this);
};
typedef std::shared_ptr<CQuorum> CQuorumPtr;
typedef std::shared_ptr<const CQuorum> CQuorumCPtr;

class CQuorumManager
{
private:
    CEvoDB& evoDb;
    CBLSWorker& blsWorker;
    CDKGSessionManager& dkgManager;

    CCriticalSection quorumsCacheCs;
    std::map<std::pair<Consensus::LLMQType, uint256>, CQuorumPtr> quorumsCache;

public:
    CQuorumManager(CEvoDB& _evoDb, CBLSWorker& _blsWorker, CDKGSessionManager& _dkgManager);

    void UpdatedBlockTip(const CBlockIndex *pindexNew, const CBlockIndex *pindexFork, bool fInitialDownload);

public:
    bool HasQuorum(Consensus::LLMQType llmqType, const uint256& quorumHash);

    // all these methods will lock cs_main
    CQuorumCPtr GetQuorum(Consensus::LLMQType llmqType,const uint256& quorumHash);
    CQuorumCPtr GetNewestQuorum(Consensus::LLMQType llmqType);
    std::vector<CQuorumCPtr> ScanQuorums(Consensus::LLMQType llmqType, size_t maxCount);
    std::vector<CQuorumCPtr> ScanQuorums(Consensus::LLMQType llmqType, const uint256& startBlock, size_t maxCount);
    CQuorumCPtr SelectQuorum(Consensus::LLMQType llmqType, const uint256& selectionHash, size_t poolSize);
    CQuorumCPtr SelectQuorum(Consensus::LLMQType llmqType, const uint256& startBlock, const uint256& selectionHash, size_t poolSize);

private:
    void EnsureQuorumConnections(Consensus::LLMQType llmqType, const CBlockIndex *pindexNew);

    bool BuildQuorumFromCommitment(const CFinalCommitment& qc, std::shared_ptr<CQuorum>& quorum) const;
    bool BuildQuorumContributions(const CFinalCommitment& fqc, std::shared_ptr<CQuorum>& quorum) const;
};

extern CQuorumManager* quorumManager;

}

#endif //DASH_QUORUMS_H
