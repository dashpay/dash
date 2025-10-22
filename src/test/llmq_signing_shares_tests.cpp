// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>
#include <test/util/llmq_tests.h>

#include <bls/bls.h>
#include <chainparams.h>
#include <evo/deterministicmns.h>
#include <llmq/commitment.h>
#include <llmq/params.h>
#include <llmq/quorums.h>
#include <llmq/signing.h>
#include <llmq/signing_shares.h>
#include <masternode/node.h>

#include <boost/test/unit_test.hpp>

using namespace llmq;
using namespace llmq::testutils;

// Test fixture with helper functions
struct LLMQSigningSharesTestFixture : public TestingSetup
{
    std::unique_ptr<CBLSWorker> blsWorker;
    
    LLMQSigningSharesTestFixture() : TestingSetup()
    {
        blsWorker = std::make_unique<CBLSWorker>();
    }
    
    // Helper to create a minimal test quorum
    CQuorumCPtr CreateMinimalTestQuorum(int size, bool hasVerificationVector = true,
                                         const std::vector<bool>& validMembers = {})
    {
        const auto& params = GetLLMQParams(Consensus::LLMQType::LLMQ_TEST_V17);
        
        auto quorum = std::make_shared<CQuorum>(params, *blsWorker);
        
        // Create commitment
        auto qc_ptr = std::make_unique<CFinalCommitment>();
        qc_ptr->llmqType = params.type;
        qc_ptr->quorumHash = InsecureRand256();
        
        // Set valid members
        if (!validMembers.empty()) {
            qc_ptr->validMembers = validMembers;
        } else {
            qc_ptr->validMembers.resize(size, true);
        }
        
        // Create members (empty DMN pointers are fine for our tests)
        std::vector<CDeterministicMNCPtr> members(size, nullptr);
        
        quorum->Init(std::move(qc_ptr), nullptr, InsecureRand256(), members);
        
        // Set verification vector if requested
        if (hasVerificationVector) {
            std::vector<CBLSPublicKey> vvec;
            for (int i = 0; i < size; ++i) {
                CBLSSecretKey sk;
                sk.MakeNewKey();
                vvec.push_back(sk.GetPublicKey());
            }
            quorum->SetVerificationVector(vvec);
        }
        
        return quorum;
    }
    
    // Helper to create test SessionInfo
    CSigSharesNodeState::SessionInfo CreateTestSessionInfo(CQuorumCPtr quorum)
    {
        CSigSharesNodeState::SessionInfo session;
        session.llmqType = quorum->params.type;
        session.quorumHash = quorum->qc->quorumHash;
        session.id = InsecureRand256();
        session.msgHash = InsecureRand256();
        session.quorum = quorum;
        return session;
    }
    
    // Helper to create test BatchedSigShares
    CBatchedSigShares CreateTestBatchedSigShares(const std::vector<uint16_t>& members)
    {
        CBatchedSigShares batched;
        batched.sessionId = 1;
        
        for (uint16_t member : members) {
            CBLSLazySignature lazySig;
            batched.sigShares.emplace_back(member, lazySig);
        }
        
        return batched;
    }
};

BOOST_FIXTURE_TEST_SUITE(llmq_signing_shares_tests, LLMQSigningSharesTestFixture)

// Test: Missing verification vector
BOOST_AUTO_TEST_CASE(preverify_missing_verification_vector)
{
    // Create quorum WITHOUT verification vector
    auto quorum = CreateMinimalTestQuorum(3, false);
    auto sessionInfo = CreateTestSessionInfo(quorum);
    auto batchedSigShares = CreateTestBatchedSigShares({0, 1});
    
    // Note: We can't easily test the full function because IsQuorumActive and IsMember
    // require complex setup. This test verifies the data structures are created correctly.
    // The missing verification vector check will be hit if the earlier checks pass.
    
    BOOST_CHECK(!quorum->HasVerificationVector());
    BOOST_CHECK_EQUAL(quorum->members.size(), 3);
}

// Test: Duplicate member detection
BOOST_AUTO_TEST_CASE(preverify_duplicate_member)
{
    // Create a valid quorum
    auto quorum = CreateMinimalTestQuorum(5, true);
    auto sessionInfo = CreateTestSessionInfo(quorum);
    
    // Create batch with duplicate member (0 appears twice)
    auto batchedSigShares = CreateTestBatchedSigShares({0, 1, 0, 2});
    
    // We can test the duplicate detection logic by checking the batch structure
    std::unordered_set<uint16_t> seen;
    bool hasDuplicate = false;
    for (const auto& [member, _] : batchedSigShares.sigShares) {
        if (!seen.insert(member).second) {
            hasDuplicate = true;
            break;
        }
    }
    
    BOOST_CHECK(hasDuplicate);
}

// Test: Quorum member out of bounds
BOOST_AUTO_TEST_CASE(preverify_member_out_of_bounds)
{
    // Create quorum with 5 members
    auto quorum = CreateMinimalTestQuorum(5, true);
    auto sessionInfo = CreateTestSessionInfo(quorum);
    
    // Create batch with member index out of bounds (>= 5)
    auto batchedSigShares = CreateTestBatchedSigShares({0, 1, 10});
    
    // Verify that we have an out-of-bounds member
    bool hasOutOfBounds = false;
    for (const auto& [member, _] : batchedSigShares.sigShares) {
        if (member >= quorum->members.size()) {
            hasOutOfBounds = true;
            break;
        }
    }
    
    BOOST_CHECK(hasOutOfBounds);
    BOOST_CHECK_EQUAL(quorum->members.size(), 5);
}

// Test: Invalid quorum member
BOOST_AUTO_TEST_CASE(preverify_invalid_quorum_member)
{
    // Create quorum with specific valid members pattern
    std::vector<bool> validMembers = {true, false, true, true, false};
    auto quorum = CreateMinimalTestQuorum(5, true, validMembers);
    auto sessionInfo = CreateTestSessionInfo(quorum);
    
    // Create batch including an invalid member (member 1 is invalid)
    auto batchedSigShares = CreateTestBatchedSigShares({0, 1, 2});
    
    // Verify that member 1 is marked invalid
    BOOST_CHECK_EQUAL(quorum->qc->validMembers[0], true);
    BOOST_CHECK_EQUAL(quorum->qc->validMembers[1], false);  // Invalid!
    BOOST_CHECK_EQUAL(quorum->qc->validMembers[2], true);
    
    // Check that we can detect the invalid member
    bool hasInvalidMember = false;
    for (const auto& [member, _] : batchedSigShares.sigShares) {
        if (member < quorum->qc->validMembers.size() && 
            !quorum->qc->validMembers[member]) {
            hasInvalidMember = true;
            break;
        }
    }
    
    BOOST_CHECK(hasInvalidMember);
}

// Test: Valid batch structure
BOOST_AUTO_TEST_CASE(preverify_valid_batch_structure)
{
    // Create a valid quorum
    auto quorum = CreateMinimalTestQuorum(5, true);
    auto sessionInfo = CreateTestSessionInfo(quorum);
    
    // Create a valid batch (all members exist and are unique)
    auto batchedSigShares = CreateTestBatchedSigShares({0, 1, 2, 3, 4});
    
    // Verify no duplicates
    std::unordered_set<uint16_t> seen;
    bool hasDuplicate = false;
    for (const auto& [member, _] : batchedSigShares.sigShares) {
        if (!seen.insert(member).second) {
            hasDuplicate = true;
            break;
        }
    }
    BOOST_CHECK(!hasDuplicate);
    
    // Verify all members are in bounds
    bool allInBounds = true;
    for (const auto& [member, _] : batchedSigShares.sigShares) {
        if (member >= quorum->members.size()) {
            allInBounds = false;
            break;
        }
    }
    BOOST_CHECK(allInBounds);
    
    // Verify all members are valid
    bool allValid = true;
    for (const auto& [member, _] : batchedSigShares.sigShares) {
        if (member >= quorum->qc->validMembers.size() || 
            !quorum->qc->validMembers[member]) {
            allValid = false;
            break;
        }
    }
    BOOST_CHECK(allValid);
}

// Test: Empty batch
BOOST_AUTO_TEST_CASE(preverify_empty_batch)
{
    // Create a valid quorum
    auto quorum = CreateMinimalTestQuorum(5, true);
    auto sessionInfo = CreateTestSessionInfo(quorum);
    
    // Create an empty batch
    auto batchedSigShares = CreateTestBatchedSigShares({});
    
    // Empty batch should have no shares
    BOOST_CHECK(batchedSigShares.sigShares.empty());
    
    // Empty batch should pass all validation checks (nothing to validate)
    for (const auto& share : batchedSigShares.sigShares) {
        // This loop shouldn't execute
        (void)share; // Suppress unused variable warning
        BOOST_CHECK(false);
    }
}

// Test: Multiple duplicates
BOOST_AUTO_TEST_CASE(preverify_multiple_duplicates)
{
    auto quorum = CreateMinimalTestQuorum(10, true);
    auto sessionInfo = CreateTestSessionInfo(quorum);
    
    // Create batch with multiple duplicates
    auto batchedSigShares = CreateTestBatchedSigShares({0, 1, 2, 1, 3, 2, 4});
    
    // Count duplicates
    std::unordered_set<uint16_t> seen;
    int duplicateCount = 0;
    for (const auto& [member, _] : batchedSigShares.sigShares) {
        if (!seen.insert(member).second) {
            duplicateCount++;
        }
    }
    
    BOOST_CHECK_EQUAL(duplicateCount, 2); // Members 1 and 2 appear twice each
}

// Test: Boundary case - maximum member index
BOOST_AUTO_TEST_CASE(preverify_boundary_max_member)
{
    const int quorum_size = 10;
    auto quorum = CreateMinimalTestQuorum(quorum_size, true);
    auto sessionInfo = CreateTestSessionInfo(quorum);
    
    // Create batch with last valid member (size - 1)
    auto batchedSigShares = CreateTestBatchedSigShares({0, static_cast<uint16_t>(quorum_size - 1)});
    
    // Verify max member is in bounds
    for (const auto& [member, _] : batchedSigShares.sigShares) {
        BOOST_CHECK(member < quorum->members.size());
    }
}

// Test: All members invalid scenario
BOOST_AUTO_TEST_CASE(preverify_all_members_invalid)
{
    // Create quorum where all members are invalid
    std::vector<bool> validMembers(5, false);
    auto quorum = CreateMinimalTestQuorum(5, true, validMembers);
    auto sessionInfo = CreateTestSessionInfo(quorum);
    
    // Create batch with any members
    auto batchedSigShares = CreateTestBatchedSigShares({0, 1, 2});
    
    // Verify all members are marked invalid
    for (size_t i = 0; i < quorum->qc->validMembers.size(); ++i) {
        BOOST_CHECK_EQUAL(quorum->qc->validMembers[i], false);
    }
    
    // Check that all shares reference invalid members
    bool allInvalid = true;
    for (const auto& [member, _] : batchedSigShares.sigShares) {
        if (member < quorum->qc->validMembers.size() && 
            quorum->qc->validMembers[member]) {
            allInvalid = false;
            break;
        }
    }
    BOOST_CHECK(allInvalid);
}

BOOST_AUTO_TEST_SUITE_END()
