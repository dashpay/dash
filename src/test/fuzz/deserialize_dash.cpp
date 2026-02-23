// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Deserialization fuzz targets for Dash-specific types.
// Follows the same pattern as deserialize.cpp for Bitcoin Core types.

#include <test/fuzz/fuzz.h>
#include <test/util/setup_common.h>

#include <streams.h>
#include <version.h>

#include <bls/bls_ies.h>
#include <coinjoin/coinjoin.h>
#include <evo/assetlocktx.h>
#include <evo/cbtx.h>
#include <evo/creditpool.h>
#include <evo/deterministicmns.h>
#include <evo/dmnstate.h>
#include <evo/mnauth.h>
#include <evo/mnhftx.h>
#include <evo/providertx.h>
#include <evo/simplifiedmns.h>
#include <evo/smldiff.h>
#include <governance/common.h>
#include <governance/object.h>
#include <governance/vote.h>
#include <governance/votedb.h>
#include <llmq/commitment.h>
#include <llmq/dkgsession.h>
#include <llmq/quorums.h>
#include <llmq/signing.h>
#include <llmq/signing_shares.h>
#include <llmq/snapshot.h>

#include <exception>
#include <optional>
#include <stdexcept>
#include <stdint.h>

namespace {

const BasicTestingSetup* g_setup;

struct dash_invalid_fuzzing_input_exception : public std::exception {
};

template <typename T>
void DashDeserializeFromFuzzingInput(FuzzBufferType buffer, T& obj,
                                     const std::optional<int> protocol_version = std::nullopt,
                                     const int ser_type = SER_NETWORK)
{
    CDataStream ds(buffer, ser_type, PROTOCOL_VERSION);
    if (protocol_version) {
        ds.SetVersion(*protocol_version);
    } else {
        try {
            int version;
            ds >> version;
            ds.SetVersion(version);
        } catch (const std::ios_base::failure&) {
            throw dash_invalid_fuzzing_input_exception();
        }
    }
    try {
        ds >> obj;
    } catch (const std::ios_base::failure&) {
        throw dash_invalid_fuzzing_input_exception();
    }
}

} // namespace

void initialize_deserialize_dash()
{
    static const auto testing_setup = MakeNoLogFileContext<>();
    g_setup = testing_setup.get();
}

#define FUZZ_TARGET_DASH_DESERIALIZE(name, code)                \
    FUZZ_TARGET(name, .init = initialize_deserialize_dash)      \
    {                                                           \
        try {                                                   \
            code                                                \
        } catch (const dash_invalid_fuzzing_input_exception&) { \
        }                                                       \
    }

// --- evo/ types: Provider transactions ---

FUZZ_TARGET_DASH_DESERIALIZE(dash_proreg_tx_deserialize, {
    CProRegTx obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_proupserv_tx_deserialize, {
    CProUpServTx obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_proupreg_tx_deserialize, {
    CProUpRegTx obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_prouprev_tx_deserialize, {
    CProUpRevTx obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})

// --- evo/ types: Asset Lock/Unlock (L1↔L2 bridge) ---

FUZZ_TARGET_DASH_DESERIALIZE(dash_asset_lock_payload_deserialize, {
    CAssetLockPayload obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_asset_unlock_payload_deserialize, {
    CAssetUnlockPayload obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})

// --- evo/ types: Coinbase special payload ---

FUZZ_TARGET_DASH_DESERIALIZE(dash_cbtx_deserialize, {
    CCbTx obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})

// --- evo/ types: Credit pool ---

FUZZ_TARGET_DASH_DESERIALIZE(dash_credit_pool_deserialize, {
    CCreditPool obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})

// --- evo/ types: Deterministic masternode ---

FUZZ_TARGET_DASH_DESERIALIZE(dash_deterministic_mn_deserialize, {
    CDeterministicMN obj(0 /* internalId, will be overwritten by deserialization */);
    DashDeserializeFromFuzzingInput(buffer, obj);
})

// --- evo/ types: Deterministic masternode state ---

FUZZ_TARGET_DASH_DESERIALIZE(dash_dmn_state_deserialize, {
    CDeterministicMNState obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_dmn_state_diff_deserialize, {
    CDeterministicMNStateDiff obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})

// --- evo/ types: Simplified MN list ---

FUZZ_TARGET_DASH_DESERIALIZE(dash_smn_list_entry_deserialize, {
    CSimplifiedMNListEntry obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_get_smn_list_diff_deserialize, {
    CGetSimplifiedMNListDiff obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_smn_list_diff_deserialize, {
    CSimplifiedMNListDiff obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})

// --- evo/ types: MN auth and hard fork signaling ---

FUZZ_TARGET_DASH_DESERIALIZE(dash_mnauth_deserialize, {
    CMNAuth obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_mnhf_tx_deserialize, {
    MNHFTx obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_mnhf_tx_payload_deserialize, {
    MNHFTxPayload obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})

// --- llmq/ types: Quorum commitment ---

FUZZ_TARGET_DASH_DESERIALIZE(dash_final_commitment_deserialize, {
    llmq::CFinalCommitment obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_final_commitment_tx_payload_deserialize, {
    llmq::CFinalCommitmentTxPayload obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})

// --- llmq/ types: DKG messages ---

FUZZ_TARGET_DASH_DESERIALIZE(dash_dkg_complaint_deserialize, {
    llmq::CDKGComplaint obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_dkg_justification_deserialize, {
    llmq::CDKGJustification obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_dkg_premature_commitment_deserialize, {
    llmq::CDKGPrematureCommitment obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})

// --- llmq/ types: Signing ---

FUZZ_TARGET_DASH_DESERIALIZE(dash_recovered_sig_deserialize, {
    llmq::CRecoveredSig obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_sig_share_deserialize, {
    llmq::CSigShare obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_sig_ses_ann_deserialize, {
    llmq::CSigSesAnn obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_sig_shares_inv_deserialize, {
    llmq::CSigSharesInv obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_batched_sig_shares_deserialize, {
    llmq::CBatchedSigShares obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})

// --- llmq/ types: Quorum data and rotation ---

FUZZ_TARGET_DASH_DESERIALIZE(dash_quorum_data_request_deserialize, {
    llmq::CQuorumDataRequest obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_get_quorum_rotation_info_deserialize, {
    llmq::CGetQuorumRotationInfo obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_quorum_snapshot_deserialize, {
    llmq::CQuorumSnapshot obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})

// --- governance/ types ---

FUZZ_TARGET_DASH_DESERIALIZE(dash_governance_object_common_deserialize, {
    Governance::Object obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_governance_object_deserialize, {
    CGovernanceObject obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_governance_vote_deserialize, {
    CGovernanceVote obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_vote_instance_deserialize, {
    vote_instance_t obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_vote_rec_deserialize, {
    vote_rec_t obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_governance_vote_file_deserialize, {
    CGovernanceObjectVoteFile obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})

// --- bls/ types ---

FUZZ_TARGET_DASH_DESERIALIZE(dash_bls_ies_encrypted_blob_deserialize, {
    CBLSIESEncryptedBlob obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_bls_ies_multi_recipient_blobs_deserialize, {
    CBLSIESMultiRecipientBlobs obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})

// --- coinjoin/ types ---

FUZZ_TARGET_DASH_DESERIALIZE(dash_coinjoin_status_update_deserialize, {
    CCoinJoinStatusUpdate obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_coinjoin_accept_deserialize, {
    CCoinJoinAccept obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_coinjoin_entry_deserialize, {
    CCoinJoinEntry obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_coinjoin_queue_deserialize, {
    CCoinJoinQueue obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_DESERIALIZE(dash_coinjoin_broadcast_tx_deserialize, {
    CCoinJoinBroadcastTx obj;
    DashDeserializeFromFuzzingInput(buffer, obj);
})
