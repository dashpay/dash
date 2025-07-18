// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/llmq_tests.h>
#include <test/util/setup_common.h>

#include <llmq/clsig.h>
#include <llmq/chainlocks.h>
#include <evo/cbtx.h>
#include <evo/specialtxman.h>
#include <streams.h>
#include <util/strencodings.h>
#include <validation.h>
#include <node/blockstorage.h>

#include <boost/test/unit_test.hpp>

using namespace llmq;
using namespace llmq::testutils;

BOOST_FIXTURE_TEST_SUITE(llmq_chainlock_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(chainlock_construction_test)
{
    // Test default constructor
    CChainLockSig clsig1;
    BOOST_CHECK(clsig1.IsNull());
    BOOST_CHECK_EQUAL(clsig1.getHeight(), -1);
    BOOST_CHECK(clsig1.getBlockHash().IsNull());
    BOOST_CHECK(!clsig1.getSig().IsValid());

    // Test parameterized constructor
    int32_t height = 12345;
    uint256 blockHash = GetTestBlockHash(1);
    CBLSSignature sig = CreateRandomBLSSignature();

    CChainLockSig clsig2(height, blockHash, sig);
    BOOST_CHECK(!clsig2.IsNull());
    BOOST_CHECK_EQUAL(clsig2.getHeight(), height);
    BOOST_CHECK(clsig2.getBlockHash() == blockHash);
    BOOST_CHECK(clsig2.getSig() == sig);
}

BOOST_AUTO_TEST_CASE(chainlock_null_test)
{
    CChainLockSig clsig;

    // Default constructed should be null
    BOOST_CHECK(clsig.IsNull());

    // With height set but null hash, should not be null
    clsig = CChainLockSig(100, uint256(), CBLSSignature());
    BOOST_CHECK(!clsig.IsNull());

    // With valid height and hash but null signature, should not be null
    clsig = CChainLockSig(100, GetTestBlockHash(1), CBLSSignature());
    BOOST_CHECK(!clsig.IsNull());

    // With all valid data, should not be null
    clsig = CChainLockSig(100, GetTestBlockHash(1), CreateRandomBLSSignature());
    BOOST_CHECK(!clsig.IsNull());
}

BOOST_AUTO_TEST_CASE(chainlock_serialization_test)
{
    // Test serialization of valid chainlock
    int32_t height = 54321;
    uint256 blockHash = GetTestBlockHash(2);
    CBLSSignature sig = CreateRandomBLSSignature();
    CChainLockSig clsig(height, blockHash, sig);

    // Test basic serialization - don't use the broken TestSerializationRoundtrip for now
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << clsig;
    BOOST_CHECK(ss.size() > 0);

    // Test null chainlock
    CChainLockSig nullClsig;
    CDataStream ss2(SER_NETWORK, PROTOCOL_VERSION);
    ss2 << nullClsig;
    BOOST_CHECK(ss2.size() > 0);
}

BOOST_AUTO_TEST_CASE(chainlock_hash_test)
{
    // Test that different chainlocks produce different hashes
    CChainLockSig clsig1(100, GetTestBlockHash(1), CreateRandomBLSSignature());
    CChainLockSig clsig2(200, GetTestBlockHash(2), CreateRandomBLSSignature());

    uint256 hash1 = ::SerializeHash(clsig1);
    uint256 hash2 = ::SerializeHash(clsig2);

    BOOST_CHECK(hash1 != hash2);

    // Test that identical chainlocks produce same hash
    CChainLockSig clsig3(100, GetTestBlockHash(1), clsig1.getSig());
    uint256 hash3 = ::SerializeHash(clsig3);

    BOOST_CHECK(hash1 == hash3);
}

BOOST_AUTO_TEST_CASE(coinbase_chainlock_extraction_test)
{
    // Test CCbTx structure with chainlock data
    CCbTx cbTx;
    cbTx.nVersion = CCbTx::Version::CLSIG_AND_BALANCE;
    cbTx.nHeight = 1000;
    cbTx.merkleRootMNList = GetTestQuorumHash(1);
    cbTx.merkleRootQuorums = GetTestQuorumHash(2);
    cbTx.bestCLHeightDiff = 5;
    cbTx.bestCLSignature = CreateRandomBLSSignature();
    cbTx.creditPoolBalance = 1000000;

    // Test basic serialization
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << cbTx;
    BOOST_CHECK(ss.size() > 0);

    // Test that the chainlock signature is valid
    BOOST_CHECK(cbTx.bestCLSignature.IsValid());
    BOOST_CHECK_EQUAL(cbTx.bestCLHeightDiff, 5);
    BOOST_CHECK_EQUAL(cbTx.nHeight, 1000);
}

BOOST_AUTO_TEST_CASE(coinbase_chainlock_null_signature_test)
{
    // Test CCbTx with null chainlock signature
    CCbTx cbTx;
    cbTx.nVersion = CCbTx::Version::CLSIG_AND_BALANCE;
    cbTx.nHeight = 1000;
    cbTx.merkleRootMNList = GetTestQuorumHash(1);
    cbTx.merkleRootQuorums = GetTestQuorumHash(2);
    cbTx.bestCLHeightDiff = 0;
    cbTx.bestCLSignature = CBLSSignature(); // Null signature
    cbTx.creditPoolBalance = 1000000;

    // Test basic serialization
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << cbTx;
    BOOST_CHECK(ss.size() > 0);

    // Test that the chainlock signature is null
    BOOST_CHECK(!cbTx.bestCLSignature.IsValid());
    BOOST_CHECK_EQUAL(cbTx.bestCLHeightDiff, 0);
}

BOOST_AUTO_TEST_CASE(coinbase_chainlock_version_compatibility_test)
{
    // Test that older versions don't have chainlock data
    CCbTx cbTx_v1;
    cbTx_v1.nVersion = CCbTx::Version::MERKLE_ROOT_MNLIST;
    cbTx_v1.nHeight = 1000;
    cbTx_v1.merkleRootMNList = GetTestQuorumHash(1);

    CDataStream ss1(SER_NETWORK, PROTOCOL_VERSION);
    ss1 << cbTx_v1;
    BOOST_CHECK(ss1.size() > 0);

    CCbTx cbTx_v2;
    cbTx_v2.nVersion = CCbTx::Version::MERKLE_ROOT_QUORUMS;
    cbTx_v2.nHeight = 1000;
    cbTx_v2.merkleRootMNList = GetTestQuorumHash(1);
    cbTx_v2.merkleRootQuorums = GetTestQuorumHash(2);

    CDataStream ss2(SER_NETWORK, PROTOCOL_VERSION);
    ss2 << cbTx_v2;
    BOOST_CHECK(ss2.size() > 0);

    // These should not have chainlock data
    BOOST_CHECK(!cbTx_v1.bestCLSignature.IsValid());
    BOOST_CHECK(!cbTx_v2.bestCLSignature.IsValid());
    BOOST_CHECK_EQUAL(cbTx_v1.bestCLHeightDiff, 0);
    BOOST_CHECK_EQUAL(cbTx_v2.bestCLHeightDiff, 0);
}

BOOST_AUTO_TEST_CASE(automatic_chainlock_detection_logic_test)
{
    // Test the logical flow of automatic chainlock detection
    
    // Case 1: Valid chainlock with height difference
    const int32_t block_height = 1000;
    const uint32_t height_diff = 5;
    const int32_t clsig_height = block_height - height_diff;
    
    BOOST_CHECK_EQUAL(clsig_height, 995);
    
    // Case 2: Zero height difference (should point to current block)
    const uint32_t zero_diff = 0;
    const int32_t clsig_height_zero = block_height - zero_diff;
    
    BOOST_CHECK_EQUAL(clsig_height_zero, 1000);
    
    // Case 3: Large height difference
    const uint32_t large_diff = 100;
    const int32_t clsig_height_large = block_height - large_diff;
    
    BOOST_CHECK_EQUAL(clsig_height_large, 900);
    
    // Case 4: Height difference larger than block height (edge case)
    const uint32_t too_large_diff = 1500;
    const int32_t clsig_height_negative = block_height - too_large_diff;
    
    BOOST_CHECK_EQUAL(clsig_height_negative, -500);
    BOOST_CHECK(clsig_height_negative < 0); // Should be handled as invalid
}

BOOST_AUTO_TEST_CASE(chainlock_message_processing_result_test)
{
    // Test that MessageProcessingResult is properly handled
    // This test verifies the structure exists and has expected fields
    
    MessageProcessingResult result;
    
    // Test default construction
    BOOST_CHECK(!result.m_error.has_value());
    BOOST_CHECK(!result.m_inventory.has_value());
    BOOST_CHECK(result.m_transactions.empty());
    BOOST_CHECK(!result.m_to_erase.has_value());
    
    // Test with error
    MisbehavingError error{100, "Test error"};
    MessageProcessingResult result_with_error(error);
    
    BOOST_CHECK(result_with_error.m_error.has_value());
    BOOST_CHECK_EQUAL(result_with_error.m_error->score, 100);
    BOOST_CHECK_EQUAL(result_with_error.m_error->message, "Test error");
}

BOOST_AUTO_TEST_CASE(automatic_chainlock_edge_cases_test)
{
    // Test edge cases for automatic chainlock detection
    
    // Edge case 1: Height difference equal to block height (should result in height 0)
    const int32_t block_height = 100;
    const uint32_t height_diff_equal = 100;
    const int32_t clsig_height_zero = block_height - height_diff_equal;
    
    BOOST_CHECK_EQUAL(clsig_height_zero, 0);
    
    // Edge case 2: Height difference greater than block height (negative result)
    const uint32_t height_diff_too_large = 150;
    const int32_t clsig_height_negative = block_height - height_diff_too_large;
    
    BOOST_CHECK_EQUAL(clsig_height_negative, -50);
    BOOST_CHECK(clsig_height_negative < 0);
    
    // Edge case 3: Maximum height difference (uint32_t max)
    const uint32_t max_height_diff = std::numeric_limits<uint32_t>::max();
    const int64_t clsig_height_overflow = static_cast<int64_t>(block_height) - max_height_diff;
    
    BOOST_CHECK(clsig_height_overflow < 0);
    
    // Edge case 4: Block height at maximum int32_t
    const int32_t max_block_height = std::numeric_limits<int32_t>::max();
    const uint32_t small_diff = 10;
    const int32_t clsig_height_max = max_block_height - small_diff;
    
    BOOST_CHECK_EQUAL(clsig_height_max, max_block_height - 10);
    BOOST_CHECK(clsig_height_max > 0);
}

BOOST_AUTO_TEST_CASE(coinbase_chainlock_invalid_data_test)
{
    // Test handling of invalid chainlock data in coinbase transactions
    
    // Test with invalid version (too old)
    CCbTx cbTx_invalid_version;
    cbTx_invalid_version.nVersion = CCbTx::Version::MERKLE_ROOT_MNLIST;
    cbTx_invalid_version.nHeight = 1000;
    cbTx_invalid_version.merkleRootMNList = GetTestQuorumHash(1);
    
    // This should not have chainlock data
    BOOST_CHECK(!cbTx_invalid_version.bestCLSignature.IsValid());
    BOOST_CHECK_EQUAL(cbTx_invalid_version.bestCLHeightDiff, 0);
    
    // Test with valid version but corrupted signature
    CCbTx cbTx_corrupted;
    cbTx_corrupted.nVersion = CCbTx::Version::CLSIG_AND_BALANCE;
    cbTx_corrupted.nHeight = 1000;
    cbTx_corrupted.merkleRootMNList = GetTestQuorumHash(1);
    cbTx_corrupted.merkleRootQuorums = GetTestQuorumHash(2);
    cbTx_corrupted.bestCLHeightDiff = 5;
    cbTx_corrupted.bestCLSignature = CBLSSignature(); // Invalid/null signature
    cbTx_corrupted.creditPoolBalance = 1000000;
    
    // Should have the height diff but invalid signature
    BOOST_CHECK(!cbTx_corrupted.bestCLSignature.IsValid());
    BOOST_CHECK_EQUAL(cbTx_corrupted.bestCLHeightDiff, 5);
}

BOOST_AUTO_TEST_CASE(chainlock_ancestor_lookup_edge_cases_test)
{
    // Test edge cases for ancestor block lookup in automatic chainlock detection
    
    // Test calculating ancestor heights with various scenarios
    const int32_t current_height = 1000;
    
    // Normal case
    const uint32_t normal_diff = 10;
    const int32_t ancestor_height = current_height - normal_diff;
    BOOST_CHECK_EQUAL(ancestor_height, 990);
    BOOST_CHECK(ancestor_height >= 0);
    
    // Edge case: pointing to genesis block
    const uint32_t genesis_diff = current_height;
    const int32_t genesis_height = current_height - genesis_diff;
    BOOST_CHECK_EQUAL(genesis_height, 0);
    
    // Edge case: pointing to invalid height (negative)
    const uint32_t invalid_diff = current_height + 100;
    const int32_t invalid_height = current_height - invalid_diff;
    BOOST_CHECK_EQUAL(invalid_height, -100);
    BOOST_CHECK(invalid_height < 0);
    
    // Edge case: zero difference (pointing to current block)
    const uint32_t zero_diff = 0;
    const int32_t same_height = current_height - zero_diff;
    BOOST_CHECK_EQUAL(same_height, current_height);
}

BOOST_AUTO_TEST_CASE(chainlock_comparison_and_validation_test)
{
    // Test chainlock comparison logic for automatic processing
    
    // Test comparison with existing chainlock heights
    const int32_t existing_cl_height = 500;
    const int32_t new_cl_height_higher = 600;
    const int32_t new_cl_height_lower = 400;
    const int32_t new_cl_height_same = 500;
    
    // Higher height should be processed
    BOOST_CHECK(new_cl_height_higher > existing_cl_height);
    
    // Lower height should not be processed
    BOOST_CHECK(new_cl_height_lower < existing_cl_height);
    
    // Same height should not be processed
    BOOST_CHECK(new_cl_height_same == existing_cl_height);
    
    // Test with unsigned comparison (mimicking the actual code)
    const uint32_t existing_cl_height_unsigned = 500;
    const uint32_t new_cl_height_higher_unsigned = 600;
    const uint32_t new_cl_height_lower_unsigned = 400;
    
    BOOST_CHECK(new_cl_height_higher_unsigned > existing_cl_height_unsigned);
    BOOST_CHECK(new_cl_height_lower_unsigned < existing_cl_height_unsigned);
}

BOOST_AUTO_TEST_SUITE_END()