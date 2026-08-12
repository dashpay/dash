// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LLMQ_NET_SIGNING_H
#define BITCOIN_LLMQ_NET_SIGNING_H

#include <ctpl_stl.h>
#include <llmq/signing_shares.h>
#include <llmq/types.h>
#include <net_processing.h>
#include <util/threadinterrupt.h>
#include <util/time.h>
#include <validationinterface.h>

#include <thread>

#include <atomic>
#include <memory>

class CSporkManager;

namespace llmq {
class CSigSharesManager;
class CSigningManager;

//! Decode a QBSIGSHARES payload into its vector of CBatchedSigShares.
//!
//! The inner sigShares vector of each batch is bounded by CBatchedSigShares's
//! SERIALIZE_METHODS via LIMITED_VECTOR, but many individually-valid batches
//! could still exceed the total sig-share cap, so this bounds the outer batch
//! count and checks the running total of inner sig shares as it decodes,
//! stopping before an attacker forces us through the full cross product of the
//! per-vector limits. A wire count above the cap (outer batch count or running
//! inner total) throws std::ios_base::failure once detected, leaving the caller
//! to log, ban, and rethrow uniformly.
std::vector<CBatchedSigShares> UnserializeBatchedSigShares(CDataStream& vRecv);

class NetSigning final : public NetHandler, public CValidationInterface
{
public:
    NetSigning(PeerManagerInternal* peer_manager, CSigningManager& sig_manager, CSigSharesManager* shares_manager,
               const CSporkManager& sporkman) :
        NetHandler(peer_manager),
        m_sig_manager{sig_manager},
        m_shares_manager{shares_manager},
        m_sporkman{sporkman}
    {
        workInterrupt.reset();
    }
    void ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv) override;

    [[nodiscard]] bool ProcessPendingRecoveredSigs();
    void ProcessRecoveredSig(std::shared_ptr<const CRecoveredSig> recoveredSig, bool consider_proactive_relay);

protected:
    // CValidationInterface
    void NotifyRecoveredSig(const std::shared_ptr<const CRecoveredSig>& sig, bool proactive_relay) override;

    // NetSigning
    void Start() override;
    void Stop() override;
    void Interrupt() override { workInterrupt(); };

private:
    void WorkThreadSigning();
    void WorkThreadCleaning();
    void WorkThreadDispatcher();

    void ProcessPendingSigShares(
        std::unordered_map<NodeId, std::vector<CSigShare>>&& sigSharesByNodes,
        std::unordered_map<std::pair<Consensus::LLMQType, uint256>, CQuorumCPtr, StaticSaltedHasher>&& quorums);

    void RemoveDisconnectedOrDiscouragedNodeStates();
    void BanNode(NodeId nodeid);

private:
    CSigningManager& m_sig_manager;
    CSigSharesManager* m_shares_manager;
    const CSporkManager& m_sporkman;

    CleanupThrottler<NodeClock> cleanupThrottler;

    std::thread signing_thread;
    std::thread shares_cleaning_thread;
    std::thread shares_dispatcher_thread;
    mutable ctpl::thread_pool worker_pool;
    std::atomic<int> unverified_batches{0};

    CThreadInterrupt workInterrupt;
};

} // namespace llmq

#endif // BITCOIN_LLMQ_NET_SIGNING_H
