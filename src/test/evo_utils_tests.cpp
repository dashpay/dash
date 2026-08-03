// Copyright (c) 2022-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <chainparams.h>
#include <llmq/context.h>
#include <llmq/options.h>
#include <llmq/utils.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

using node::NodeContext;

/* TODO: rename this file and test to llmq_options_test */
BOOST_AUTO_TEST_SUITE(evo_utils_tests)

void Test(NodeContext& node)
{
    using namespace llmq;
    auto tip = WITH_LOCK(::cs_main, return node.chainman->ActiveTip());
    const auto& consensus_params = Params().GetConsensus();
    BOOST_CHECK_EQUAL(node.chainman->IsQuorumTypeEnabled(consensus_params.llmqTypeDIP0024InstantSend, tip,
                                                         /*optDIP0024IsActive=*/false, /*optHaveDIP0024Quorums=*/false),
                      false);
    BOOST_CHECK_EQUAL(node.chainman->IsQuorumTypeEnabled(consensus_params.llmqTypeDIP0024InstantSend, tip,
                                                         /*optDIP0024IsActive=*/true, /*optHaveDIP0024Quorums=*/false),
                      true);
    BOOST_CHECK_EQUAL(node.chainman->IsQuorumTypeEnabled(consensus_params.llmqTypeDIP0024InstantSend, tip,
                                                         /*optDIP0024IsActive=*/true, /*optHaveDIP0024Quorums=*/true),
                      true);
    BOOST_CHECK_EQUAL(node.chainman->IsQuorumTypeEnabled(consensus_params.llmqTypeChainLocks, tip,
                                                         /*optDIP0024IsActive=*/false, /*optHaveDIP0024Quorums=*/false),
                      true);
    BOOST_CHECK_EQUAL(node.chainman->IsQuorumTypeEnabled(consensus_params.llmqTypeChainLocks, tip,
                                                         /*optDIP0024IsActive=*/true, /*optHaveDIP0024Quorums=*/false),
                      true);
    BOOST_CHECK_EQUAL(node.chainman->IsQuorumTypeEnabled(consensus_params.llmqTypeChainLocks, tip,
                                                         /*optDIP0024IsActive=*/true, /*optHaveDIP0024Quorums=*/true),
                      true);
    BOOST_CHECK_EQUAL(node.chainman->IsQuorumTypeEnabled(consensus_params.llmqTypePlatform, tip,
                                                         /*optDIP0024IsActive=*/false, /*optHaveDIP0024Quorums=*/false),
                      Params().IsTestChain());
    BOOST_CHECK_EQUAL(node.chainman->IsQuorumTypeEnabled(consensus_params.llmqTypePlatform, tip,
                                                         /*optDIP0024IsActive=*/true, /*optHaveDIP0024Quorums=*/false),
                      Params().IsTestChain());
    BOOST_CHECK_EQUAL(node.chainman->IsQuorumTypeEnabled(consensus_params.llmqTypePlatform, tip,
                                                         /*optDIP0024IsActive=*/true, /*optHaveDIP0024Quorums=*/true),
                      Params().IsTestChain());
    BOOST_CHECK_EQUAL(node.chainman->IsQuorumTypeEnabled(consensus_params.llmqTypeMnhf, tip,
                                                         /*optDIP0024IsActive=*/false, /*optHaveDIP0024Quorums=*/false),
                      true);
    BOOST_CHECK_EQUAL(node.chainman->IsQuorumTypeEnabled(consensus_params.llmqTypeMnhf, tip,
                                                         /*optDIP0024IsActive=*/true, /*optHaveDIP0024Quorums=*/false),
                      true);
    BOOST_CHECK_EQUAL(node.chainman->IsQuorumTypeEnabled(consensus_params.llmqTypeMnhf, tip,
                                                         /*optDIP0024IsActive=*/true, /*optHaveDIP0024Quorums=*/true),
                      true);
}

BOOST_FIXTURE_TEST_CASE(utils_IsQuorumTypeEnabled_tests_regtest, RegTestingSetup)
{
    Test(m_node);
}

BOOST_FIXTURE_TEST_CASE(utils_IsQuorumTypeEnabled_tests_mainnet, TestingSetup)
{
    Test(m_node);
}

// Regression: a genesis quorum base has pprev == nullptr.
// Pre-fix that pointer was fed to IsQuorumTypeEnabled's gsl::not_null parameter,
// whose converting constructor calls Expects() and so std::terminate()s the
// process. That is [[noreturn]] noexcept, not an exception, so no try/catch in
// the validation or net-processing stack could contain it.
//
// Note the two guards below are not equivalent. Passing a literal nullptr was a
// *compile* error pre-fix (not_null(std::nullptr_t) is deleted), so that line
// only pins the relaxed signature. The GetAllQuorumMembers() call is the one
// that reproduced the abort, since the null arrives through a runtime pointer.
// The end-to-end consensus path is covered by
// llmq_commitment_tests/commitment_genesis_quorum_hash_rejected_test.
BOOST_FIXTURE_TEST_CASE(genesis_quorum_base_null_pprev_safe, RegTestingSetup)
{
    const CBlockIndex* genesis = WITH_LOCK(::cs_main, return m_node.chainman->ActiveTip());
    BOOST_REQUIRE(genesis != nullptr);
    BOOST_REQUIRE_EQUAL(genesis->nHeight, 0);
    BOOST_REQUIRE(genesis->pprev == nullptr);

    const auto llmq_type = Params().GetConsensus().llmqTypeChainLocks;

    // IsQuorumTypeEnabled sink pattern (NetDKG passes pQuorumBaseBlockIndex->pprev).
    BOOST_CHECK(!m_node.chainman->IsQuorumTypeEnabled(llmq_type, nullptr));

    // GetAllQuorumMembers with m_base_index == genesis.
    const llmq::UtilParameters util_params{*Assert(m_node.dmnman),
                                           *Assert(m_node.llmq_ctx)->qsnapman,
                                           *Assert(m_node.chainman),
                                           genesis};
    const auto members = llmq::utils::GetAllQuorumMembers(llmq_type, util_params);
    BOOST_CHECK(members.empty());
}

BOOST_AUTO_TEST_SUITE_END()
