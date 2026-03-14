// Copyright (c) 2018-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LLMQ_OBSERVER_QUORUMS_H
#define BITCOIN_LLMQ_OBSERVER_QUORUMS_H

#include <llmq/quorumsman.h>

#include <gsl/pointers.h>

#include <span.h>

class CBlockIndex;
class CConnman;
class CDataStream;
class CDeterministicMNManager;
class CMasternodeSync;
class CNode;
class CSporkManager;
struct MessageProcessingResult;
namespace llmq {
class CQuorum;
class CQuorumDataRequest;
class CQuorumManager;
class CQuorumSnapshotManager;
} // namespace llmq

namespace llmq {

class QuorumObserver final : public QuorumRoleBase, public QuorumRole
{
public:
    QuorumObserver() = delete;
    QuorumObserver(const QuorumObserver&) = delete;
    QuorumObserver& operator=(const QuorumObserver&) = delete;
    explicit QuorumObserver(CConnman& connman, CDeterministicMNManager& dmnman, CQuorumManager& qman,
                            CQuorumSnapshotManager& qsnapman, const ChainstateManager& chainman,
                            const CMasternodeSync& mn_sync, const CSporkManager& sporkman,
                            const llmq::QvvecSyncModeMap& sync_map, bool quorums_recovery);
    ~QuorumObserver() override = default;

    bool IsMasternode() const override;
    bool IsWatching() const override;

    bool SetQuorumSecretKeyShare(CQuorum& quorum, Span<CBLSSecretKey> skContributions) const override;

    [[nodiscard]] MessageProcessingResult ProcessContribQGETDATA(
        bool request_limit_exceeded, CDataStream& vStream,
        const CQuorum& quorum, CQuorumDataRequest& request,
        gsl::not_null<const CBlockIndex*> block_index) override;

    [[nodiscard]] MessageProcessingResult ProcessContribQDATA(
        CNode& pfrom, CDataStream& vStream,
        CQuorum& quorum, CQuorumDataRequest& request) override;
};
} // namespace llmq

#endif // BITCOIN_LLMQ_OBSERVER_QUORUMS_H
