// Copyright (c) 2018-2019 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/quorums.h>
#include <llmq/quorums_blockprocessor.h>
#include <llmq/quorums_dkgsession.h>
#include <llmq/quorums_dkgsessionmgr.h>
#include <llmq/quorums_init.h>
#include <llmq/quorums_utils.h>

#include <evo/specialtx.h>

#include <masternode/activemasternode.h>
#include <chainparams.h>
#include <init.h>
#include <masternode/masternode-sync.h>
#include <net_processing.h>
#include <netmessagemaker.h>
#include <univalue.h>
#include <validation.h>

#include <cxxtimer.hpp>

namespace llmq
{

static const std::string DB_QUORUM_SK_SHARE = "q_Qsk";
static const std::string DB_QUORUM_QUORUM_VVEC = "q_Qqvvec";

CQuorumManager* quorumManager;

CCriticalSection cs_data_requests;
static std::unordered_map<std::pair<uint256, bool>, CQuorumDataRequest, StaticSaltedHasher> mapQuorumDataRequests;

static uint256 MakeQuorumKey(const CQuorum& q)
{
    CHashWriter hw(SER_NETWORK, 0);
    hw << q.params.type;
    hw << q.qc.quorumHash;
    for (const auto& dmn : q.members) {
        hw << dmn->proTxHash;
    }
    return hw.GetHash();
}

CQuorum::~CQuorum()
{
    // most likely the thread is already done
    stopCachePopulatorThread = true;
}

void CQuorum::Init(const CFinalCommitment& _qc, const CBlockIndex* _pindexQuorum, const uint256& _minedBlockHash, const std::vector<CDeterministicMNCPtr>& _members)
{
    qc = _qc;
    pindexQuorum = _pindexQuorum;
    members = _members;
    minedBlockHash = _minedBlockHash;
}

bool CQuorum::SetVerificationVector(const BLSVerificationVector& quorumVecIn)
{
    if (::SerializeHash(quorumVecIn) != qc.quorumVvecHash) {
        return false;
    }
    quorumVvec = std::make_shared<BLSVerificationVector>(quorumVecIn);
    return true;
}

bool CQuorum::SetSecretKeyShare(const CBLSSecretKey& secretKeyShare)
{
    if (!secretKeyShare.IsValid() || (secretKeyShare.GetPublicKey() != GetPubKeyShare(GetMemberIndex(activeMasternodeInfo.proTxHash)))) {
        return false;
    }
    skShare = secretKeyShare;
    return true;
}

bool CQuorum::IsMember(const uint256& proTxHash) const
{
    for (auto& dmn : members) {
        if (dmn->proTxHash == proTxHash) {
            return true;
        }
    }
    return false;
}

bool CQuorum::IsValidMember(const uint256& proTxHash) const
{
    for (size_t i = 0; i < members.size(); i++) {
        if (members[i]->proTxHash == proTxHash) {
            return qc.validMembers[i];
        }
    }
    return false;
}

CBLSPublicKey CQuorum::GetPubKeyShare(size_t memberIdx) const
{
    if (quorumVvec == nullptr || memberIdx >= members.size() || !qc.validMembers[memberIdx]) {
        return CBLSPublicKey();
    }
    auto& m = members[memberIdx];
    return blsCache.BuildPubKeyShare(m->proTxHash, quorumVvec, CBLSId(m->proTxHash));
}

CBLSSecretKey CQuorum::GetSkShare() const
{
    return skShare;
}

int CQuorum::GetMemberIndex(const uint256& proTxHash) const
{
    for (size_t i = 0; i < members.size(); i++) {
        if (members[i]->proTxHash == proTxHash) {
            return (int)i;
        }
    }
    return -1;
}

void CQuorum::WriteContributions(CEvoDB& evoDb)
{
    uint256 dbKey = MakeQuorumKey(*this);

    if (quorumVvec != nullptr) {
        evoDb.GetRawDB().Write(std::make_pair(DB_QUORUM_QUORUM_VVEC, dbKey), *quorumVvec);
    }
    if (skShare.IsValid()) {
        evoDb.GetRawDB().Write(std::make_pair(DB_QUORUM_SK_SHARE, dbKey), skShare);
    }
}

bool CQuorum::ReadContributions(CEvoDB& evoDb)
{
    uint256 dbKey = MakeQuorumKey(*this);

    BLSVerificationVector qv;
    if (evoDb.Read(std::make_pair(DB_QUORUM_QUORUM_VVEC, dbKey), qv)) {
        quorumVvec = std::make_shared<BLSVerificationVector>(std::move(qv));
    } else {
        return false;
    }

    // We ignore the return value here as it is ok if this fails. If it fails, it usually means that we are not a
    // member of the quorum but observed the whole DKG process to have the quorum verification vector.
    evoDb.Read(std::make_pair(DB_QUORUM_SK_SHARE, dbKey), skShare);

    return true;
}

void CQuorum::StartCachePopulatorThread(std::shared_ptr<CQuorum> _this)
{
    if (_this->quorumVvec == nullptr) {
        return;
    }

    cxxtimer::Timer t(true);
    LogPrint(BCLog::LLMQ, "CQuorum::StartCachePopulatorThread -- start\n");

    // this thread will exit after some time
    // when then later some other thread tries to get keys, it will be much faster
    _this->cachePopulatorThread = std::thread([_this, t]() {
        RenameThread("dash-q-cachepop");
        for (size_t i = 0; i < _this->members.size() && !_this->stopCachePopulatorThread && !ShutdownRequested(); i++) {
            if (_this->qc.validMembers[i]) {
                _this->GetPubKeyShare(i);
            }
        }
        LogPrint(BCLog::LLMQ, "CQuorum::StartCachePopulatorThread -- done. time=%d\n", t.count());
    });
    _this->cachePopulatorThread.detach();
}

CQuorumManager::CQuorumManager(CEvoDB& _evoDb, CBLSWorker& _blsWorker, CDKGSessionManager& _dkgManager) :
    evoDb(_evoDb),
    blsWorker(_blsWorker),
    dkgManager(_dkgManager)
{
    CLLMQUtils::InitQuorumsCache(mapQuorumsCache);
    CLLMQUtils::InitQuorumsCache(scanQuorumsCache);
}

void CQuorumManager::UpdatedBlockTip(const CBlockIndex* pindexNew, bool fInitialDownload) const
{
    if (!masternodeSync.IsBlockchainSynced()) {
        return;
    }

    for (auto& p : Params().GetConsensus().llmqs) {
        EnsureQuorumConnections(p.first, pindexNew);
    }

    // Cleanup expired data requests
    LOCK(cs_data_requests);
    auto it = mapQuorumDataRequests.begin();
    while (it != mapQuorumDataRequests.end()) {
        if (it->second.IsExpired()) {
            it = mapQuorumDataRequests.erase(it);
        } else {
            ++it;
        }
    }
}

void CQuorumManager::EnsureQuorumConnections(Consensus::LLMQType llmqType, const CBlockIndex* pindexNew) const
{
    const auto& params = Params().GetConsensus().llmqs.at(llmqType);

    auto myProTxHash = activeMasternodeInfo.proTxHash;
    auto lastQuorums = ScanQuorums(llmqType, pindexNew, (size_t)params.keepOldConnections);

    auto connmanQuorumsToDelete = g_connman->GetMasternodeQuorums(llmqType);

    // don't remove connections for the currently in-progress DKG round
    int curDkgHeight = pindexNew->nHeight - (pindexNew->nHeight % params.dkgInterval);
    auto curDkgBlock = pindexNew->GetAncestor(curDkgHeight)->GetBlockHash();
    connmanQuorumsToDelete.erase(curDkgBlock);

    bool allowWatch = gArgs.GetBoolArg("-watchquorums", DEFAULT_WATCH_QUORUMS);
    for (auto& quorum : lastQuorums) {
        if (!quorum->IsMember(myProTxHash) && !allowWatch) {
            continue;
        }

        CLLMQUtils::EnsureQuorumConnections(llmqType, quorum->pindexQuorum, myProTxHash, allowWatch);

        connmanQuorumsToDelete.erase(quorum->qc.quorumHash);
    }

    for (auto& qh : connmanQuorumsToDelete) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- removing masternodes quorum connections for quorum %s:\n", __func__, qh.ToString());
        g_connman->RemoveMasternodeQuorumNodes(llmqType, qh);
    }
}

CQuorumPtr CQuorumManager::BuildQuorumFromCommitment(const Consensus::LLMQType llmqType, const CBlockIndex* pindexQuorum) const
{
    AssertLockHeld(quorumsCacheCs);
    assert(pindexQuorum);

    CFinalCommitment qc;
    const uint256& quorumHash{pindexQuorum->GetBlockHash()};
    uint256 minedBlockHash;
    if (!quorumBlockProcessor->GetMinedCommitment(llmqType, quorumHash, qc, minedBlockHash)) {
        return nullptr;
    }
    assert(qc.quorumHash == pindexQuorum->GetBlockHash());

    auto quorum = std::make_shared<CQuorum>(llmq::GetLLMQParams(llmqType), blsWorker);
    auto members = CLLMQUtils::GetAllQuorumMembers((Consensus::LLMQType)qc.llmqType, pindexQuorum);

    quorum->Init(qc, pindexQuorum, minedBlockHash, members);

    bool hasValidVvec = false;
    if (quorum->ReadContributions(evoDb)) {
        hasValidVvec = true;
    } else {
        if (BuildQuorumContributions(qc, quorum)) {
            quorum->WriteContributions(evoDb);
            hasValidVvec = true;
        } else {
            LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- quorum.ReadContributions and BuildQuorumContributions for block %s failed\n", __func__, qc.quorumHash.ToString());
        }
    }

    if (hasValidVvec) {
        // pre-populate caches in the background
        // recovering public key shares is quite expensive and would result in serious lags for the first few signing
        // sessions if the shares would be calculated on-demand
        CQuorum::StartCachePopulatorThread(quorum);
    }

    mapQuorumsCache[llmqType].insert(quorumHash, quorum);

    return quorum;
}

bool CQuorumManager::BuildQuorumContributions(const CFinalCommitment& fqc, std::shared_ptr<CQuorum>& quorum) const
{
    std::vector<uint16_t> memberIndexes;
    std::vector<BLSVerificationVectorPtr> vvecs;
    BLSSecretKeyVector skContributions;
    if (!dkgManager.GetVerifiedContributions((Consensus::LLMQType)fqc.llmqType, quorum->pindexQuorum, fqc.validMembers, memberIndexes, vvecs, skContributions)) {
        return false;
    }

    BLSVerificationVectorPtr quorumVvec;
    CBLSSecretKey skShare;

    cxxtimer::Timer t2(true);
    quorumVvec = blsWorker.BuildQuorumVerificationVector(vvecs);
    if (quorumVvec == nullptr) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- failed to build quorumVvec\n", __func__);
        // without the quorum vvec, there can't be a skShare, so we fail here. Failure is not fatal here, as it still
        // allows to use the quorum as a non-member (verification through the quorum pub key)
        return false;
    }
    skShare = blsWorker.AggregateSecretKeys(skContributions);
    if (!skShare.IsValid()) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- failed to build skShare\n", __func__);
        // We don't bail out here as this is not a fatal error and still allows us to recover public key shares (as we
        // have a valid quorum vvec at this point)
    }
    t2.stop();

    LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- built quorum vvec and skShare. time=%d\n", __func__, t2.count());

    quorum->quorumVvec = quorumVvec;
    quorum->skShare = skShare;

    return true;
}

bool CQuorumManager::HasQuorum(Consensus::LLMQType llmqType, const uint256& quorumHash)
{
    return quorumBlockProcessor->HasMinedCommitment(llmqType, quorumHash);
}

bool CQuorumManager::RequestQuorumData(CNode* pFrom, Consensus::LLMQType llmqType, const CBlockIndex* pQuorumIndex, uint16_t nDataMask, const uint256& proTxHash)
{
    if (pFrom->nVersion < LLMQ_DATA_MESSAGES_VERSION) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- Version must be %d or greater.\n", __func__, LLMQ_DATA_MESSAGES_VERSION);
        return false;
    }
    if (pFrom == nullptr || (pFrom->verifiedProRegTxHash.IsNull() && !pFrom->qwatch)) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- pFrom is not neither a verified masternode nor qwatch connection\n", __func__);
        return false;
    }
    if (Params().GetConsensus().llmqs.count(llmqType) == 0) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- Invalid llmqType: %d\n", __func__, llmqType);
        return false;
    }
    if (pQuorumIndex == nullptr) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- Invalid pQuorumIndex: nullptr\n", __func__);
        return false;
    }
    if (GetQuorum(llmqType, pQuorumIndex) == nullptr) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- Quorum not found: %s, %d\n", __func__, pQuorumIndex->GetBlockHash().ToString(), llmqType);
        return false;
    }

    LOCK(cs_data_requests);
    auto key = std::make_pair(pFrom->verifiedProRegTxHash, true);
    auto it = mapQuorumDataRequests.emplace(key, CQuorumDataRequest(llmqType, pQuorumIndex->GetBlockHash(), nDataMask, proTxHash));
    if (!it.second && !it.first->second.IsExpired()) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- Already requested\n", __func__);
        return false;
    }

    CNetMsgMaker msgMaker(pFrom->GetSendVersion());
    g_connman->PushMessage(pFrom, msgMaker.Make(NetMsgType::QGETDATA, it.first->second));

    return true;
}

std::vector<CQuorumCPtr> CQuorumManager::ScanQuorums(Consensus::LLMQType llmqType, size_t nCountRequested) const
{
    const CBlockIndex* pindex;
    {
        LOCK(cs_main);
        pindex = chainActive.Tip();
    }
    return ScanQuorums(llmqType, pindex, nCountRequested);
}

std::vector<CQuorumCPtr> CQuorumManager::ScanQuorums(Consensus::LLMQType llmqType, const CBlockIndex* pindexStart, size_t nCountRequested) const
{
    if (pindexStart == nullptr || nCountRequested == 0) {
        return {};
    }

    bool fCacheExists{false};
    void* pIndexScanCommitments{(void*)pindexStart};
    size_t nScanCommitments{nCountRequested};
    std::vector<CQuorumCPtr> vecResultQuorums;

    {
        LOCK(quorumsCacheCs);
        auto& cache = scanQuorumsCache[llmqType];
        fCacheExists = cache.get(pindexStart->GetBlockHash(), vecResultQuorums);
        if (fCacheExists) {
            // We have exactly what requested so just return it
            if (vecResultQuorums.size() == nCountRequested) {
                return vecResultQuorums;
            }
            // If we have more cached than requested return only a subvector
            if (vecResultQuorums.size() > nCountRequested) {
                return {vecResultQuorums.begin(), vecResultQuorums.begin() + nCountRequested};
            }
            // If we have cached quorums but not enough, subtract what we have from the count and the set correct index where to start
            // scanning for the rests
            if(vecResultQuorums.size() > 0) {
                nScanCommitments -= vecResultQuorums.size();
                pIndexScanCommitments = (void*)vecResultQuorums.back()->pindexQuorum->pprev;
            }
        } else {
            // If there is nothing in cache request at least cache.max_size() because this gets cached then later
            nScanCommitments = std::max(nCountRequested, cache.max_size());
        }
    }
    // Get the block indexes of the mined commitments to build the required quorums from
    auto quorumIndexes = quorumBlockProcessor->GetMinedCommitmentsUntilBlock(llmqType, (const CBlockIndex*)pIndexScanCommitments, nScanCommitments);
    vecResultQuorums.reserve(vecResultQuorums.size() + quorumIndexes.size());

    for (auto& quorumIndex : quorumIndexes) {
        assert(quorumIndex);
        auto quorum = GetQuorum(llmqType, quorumIndex);
        assert(quorum != nullptr);
        vecResultQuorums.emplace_back(quorum);
    }

    size_t nCountResult{vecResultQuorums.size()};
    if (nCountResult > 0 && !fCacheExists) {
        LOCK(quorumsCacheCs);
        // Don't cache more than cache.max_size() elements
        auto& cache = scanQuorumsCache[llmqType];
        size_t nCacheEndIndex = std::min(nCountResult, cache.max_size());
        cache.emplace(pindexStart->GetBlockHash(), {vecResultQuorums.begin(), vecResultQuorums.begin() + nCacheEndIndex});
    }
    // Don't return more than nCountRequested elements
    size_t nResultEndIndex = std::min(nCountResult, nCountRequested);
    return {vecResultQuorums.begin(), vecResultQuorums.begin() + nResultEndIndex};
}

CQuorumCPtr CQuorumManager::GetQuorum(Consensus::LLMQType llmqType, const uint256& quorumHash) const
{
    CBlockIndex* pindexQuorum;
    {
        LOCK(cs_main);

        pindexQuorum = LookupBlockIndex(quorumHash);
        if (!pindexQuorum) {
            LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- block %s not found\n", __func__, quorumHash.ToString());
            return nullptr;
        }
    }
    return GetQuorum(llmqType, pindexQuorum);
}

CQuorumCPtr CQuorumManager::GetQuorum(Consensus::LLMQType llmqType, const CBlockIndex* pindexQuorum) const
{
    assert(pindexQuorum);

    auto quorumHash = pindexQuorum->GetBlockHash();

    // we must check this before we look into the cache. Reorgs might have happened which would mean we might have
    // cached quorums which are not in the active chain anymore
    if (!HasQuorum(llmqType, quorumHash)) {
        return nullptr;
    }

    LOCK(quorumsCacheCs);
    CQuorumPtr pQuorum;
    if (mapQuorumsCache[llmqType].get(quorumHash, pQuorum)) {
        return pQuorum;
    }

    return BuildQuorumFromCommitment(llmqType, pindexQuorum);
}

void CQuorumManager::ProcessMessage(CNode* pFrom, const std::string& strCommand, CDataStream& vRecv)
{
    auto strFunc = __func__;
    auto error = [&](const std::string strError, bool fIncreaseScore = true, int nScore = 10) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- %s: %s, from peer=%d\n", strFunc, strCommand, strError, pFrom->GetId());
        if (fIncreaseScore) {
            LOCK(cs_main);
            Misbehaving(pFrom->GetId(), nScore);
        }
    };

    if (strCommand == NetMsgType::QGETDATA) {

        if (!fMasternodeMode || pFrom == nullptr || (pFrom->verifiedProRegTxHash.IsNull() && !pFrom->qwatch)) {
            error("Not a verified masternode or a qwatch connection");
            return;
        }

        CQuorumDataRequest request;
        vRecv >> request;

        auto sendQDATA = [&](CQuorumDataRequest::Errors nError = CQuorumDataRequest::Errors::UNDEFINED,
                             const CDataStream& body = CDataStream(SER_NETWORK, PROTOCOL_VERSION)) {
            request.SetError(nError);
            CDataStream ssResponse(SER_NETWORK, pFrom->GetSendVersion(), request, body);
            g_connman->PushMessage(pFrom, CNetMsgMaker(pFrom->GetSendVersion()).Make(NetMsgType::QDATA, ssResponse));
        };

        {
            LOCK2(cs_main, cs_data_requests);
            auto key = std::make_pair(pFrom->verifiedProRegTxHash, false);
            auto it = mapQuorumDataRequests.find(key);
            if (it == mapQuorumDataRequests.end()) {
                it = mapQuorumDataRequests.emplace(key, request).first;
            } else if(it->second.IsExpired()) {
                it->second = request;
            } else {
                error("Request limit exceeded", true, 25);
            }
        }

        if (Params().GetConsensus().llmqs.count(request.GetLLMQType()) == 0) {
            sendQDATA(CQuorumDataRequest::Errors::QUORUM_TYPE_INVALID);
            return;
        }

        const CBlockIndex* pQuorumIndex{nullptr};
        {
            LOCK(cs_main);
            pQuorumIndex = LookupBlockIndex(request.GetQuorumHash());
        }
        if (pQuorumIndex == nullptr) {
            sendQDATA(CQuorumDataRequest::Errors::QUORUM_BLOCK_NOT_FOUND);
            return;
        }

        const CQuorumCPtr pQuorum = GetQuorum(request.GetLLMQType(), pQuorumIndex);
        if (pQuorum == nullptr) {
            sendQDATA(CQuorumDataRequest::Errors::QUORUM_NOT_FOUND);
            return;
        }

        CDataStream ssResponseData(SER_NETWORK, pFrom->GetSendVersion());

        if (request.GetDataMask() & CQuorumDataRequest::QUORUM_VERIFICATION_VECTOR) {

            if (!pQuorum->quorumVvec) {
                sendQDATA(CQuorumDataRequest::Errors::QUORUM_VERIFICATION_VECTOR_MISSING);
                return;
            }

            ssResponseData << *pQuorum->quorumVvec;
        }

        if (request.GetDataMask() & CQuorumDataRequest::ENCRYPTED_CONTRIBUTIONS) {

            int memberIdx = pQuorum->GetMemberIndex(request.GetProTxHash());
            if (memberIdx == -1) {
                sendQDATA(CQuorumDataRequest::Errors::MASTERNODE_IS_NO_MEMBER);
                return;
            }

            std::vector<CBLSIESEncryptedObject<CBLSSecretKey>> vecEncrypted;
            if (!quorumDKGSessionManager->GetEncryptedContributions(request.GetLLMQType(), pQuorumIndex, pQuorum->qc.validMembers, request.GetProTxHash(), vecEncrypted)) {
                sendQDATA(CQuorumDataRequest::Errors::ENCRYPTED_CONTRIBUTIONS_MISSING);
                return;
            }

            ssResponseData << vecEncrypted;
        }

        sendQDATA(CQuorumDataRequest::Errors::NONE, ssResponseData);
        return;
    }

    if (strCommand == NetMsgType::QDATA) {

        if (!fMasternodeMode || pFrom == nullptr || (pFrom->verifiedProRegTxHash.IsNull() && !pFrom->qwatch)) {
            error("Not a verified masternode or a qwatch connection");
            return;
        }

        CQuorumDataRequest request;
        vRecv >> request;

        {
            LOCK2(cs_main, cs_data_requests);
            auto it = mapQuorumDataRequests.find(std::make_pair(pFrom->verifiedProRegTxHash, true));
            if (it == mapQuorumDataRequests.end()) {
                error("Not requested");
                return;
            }
            if (it->second.IsProcessed()) {
                error("Already received");
                return;
            }
            if (request != it->second) {
                error("Not like requested");
                return;
            }
            it->second.SetProcessed();
        }

        if (request.GetError() != CQuorumDataRequest::Errors::NONE) {
            error(strprintf("Error %d", request.GetError()), false);
            return;
        }

        CQuorumPtr pQuorum;
        {
            LOCK(quorumsCacheCs);
            if (!mapQuorumsCache[request.GetLLMQType()].get(request.GetQuorumHash(), pQuorum)) {
                error("Quorum not found", false); // Don't bump score because we asked for it
                return;
            }
        }

        if (request.GetDataMask() & CQuorumDataRequest::QUORUM_VERIFICATION_VECTOR) {

            BLSVerificationVector verficationVector;
            vRecv >> verficationVector;

            if (pQuorum->SetVerificationVector(verficationVector)) {
                CQuorum::StartCachePopulatorThread(pQuorum);
            } else {
                error("Invalid quorum verification vector");
                return;
            }
        }

        if (request.GetDataMask() & CQuorumDataRequest::ENCRYPTED_CONTRIBUTIONS) {

            if (pQuorum->quorumVvec->size() != pQuorum->params.threshold) {
                error("No valid quorum verification vector available", false); // Don't bump score because we asked for it
                return;
            }

            int memberIdx = pQuorum->GetMemberIndex(request.GetProTxHash());
            if (memberIdx == -1) {
                error("Not a member of the quorum", false); // Don't bump score because we asked for it
                return;
            }

            std::vector<CBLSIESEncryptedObject<CBLSSecretKey>> vecEncrypted;
            vRecv >> vecEncrypted;

            BLSSecretKeyVector vecSecretKeys;
            vecSecretKeys.resize(vecEncrypted.size());
            for (size_t i = 0; i < vecEncrypted.size(); ++i) {
                if (!vecEncrypted[i].Decrypt(memberIdx, *activeMasternodeInfo.blsKeyOperator, vecSecretKeys[i], PROTOCOL_VERSION)) {
                    error("Failed to decrypt");
                    return;
                }
            }

            CBLSSecretKey secretKeyShare = blsWorker.AggregateSecretKeys(vecSecretKeys);
            if (!pQuorum->SetSecretKeyShare(secretKeyShare)) {
                error("Invalid secret key share received");
                return;
            }
        }
        pQuorum->WriteContributions(evoDb);
        return;
    }
}

} // namespace llmq
