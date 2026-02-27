// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Roundtrip (deserialize -> serialize -> deserialize -> serialize) fuzz targets
// for Dash-specific types.

#include <test/fuzz/fuzz.h>
#include <test/util/setup_common.h>

#include <streams.h>
#include <version.h>

// evo/ types
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

// llmq/ types
#include <llmq/commitment.h>
#include <llmq/dkgsession.h>
#include <llmq/quorums.h>
#include <llmq/signing.h>
#include <llmq/signing_shares.h>
#include <llmq/snapshot.h>

// governance/ types
#include <governance/object.h>

// bls/ types
#include <bls/bls_ies.h>

// coinjoin/ types
#include <coinjoin/coinjoin.h>

#include <cassert>
#include <exception>
#include <stdint.h>

namespace {

const BasicTestingSetup* g_setup;

struct dash_invalid_fuzzing_input_exception : public std::exception {
};

template <typename T>
void DashRoundtripFromFuzzingInput(FuzzBufferType buffer, T& obj)
{
    CDataStream ds(buffer, SER_NETWORK, INIT_PROTO_VERSION);
    try {
        int version;
        ds >> version;
        ds.SetVersion(version);
    } catch (const std::ios_base::failure&) {
        throw dash_invalid_fuzzing_input_exception();
    }

    try {
        ds >> obj;
    } catch (const std::ios_base::failure&) {
        throw dash_invalid_fuzzing_input_exception();
    }

    CDataStream sink(SER_NETWORK, ds.GetVersion());
    sink << obj;
    assert(!sink.empty());

    CDataStream ds2(SER_NETWORK, ds.GetVersion());
    ds2 << obj;

    T obj2 = obj;
    CDataStream ds3(Span<const std::byte>{ds2}, SER_NETWORK, ds.GetVersion());
    try {
        ds3 >> obj2;
    } catch (const std::ios_base::failure&) {
        assert(false);
    }

    CDataStream ds4(SER_NETWORK, ds.GetVersion());
    ds4 << obj2;

    assert(MakeByteSpan(ds2) == MakeByteSpan(ds4));
}

template <typename T>
void DashRoundtripFromFuzzingInput(FuzzBufferType buffer)
{
    T obj;
    DashRoundtripFromFuzzingInput(buffer, obj);
}

} // namespace

void initialize_roundtrip_dash()
{
    static const auto testing_setup = MakeNoLogFileContext<>();
    g_setup = testing_setup.get();
}

#define FUZZ_TARGET_DASH_ROUNDTRIP(name, code)                  \
    FUZZ_TARGET(name, .init = initialize_roundtrip_dash)        \
    {                                                           \
        try {                                                   \
            code                                                \
        } catch (const dash_invalid_fuzzing_input_exception&) { \
        }                                                       \
    }

FUZZ_TARGET_DASH_ROUNDTRIP(dash_proreg_tx_roundtrip, { DashRoundtripFromFuzzingInput<CProRegTx>(buffer); })
FUZZ_TARGET_DASH_ROUNDTRIP(dash_proupserv_tx_roundtrip, { DashRoundtripFromFuzzingInput<CProUpServTx>(buffer); })
FUZZ_TARGET_DASH_ROUNDTRIP(dash_proupreg_tx_roundtrip, { DashRoundtripFromFuzzingInput<CProUpRegTx>(buffer); })
FUZZ_TARGET_DASH_ROUNDTRIP(dash_prouprev_tx_roundtrip, { DashRoundtripFromFuzzingInput<CProUpRevTx>(buffer); })

FUZZ_TARGET_DASH_ROUNDTRIP(dash_asset_lock_payload_roundtrip,
                           { DashRoundtripFromFuzzingInput<CAssetLockPayload>(buffer); })
FUZZ_TARGET_DASH_ROUNDTRIP(dash_asset_unlock_payload_roundtrip,
                           { DashRoundtripFromFuzzingInput<CAssetUnlockPayload>(buffer); })

FUZZ_TARGET_DASH_ROUNDTRIP(dash_cbtx_roundtrip, { DashRoundtripFromFuzzingInput<CCbTx>(buffer); })
FUZZ_TARGET_DASH_ROUNDTRIP(dash_credit_pool_roundtrip, { DashRoundtripFromFuzzingInput<CCreditPool>(buffer); })

FUZZ_TARGET_DASH_ROUNDTRIP(dash_deterministic_mn_roundtrip, {
    CDeterministicMN obj(0 /* internalId, overwritten by deserialization */);
    DashRoundtripFromFuzzingInput(buffer, obj);
})
FUZZ_TARGET_DASH_ROUNDTRIP(dash_dmn_state_roundtrip, { DashRoundtripFromFuzzingInput<CDeterministicMNState>(buffer); })
FUZZ_TARGET_DASH_ROUNDTRIP(dash_dmn_state_diff_roundtrip,
                           { DashRoundtripFromFuzzingInput<CDeterministicMNStateDiff>(buffer); })

FUZZ_TARGET_DASH_ROUNDTRIP(dash_smn_list_entry_roundtrip,
                           { DashRoundtripFromFuzzingInput<CSimplifiedMNListEntry>(buffer); })
FUZZ_TARGET_DASH_ROUNDTRIP(dash_get_smn_list_diff_roundtrip,
                           { DashRoundtripFromFuzzingInput<CGetSimplifiedMNListDiff>(buffer); })
FUZZ_TARGET_DASH_ROUNDTRIP(dash_smn_list_diff_roundtrip,
                           { DashRoundtripFromFuzzingInput<CSimplifiedMNListDiff>(buffer); })

FUZZ_TARGET_DASH_ROUNDTRIP(dash_mnauth_roundtrip, { DashRoundtripFromFuzzingInput<CMNAuth>(buffer); })
FUZZ_TARGET_DASH_ROUNDTRIP(dash_mnhf_tx_payload_roundtrip, { DashRoundtripFromFuzzingInput<MNHFTxPayload>(buffer); })

FUZZ_TARGET_DASH_ROUNDTRIP(dash_final_commitment_roundtrip,
                           { DashRoundtripFromFuzzingInput<llmq::CFinalCommitment>(buffer); })
FUZZ_TARGET_DASH_ROUNDTRIP(dash_final_commitment_tx_payload_roundtrip,
                           { DashRoundtripFromFuzzingInput<llmq::CFinalCommitmentTxPayload>(buffer); })

FUZZ_TARGET_DASH_ROUNDTRIP(dash_dkg_complaint_roundtrip, { DashRoundtripFromFuzzingInput<llmq::CDKGComplaint>(buffer); })
FUZZ_TARGET_DASH_ROUNDTRIP(dash_dkg_justification_roundtrip,
                           { DashRoundtripFromFuzzingInput<llmq::CDKGJustification>(buffer); })
FUZZ_TARGET_DASH_ROUNDTRIP(dash_dkg_premature_commitment_roundtrip,
                           { DashRoundtripFromFuzzingInput<llmq::CDKGPrematureCommitment>(buffer); })

FUZZ_TARGET_DASH_ROUNDTRIP(dash_recovered_sig_roundtrip, { DashRoundtripFromFuzzingInput<llmq::CRecoveredSig>(buffer); })
FUZZ_TARGET_DASH_ROUNDTRIP(dash_sig_share_roundtrip, { DashRoundtripFromFuzzingInput<llmq::CSigShare>(buffer); })
FUZZ_TARGET_DASH_ROUNDTRIP(dash_sig_ses_ann_roundtrip, { DashRoundtripFromFuzzingInput<llmq::CSigSesAnn>(buffer); })
FUZZ_TARGET_DASH_ROUNDTRIP(dash_sig_shares_inv_roundtrip, { DashRoundtripFromFuzzingInput<llmq::CSigSharesInv>(buffer); })
FUZZ_TARGET_DASH_ROUNDTRIP(dash_batched_sig_shares_roundtrip,
                           { DashRoundtripFromFuzzingInput<llmq::CBatchedSigShares>(buffer); })

FUZZ_TARGET_DASH_ROUNDTRIP(dash_quorum_data_request_roundtrip,
                           { DashRoundtripFromFuzzingInput<llmq::CQuorumDataRequest>(buffer); })
FUZZ_TARGET_DASH_ROUNDTRIP(dash_get_quorum_rotation_info_roundtrip,
                           { DashRoundtripFromFuzzingInput<llmq::CGetQuorumRotationInfo>(buffer); })
FUZZ_TARGET_DASH_ROUNDTRIP(dash_quorum_snapshot_roundtrip,
                           { DashRoundtripFromFuzzingInput<llmq::CQuorumSnapshot>(buffer); })

FUZZ_TARGET_DASH_ROUNDTRIP(dash_governance_object_roundtrip,
                           { DashRoundtripFromFuzzingInput<CGovernanceObject>(buffer); })

FUZZ_TARGET_DASH_ROUNDTRIP(dash_bls_ies_encrypted_blob_roundtrip,
                           { DashRoundtripFromFuzzingInput<CBLSIESEncryptedBlob>(buffer); })
FUZZ_TARGET_DASH_ROUNDTRIP(dash_bls_ies_multi_recipient_blobs_roundtrip,
                           { DashRoundtripFromFuzzingInput<CBLSIESMultiRecipientBlobs>(buffer); })

FUZZ_TARGET_DASH_ROUNDTRIP(dash_coinjoin_status_update_roundtrip,
                           { DashRoundtripFromFuzzingInput<CCoinJoinStatusUpdate>(buffer); })
FUZZ_TARGET_DASH_ROUNDTRIP(dash_coinjoin_accept_roundtrip, { DashRoundtripFromFuzzingInput<CCoinJoinAccept>(buffer); })
FUZZ_TARGET_DASH_ROUNDTRIP(dash_coinjoin_entry_roundtrip, { DashRoundtripFromFuzzingInput<CCoinJoinEntry>(buffer); })
FUZZ_TARGET_DASH_ROUNDTRIP(dash_coinjoin_queue_roundtrip, { DashRoundtripFromFuzzingInput<CCoinJoinQueue>(buffer); })
FUZZ_TARGET_DASH_ROUNDTRIP(dash_coinjoin_broadcast_tx_roundtrip,
                           { DashRoundtripFromFuzzingInput<CCoinJoinBroadcastTx>(buffer); })
