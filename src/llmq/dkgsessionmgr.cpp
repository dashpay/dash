// Copyright (c) 2018-2021 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/dkgsessionmgr.h>
#include <llmq/debug.h>
#include <llmq/utils.h>

#include <evo/deterministicmns.h>

#include <chainparams.h>
#include <net_processing.h>
#include <spork.h>
#include <validation.h>

namespace llmq
{

CDKGSessionManager* quorumDKGSessionManager;

static const std::string DB_VVEC = "qdkg_V";
static const std::string DB_SKCONTRIB = "qdkg_S";
static const std::string DB_ENC_CONTRIB = "qdkg_E";

CDKGSessionManager::CDKGSessionManager(CBLSWorker& _blsWorker, bool unitTests, bool fWipe) :
    blsWorker(_blsWorker)
{
    db = std::make_unique<CDBWrapper>(unitTests ? "" : (GetDataDir() / "llmq/dkgdb"), 1 << 20, unitTests, fWipe);
    MigrateDKG();

    for (const auto& [llmqType, llmqParams] : Params().GetConsensus().llmqs) {
        dkgSessionHandlers.emplace(std::piecewise_construct,
                std::forward_as_tuple(llmqType),
                std::forward_as_tuple(llmqParams, blsWorker, *this));
    }
}

CDKGSessionManager::~CDKGSessionManager() = default;

void CDKGSessionManager::MigrateDKG()
{
    if (!db->IsEmpty()) return;

    LogPrint(BCLog::LLMQ, "CDKGSessionManager::%d -- start\n", __func__);

    CDBBatch batch(*db);
    auto oldDb = std::make_unique<CDBWrapper>(GetDataDir() / "llmq", 8 << 20);
    std::unique_ptr<CDBIterator> pcursor(oldDb->NewIterator());

    auto start_vvec = std::make_tuple(DB_VVEC, (Consensus::LLMQType)0, uint256(), uint256());
    pcursor->Seek(start_vvec);

    while (pcursor->Valid()) {
        decltype(start_vvec) k;
        BLSVerificationVector v;

        if (!pcursor->GetKey(k) || std::get<0>(k) != DB_VVEC) {
            break;
        }
        if (!pcursor->GetValue(v)) {
            break;
        }

        batch.Write(k, v);

        if (batch.SizeEstimate() >= (1 << 24)) {
            db->WriteBatch(batch);
            batch.Clear();
        }

        pcursor->Next();
    }

    auto start_contrib = std::make_tuple(DB_SKCONTRIB, (Consensus::LLMQType)0, uint256(), uint256());
    pcursor->Seek(start_contrib);

    while (pcursor->Valid()) {
        decltype(start_contrib) k;
        CBLSSecretKey v;

        if (!pcursor->GetKey(k) || std::get<0>(k) != DB_SKCONTRIB) {
            break;
        }
        if (!pcursor->GetValue(v)) {
            break;
        }

        batch.Write(k, v);

        if (batch.SizeEstimate() >= (1 << 24)) {
            db->WriteBatch(batch);
            batch.Clear();
        }

        pcursor->Next();
    }

    auto start_enc_contrib = std::make_tuple(DB_ENC_CONTRIB, (Consensus::LLMQType)0, uint256(), uint256());
    pcursor->Seek(start_enc_contrib);

    while (pcursor->Valid()) {
        decltype(start_enc_contrib) k;
        CBLSIESMultiRecipientObjects<CBLSSecretKey> v;

        if (!pcursor->GetKey(k) || std::get<0>(k) != DB_ENC_CONTRIB) {
            break;
        }
        if (!pcursor->GetValue(v)) {
            break;
        }

        batch.Write(k, v);

        if (batch.SizeEstimate() >= (1 << 24)) {
            db->WriteBatch(batch);
            batch.Clear();
        }

        pcursor->Next();
    }

    db->WriteBatch(batch);
    pcursor.reset();
    oldDb.reset();

    LogPrint(BCLog::LLMQ, "CDKGSessionManager::%d -- done\n", __func__);
}

void CDKGSessionManager::StartThreads()
{
    for (auto& [_, dkgSession] : dkgSessionHandlers) {
        dkgSession.StartThread();
    }
}

void CDKGSessionManager::StopThreads()
{
    for (auto& [_, dkgSession] : dkgSessionHandlers) {
        dkgSession.StopThread();
    }
}

void CDKGSessionManager::UpdatedBlockTip(const CBlockIndex* pindexNew, bool fInitialDownload)
{
    CleanupCache();

    if (fInitialDownload)
        return;
    if (!deterministicMNManager->IsDIP3Enforced(pindexNew->nHeight))
        return;
    if (!IsQuorumDKGEnabled())
        return;

    for (auto& [_, sessionHandler] : dkgSessionHandlers) {
        sessionHandler.UpdatedBlockTip(pindexNew);
    }
}

void CDKGSessionManager::ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv)
{
    if (!IsQuorumDKGEnabled())
        return;

    if (strCommand != NetMsgType::QCONTRIB
        && strCommand != NetMsgType::QCOMPLAINT
        && strCommand != NetMsgType::QJUSTIFICATION
        && strCommand != NetMsgType::QPCOMMITMENT
        && strCommand != NetMsgType::QWATCH) {
        return;
    }

    if (strCommand == NetMsgType::QWATCH) {
        pfrom->qwatch = true;
        return;
    }

    if (vRecv.empty()) {
        LOCK(cs_main);
        Misbehaving(pfrom->GetId(), 100);
        return;
    }

    // peek into the message and see which LLMQType it is. First byte of all messages is always the LLMQType
    auto llmqType = (Consensus::LLMQType)*vRecv.begin();
    if (!dkgSessionHandlers.count(llmqType)) {
        LOCK(cs_main);
        Misbehaving(pfrom->GetId(), 100);
        return;
    }

    dkgSessionHandlers.at(llmqType).ProcessMessage(pfrom, strCommand, vRecv);
}

bool CDKGSessionManager::AlreadyHave(const CInv& inv) const
{
    if (!IsQuorumDKGEnabled())
        return false;

    for (const auto& [_, dkgSession] : dkgSessionHandlers) {
        if (dkgSession.pendingContributions.HasSeen(inv.hash)
            || dkgSession.pendingComplaints.HasSeen(inv.hash)
            || dkgSession.pendingJustifications.HasSeen(inv.hash)
            || dkgSession.pendingPrematureCommitments.HasSeen(inv.hash)) {
            return true;
        }
    }
    return false;
}

std::optional<CDKGContribution> CDKGSessionManager::GetContribution(const uint256& hash) const
{
    if (!IsQuorumDKGEnabled()) return std::nullopt;

    for (const auto& [_, dkgSessionHandler] : dkgSessionHandlers) {
        LOCK(dkgSessionHandler.cs);
        if (dkgSessionHandler.phase < QuorumPhase_Initialized || dkgSessionHandler.phase > QuorumPhase_Contribute) {
            continue;
        }
        LOCK(dkgSessionHandler.curSession->invCs);
        auto it = dkgSessionHandler.curSession->contributions.find(hash);
        if (it != dkgSessionHandler.curSession->contributions.end()) {
            return {it->second};
        }
    }
    return std::nullopt;
}

std::optional<CDKGComplaint> CDKGSessionManager::GetComplaint(const uint256& hash) const
{
    if (!IsQuorumDKGEnabled()) return std::nullopt;

    for (const auto& [_, dkgSessionHandler] : dkgSessionHandlers) {
        LOCK(dkgSessionHandler.cs);
        if (dkgSessionHandler.phase < QuorumPhase_Contribute || dkgSessionHandler.phase > QuorumPhase_Complain) {
            continue;
        }
        LOCK(dkgSessionHandler.curSession->invCs);
        auto it = dkgSessionHandler.curSession->complaints.find(hash);
        if (it != dkgSessionHandler.curSession->complaints.end()) {
            return {it->second};
        }
    }
    return std::nullopt;
}

std::optional<CDKGJustification> CDKGSessionManager::GetJustification(const uint256& hash) const
{
    if (!IsQuorumDKGEnabled()) return std::nullopt;

    for (const auto& [_, dkgSessionHandler] : dkgSessionHandlers) {
        LOCK(dkgSessionHandler.cs);
        if (dkgSessionHandler.phase < QuorumPhase_Complain || dkgSessionHandler.phase > QuorumPhase_Justify) {
            continue;
        }
        LOCK(dkgSessionHandler.curSession->invCs);
        auto it = dkgSessionHandler.curSession->justifications.find(hash);
        if (it != dkgSessionHandler.curSession->justifications.end()) {
            return {it->second};
        }
    }
    return std::nullopt;
}

std::optional<CDKGPrematureCommitment> CDKGSessionManager::GetPrematureCommitment(const uint256& hash) const
{
    if (!IsQuorumDKGEnabled()) return std::nullopt;

    for (const auto& [_, dkgSessionHandler] : dkgSessionHandlers) {
        LOCK(dkgSessionHandler.cs);
        if (dkgSessionHandler.phase < QuorumPhase_Justify || dkgSessionHandler.phase > QuorumPhase_Commit) {
            continue;
        }
        LOCK(dkgSessionHandler.curSession->invCs);
        auto it = dkgSessionHandler.curSession->prematureCommitments.find(hash);
        if (it != dkgSessionHandler.curSession->prematureCommitments.end() && dkgSessionHandler.curSession->validCommitments.count(hash)) {
            return {it->second};
        }
    }
    return std::nullopt;
}

void CDKGSessionManager::WriteVerifiedVvecContribution(Consensus::LLMQType llmqType, const CBlockIndex* pQuorumBaseBlockIndex, const uint256& proTxHash, const BLSVerificationVectorPtr& vvec)
{
    db->Write(std::make_tuple(DB_VVEC, llmqType, pQuorumBaseBlockIndex->GetBlockHash(), proTxHash), *vvec);
}

void CDKGSessionManager::WriteVerifiedSkContribution(Consensus::LLMQType llmqType, const CBlockIndex* pQuorumBaseBlockIndex, const uint256& proTxHash, const CBLSSecretKey& skContribution)
{
    db->Write(std::make_tuple(DB_SKCONTRIB, llmqType, pQuorumBaseBlockIndex->GetBlockHash(), proTxHash), skContribution);
}

void CDKGSessionManager::WriteEncryptedContributions(Consensus::LLMQType llmqType, const CBlockIndex* pQuorumBaseBlockIndex, const uint256& proTxHash, const CBLSIESMultiRecipientObjects<CBLSSecretKey>& contributions)
{
    db->Write(std::make_tuple(DB_ENC_CONTRIB, llmqType, pQuorumBaseBlockIndex->GetBlockHash(), proTxHash), contributions);
}

std::optional<CDKGSessionManager::contributions> CDKGSessionManager::GetVerifiedContributions(Consensus::LLMQType llmqType, const CBlockIndex* pQuorumBaseBlockIndex, const std::vector<bool>& validMembers) const
{
    LOCK(contributionsCacheCs);
    auto members = CLLMQUtils::GetAllQuorumMembers(GetLLMQParams(llmqType), pQuorumBaseBlockIndex);

    CDKGSessionManager::contributions result(members.size());
    auto& [memberIndexes, vvecs, vecSkContributions] = result;
    for (size_t i = 0; i < members.size(); i++) {
        if (validMembers[i]) {
            const uint256& proTxHash = members[i]->proTxHash;
            ContributionsCacheKey cacheKey = {llmqType, pQuorumBaseBlockIndex->GetBlockHash(), proTxHash};
            auto it = contributionsCache.find(cacheKey);
            if (it == contributionsCache.end()) {
                auto vvecPtr = std::make_shared<BLSVerificationVector>();
                CBLSSecretKey skContribution;
                if (!db->Read(std::make_tuple(DB_VVEC, llmqType, pQuorumBaseBlockIndex->GetBlockHash(), proTxHash), *vvecPtr)) {
                    return std::nullopt;
                }
                db->Read(std::make_tuple(DB_SKCONTRIB, llmqType, pQuorumBaseBlockIndex->GetBlockHash(), proTxHash), skContribution);

                it = contributionsCache.emplace(cacheKey, ContributionsCacheEntry{GetTimeMillis(), vvecPtr, skContribution}).first;
            }

            memberIndexes.emplace_back(i);
            vvecs.emplace_back(it->second.vvec);
            vecSkContributions.emplace_back(it->second.skContribution);
        }
    }
    return {result};
}

bool CDKGSessionManager::GetEncryptedContributions(Consensus::LLMQType llmqType, const CBlockIndex* pQuorumBaseBlockIndex, const std::vector<bool>& validMembers, const uint256& nProTxHash, std::vector<CBLSIESEncryptedObject<CBLSSecretKey>>& vecRet) const
{
    auto members = CLLMQUtils::GetAllQuorumMembers(GetLLMQParams(llmqType), pQuorumBaseBlockIndex);

    vecRet.clear();
    vecRet.reserve(members.size());

    size_t nRequestedMemberIdx{std::numeric_limits<size_t>::max()};
    for (size_t i = 0; i < members.size(); i++) {
        if (members[i]->proTxHash == nProTxHash) {
            nRequestedMemberIdx = i;
            break;
        }
    }
    if (nRequestedMemberIdx == std::numeric_limits<size_t>::max()) {
        return false;
    }

    for (size_t i = 0; i < members.size(); i++) {
        if (validMembers[i]) {
            CBLSIESMultiRecipientObjects<CBLSSecretKey> encryptedContributions;
            if (!db->Read(std::make_tuple(DB_ENC_CONTRIB, llmqType, pQuorumBaseBlockIndex->GetBlockHash(), members[i]->proTxHash), encryptedContributions)) {
                return false;
            }
            vecRet.emplace_back(encryptedContributions.Get(nRequestedMemberIdx));
        }
    }
    return true;
}

void CDKGSessionManager::CleanupCache() const
{
    LOCK(contributionsCacheCs);
    auto curTime = GetTimeMillis();
    for (auto it = contributionsCache.begin(); it != contributionsCache.end(); ) {
        if (curTime - it->second.entryTime > MAX_CONTRIBUTION_CACHE_TIME) {
            it = contributionsCache.erase(it);
        } else {
            ++it;
        }
    }
}

bool IsQuorumDKGEnabled()
{
    return sporkManager.IsSporkActive(SPORK_17_QUORUM_DKG_ENABLED);
}

} // namespace llmq
