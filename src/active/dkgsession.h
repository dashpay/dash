// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ACTIVE_DKGSESSION_H
#define BITCOIN_ACTIVE_DKGSESSION_H

#include <llmq/blockprocessor.h>
#include <llmq/dkgsession.h>

#include <optional>
#include <utility>

class CActiveMasternodeManager;
class CDeterministicMNList;
class CSporkManager;
class CMasternodeMetaMan;

namespace llmq {
//! About a week of mainnet blocks
constexpr int SIGNING_HISTORY_WINDOW{4032};
//! About a day of mainnet blocks: how far ahead the at-risk check looks for records leaving the window
constexpr int SIGNING_HISTORY_EXPIRY_LOOKAHEAD{576};

std::pair<int, int> CountSignedCommitments(const SigningRecords& records, const CDeterministicMNList& mn_list,
                                           const uint256& proTxHash);
bool HasInsufficientSigningHistory(int valid, int signed_count);

struct SigningHistorySummary {
    //! Only quorums based after this height count: the registration or last revival, -1 if not in the list
    int cutoff_height{-1};
    int valid{0};
    int signed_count{0};
    //! Valid commitments of quorums based after the most recent signed one
    int recent_unsigned{0};
    bool voted_bad{false};
    //! Whether one more unsigned commitment would get it voted bad, now or at some point while the records of
    //! quorums based until the expiring height leave the window. Several more within the time between two
    //! gatherings can still get it voted bad without a warning first.
    bool one_miss_from_voted_bad{false};
    //! Whether the history would get it voted bad at some point while the records of quorums based until
    //! the expiring height leave the window, even without another unsigned commitment
    bool voted_bad_after_expiry{false};
};
SigningHistorySummary SummarizeSigningHistory(const SigningRecords& records, const CDeterministicMNList& mn_list,
                                              const uint256& proTxHash, int expiring_until_height);
//! Heading towards being voted bad, but not there yet
bool IsSigningHistoryAtRisk(const SigningHistorySummary& summary);
//! The expiring height to summarize records gathered at gathering_height with
int SigningHistoryExpiringUntil(int gathering_height);

class ActiveDKGSession final : public llmq::CDKGSession
{
private:
    CMasternodeMetaMan& m_mn_metaman;
    const CQuorumBlockProcessor& m_qblockman;
    const CActiveMasternodeManager& m_mn_activeman;
    const CSporkManager& m_sporkman;

public:
    ActiveDKGSession() = delete;
    ActiveDKGSession(const ActiveDKGSession&) = delete;
    ActiveDKGSession& operator=(const ActiveDKGSession&) = delete;
    ActiveDKGSession(CBLSWorker& bls_worker, CDeterministicMNManager& dmnman, CDKGDebugManager& dkgdbgman,
                     CDKGSessionManager& qdkgsman, CMasternodeMetaMan& mn_metaman,
                     const CQuorumBlockProcessor& qblockman, CQuorumSnapshotManager& qsnapman,
                     const CActiveMasternodeManager& mn_activeman, const ChainstateManager& chainman,
                     const CSporkManager& sporkman, const CBlockIndex* base_block_index,
                     const Consensus::LLMQParams& params);
    ~ActiveDKGSession() override;

public:
    // Phase 1: contribution
    std::optional<CDKGContribution> Contribute() override;
    std::optional<CDKGContribution> SendContributions() override;
    void VerifyPendingContributions() override EXCLUSIVE_LOCKS_REQUIRED(cs_pending);

    // Phase 2: complaint
    std::optional<CDKGComplaint> VerifyAndComplain(CConnman& connman) override EXCLUSIVE_LOCKS_REQUIRED(!cs_pending);
    void VerifyConnectionAndMinProtoVersions(CConnman& connman) const override;
    std::optional<CDKGComplaint> SendComplaint() override;

    // Phase 3: justification
    std::optional<CDKGJustification> VerifyAndJustify() override EXCLUSIVE_LOCKS_REQUIRED(!invCs);
    std::optional<CDKGJustification> SendJustification(const Uint256HashSet& forMembers) override;

    // Phase 4: commit
    std::optional<CDKGPrematureCommitment> VerifyAndCommit() override;
    std::optional<CDKGPrematureCommitment> SendCommitment() override;

    // Phase 5: aggregate/finalize
    std::vector<CFinalCommitment> FinalizeCommitments() EXCLUSIVE_LOCKS_REQUIRED(!invCs) override;

    // All Phases 5-in-1 for single-node-quorum
    CFinalCommitment FinalizeSingleCommitment() override;

private:
    //! CDKGSession
    bool MaybeDecrypt(const CBLSIESMultiRecipientObjects<CBLSSecretKey>& obj, size_t idx, CBLSSecretKey& ret_obj,
                      int version) override;
};
} // namespace llmq

#endif // BITCOIN_ACTIVE_DKGSESSION_H
