// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/llmq_tests.h>
#include <test/util/setup_common.h>

#include <arith_uint256.h>
#include <bls/bls.h>
#include <bls/bls_worker.h>
#include <chain.h>
#include <evo/deterministicmns.h>
#include <llmq/commitment.h>
#include <llmq/params.h>
#include <llmq/quorums.h>
#include <llmq/signing_shares.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <vector>

using namespace llmq;
using namespace llmq::testutils;

// The ShouldBan() mapping is part of the API contract: only structurally
// malformed peer data may lead to a ban, benign inability to verify never does.
static_assert(!ShouldBan(PreVerifyResult::Success));
static_assert(!ShouldBan(PreVerifyResult::QuorumTooOld));
static_assert(!ShouldBan(PreVerifyResult::NotAMember));
static_assert(!ShouldBan(PreVerifyResult::MissingVerificationVector));
static_assert(ShouldBan(PreVerifyResult::DuplicateMember));
static_assert(ShouldBan(PreVerifyResult::MemberOutOfBounds));
static_assert(ShouldBan(PreVerifyResult::MemberNotValid));

namespace {

struct SigSharesPreVerifyFixture : public BasicTestingSetup {
    const Consensus::LLMQParams& llmq_params{GetLLMQParams(Consensus::LLMQType::LLMQ_TEST)};

    // Never started: preverification must not perform any BLS work
    CBLSWorker bls_worker;
    CBlockIndex quorum_base_block_index;

    // proTxHash values are kept disjoint from GetTestQuorumHash() values
    static uint256 ProTxHashOfMember(size_t member_idx) { return ArithToUint256(arith_uint256(1000 + member_idx)); }
    static uint256 NonMemberProTxHash() { return ArithToUint256(arith_uint256(999)); }

    // Builds a quorum of llmq_params.size members whose proTxHashes are
    // ProTxHashOfMember(0..size-1), all valid unless listed in invalid_members
    CQuorumCPtr BuildQuorum(bool with_verification_vector = true, const std::vector<size_t>& invalid_members = {})
    {
        auto qc = std::make_unique<CFinalCommitment>();
        qc->llmqType = llmq_params.type;
        qc->quorumHash = GetTestQuorumHash(1);
        qc->validMembers.assign(llmq_params.size, true);
        for (size_t idx : invalid_members) {
            qc->validMembers.at(idx) = false;
        }

        std::vector<CDeterministicMNCPtr> members;
        for (size_t i = 0; i < static_cast<size_t>(llmq_params.size); i++) {
            auto dmn = std::make_shared<CDeterministicMN>(/*_internalId=*/i + 1);
            dmn->proTxHash = ProTxHashOfMember(i);
            members.push_back(std::move(dmn));
        }

        auto quorum = std::make_shared<CQuorum>(llmq_params, bls_worker, std::move(qc), &quorum_base_block_index,
                                                /*_minedBlockHash=*/uint256(), members);
        if (with_verification_vector) {
            // The pointer overload installs the vvec as-is; HasVerificationVector() only
            // null-checks it, so an empty vector is enough here. The value overload would
            // reject it against qc->quorumVvecHash.
            quorum->SetVerificationVector(std::make_shared<std::vector<CBLSPublicKey>>());
        }
        return quorum;
    }

    // Preverification only looks at the member indexes, the signatures are never inspected
    static CBatchedSigShares MakeBatch(const std::vector<uint16_t>& member_indexes)
    {
        CBatchedSigShares batch;
        for (uint16_t idx : member_indexes) {
            batch.sigShares.emplace_back(idx, CBLSLazySignature());
        }
        return batch;
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(llmq_signing_shares_tests, SigSharesPreVerifyFixture)

BOOST_AUTO_TEST_CASE(preverify_success)
{
    const auto quorum = BuildQuorum();
    const auto res = PreVerifyBatchedSigShares(ProTxHashOfMember(0), /*is_quorum_active=*/true, *quorum,
                                               MakeBatch({0, 1, 2}));
    BOOST_CHECK(res == PreVerifyResult::Success);
    BOOST_CHECK(!ShouldBan(res));
}

BOOST_AUTO_TEST_CASE(preverify_empty_batch_is_not_malformed)
{
    const auto quorum = BuildQuorum();
    const auto res = PreVerifyBatchedSigShares(ProTxHashOfMember(0), /*is_quorum_active=*/true, *quorum, MakeBatch({}));
    BOOST_CHECK(res == PreVerifyResult::Success);
    BOOST_CHECK(!ShouldBan(res));
}

BOOST_AUTO_TEST_CASE(preverify_quorum_too_old_no_ban)
{
    const auto quorum = BuildQuorum();
    const auto res = PreVerifyBatchedSigShares(ProTxHashOfMember(0), /*is_quorum_active=*/false, *quorum,
                                               MakeBatch({0, 1, 2}));
    BOOST_CHECK(res == PreVerifyResult::QuorumTooOld);
    BOOST_CHECK(!ShouldBan(res));

    // Benign prechecks run before the structural checks: a batch that would
    // otherwise be ban-worthy must still not lead to a ban if we can't verify it
    const auto res_malformed = PreVerifyBatchedSigShares(ProTxHashOfMember(0), /*is_quorum_active=*/false, *quorum,
                                                         MakeBatch({0, 0}));
    BOOST_CHECK(res_malformed == PreVerifyResult::QuorumTooOld);
    BOOST_CHECK(!ShouldBan(res_malformed));
}

BOOST_AUTO_TEST_CASE(preverify_not_a_member_no_ban)
{
    const auto quorum = BuildQuorum();
    const auto res = PreVerifyBatchedSigShares(NonMemberProTxHash(), /*is_quorum_active=*/true, *quorum,
                                               MakeBatch({0, 1, 2}));
    BOOST_CHECK(res == PreVerifyResult::NotAMember);
    BOOST_CHECK(!ShouldBan(res));
}

BOOST_AUTO_TEST_CASE(preverify_missing_verification_vector_no_ban)
{
    const auto quorum = BuildQuorum(/*with_verification_vector=*/false);
    const auto res = PreVerifyBatchedSigShares(ProTxHashOfMember(0), /*is_quorum_active=*/true, *quorum,
                                               MakeBatch({0, 1, 2}));
    BOOST_CHECK(res == PreVerifyResult::MissingVerificationVector);
    BOOST_CHECK(!ShouldBan(res));
}

BOOST_AUTO_TEST_CASE(preverify_duplicate_member_bans)
{
    const auto quorum = BuildQuorum();
    const auto res = PreVerifyBatchedSigShares(ProTxHashOfMember(0), /*is_quorum_active=*/true, *quorum,
                                               MakeBatch({0, 1, 0}));
    BOOST_CHECK(res == PreVerifyResult::DuplicateMember);
    BOOST_CHECK(ShouldBan(res));
}

BOOST_AUTO_TEST_CASE(preverify_member_out_of_bounds_bans)
{
    const auto quorum = BuildQuorum();
    const auto oob_idx = static_cast<uint16_t>(llmq_params.size); // one past the last member

    const auto res = PreVerifyBatchedSigShares(ProTxHashOfMember(0), /*is_quorum_active=*/true, *quorum,
                                               MakeBatch({oob_idx}));
    BOOST_CHECK(res == PreVerifyResult::MemberOutOfBounds);
    BOOST_CHECK(ShouldBan(res));

    // A single bad entry poisons an otherwise valid batch
    const auto res_mixed = PreVerifyBatchedSigShares(ProTxHashOfMember(0), /*is_quorum_active=*/true, *quorum,
                                                     MakeBatch({0, oob_idx}));
    BOOST_CHECK(res_mixed == PreVerifyResult::MemberOutOfBounds);
    BOOST_CHECK(ShouldBan(res_mixed));
}

BOOST_AUTO_TEST_CASE(preverify_member_not_valid_bans)
{
    const auto quorum = BuildQuorum(/*with_verification_vector=*/true, /*invalid_members=*/{1});
    const auto res = PreVerifyBatchedSigShares(ProTxHashOfMember(0), /*is_quorum_active=*/true, *quorum, MakeBatch({0, 1}));
    BOOST_CHECK(res == PreVerifyResult::MemberNotValid);
    BOOST_CHECK(ShouldBan(res));
}

BOOST_AUTO_TEST_SUITE_END()
