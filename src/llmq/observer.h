// Copyright (c) 2025-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LLMQ_OBSERVER_H
#define BITCOIN_LLMQ_OBSERVER_H

#include <llmq/quorumsman.h>

#include <validationinterface.h>

#include <gsl/pointers.h>
#include <span.h>

#include <memory>

class CBLSWorker;
class CBlockIndex;
class CDataStream;
class CDeterministicMNManager;
class CMasternodeMetaMan;
class CNode;
class CSporkManager;
struct MessageProcessingResult;
namespace llmq {
class CDKGDebugManager;
class CDKGSessionManager;
class CQuorum;
class CQuorumBlockProcessor;
class CQuorumDataRequest;
class CQuorumSnapshotManager;
} // namespace llmq
namespace util {
struct DbWrapperParams;
} // namespace util

namespace llmq {
struct ObserverContext final : public QuorumRole, public CValidationInterface {
public:
    ObserverContext() = delete;
    ObserverContext(const ObserverContext&) = delete;
    ObserverContext& operator=(const ObserverContext&) = delete;
    ObserverContext(CBLSWorker& bls_worker, CDeterministicMNManager& dmnman,
                    CMasternodeMetaMan& mn_metaman, llmq::CQuorumBlockProcessor& qblockman,
                    llmq::CQuorumManager& qman, llmq::CQuorumSnapshotManager& qsnapman, const ChainstateManager& chainman,
                    const CSporkManager& sporkman, const util::DbWrapperParams& db_params);
    ~ObserverContext();

    // QuorumRole
    // Watch-only nodes are not masternodes
    bool IsMasternode() const override { return false; }
    // We are only initialized if watch-only mode is enabled
    bool IsWatching() const override { return true; }
    bool SetQuorumSecretKeyShare(CQuorum& quorum, Span<CBLSSecretKey> skContributions) const override { return false; }
    [[nodiscard]] MessageProcessingResult ProcessContribQGETDATA(bool request_limit_exceeded, CDataStream& vStream,
                                                                 const CQuorum& quorum, CQuorumDataRequest& request,
                                                                 gsl::not_null<const CBlockIndex*> block_index) override;
    [[nodiscard]] MessageProcessingResult ProcessContribQDATA(CNode& pfrom, CDataStream& vStream, CQuorum& quorum,
                                                              CQuorumDataRequest& request) override;
protected:
    // CValidationInterface
    void UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload) override;

public:
    const std::unique_ptr<llmq::CDKGDebugManager> dkgdbgman;
    const std::unique_ptr<llmq::CDKGSessionManager> qdkgsman;
};
} // namespace llmq

#endif // BITCOIN_LLMQ_OBSERVER_H
