// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/net_dkg.h>

#include <llmq/dkgsession.h>
#include <llmq/dkgsessionmgr.h>
#include <netmessagemaker.h>
#include <protocol.h>

namespace llmq {

NetDKG::NetDKG(PeerManagerInternal* peer_manager, const CSporkManager& sporkman, CDKGSessionManager& qdkgsman) :
    NetHandler(peer_manager),
    m_qdkgsman{qdkgsman},
    m_sporkman{sporkman},
    m_active{nullptr}
{
}

NetDKG::NetDKG(PeerManagerInternal* peer_manager, const CSporkManager& sporkman, CDKGSessionManager& qdkgsman,
               CBLSWorker& bls_worker, CDeterministicMNManager& dmnman, CMasternodeMetaMan& mn_metaman,
               CDKGDebugManager& dkgdbgman, CQuorumBlockProcessor& qblockman, CQuorumSnapshotManager& qsnapman,
               const CActiveMasternodeManager& mn_activeman, const ChainstateManager& chainman, CConnman& connman) :
    NetHandler(peer_manager),
    m_qdkgsman{qdkgsman},
    m_sporkman{sporkman},
    m_active{std::make_unique<ActiveDKG>(
        ActiveDKG{bls_worker, dmnman, mn_metaman, dkgdbgman, qblockman, qsnapman, mn_activeman, chainman, connman})}
{
}

void NetDKG::ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv)
{
    auto result = m_qdkgsman.ProcessMessage(pfrom, /*is_masternode=*/m_active != nullptr, msg_type, vRecv);
    if (result.m_error) {
        m_peer_manager->PeerMisbehaving(pfrom.GetId(), result.m_error->score, result.m_error->message);
    }
    if (result.m_to_erase) {
        WITH_LOCK(::cs_main, m_peer_manager->PeerEraseObjectRequest(pfrom.GetId(), *result.m_to_erase));
    }
}

bool NetDKG::AlreadyHave(const CInv& inv)
{
    switch (inv.type) {
    case MSG_QUORUM_CONTRIB:
    case MSG_QUORUM_COMPLAINT:
    case MSG_QUORUM_JUSTIFICATION:
    case MSG_QUORUM_PREMATURE_COMMITMENT:
        return m_qdkgsman.AlreadyHave(inv);
    }
    return false;
}

bool NetDKG::ProcessGetData(CNode& pfrom, const CInv& inv, CConnman& connman, const CNetMsgMaker& msgMaker)
{
    // Default implementations of GetContribution and the other virtual methods
    // return false in observer mode; m_active is only an early exit and does
    // not affect logic.
    if (m_active == nullptr) return false;

    switch (inv.type) {
    case MSG_QUORUM_CONTRIB: {
        CDKGContribution o;
        if (m_qdkgsman.GetContribution(inv.hash, o)) {
            connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::QCONTRIB, o));
            return true;
        }
        return false;
    }
    case MSG_QUORUM_COMPLAINT: {
        CDKGComplaint o;
        if (m_qdkgsman.GetComplaint(inv.hash, o)) {
            connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::QCOMPLAINT, o));
            return true;
        }
        return false;
    }
    case MSG_QUORUM_JUSTIFICATION: {
        CDKGJustification o;
        if (m_qdkgsman.GetJustification(inv.hash, o)) {
            connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::QJUSTIFICATION, o));
            return true;
        }
        return false;
    }
    case MSG_QUORUM_PREMATURE_COMMITMENT: {
        CDKGPrematureCommitment o;
        if (m_qdkgsman.GetPrematureCommitment(inv.hash, o)) {
            connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::QPCOMMITMENT, o));
            return true;
        }
        return false;
    }
    }
    return false;
}

void NetDKGStub::ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv)
{
    if (msg_type == NetMsgType::QCONTRIB || msg_type == NetMsgType::QCOMPLAINT || msg_type == NetMsgType::QJUSTIFICATION ||
        msg_type == NetMsgType::QPCOMMITMENT || msg_type == NetMsgType::QWATCH) {
        m_peer_manager->PeerMisbehaving(pfrom.GetId(), 10);
    }
}

} // namespace llmq
