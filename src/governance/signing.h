// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_GOVERNANCE_SIGNING_H
#define BITCOIN_GOVERNANCE_SIGNING_H

#include <governance/classes.h>
#include <governance/object.h>

#include <uint256.h>

#include <memory>
#include <optional>
#include <vector>

class CActiveMasternodeManager;
class CBlockIndex;
class CConnman;
class CDeterministicMNList;
class CDeterministicMNManager;
class CGovernanceException;
class CGovernanceVote;
class ChainstateManager;
class CMasternodeSync;
class CNode;
enum vote_outcome_enum_t : int;

class GovernanceSignerParent
{
public:
    virtual ~GovernanceSignerParent() = default;

    virtual bool IsValid() const = 0;
    virtual bool GetBestSuperblock(const CDeterministicMNList& tip_mn_list,
                                   std::shared_ptr<CSuperblock>& pSuperblockRet, int nBlockHeight) = 0;
    virtual bool MasternodeRateCheck(const CGovernanceObject& govobj, bool fUpdateFailStatus = false) = 0;
    virtual bool ProcessVoteAndRelay(const CGovernanceVote& vote, CGovernanceException& exception, CConnman& connman) = 0;
    virtual int GetCachedBlockHeight() const = 0;
    virtual std::shared_ptr<CGovernanceObject> FindGovernanceObject(const uint256& nHash) = 0;
    virtual std::shared_ptr<CGovernanceObject> FindGovernanceObjectByDataHash(const uint256& nDataHash) = 0;
    virtual std::vector<std::shared_ptr<CSuperblock>> GetActiveTriggers() const = 0;
    virtual std::vector<std::shared_ptr<const CGovernanceObject>> GetApprovedProposals(
        const CDeterministicMNList& tip_mn_list) = 0;
    virtual void AddGovernanceObject(CGovernanceObject& govobj, const CNode* pfrom = nullptr) = 0;
};

class GovernanceSigner
{
private:
    CConnman& m_connman;
    CDeterministicMNManager& m_dmnman;
    GovernanceSignerParent& m_govman;
    const CActiveMasternodeManager& m_mn_activeman;
    const ChainstateManager& m_chainman;
    const CMasternodeSync& m_mn_sync;

private:
    std::optional<uint256> votedFundingYesTriggerHash{std::nullopt};

public:
    GovernanceSigner() = delete;
    GovernanceSigner(const GovernanceSigner&) = delete;
    GovernanceSigner& operator=(const GovernanceSigner&) = delete;
    explicit GovernanceSigner(CConnman& connman, CDeterministicMNManager& dmnman, GovernanceSignerParent& govman,
                              const CActiveMasternodeManager& mn_activeman, const ChainstateManager& chainman,
                              const CMasternodeSync& mn_sync);
    ~GovernanceSigner();

    void UpdatedBlockTip(const CBlockIndex* pindex);

private:
    bool HasAlreadyVotedFundingTrigger() const;
    bool VoteFundingTrigger(const uint256& nHash, const vote_outcome_enum_t outcome);
    std::optional<const CGovernanceObject> CreateGovernanceTrigger(const std::optional<const CSuperblock>& sb_opt);
    std::optional<const CSuperblock> CreateSuperblockCandidate(int nHeight) const;
    void ResetVotedFundingTrigger();
    void VoteGovernanceTriggers(const std::optional<const CGovernanceObject>& trigger_opt);
};

#endif // BITCOIN_GOVERNANCE_SIGNING_H
