// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/llmq_tests.h>
#include <test/util/setup_common.h>

#include <chain.h>
#include <consensus/validation.h>
#include <evo/specialtx.h>
#include <llmq/commitment.h>
#include <llmq/context.h>
#include <llmq/utils.h>
#include <logging.h>
#include <node/context.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <util/check.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>

using namespace llmq;
using namespace llmq::testutils;

BOOST_FIXTURE_TEST_SUITE(llmq_commitment_tests, BasicTestingSetup)

// Get test params for use in tests
static const Consensus::LLMQParams& TEST_PARAMS = GetLLMQParams(Consensus::LLMQType::LLMQ_TEST_V17);

BOOST_AUTO_TEST_CASE(commitment_null_test)
{
    CFinalCommitment commitment;

    // Test default constructor creates null commitment
    BOOST_CHECK(commitment.IsNull());
    BOOST_CHECK(commitment.quorumHash.IsNull());
    BOOST_CHECK(commitment.validMembers.empty());
    BOOST_CHECK(commitment.signers.empty());
    BOOST_CHECK(!commitment.quorumPublicKey.IsValid());
    BOOST_CHECK(!commitment.quorumSig.IsValid());

    // Note: VerifyNull requires valid LLMQ params which we can't test in unit tests
    // It's tested in functional tests
}

BOOST_AUTO_TEST_CASE(commitment_counting_test)
{
    CFinalCommitment commitment;

    // Test empty vectors
    BOOST_CHECK_EQUAL(commitment.CountSigners(), 0);
    BOOST_CHECK_EQUAL(commitment.CountValidMembers(), 0);

    // Test with various patterns
    commitment.signers = {true, false, true, true, false};
    commitment.validMembers = {true, true, false, true, true};

    BOOST_CHECK_EQUAL(commitment.CountSigners(), 3);
    BOOST_CHECK_EQUAL(commitment.CountValidMembers(), 4);

    // Test all true
    commitment.signers = std::vector<bool>(10, true);
    commitment.validMembers = std::vector<bool>(10, true);

    BOOST_CHECK_EQUAL(commitment.CountSigners(), 10);
    BOOST_CHECK_EQUAL(commitment.CountValidMembers(), 10);

    // Test all false
    commitment.signers = std::vector<bool>(10, false);
    commitment.validMembers = std::vector<bool>(10, false);

    BOOST_CHECK_EQUAL(commitment.CountSigners(), 0);
    BOOST_CHECK_EQUAL(commitment.CountValidMembers(), 0);
}

BOOST_AUTO_TEST_CASE(commitment_verify_sizes_test)
{
    CFinalCommitment commitment;
    commitment.llmqType = TEST_PARAMS.type;

    // Test with incorrect sizes (TEST_PARAMS.size is 3, so use a different size)
    commitment.validMembers = std::vector<bool>(5, true);
    commitment.signers = std::vector<bool>(5, true);
    BOOST_CHECK(!commitment.VerifySizes(TEST_PARAMS));

    // Test with correct sizes
    commitment.validMembers = std::vector<bool>(TEST_PARAMS.size, true);
    commitment.signers = std::vector<bool>(TEST_PARAMS.size, true);
    BOOST_CHECK(commitment.VerifySizes(TEST_PARAMS));

    // Test with mismatched sizes
    commitment.validMembers = std::vector<bool>(TEST_PARAMS.size, true);
    commitment.signers = std::vector<bool>(TEST_PARAMS.size + 1, true);
    BOOST_CHECK(!commitment.VerifySizes(TEST_PARAMS));
}

BOOST_FIXTURE_TEST_CASE(commitment_check_undersized_bitset_debug_log_test, RegTestingSetup)
{
    // Catches the OOB-read regression in CheckLLMQCommitment's debug-log loop
    // by capturing log output rather than relying on undefined behaviour to
    // trip a sanitizer. The wire-format validMembers DYNBITSET can deserialize
    // smaller than llmq_params.size; before the clamp the loop iterated up to
    // llmq_params.size and emitted v[0], v[1], ... reading past the bitset.
    // With the clamp an empty bitset must produce "validMembers[]".
    struct LogCaptureGuard {
        const uint64_t saved_categories{LogInstance().GetCategoryMask()};
        std::list<std::function<void(const std::string&)>>::iterator it;
        explicit LogCaptureGuard(std::vector<std::string>& sink)
        {
            LogInstance().EnableCategory(BCLog::LLMQ);
            it = LogInstance().PushBackCallback(
                [&sink](const std::string& msg) { sink.push_back(msg); });
        }
        ~LogCaptureGuard()
        {
            LogInstance().DeleteCallback(it);
            if (!(saved_categories & BCLog::LLMQ)) {
                LogInstance().DisableCategory(BCLog::LLMQ);
            }
        }
    };

    std::vector<std::string> log_lines;
    LogCaptureGuard guard{log_lines};
    BOOST_REQUIRE(LogAcceptDebug(BCLog::LLMQ));

    CFinalCommitmentTxPayload payload;
    payload.nVersion = CFinalCommitmentTxPayload::CURRENT_VERSION;
    // base_index below sits at height 0, so CheckLLMQCommitment expects
    // qcTx.nHeight == 1. Setting it to 2 forces the function to fail with
    // "bad-qc-height" after the pre-validation debug-log block but before
    // LookupBlockIndex / Verify / VerifySizes, which is the path the clamp
    // is meant to harden.
    payload.nHeight = 2;
    payload.commitment.nVersion = CFinalCommitment::LEGACY_BLS_NON_INDEXED_QUORUM_VERSION;
    payload.commitment.llmqType = TEST_PARAMS.type;
    payload.commitment.quorumHash = GetTestQuorumHash(1);
    // TEST_PARAMS.size is 3; an empty bitset models a malformed mined commitment.
    payload.commitment.signers      = std::vector<bool>();
    payload.commitment.validMembers = std::vector<bool>();

    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::SPECIAL_VERSION;
    mtx.nType    = TRANSACTION_QUORUM_COMMITMENT;
    SetTxPayload(mtx, payload);
    const CTransaction tx{mtx};

    // Sanity check that the undersized bitsets survive payload round-trip,
    // so the call below is genuinely exercising the OOB-prone code path.
    const auto opt_round_trip = GetTxPayload<CFinalCommitmentTxPayload>(tx);
    BOOST_REQUIRE(opt_round_trip.has_value());
    BOOST_CHECK_LT(opt_round_trip->commitment.validMembers.size(), static_cast<size_t>(TEST_PARAMS.size));
    BOOST_CHECK_LT(opt_round_trip->commitment.signers.size(),     static_cast<size_t>(TEST_PARAMS.size));

    CBlockIndex base_index;
    base_index.nHeight = 0;

    const llmq::UtilParameters util_params{*Assert(m_node.dmnman),
                                           *Assert(m_node.llmq_ctx)->qsnapman,
                                           *Assert(m_node.chainman),
                                           &base_index};

    TxValidationState state;
    BOOST_CHECK(!llmq::CheckLLMQCommitment(util_params, tx, state));
    BOOST_CHECK(state.IsInvalid());
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-qc-height");

    // Locate the validMembers debug line emitted by CheckLLMQCommitment and
    // assert the loop was clamped: with an empty bitset the rendered list must
    // be empty. Old code emitted "v[0]=..." here even though the bitset had no
    // elements, so checking for the absence of "v[0]" pins down the regression.
    const std::string marker = "CFinalCommitment -- CheckLLMQCommitment";
    const auto it = std::find_if(log_lines.begin(), log_lines.end(),
                                 [&marker](const std::string& s) {
                                     return s.find(marker) != std::string::npos;
                                 });
    BOOST_REQUIRE_MESSAGE(it != log_lines.end(),
                          "CheckLLMQCommitment debug line not captured");
    BOOST_CHECK_MESSAGE(it->find("validMembers[]") != std::string::npos,
                        "expected clamped 'validMembers[]', got: " + *it);
    BOOST_CHECK_MESSAGE(it->find("v[0]") == std::string::npos,
                        "unexpected v[0] in clamped log line: " + *it);
}

BOOST_AUTO_TEST_CASE(commitment_serialization_test)
{
    // Test with valid commitment
    CFinalCommitment commitment = CreateValidCommitment(TEST_PARAMS, GetTestQuorumHash(1));

    // Test serialization preserves all fields
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << commitment;

    CFinalCommitment deserialized;
    ss >> deserialized;

    BOOST_CHECK_EQUAL(commitment.llmqType, deserialized.llmqType);
    BOOST_CHECK(commitment.quorumHash == deserialized.quorumHash);
    BOOST_CHECK(commitment.validMembers == deserialized.validMembers);
    BOOST_CHECK(commitment.signers == deserialized.signers);
    BOOST_CHECK(commitment.quorumVvecHash == deserialized.quorumVvecHash);
    BOOST_CHECK(commitment.quorumPublicKey == deserialized.quorumPublicKey);
    BOOST_CHECK(commitment.quorumSig == deserialized.quorumSig);
    BOOST_CHECK(commitment.membersSig == deserialized.membersSig);
    BOOST_CHECK_EQUAL(commitment.IsNull(), deserialized.IsNull());
}

BOOST_AUTO_TEST_CASE(commitment_version_test)
{
    // Test version calculation (first param is rotation enabled, second is basic scheme active)
    // With rotation enabled and basic scheme
    BOOST_CHECK_EQUAL(CFinalCommitment::GetVersion(true, true), CFinalCommitment::BASIC_BLS_INDEXED_QUORUM_VERSION);
    // With rotation enabled but legacy scheme
    BOOST_CHECK_EQUAL(CFinalCommitment::GetVersion(true, false), CFinalCommitment::LEGACY_BLS_INDEXED_QUORUM_VERSION);
    // Without rotation but basic scheme
    BOOST_CHECK_EQUAL(CFinalCommitment::GetVersion(false, true), CFinalCommitment::BASIC_BLS_NON_INDEXED_QUORUM_VERSION);
    // Without rotation and legacy scheme
    BOOST_CHECK_EQUAL(CFinalCommitment::GetVersion(false, false), CFinalCommitment::LEGACY_BLS_NON_INDEXED_QUORUM_VERSION);
}

BOOST_AUTO_TEST_CASE(commitment_json_test)
{
    CFinalCommitment commitment = CreateValidCommitment(TEST_PARAMS, GetTestQuorumHash(1));

    UniValue json = commitment.ToJson();

    // Verify JSON contains expected fields
    BOOST_CHECK(json.exists("llmqType"));
    BOOST_CHECK(json.exists("quorumHash"));
    BOOST_CHECK(json.exists("signers"));
    BOOST_CHECK(json.exists("validMembers"));
    BOOST_CHECK(json.exists("quorumPublicKey"));
    BOOST_CHECK(json.exists("quorumVvecHash"));
    BOOST_CHECK(json.exists("quorumSig"));
    BOOST_CHECK(json.exists("membersSig"));

    // Verify counts are included
    BOOST_CHECK(json.exists("signersCount"));
    BOOST_CHECK(json.exists("validMembersCount"));

    BOOST_CHECK_EQUAL(json["signersCount"].getInt<int>(), commitment.CountSigners());
    BOOST_CHECK_EQUAL(json["validMembersCount"].getInt<int>(), commitment.CountValidMembers());
}

BOOST_AUTO_TEST_CASE(commitment_bitvector_json_test)
{
    // Test bit vector serialization through JSON output
    CFinalCommitment commitment;
    commitment.llmqType = TEST_PARAMS.type;
    commitment.quorumHash = GetTestQuorumHash(1);

    // Test empty vectors
    commitment.validMembers.clear();
    commitment.signers.clear();
    UniValue json = commitment.ToJson();
    BOOST_CHECK_EQUAL(json["validMembers"].get_str(), "");
    BOOST_CHECK_EQUAL(json["signers"].get_str(), "");

    // Test single byte patterns
    commitment.validMembers = std::vector<bool>(8, false);
    commitment.signers = std::vector<bool>(8, false);
    json = commitment.ToJson();
    BOOST_CHECK_EQUAL(json["validMembers"].get_str(), "00");
    BOOST_CHECK_EQUAL(json["signers"].get_str(), "00");

    commitment.validMembers = std::vector<bool>(8, true);
    commitment.signers = std::vector<bool>(8, true);
    json = commitment.ToJson();
    BOOST_CHECK_EQUAL(json["validMembers"].get_str(), "ff");
    BOOST_CHECK_EQUAL(json["signers"].get_str(), "ff");

    // Test specific patterns
    // Note: Bit order in serialization is LSB first within each byte
    commitment.validMembers = {true, false, true, false, true, false, true, false}; // 0x55 (01010101 in LSB)
    commitment.signers = {false, true, false, true, false, true, false, true};      // 0xAA (10101010 in LSB)
    json = commitment.ToJson();
    BOOST_CHECK_EQUAL(json["validMembers"].get_str(), "55");
    BOOST_CHECK_EQUAL(json["signers"].get_str(), "aa");

    // Test non-byte-aligned sizes (should pad with zeros)
    commitment.validMembers = {true, true, true, true, true}; // 0x1F padded
    commitment.signers = commitment.validMembers;
    json = commitment.ToJson();
    BOOST_CHECK_EQUAL(json["validMembers"].get_str(), "1f");
    BOOST_CHECK_EQUAL(json["signers"].get_str(), "1f");
}

BOOST_AUTO_TEST_CASE(commitment_verify_null_edge_cases)
{
    CFinalCommitment commitment;

    // Fresh commitment should be null
    BOOST_CHECK(commitment.IsNull());

    // Setting quorumHash alone doesn't make it non-null
    // (IsNull() doesn't check quorumHash)
    commitment.quorumHash = GetTestQuorumHash(1);
    BOOST_CHECK(commitment.IsNull());
    commitment.quorumHash.SetNull();

    // Setting llmqType alone doesn't make it non-null
    commitment.llmqType = Consensus::LLMQType::LLMQ_TEST;
    BOOST_CHECK(commitment.IsNull());
    commitment.llmqType = Consensus::LLMQType::LLMQ_NONE;

    // Setting validMembers with true values makes it non-null
    commitment.validMembers = {true};
    BOOST_CHECK(!commitment.IsNull());
    commitment.validMembers.clear();

    // Setting signers with only false values keeps it null
    commitment.signers = {false};
    BOOST_CHECK(commitment.IsNull());

    // Setting signers with true values makes it non-null
    commitment.signers = {true};
    BOOST_CHECK(!commitment.IsNull());
    commitment.signers.clear();

    // Setting quorumPublicKey makes it non-null
    commitment.quorumPublicKey = CreateRandomBLSPublicKey();
    BOOST_CHECK(!commitment.IsNull());

    // Reset and test quorumVvecHash
    commitment = CFinalCommitment{};
    commitment.quorumVvecHash = GetTestQuorumHash(2);
    BOOST_CHECK(!commitment.IsNull());

    // Reset and test signatures
    commitment = CFinalCommitment{};
    commitment.membersSig = CreateRandomBLSSignature();
    BOOST_CHECK(!commitment.IsNull());

    commitment = CFinalCommitment{};
    commitment.quorumSig = CreateRandomBLSSignature();
    BOOST_CHECK(!commitment.IsNull());
}

BOOST_AUTO_TEST_CASE(commitment_tx_payload_test)
{
    CFinalCommitmentTxPayload payload;
    payload.nHeight = 12345;
    payload.commitment = CreateValidCommitment(TEST_PARAMS, GetTestQuorumHash(1));

    // Test basic construction
    BOOST_CHECK_EQUAL(payload.nVersion, CFinalCommitmentTxPayload::CURRENT_VERSION);
    BOOST_CHECK_EQUAL(payload.nHeight, 12345);
    BOOST_CHECK(!payload.commitment.IsNull());
}

BOOST_AUTO_TEST_CASE(build_commitment_hash_test)
{
    // Test deterministic hash generation
    uint256 hash1 = llmq::BuildCommitmentHash(TEST_PARAMS.type, GetTestQuorumHash(1),
                                              CreateBitVector(TEST_PARAMS.size, {0, 1, 2}), CreateRandomBLSPublicKey(),
                                              GetTestQuorumHash(2));

    // Same inputs should produce same hash
    uint256 hash2 = llmq::BuildCommitmentHash(TEST_PARAMS.type, GetTestQuorumHash(1),
                                              CreateBitVector(TEST_PARAMS.size, {0, 1, 2}), CreateRandomBLSPublicKey(),
                                              GetTestQuorumHash(2));

    // Different quorum hash should produce different hash
    uint256 hash3 = llmq::BuildCommitmentHash(TEST_PARAMS.type,
                                              GetTestQuorumHash(2), // Different
                                              CreateBitVector(TEST_PARAMS.size, {0, 1, 2}), CreateRandomBLSPublicKey(),
                                              GetTestQuorumHash(2));

    BOOST_CHECK(hash1 != hash2); // Different pubkeys
    BOOST_CHECK(hash1 != hash3);
    BOOST_CHECK(hash2 != hash3);

    // Test with same deterministic data
    CBLSPublicKey fixedPubKey = CreateRandomBLSPublicKey();
    uint256 hash4 = llmq::BuildCommitmentHash(TEST_PARAMS.type, GetTestQuorumHash(1),
                                              CreateBitVector(TEST_PARAMS.size, {0, 1, 2}), fixedPubKey,
                                              GetTestQuorumHash(2));

    uint256 hash5 = llmq::BuildCommitmentHash(TEST_PARAMS.type, GetTestQuorumHash(1),
                                              CreateBitVector(TEST_PARAMS.size, {0, 1, 2}), fixedPubKey,
                                              GetTestQuorumHash(2));

    BOOST_CHECK(hash4 == hash5);
}

BOOST_AUTO_TEST_SUITE_END()
