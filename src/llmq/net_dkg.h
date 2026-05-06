// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LLMQ_NET_DKG_H
#define BITCOIN_LLMQ_NET_DKG_H

#include <net_processing.h>

#include <memory>
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
class CDKGDebugManager;
class CDKGSessionManager;
class CQuorumBlockProcessor;
class CQuorumSnapshotManager;
} // namespace llmq

namespace llmq {
/**
 * NetHandler responsible for DKG networking:
 *  - QCONTRIB / QCOMPLAINT / QJUSTIFICATION / QPCOMMITMENT / QWATCH ProcessMessage
 *    routing into CDKGSessionManager. The resulting MessageProcessingResult is
 *    consumed locally via PeerManagerInternal and never propagated up.
 *  - AlreadyHave for the four MSG_QUORUM_* DKG inv types.
 *  - ProcessGetData for the four MSG_QUORUM_* DKG inv types (active mode only;
 *    in observer mode the underlying Get* calls return false by construction).
 *
 * Active-mode-only deps live in @ref ActiveDKG; @ref m_active is null in
 * observer mode and non-null in active mode (all-or-none).
 *
 * On nodes that run neither active nor observer mode, register @ref NetDKGStub
 * instead.
 */
class NetDKG final : public NetHandler
{
public:
    //! Observer-mode constructor.
    NetDKG(PeerManagerInternal* peer_manager, const CSporkManager& sporkman, CDKGSessionManager& qdkgsman);

    //! Active-mode constructor: takes the masternode-only dep bundle as required references.
    NetDKG(PeerManagerInternal* peer_manager, const CSporkManager& sporkman, CDKGSessionManager& qdkgsman,
           CBLSWorker& bls_worker, CDeterministicMNManager& dmnman, CMasternodeMetaMan& mn_metaman,
           CDKGDebugManager& dkgdbgman, CQuorumBlockProcessor& qblockman, CQuorumSnapshotManager& qsnapman,
           const CActiveMasternodeManager& mn_activeman, const ChainstateManager& chainman, CConnman& connman);

    // NetHandler
    void ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv) override;
    bool AlreadyHave(const CInv& inv) override;
    bool ProcessGetData(CNode& pfrom, const CInv& inv, CConnman& connman, const CNetMsgMaker& msgMaker) override;
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
        CBLSWorker& bls_worker;
        CDeterministicMNManager& dmnman;
        CMasternodeMetaMan& mn_metaman;
        CDKGDebugManager& dkgdbgman;
        CQuorumBlockProcessor& qblockman;
        CQuorumSnapshotManager& qsnapman;
        const CActiveMasternodeManager& mn_activeman;
        const ChainstateManager& chainman;
        CConnman& connman;
    };

    void PhaseHandlerThread(ActiveDKGSessionHandler& handler);
    void HandleDKGRound(ActiveDKGSessionHandler& handler);

    CDKGSessionManager& m_qdkgsman;
    const CSporkManager& m_sporkman;
    const std::unique_ptr<ActiveDKG> m_active; //!< null in observer mode, non-null in active mode

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
    void ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv) override;
};
} // namespace llmq

#endif // BITCOIN_LLMQ_NET_DKG_H
