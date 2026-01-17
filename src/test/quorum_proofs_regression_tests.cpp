// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <bls/bls.h>
#include <evo/evodb.h>
#include <llmq/blockprocessor.h>
#include <llmq/context.h>
#include <llmq/quorumproofs.h>
#include <primitives/block.h>
#include <primitives/transaction.h>

#include <boost/test/unit_test.hpp>

//
// REGRESSION TESTS for security issues identified in code review
// These tests should FAIL before the fix and PASS after
//

// Use RegTestingSetup for tests that need full node infrastructure
BOOST_FIXTURE_TEST_SUITE(quorum_proofs_regression_tests, RegTestingSetup)

// Trivial test case to ensure the suite always has at least one test case
// even in builds where some functionality may not be available
BOOST_AUTO_TEST_CASE(trivially_passes) { BOOST_CHECK(true); }

// Regression test: Forged chainlock signature should be REJECTED
// BUG: VerifyProofChain only checks signature.IsValid() (format), not actual BLS verification
// This test FAILS before the fix (error is NOT about signature), PASSES after (error IS about signature)
BOOST_AUTO_TEST_CASE(forged_chainlock_signature_rejected)
{
    // Skip if llmq_ctx is not available (shouldn't happen in RegTestingSetup)
    if (!m_node.llmq_ctx || !m_node.llmq_ctx->quorum_block_processor) {
        BOOST_TEST_MESSAGE("Skipping test: LLMQ context not available");
        return;
    }

    // Create the proof manager
    llmq::CQuorumProofManager proofManager(*m_node.evodb, *m_node.llmq_ctx->quorum_block_processor);

    // Create a legitimate quorum key
    CBLSSecretKey legitimateKey;
    legitimateKey.MakeNewKey();

    // Create an ATTACKER's key (different from legitimate)
    CBLSSecretKey attackerKey;
    attackerKey.MakeNewKey();

    // Create checkpoint with the LEGITIMATE quorum key
    llmq::QuorumCheckpoint checkpoint;
    checkpoint.blockHash = uint256::ONE;
    checkpoint.height = 99;

    llmq::QuorumCheckpoint::QuorumEntry checkpointQuorum;
    checkpointQuorum.quorumHash = uint256::TWO;
    checkpointQuorum.quorumType = Consensus::LLMQType::LLMQ_TEST;
    checkpointQuorum.publicKey = legitimateKey.GetPublicKey();
    checkpoint.chainlockQuorums.push_back(checkpointQuorum);

    // Create a chainlock signed with ATTACKER's key (not the checkpoint's key)
    llmq::ChainlockProofEntry clEntry;
    clEntry.nHeight = 100;
    clEntry.blockHash = uint256::ONE;
    // Sign with attacker's key - this is the forged signature
    clEntry.signature = attackerKey.Sign(clEntry.blockHash, /*specificLegacyScheme=*/false);

    // Verify the signature is format-valid but cryptographically invalid
    BOOST_CHECK(clEntry.signature.IsValid());  // Format is valid
    BOOST_CHECK(!clEntry.signature.VerifyInsecure(legitimateKey.GetPublicKey(), clEntry.blockHash, false));  // But doesn't verify

    // Create minimal proof chain with the forged chainlock
    llmq::QuorumProofChain chain;

    // Add a header
    CBlockHeader header;
    header.nVersion = 1;
    header.hashPrevBlock = uint256::ZERO;
    header.hashMerkleRoot = uint256::ONE;
    header.nTime = 1234567890;
    header.nBits = 0x1d00ffff;
    header.nNonce = 1;
    chain.headers.push_back(header);

    // Add the forged chainlock
    chain.chainlocks.push_back(clEntry);

    // Add minimal quorum proof
    llmq::QuorumCommitmentProof qProof;
    qProof.commitment.llmqType = Consensus::LLMQType::LLMQ_TEST;
    qProof.commitment.quorumHash = uint256::TWO;
    qProof.chainlockIndex = 0;

    CMutableTransaction mtx;
    mtx.nVersion = 3;
    mtx.nType = TRANSACTION_COINBASE;
    qProof.coinbaseTx = MakeTransactionRef(mtx);
    chain.quorumProofs.push_back(qProof);

    // Call VerifyProofChain
    auto result = proofManager.VerifyProofChain(
        checkpoint, chain,
        Consensus::LLMQType::LLMQ_TEST, uint256::TWO);

    // The result should be invalid
    BOOST_CHECK(!result.valid);

    // REGRESSION CHECK: The error should mention "signature" because we're testing
    // that forged signatures are caught. If the error is about something else
    // (like "merkle proof" or "coinbase"), the signature check is not working.
    //
    // BEFORE FIX: This check FAILS because error is NOT about signature
    // AFTER FIX: This check PASSES because error IS about signature
    bool errorMentionsSignature = result.error.find("signature") != std::string::npos ||
                                   result.error.find("Signature") != std::string::npos;
    BOOST_CHECK_MESSAGE(errorMentionsSignature,
        "Expected error about signature verification, got: " + result.error);
}

// Regression test: Discontinuous header chain should be REJECTED
// BUG: VerifyProofChain doesn't validate header chain continuity
// This test FAILS before the fix (error is NOT about headers), PASSES after
BOOST_AUTO_TEST_CASE(discontinuous_headers_rejected)
{
    if (!m_node.llmq_ctx || !m_node.llmq_ctx->quorum_block_processor) {
        BOOST_TEST_MESSAGE("Skipping test: LLMQ context not available");
        return;
    }

    llmq::CQuorumProofManager proofManager(*m_node.evodb, *m_node.llmq_ctx->quorum_block_processor);

    // Create checkpoint
    llmq::QuorumCheckpoint checkpoint;
    checkpoint.blockHash = uint256::ONE;
    checkpoint.height = 99;

    CBLSSecretKey sk;
    sk.MakeNewKey();

    llmq::QuorumCheckpoint::QuorumEntry checkpointQuorum;
    checkpointQuorum.quorumHash = uint256::TWO;
    checkpointQuorum.quorumType = Consensus::LLMQType::LLMQ_TEST;
    checkpointQuorum.publicKey = sk.GetPublicKey();
    checkpoint.chainlockQuorums.push_back(checkpointQuorum);

    // Create proof chain with DISCONTINUOUS headers
    llmq::QuorumProofChain chain;

    CBlockHeader header1;
    header1.nVersion = 1;
    header1.hashPrevBlock = uint256::ZERO;
    header1.hashMerkleRoot = uint256::ONE;
    header1.nTime = 1234567890;
    header1.nBits = 0x1d00ffff;
    header1.nNonce = 1;

    CBlockHeader header2;
    header2.nVersion = 1;
    // BUG TRIGGER: prevBlockHash does NOT match header1.GetHash()
    header2.hashPrevBlock = uint256::TWO;  // Should be header1.GetHash()
    header2.hashMerkleRoot = uint256::TWO;
    header2.nTime = 1234567891;
    header2.nBits = 0x1d00ffff;
    header2.nNonce = 2;

    chain.headers.push_back(header1);
    chain.headers.push_back(header2);

    // Add chainlock
    llmq::ChainlockProofEntry clEntry;
    clEntry.nHeight = 100;
    clEntry.blockHash = header1.GetHash();
    clEntry.signature = sk.Sign(clEntry.blockHash, false);
    chain.chainlocks.push_back(clEntry);

    // Add quorum proof
    llmq::QuorumCommitmentProof qProof;
    qProof.commitment.llmqType = Consensus::LLMQType::LLMQ_TEST;
    qProof.commitment.quorumHash = uint256::TWO;
    qProof.chainlockIndex = 0;

    CMutableTransaction mtx;
    mtx.nVersion = 3;
    mtx.nType = TRANSACTION_COINBASE;
    qProof.coinbaseTx = MakeTransactionRef(mtx);
    chain.quorumProofs.push_back(qProof);

    auto result = proofManager.VerifyProofChain(
        checkpoint, chain,
        Consensus::LLMQType::LLMQ_TEST, uint256::TWO);

    BOOST_CHECK(!result.valid);

    // REGRESSION CHECK: Error should mention "header" or "continuous" or "chain"
    // BEFORE FIX: This FAILS because error is about something else
    // AFTER FIX: This PASSES because error is about header continuity
    bool errorMentionsHeaders = result.error.find("header") != std::string::npos ||
                                 result.error.find("Header") != std::string::npos ||
                                 result.error.find("continuous") != std::string::npos ||
                                 result.error.find("chain") != std::string::npos;
    BOOST_CHECK_MESSAGE(errorMentionsHeaders,
        "Expected error about header chain continuity, got: " + result.error);
}

BOOST_AUTO_TEST_SUITE_END()
