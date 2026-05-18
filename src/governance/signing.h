// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_GOVERNANCE_SIGNING_H
#define BITCOIN_GOVERNANCE_SIGNING_H

#include <governance/object.h>
#include <governance/superblock.h>

#include <uint256.h>

#include <memory>
#include <optional>
#include <vector>

class CActiveMasternodeManager;
class CBlockIndex;
class CConnman;
class CDeterministicMNManager;
class CGovernanceManager;
class ChainstateManager;
class CMasternodeSync;

namespace governance {
class SuperblockManager;
}

class GovernanceSigner
{
private:
    CConnman& m_connman;
    CDeterministicMNManager& m_dmnman;
    CGovernanceManager& m_govman;
    governance::SuperblockManager& m_superblocks;
    const CActiveMasternodeManager& m_mn_activeman;
    const ChainstateManager& m_chainman;
    const CMasternodeSync& m_mn_sync;

private:
    std::optional<uint256> votedFundingYesTriggerHash{std::nullopt};

public:
    GovernanceSigner() = delete;
    GovernanceSigner(const GovernanceSigner&) = delete;
    GovernanceSigner& operator=(const GovernanceSigner&) = delete;
    explicit GovernanceSigner(CConnman& connman, CDeterministicMNManager& dmnman, CGovernanceManager& govman,
                              governance::SuperblockManager& superblocks, const CActiveMasternodeManager& mn_activeman,
                              const ChainstateManager& chainman, const CMasternodeSync& mn_sync);
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
