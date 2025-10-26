// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/llmq_tests.h>
#include <test/util/setup_common.h>

#include <bls/bls.h>
#include <chainparams.h>
#include <evo/deterministicmns.h>
#include <llmq/commitment.h>
#include <llmq/params.h>
#include <llmq/quorums.h>
#include <llmq/signing_shares.h>

#include <boost/test/unit_test.hpp>

using namespace llmq;
using namespace llmq::testutils;

/**
 * Unit tests for LLMQ signature share validation logic
 *
 * These tests verify ValidateBatchedSigSharesStructure(), which contains the pure
 * validation logic extracted from PreVerifyBatchedSigShares() for testability.
 *
 * Tests cover:
 * - Duplicate member detection
 * - Member index bounds checking
 * - Invalid member validation
 * - Result structure correctness
 */

// Helper to create a mock quorum for testing
struct MockQuorumBuilder {
    std::shared_ptr<CQuorum> quorum;
    std::unique_ptr<CBLSWorker> blsWorker;

    MockQuorumBuilder(int size, const std::vector<bool>& validMembers = {})
    {
        blsWorker = std::make_unique<CBLSWorker>();
        const auto& params = GetLLMQParams(Consensus::LLMQType::LLMQ_TEST_V17);

        quorum = std::make_shared<CQuorum>(params, *blsWorker);

        // Create commitment
        auto qc_ptr = std::make_unique<CFinalCommitment>();
        qc_ptr->llmqType = params.type;
        qc_ptr->quorumHash = GetTestQuorumHash(1);

        // Set valid members
        if (!validMembers.empty()) {
            qc_ptr->validMembers = validMembers;
        } else {
            qc_ptr->validMembers.resize(size, true);
        }

        // Create placeholder member list (needed for size checks)
        std::vector<CDeterministicMNCPtr> members(size, nullptr);

        quorum->Init(std::move(qc_ptr), nullptr, GetTestBlockHash(1), members);

        // Add verification vector
        std::vector<CBLSPublicKey> vvec;
        for (int i = 0; i < size; ++i) {
            vvec.push_back(CreateRandomBLSPublicKey());
        }
        quorum->SetVerificationVector(vvec);
    }

    CQuorum& GetQuorum() const { return *quorum; }
};

// Helper to create batched sig shares
CBatchedSigShares CreateBatchedSigShares(const std::vector<uint16_t>& members)
{
    CBatchedSigShares batched;
    batched.sessionId = 1;

    for (uint16_t member : members) {
        CBLSLazySignature lazySig;
        batched.sigShares.emplace_back(member, lazySig);
    }

    return batched;
}

BOOST_FIXTURE_TEST_SUITE(llmq_signing_shares_tests, BasicTestingSetup)

//
// Test the PreVerifyBatchedResult structure
//

BOOST_AUTO_TEST_CASE(result_structure_success)
{
    PreVerifyBatchedResult result{PreVerifyResult::Success, false};

    BOOST_CHECK(result.IsSuccess());
    BOOST_CHECK_EQUAL(result.result, PreVerifyResult::Success);
    BOOST_CHECK_EQUAL(result.should_ban, false);
}

BOOST_AUTO_TEST_CASE(result_structure_ban_errors)
{
    // Test ban-worthy errors
    PreVerifyBatchedResult dup{PreVerifyResult::DuplicateMember, true};
    BOOST_CHECK(!dup.IsSuccess());
    BOOST_CHECK(dup.should_ban);

    PreVerifyBatchedResult bounds{PreVerifyResult::QuorumMemberOutOfBounds, true};
    BOOST_CHECK(!bounds.IsSuccess());
    BOOST_CHECK(bounds.should_ban);

    PreVerifyBatchedResult invalid{PreVerifyResult::QuorumMemberNotValid, true};
    BOOST_CHECK(!invalid.IsSuccess());
    BOOST_CHECK(invalid.should_ban);
}

BOOST_AUTO_TEST_CASE(result_structure_non_ban_errors)
{
    // Test non-ban errors
    PreVerifyBatchedResult old{PreVerifyResult::QuorumTooOld, false};
    BOOST_CHECK(!old.IsSuccess());
    BOOST_CHECK(!old.should_ban);

    PreVerifyBatchedResult not_member{PreVerifyResult::NotAMember, false};
    BOOST_CHECK(!not_member.IsSuccess());
    BOOST_CHECK(!not_member.should_ban);

    PreVerifyBatchedResult no_vvec{PreVerifyResult::MissingVerificationVector, false};
    BOOST_CHECK(!no_vvec.IsSuccess());
    BOOST_CHECK(!no_vvec.should_ban);
}

//
// Test ValidateBatchedSigSharesStructure - the extracted validation logic
//

BOOST_AUTO_TEST_CASE(validate_success)
{
    MockQuorumBuilder builder(5);
    auto& quorum = builder.GetQuorum();

    // Valid batch with no duplicates, all members in bounds and valid
    auto batched = CreateBatchedSigShares({0, 1, 2, 3, 4});

    auto result = CSigSharesManager::ValidateBatchedSigSharesStructure(quorum, batched);

    BOOST_CHECK(result.IsSuccess());
    BOOST_CHECK_EQUAL(result.result, PreVerifyResult::Success);
    BOOST_CHECK_EQUAL(result.should_ban, false);
}

BOOST_AUTO_TEST_CASE(validate_empty_batch)
{
    MockQuorumBuilder builder(5);
    auto& quorum = builder.GetQuorum();

    // Empty batch should succeed
    auto batched = CreateBatchedSigShares({});

    auto result = CSigSharesManager::ValidateBatchedSigSharesStructure(quorum, batched);

    BOOST_CHECK(result.IsSuccess());
}

BOOST_AUTO_TEST_CASE(validate_duplicate_member_first_occurrence)
{
    MockQuorumBuilder builder(5);
    auto& quorum = builder.GetQuorum();

    // Duplicate member (0 appears twice)
    auto batched = CreateBatchedSigShares({0, 1, 0, 2});

    auto result = CSigSharesManager::ValidateBatchedSigSharesStructure(quorum, batched);

    BOOST_CHECK(!result.IsSuccess());
    BOOST_CHECK_EQUAL(result.result, PreVerifyResult::DuplicateMember);
    BOOST_CHECK(result.should_ban);
}

BOOST_AUTO_TEST_CASE(validate_duplicate_member_multiple)
{
    MockQuorumBuilder builder(10);
    auto& quorum = builder.GetQuorum();

    // Multiple duplicates - should catch first one (member 1)
    auto batched = CreateBatchedSigShares({0, 1, 2, 1, 3, 2, 4});

    auto result = CSigSharesManager::ValidateBatchedSigSharesStructure(quorum, batched);

    BOOST_CHECK(!result.IsSuccess());
    BOOST_CHECK_EQUAL(result.result, PreVerifyResult::DuplicateMember);
    BOOST_CHECK(result.should_ban);
}

BOOST_AUTO_TEST_CASE(validate_member_out_of_bounds)
{
    const int quorum_size = 5;
    MockQuorumBuilder builder(quorum_size);
    auto& quorum = builder.GetQuorum();

    // Member 10 is out of bounds (>= 5)
    auto batched = CreateBatchedSigShares({0, 1, 10});

    auto result = CSigSharesManager::ValidateBatchedSigSharesStructure(quorum, batched);

    BOOST_CHECK(!result.IsSuccess());
    BOOST_CHECK_EQUAL(result.result, PreVerifyResult::QuorumMemberOutOfBounds);
    BOOST_CHECK(result.should_ban);
}

BOOST_AUTO_TEST_CASE(validate_member_at_max_valid_index)
{
    const int quorum_size = 10;
    MockQuorumBuilder builder(quorum_size);
    auto& quorum = builder.GetQuorum();

    // Max valid index is size - 1, which should succeed
    auto batched = CreateBatchedSigShares({0, static_cast<uint16_t>(quorum_size - 1)});

    auto result = CSigSharesManager::ValidateBatchedSigSharesStructure(quorum, batched);

    BOOST_CHECK(result.IsSuccess());
}

BOOST_AUTO_TEST_CASE(validate_invalid_member)
{
    // Create quorum with specific valid members pattern
    std::vector<bool> validMembers = {true, false, true, true, false};
    MockQuorumBuilder builder(5, validMembers);
    auto& quorum = builder.GetQuorum();

    // Member 1 is invalid (marked false in validMembers)
    auto batched = CreateBatchedSigShares({0, 1, 2});

    auto result = CSigSharesManager::ValidateBatchedSigSharesStructure(quorum, batched);

    BOOST_CHECK(!result.IsSuccess());
    BOOST_CHECK_EQUAL(result.result, PreVerifyResult::QuorumMemberNotValid);
    BOOST_CHECK(result.should_ban);
}

BOOST_AUTO_TEST_CASE(validate_all_members_valid)
{
    // Create quorum with specific valid members pattern
    std::vector<bool> validMembers = {true, false, true, true, false};
    MockQuorumBuilder builder(5, validMembers);
    auto& quorum = builder.GetQuorum();

    // Only use valid members (0, 2, 3)
    auto batched = CreateBatchedSigShares({0, 2, 3});

    auto result = CSigSharesManager::ValidateBatchedSigSharesStructure(quorum, batched);

    BOOST_CHECK(result.IsSuccess());
}

BOOST_AUTO_TEST_CASE(validate_all_members_invalid)
{
    // Create quorum where all members are invalid
    std::vector<bool> validMembers(5, false);
    MockQuorumBuilder builder(5, validMembers);
    auto& quorum = builder.GetQuorum();

    // All members are invalid
    auto batched = CreateBatchedSigShares({0, 1, 2});

    auto result = CSigSharesManager::ValidateBatchedSigSharesStructure(quorum, batched);

    BOOST_CHECK(!result.IsSuccess());
    BOOST_CHECK_EQUAL(result.result, PreVerifyResult::QuorumMemberNotValid);
    BOOST_CHECK(result.should_ban);
}

BOOST_AUTO_TEST_CASE(validate_error_priority_duplicate_before_invalid)
{
    // Verify that duplicate check happens before validity check in the same iteration
    std::vector<bool> validMembers = {true, false, true, false, true};
    MockQuorumBuilder builder(5, validMembers);
    auto& quorum = builder.GetQuorum();

    // Member 2 appears twice (at positions 0 and 1)
    // This ensures duplicate is detected before we process any invalid members
    auto batched = CreateBatchedSigShares({2, 2, 1});

    auto result = CSigSharesManager::ValidateBatchedSigSharesStructure(quorum, batched);

    BOOST_CHECK(!result.IsSuccess());
    BOOST_CHECK_EQUAL(result.result, PreVerifyResult::DuplicateMember);
    BOOST_CHECK(result.should_ban);
}

BOOST_AUTO_TEST_CASE(validate_error_priority_bounds_before_invalid)
{
    // Verify that bounds check happens before validity check
    std::vector<bool> validMembers = {true, false, true};
    MockQuorumBuilder builder(3, validMembers);
    auto& quorum = builder.GetQuorum();

    // Member 10 is out of bounds, comes before member 1 which is invalid
    auto batched = CreateBatchedSigShares({0, 10});

    auto result = CSigSharesManager::ValidateBatchedSigSharesStructure(quorum, batched);

    BOOST_CHECK(!result.IsSuccess());
    BOOST_CHECK_EQUAL(result.result, PreVerifyResult::QuorumMemberOutOfBounds);
    BOOST_CHECK(result.should_ban);
}

BOOST_AUTO_TEST_SUITE_END()
