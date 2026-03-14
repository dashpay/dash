// Copyright (c) 2018-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/observer/quorums.h>

#include <llmq/quorums.h>
#include <msg_result.h>

#include <chain.h>
#include <net.h>

namespace llmq {
QuorumObserver::QuorumObserver(CConnman& connman, CDeterministicMNManager& dmnman, CQuorumManager& qman,
                               CQuorumSnapshotManager& qsnapman, const ChainstateManager& chainman,
                               const CMasternodeSync& mn_sync, const CSporkManager& sporkman,
                               const llmq::QvvecSyncModeMap& sync_map, bool quorums_recovery) :
    QuorumRoleBase{connman, dmnman, qman, qsnapman, chainman, mn_sync, sporkman, sync_map, quorums_recovery}
{
}

bool QuorumObserver::IsMasternode() const
{
    // Watch-only nodes are not masternodes
    return false;
}

bool QuorumObserver::IsWatching() const
{
    // We are only initialized if watch-only mode is enabled
    return true;
}

bool QuorumObserver::SetQuorumSecretKeyShare(CQuorum& quorum, Span<CBLSSecretKey> skContributions) const
{
    return false;
}

MessageProcessingResult QuorumObserver::ProcessContribQGETDATA(bool request_limit_exceeded, CDataStream& vStream,
                                                               const CQuorum& quorum, CQuorumDataRequest& request,
                                                               gsl::not_null<const CBlockIndex*> block_index)
{
    // Watch-only nodes cannot provide encrypted contributions
    if (request.GetDataMask() & CQuorumDataRequest::ENCRYPTED_CONTRIBUTIONS) {
        request.SetError(CQuorumDataRequest::Errors::ENCRYPTED_CONTRIBUTIONS_MISSING);
        return request_limit_exceeded ? MisbehavingError{25, "request limit exceeded"} : MessageProcessingResult{};
    }
    return {};
}

MessageProcessingResult QuorumObserver::ProcessContribQDATA(CNode& pfrom, CDataStream& vStream,
                                                            CQuorum& quorum, CQuorumDataRequest& request)
{
    // Watch-only nodes ignore encrypted contributions
    return {};
}

} // namespace llmq
