// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/llmq_tests.h>
#include <test/util/setup_common.h>

#include <consensus/params.h>
#include <llmq/commitment.h>
#include <llmq/utils.h>

#include <boost/test/unit_test.hpp>

#include <vector>

using namespace llmq;
using namespace llmq::testutils;

BOOST_FIXTURE_TEST_SUITE(llmq_hash_tests, BasicTestingSetup)

// Get test params for use in tests
static const Consensus::LLMQParams& TEST_PARAMS = GetLLMQParams(Consensus::LLMQType::LLMQ_TEST_V17);
static const Consensus::LLMQParams& TEST_PARAMS_ALT = GetLLMQParams(Consensus::LLMQType::LLMQ_TEST);

BOOST_AUTO_TEST_CASE(build_commitment_hash_deterministic_test)
{
    // Setup test data
    Consensus::LLMQType llmqType = TEST_PARAMS.type;
    uint256 quorumHash = GetTestQuorumHash(1);
    std::vector<bool> validMembers = CreateBitVector(5, {0, 1, 2, 3});
    CBLSPublicKey quorumPubKey = CreateRandomBLSPublicKey();
    uint256 vvecHash = GetTestQuorumHash(2);

    // Generate hash multiple times with same inputs
    uint256 hash1 = BuildCommitmentHash(llmqType, quorumHash, validMembers, quorumPubKey, vvecHash, BLSScheme::Basic);
    uint256 hash2 = BuildCommitmentHash(llmqType, quorumHash, validMembers, quorumPubKey, vvecHash, BLSScheme::Basic);
    uint256 hash3 = BuildCommitmentHash(llmqType, quorumHash, validMembers, quorumPubKey, vvecHash, BLSScheme::Basic);

    // All hashes should be identical (deterministic)
    BOOST_CHECK(hash1 == hash2);
    BOOST_CHECK(hash2 == hash3);
    BOOST_CHECK(!hash1.IsNull());
}

BOOST_AUTO_TEST_CASE(build_commitment_hash_sensitivity_test)
{
    // Base test data
    Consensus::LLMQType llmqType = TEST_PARAMS.type;
    uint256 quorumHash = GetTestQuorumHash(1);
    std::vector<bool> validMembers = CreateBitVector(5, {0, 1, 2, 3});
    CBLSPublicKey quorumPubKey = CreateRandomBLSPublicKey();
    uint256 vvecHash = GetTestQuorumHash(2);

    uint256 baseHash = BuildCommitmentHash(llmqType, quorumHash, validMembers, quorumPubKey, vvecHash, BLSScheme::Basic);

    // Test sensitivity to llmqType change
    uint256 hashDiffType = BuildCommitmentHash(TEST_PARAMS_ALT.type, // Different type
                                               quorumHash, validMembers, quorumPubKey, vvecHash, BLSScheme::Basic);
    BOOST_CHECK(baseHash != hashDiffType);

    // Test sensitivity to quorumHash change
    uint256 hashDiffQuorum = BuildCommitmentHash(llmqType,
                                                 GetTestQuorumHash(99), // Different quorum hash
                                                 validMembers, quorumPubKey, vvecHash, BLSScheme::Basic);
    BOOST_CHECK(baseHash != hashDiffQuorum);

    // Test sensitivity to validMembers change
    std::vector<bool> differentMembers = CreateBitVector(5, {0, 1, 2}); // One less member
    uint256 hashDiffMembers = BuildCommitmentHash(llmqType, quorumHash,
                                                  differentMembers, // Different valid members
                                                  quorumPubKey, vvecHash, BLSScheme::Basic);
    BOOST_CHECK(baseHash != hashDiffMembers);

    // Test sensitivity to quorumPubKey change
    CBLSPublicKey differentPubKey = CreateRandomBLSPublicKey();
    uint256 hashDiffPubKey = BuildCommitmentHash(llmqType, quorumHash, validMembers,
                                                 differentPubKey, // Different public key
                                                 vvecHash, BLSScheme::Basic);
    BOOST_CHECK(baseHash != hashDiffPubKey);

    // Test sensitivity to vvecHash change
    uint256 hashDiffVvec = BuildCommitmentHash(llmqType, quorumHash, validMembers, quorumPubKey,
                                               GetTestQuorumHash(99), // Different vvec hash
                                               BLSScheme::Basic);
    BOOST_CHECK(baseHash != hashDiffVvec);

    // Null quorum hash, null vvec hash, and an invalid pubkey still change the hash.
    uint256 hashNullQuorum = BuildCommitmentHash(llmqType, uint256(), validMembers, quorumPubKey, vvecHash,
                                                 BLSScheme::Basic);
    BOOST_CHECK(!hashNullQuorum.IsNull());
    BOOST_CHECK(baseHash != hashNullQuorum);

    CBLSPublicKey invalidPubKey;
    uint256 hashInvalidKey = BuildCommitmentHash(llmqType, quorumHash, validMembers, invalidPubKey, vvecHash,
                                                 BLSScheme::Basic);
    BOOST_CHECK(!hashInvalidKey.IsNull());
    BOOST_CHECK(baseHash != hashInvalidKey);

    uint256 hashNullVvec = BuildCommitmentHash(llmqType, quorumHash, validMembers, quorumPubKey, uint256(),
                                               BLSScheme::Basic);
    BOOST_CHECK(!hashNullVvec.IsNull());
    BOOST_CHECK(baseHash != hashNullVvec);
}

BOOST_AUTO_TEST_CASE(build_commitment_hash_edge_cases_test)
{
    // Test with empty valid members
    std::vector<bool> emptyMembers;
    uint256 hashEmpty = BuildCommitmentHash(TEST_PARAMS.type, GetTestQuorumHash(1), emptyMembers,
                                            CreateRandomBLSPublicKey(), GetTestQuorumHash(2), BLSScheme::Basic);
    BOOST_CHECK(!hashEmpty.IsNull());

    // Test with all members valid
    std::vector<bool> allValid(100, true);
    uint256 hashAllValid = BuildCommitmentHash(TEST_PARAMS.type, GetTestQuorumHash(1), allValid,
                                               CreateRandomBLSPublicKey(), GetTestQuorumHash(2), BLSScheme::Basic);
    BOOST_CHECK(!hashAllValid.IsNull());

    // Test with no members valid
    std::vector<bool> noneValid(100, false);
    uint256 hashNoneValid = BuildCommitmentHash(TEST_PARAMS.type, GetTestQuorumHash(1), noneValid,
                                                CreateRandomBLSPublicKey(), GetTestQuorumHash(2), BLSScheme::Basic);
    BOOST_CHECK(!hashNoneValid.IsNull());

    // All three should produce different hashes
    BOOST_CHECK(hashEmpty != hashAllValid);
    BOOST_CHECK(hashEmpty != hashNoneValid);
    BOOST_CHECK(hashAllValid != hashNoneValid);
}

BOOST_AUTO_TEST_CASE(build_commitment_hash_bit_pattern_test)
{
    // Test that different bit patterns produce different hashes
    Consensus::LLMQType llmqType = TEST_PARAMS_ALT.type;
    uint256 quorumHash = GetTestQuorumHash(1);
    CBLSPublicKey quorumPubKey = CreateRandomBLSPublicKey();
    uint256 vvecHash = GetTestQuorumHash(2);

    // Create various bit patterns
    std::vector<std::vector<bool>> patterns = {
        CreateBitVector(10, {}),                             // All false
        CreateBitVector(10, {0}),                            // First only
        CreateBitVector(10, {9}),                            // Last only
        CreateBitVector(10, {0, 9}),                         // First and last
        CreateBitVector(10, {1, 3, 5, 7}),                   // Alternating
        CreateBitVector(10, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}), // All true
    };

    std::vector<uint256> hashes;
    for (const auto& pattern : patterns) {
        uint256 hash = BuildCommitmentHash(llmqType, quorumHash, pattern, quorumPubKey, vvecHash, BLSScheme::Basic);
        hashes.push_back(hash);
        BOOST_CHECK(!hash.IsNull());
    }

    // All hashes should be unique
    for (size_t i = 0; i < hashes.size(); ++i) {
        for (size_t j = i + 1; j < hashes.size(); ++j) {
            BOOST_CHECK(hashes[i] != hashes[j]);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
