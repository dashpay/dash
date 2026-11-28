// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LLMQ_NET_DKG_H
#define BITCOIN_LLMQ_NET_DKG_H

#include <consensus/params.h>
#include <llmq/cache.h>
#include <net_processing.h>
#include <sync.h>
#include <uint256.h>

#include <map>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

class CActiveMasternodeManager;
class CBLSWorker;
class CConnman;
class CDeterministicMNManager;
class ChainstateManager;
class CMasternodeMetaMan;
class CSporkManager;
namespace llmq {
class ActiveDKGSessionHandler;
class CDKGComplaint;
class CDKGContribution;
class CDKGDebugManager;
class CDKGJustification;
class CDKGPrematureCommitment;
class CDKGSessionManager;
class CQuorumBlockProcessor;
class CQuorumManager;
class CQuorumSnapshotManager;
class QuorumRole;
} // namespace llmq

namespace llmq {

/**
 * Framing-only validation of a raw DKG payload, run at network intake before
 * retention: checks truncation, trailing bytes, and quorum-param bounds on a
 * copy of @p payload without decoding any BLS object. Typed deserialization
 * happens exactly once, later, on the DKG worker thread.
 *
 * @warning This walk mirrors the (Un)serialize implementations in
 *          llmq/dkgmessages.h. Any change to those must be reflected here;
 *          src/test/fuzz/dkg_message_framing.cpp asserts that this never
 *          rejects a payload the worker would accept.
 */
bool CheckDKGMessageWireStructure(std::string_view msg_type, const DataStream& payload,
                                  const Consensus::LLMQParams& params);

/**
 * Param-only structural bounds on a typed DKG message, run by the DKG worker
 * right after deserializing queued bytes and before PreVerifyMessage.
 */
bool CheckDKGMessageStructure(const CDKGContribution& qc, const Consensus::LLMQParams& params);
bool CheckDKGMessageStructure(const CDKGComplaint& qc, const Consensus::LLMQParams& params);
bool CheckDKGMessageStructure(const CDKGJustification& qj, const Consensus::LLMQParams& params);
bool CheckDKGMessageStructure(const CDKGPrematureCommitment& qc, const Consensus::LLMQParams& params);

/**
 * NetHandler responsible for DKG networking:
 *  - QCONTRIB / QCOMPLAINT / QJUSTIFICATION / QPCOMMITMENT / QWATCH ProcessMessage
 *    routing into CDKGSessionManager in active mode. Observer mode ignores DKG
 *    round payloads because it has no worker to consume them.
 *  - AlreadyHave for the four MSG_QUORUM_* DKG inv types. Observer mode returns
 *    true so it does not request payloads it cannot consume.
 *  - ProcessGetData for the four MSG_QUORUM_* DKG inv types (active mode only;
 *    in observer mode the underlying Get* calls return false by construction).
 *
 * Active-mode-only deps live in @ref ActiveDKG; @ref m_active is null in
 * observer mode and non-null in active mode (all-or-none). Observers neither
 * request nor retain DKG round payloads.
 *
 * On nodes that run neither active nor observer mode, register @ref NetDKGStub
 * instead.
 */
class NetDKG final : public NetHandler
{
public:
    //! Observer-mode constructor.
    NetDKG(PeerManagerInternal* peer_manager, const CSporkManager& sporkman, CDKGSessionManager& qdkgsman,
           const ChainstateManager& chainman, CQuorumManager& qman, QuorumRole& role);

    //! Active-mode constructor: takes the masternode-only dep bundle as required references.
    NetDKG(PeerManagerInternal* peer_manager, const CSporkManager& sporkman, CDKGSessionManager& qdkgsman,
           const ChainstateManager& chainman, bool quorums_watch, CQuorumManager& qman, QuorumRole& role,
           CBLSWorker& bls_worker, CDeterministicMNManager& dmnman, CMasternodeMetaMan& mn_metaman,
           CDKGDebugManager& dkgdbgman, CQuorumBlockProcessor& qblockman, CQuorumSnapshotManager& qsnapman,
           const CActiveMasternodeManager& mn_activeman, CConnman& connman);

    ~NetDKG() override;

    // NetHandler
    void ProcessMessage(CNode& pfrom, const std::string& msg_type, DataStream& vRecv) override
        EXCLUSIVE_LOCKS_REQUIRED(!cs_indexed_quorums_cache);
    bool AlreadyHave(const CInv& inv) override;
    bool ProcessGetData(CNode& pfrom, const CInv& inv) override;
    /**
     * Drives one phase-handler thread per ActiveDKGSessionHandler in active mode;
     * no-op in observer mode (no curSession to drive).
     */
    void Start() override;
    void Stop() override;
    void Interrupt() override;

private:
    //! Bundle of refs that exist only in active masternode mode.
    struct ActiveDKG {
        CDeterministicMNManager& dmnman;
        CMasternodeMetaMan& mn_metaman;
        CDKGDebugManager& dkgdbgman;
        CQuorumBlockProcessor& qblockman;
        CQuorumSnapshotManager& qsnapman;
        CConnman& connman;
    };

    void PhaseHandlerThread(ActiveDKGSessionHandler& handler);
    void HandleDKGRound(ActiveDKGSessionHandler& handler);

    CDKGSessionManager& m_qdkgsman;
    CQuorumManager& m_qman;
    const CSporkManager& m_sporkman;
    const ChainstateManager& m_chainman;
    const std::unique_ptr<ActiveDKG> m_active; //!< null in observer mode, non-null in active mode

    /** Cache: quorum hash → quorum index, populated lazily by ProcessMessage. */
    mutable Mutex cs_indexed_quorums_cache;
    mutable PerLlmqTypeCache<int> indexed_quorums_cache GUARDED_BY(cs_indexed_quorums_cache);

    std::vector<std::thread> m_phase_threads;
};

/**
 * Minimal NetHandler installed on nodes that run neither active nor observer
 * DKG mode. Just punishes peers that push DKG messages we cannot serve.
 */
class NetDKGStub final : public NetHandler
{
public:
    explicit NetDKGStub(PeerManagerInternal* peer_manager) :
        NetHandler(peer_manager)
    {
    }

    // NetHandler
    void ProcessMessage(CNode& pfrom, const std::string& msg_type, DataStream& vRecv) override;
};
} // namespace llmq

#endif // BITCOIN_LLMQ_NET_DKG_H
