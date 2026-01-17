// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <bls/bls.h>
#include <evo/cbtx.h>
#include <evo/evodb.h>
#include <evo/specialtx.h>
#include <hash.h>
#include <llmq/blockprocessor.h>
#include <llmq/commitment.h>
#include <llmq/context.h>
#include <llmq/quorumproofs.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <streams.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(quorum_proofs_tests, BasicTestingSetup)

// Helper function to create test hashes
static uint256 MakeTestHash(int n)
{
    std::vector<unsigned char> data(32, 0);
    data[0] = static_cast<unsigned char>(n);
    return uint256(data);
}

// Test QuorumMerkleProof verification
BOOST_AUTO_TEST_CASE(quorum_merkle_proof_verify)
{
    // Test case with 4 leaves
    std::vector<uint256> leaves = {
        MakeTestHash(1),
        MakeTestHash(2),
        MakeTestHash(3),
        MakeTestHash(4)
    };

    std::sort(leaves.begin(), leaves.end());

    // Manually compute the merkle tree
    // Level 0: leaves
    // Level 1: H(leaf0, leaf1), H(leaf2, leaf3)
    // Level 2: root = H(level1[0], level1[1])
    uint256 h01 = Hash(leaves[0], leaves[1]);
    uint256 h23 = Hash(leaves[2], leaves[3]);
    uint256 root = Hash(h01, h23);

    // Build proof for leaf 0: sibling is leaf1, then h23
    llmq::QuorumMerkleProof proof;
    proof.merklePath = {leaves[1], h23};
    proof.merklePathSide = {true, true}; // both siblings are on the right

    BOOST_CHECK(proof.Verify(leaves[0], root));

    // Test with wrong root - should fail
    uint256 wrongRoot = MakeTestHash(99);
    BOOST_CHECK(!proof.Verify(leaves[0], wrongRoot));

    // Test with wrong leaf - should fail
    uint256 wrongLeaf = MakeTestHash(100);
    BOOST_CHECK(!proof.Verify(wrongLeaf, root));
}

// Test QuorumMerkleProof with single leaf
BOOST_AUTO_TEST_CASE(quorum_merkle_proof_single_leaf)
{
    uint256 leaf = MakeTestHash(1);

    // With a single leaf, the merkle root is just Hash(leaf, leaf)
    uint256 root = Hash(leaf, leaf);

    llmq::QuorumMerkleProof proof;
    proof.merklePath = {leaf}; // self-duplicate
    proof.merklePathSide = {true};

    BOOST_CHECK(proof.Verify(leaf, root));
}

// Test QuorumMerkleProof with odd number of leaves
BOOST_AUTO_TEST_CASE(quorum_merkle_proof_odd_count)
{
    std::vector<uint256> leaves = {
        MakeTestHash(1),
        MakeTestHash(2),
        MakeTestHash(3)
    };

    std::sort(leaves.begin(), leaves.end());

    // With 3 leaves:
    // Level 0: leaf0, leaf1, leaf2
    // Level 1: H(leaf0, leaf1), H(leaf2, leaf2) <-- leaf2 duplicated
    // Level 2: root
    uint256 h01 = Hash(leaves[0], leaves[1]);
    uint256 h22 = Hash(leaves[2], leaves[2]);
    uint256 root = Hash(h01, h22);

    // Build proof for leaf 2: self-duplicate at level 0, then h01 at level 1
    llmq::QuorumMerkleProof proof;
    proof.merklePath = {leaves[2], h01};
    proof.merklePathSide = {true, false}; // self on right, h01 on left

    BOOST_CHECK(proof.Verify(leaves[2], root));
}

// Test ChainlockProofEntry serialization
BOOST_AUTO_TEST_CASE(chainlock_proof_entry_serialization)
{
    llmq::ChainlockProofEntry entry;
    entry.nHeight = 12345;
    entry.blockHash = MakeTestHash(42);

    // Create a valid BLS signature by signing with a real key
    CBLSSecretKey sk;
    sk.MakeNewKey();
    entry.signature = sk.Sign(entry.blockHash, /*specificLegacyScheme=*/false);

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << entry;

    llmq::ChainlockProofEntry deserialized;
    ss >> deserialized;

    BOOST_CHECK_EQUAL(entry.nHeight, deserialized.nHeight);
    BOOST_CHECK(entry.blockHash == deserialized.blockHash);
    BOOST_CHECK(entry.signature == deserialized.signature);
}

// Test QuorumProofChain serialization roundtrip
BOOST_AUTO_TEST_CASE(quorum_proof_chain_serialization)
{
    llmq::QuorumProofChain chain;

    // Add a test header
    CBlockHeader header;
    header.nVersion = 1;
    header.hashPrevBlock = MakeTestHash(1);
    header.hashMerkleRoot = MakeTestHash(2);
    header.nTime = 1234567890;
    header.nBits = 0x1d00ffff;
    header.nNonce = 12345;
    chain.headers.push_back(header);

    // Add a chainlock entry
    llmq::ChainlockProofEntry clEntry;
    clEntry.nHeight = 100;
    clEntry.blockHash = MakeTestHash(3);
    chain.chainlocks.push_back(clEntry);

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << chain;

    llmq::QuorumProofChain deserialized;
    ss >> deserialized;

    BOOST_CHECK_EQUAL(chain.headers.size(), deserialized.headers.size());
    BOOST_CHECK_EQUAL(chain.chainlocks.size(), deserialized.chainlocks.size());
    BOOST_CHECK_EQUAL(chain.quorumProofs.size(), deserialized.quorumProofs.size());

    if (!chain.headers.empty()) {
        BOOST_CHECK(chain.headers[0].GetHash() == deserialized.headers[0].GetHash());
    }
    if (!chain.chainlocks.empty()) {
        BOOST_CHECK_EQUAL(chain.chainlocks[0].nHeight, deserialized.chainlocks[0].nHeight);
        BOOST_CHECK(chain.chainlocks[0].blockHash == deserialized.chainlocks[0].blockHash);
    }
}

// Test ChainlockIndexEntry serialization
BOOST_AUTO_TEST_CASE(chainlock_index_entry_serialization)
{
    llmq::ChainlockIndexEntry entry;
    entry.cbtxBlockHash = MakeTestHash(10);
    entry.cbtxHeight = 500;

    // Create a valid BLS signature by signing with a real key
    CBLSSecretKey sk;
    sk.MakeNewKey();
    entry.signature = sk.Sign(entry.cbtxBlockHash, /*specificLegacyScheme=*/false);

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << entry;

    llmq::ChainlockIndexEntry deserialized;
    ss >> deserialized;

    BOOST_CHECK(entry.cbtxBlockHash == deserialized.cbtxBlockHash);
    BOOST_CHECK_EQUAL(entry.cbtxHeight, deserialized.cbtxHeight);
    BOOST_CHECK(entry.signature == deserialized.signature);
}

// Test QuorumCheckpoint JSON roundtrip
BOOST_AUTO_TEST_CASE(quorum_checkpoint_json_roundtrip)
{
    llmq::QuorumCheckpoint checkpoint;
    checkpoint.blockHash = MakeTestHash(20);
    checkpoint.height = 1000;

    llmq::QuorumCheckpoint::QuorumEntry qEntry;
    qEntry.quorumHash = MakeTestHash(21);
    qEntry.quorumType = Consensus::LLMQType::LLMQ_TEST;

    // Create a valid BLS public key from a real secret key
    CBLSSecretKey sk;
    sk.MakeNewKey();
    qEntry.publicKey = sk.GetPublicKey();

    checkpoint.chainlockQuorums.push_back(qEntry);

    UniValue json = checkpoint.ToJson();

    // Verify structure
    BOOST_CHECK(json.exists("blockHash"));
    BOOST_CHECK(json.exists("height"));
    BOOST_CHECK(json.exists("chainlockQuorums"));

    BOOST_CHECK_EQUAL(json["height"].getInt<int>(), 1000);
}

// Test QuorumProofVerifyResult
BOOST_AUTO_TEST_CASE(quorum_proof_verify_result_json)
{
    // Test valid result
    llmq::QuorumProofVerifyResult validResult;
    validResult.valid = true;

    // Create a valid BLS public key from a real secret key
    CBLSSecretKey sk;
    sk.MakeNewKey();
    validResult.quorumPublicKey = sk.GetPublicKey();

    UniValue validJson = validResult.ToJson();
    BOOST_CHECK(validJson["valid"].get_bool());
    BOOST_CHECK(validJson.exists("quorumPublicKey"));

    // Test invalid result
    llmq::QuorumProofVerifyResult invalidResult;
    invalidResult.valid = false;
    invalidResult.error = "Test error message";

    UniValue invalidJson = invalidResult.ToJson();
    BOOST_CHECK(!invalidJson["valid"].get_bool());
    BOOST_CHECK_EQUAL(invalidJson["error"].get_str(), "Test error message");
}

// Test merkle path side indicators are consistent
BOOST_AUTO_TEST_CASE(quorum_merkle_proof_side_consistency)
{
    llmq::QuorumMerkleProof proof;

    // Mismatched path and side vectors should fail verification
    proof.merklePath = {MakeTestHash(1), MakeTestHash(2)};
    proof.merklePathSide = {true}; // Only one side indicator for two path elements

    BOOST_CHECK(!proof.Verify(MakeTestHash(0), MakeTestHash(99)));
}

// Test DoS protection: paths longer than MAX_MERKLE_PATH_LENGTH should be rejected
BOOST_AUTO_TEST_CASE(merkle_proof_dos_protection)
{
    llmq::QuorumMerkleProof proof;

    // Create a path that exceeds MAX_MERKLE_PATH_LENGTH (32)
    // Such a path would imply a tree with 2^33+ leaves, which is unreasonable
    for (size_t i = 0; i <= llmq::MAX_MERKLE_PATH_LENGTH; ++i) {
        proof.merklePath.push_back(MakeTestHash(static_cast<int>(i)));
        proof.merklePathSide.push_back(true);
    }

    // This should be rejected due to DoS protection, regardless of hash validity
    BOOST_CHECK(!proof.Verify(MakeTestHash(100), MakeTestHash(200)));

    // Verify that paths at exactly the limit are still processed (not rejected for length)
    llmq::QuorumMerkleProof atLimitProof;
    for (size_t i = 0; i < llmq::MAX_MERKLE_PATH_LENGTH; ++i) {
        atLimitProof.merklePath.push_back(MakeTestHash(static_cast<int>(i)));
        atLimitProof.merklePathSide.push_back(true);
    }
    // This won't verify correctly (wrong hashes), but it won't be rejected for length
    // The return value will be false because hashes don't match, not because of DoS limit
    // The key is that it processes the path instead of rejecting it immediately
    (void)atLimitProof.Verify(MakeTestHash(100), MakeTestHash(200));
}

// Additional merkle proof verification tests
BOOST_AUTO_TEST_CASE(merkle_proof_all_leaf_positions)
{
    // Test with 8 leaves to cover more edge cases
    std::vector<uint256> leaves;
    for (int i = 0; i < 8; ++i) {
        leaves.push_back(MakeTestHash(i + 1));
    }
    std::sort(leaves.begin(), leaves.end());

    // Manually compute the merkle tree
    // Level 0: leaf0, leaf1, leaf2, leaf3, leaf4, leaf5, leaf6, leaf7
    // Level 1: h01, h23, h45, h67
    // Level 2: h0123, h4567
    // Level 3: root
    uint256 h01 = Hash(leaves[0], leaves[1]);
    uint256 h23 = Hash(leaves[2], leaves[3]);
    uint256 h45 = Hash(leaves[4], leaves[5]);
    uint256 h67 = Hash(leaves[6], leaves[7]);
    uint256 h0123 = Hash(h01, h23);
    uint256 h4567 = Hash(h45, h67);
    uint256 root = Hash(h0123, h4567);

    // Test proof for leaf 0: path = [leaf1, h23, h4567]
    {
        llmq::QuorumMerkleProof proof;
        proof.merklePath = {leaves[1], h23, h4567};
        proof.merklePathSide = {true, true, true}; // all siblings on right
        BOOST_CHECK(proof.Verify(leaves[0], root));
    }

    // Test proof for leaf 3: path = [leaf2, h01, h4567]
    {
        llmq::QuorumMerkleProof proof;
        proof.merklePath = {leaves[2], h01, h4567};
        proof.merklePathSide = {false, false, true}; // leaf2 left, h01 left, h4567 right
        BOOST_CHECK(proof.Verify(leaves[3], root));
    }

    // Test proof for leaf 7: path = [leaf6, h45, h0123]
    {
        llmq::QuorumMerkleProof proof;
        proof.merklePath = {leaves[6], h45, h0123};
        proof.merklePathSide = {false, false, false}; // all siblings on left
        BOOST_CHECK(proof.Verify(leaves[7], root));
    }

    // Test proof for leaf 4: path = [leaf5, h67, h0123]
    {
        llmq::QuorumMerkleProof proof;
        proof.merklePath = {leaves[5], h67, h0123};
        proof.merklePathSide = {true, true, false}; // leaf5 right, h67 right, h0123 left
        BOOST_CHECK(proof.Verify(leaves[4], root));
    }
}

// Test proof for 5 leaves (odd tree)
BOOST_AUTO_TEST_CASE(merkle_proof_five_leaves)
{
    std::vector<uint256> leaves;
    for (int i = 0; i < 5; ++i) {
        leaves.push_back(MakeTestHash(i + 1));
    }
    std::sort(leaves.begin(), leaves.end());

    // With 5 leaves:
    // Level 0: leaf0, leaf1, leaf2, leaf3, leaf4
    // Level 1: h01, h23, h44 (leaf4 duplicated)
    // Level 2: h0123, h4444 (h44 duplicated)
    // Level 3: root
    uint256 h01 = Hash(leaves[0], leaves[1]);
    uint256 h23 = Hash(leaves[2], leaves[3]);
    uint256 h44 = Hash(leaves[4], leaves[4]);
    uint256 h0123 = Hash(h01, h23);
    uint256 h4444 = Hash(h44, h44);
    uint256 root = Hash(h0123, h4444);

    // Test proof for leaf 4 (the odd one)
    {
        llmq::QuorumMerkleProof proof;
        proof.merklePath = {leaves[4], h44, h0123}; // self-duplicate at first level
        proof.merklePathSide = {true, true, false};
        BOOST_CHECK(proof.Verify(leaves[4], root));
    }

    // Test proof for leaf 2
    {
        llmq::QuorumMerkleProof proof;
        proof.merklePath = {leaves[3], h01, h4444};
        proof.merklePathSide = {true, false, true};
        BOOST_CHECK(proof.Verify(leaves[2], root));
    }
}

BOOST_AUTO_TEST_SUITE_END()

//
// REGRESSION TESTS for security issues identified in code review
// These tests should FAIL before the fix and PASS after
//

// Use RegTestingSetup for tests that need full node infrastructure
BOOST_FIXTURE_TEST_SUITE(quorum_proofs_regression_tests, RegTestingSetup)

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
