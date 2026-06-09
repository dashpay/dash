// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/llmq_tests.h>
#include <test/util/setup_common.h>

#include <consensus/params.h>
#include <llmq/signing_shares.h>

#include <boost/test/unit_test.hpp>

using namespace llmq;
using namespace llmq::testutils;

BOOST_FIXTURE_TEST_SUITE(llmq_signing_shares_tests, BasicTestingSetup)

static CSigSesAnn MakeAnn(uint32_t session_id, uint32_t nonce, Consensus::LLMQType llmq_type = Consensus::LLMQType::LLMQ_50_60)
{
    return CSigSesAnn{session_id, llmq_type, GetTestQuorumHash(1), GetTestQuorumHash(2), GetTestQuorumHash(nonce)};
}

BOOST_AUTO_TEST_CASE(sig_ses_ann_respects_session_limit_but_allows_refresh)
{
    CSigSharesNodeState node_state;

    const CSigSesAnn ann1{MakeAnn(1, 1)};
    const CSigSesAnn ann2{MakeAnn(2, 2)};
    const CSigSesAnn ann3{MakeAnn(3, 3)};
    constexpr size_t max_sessions{2};

    BOOST_CHECK(node_state.CanCreateSessionFromAnn(ann1, max_sessions));
    node_state.GetOrCreateSessionFromAnn(ann1);
    BOOST_CHECK_EQUAL(node_state.GetSessionCount(), 1U);

    BOOST_CHECK(node_state.CanCreateSessionFromAnn(ann2, max_sessions));
    node_state.GetOrCreateSessionFromAnn(ann2);
    BOOST_CHECK_EQUAL(node_state.GetSessionCount(), max_sessions);

    BOOST_CHECK(!node_state.CanCreateSessionFromAnn(ann3, max_sessions));

    const CSigSesAnn ann1_refresh{4, Consensus::LLMQType::LLMQ_50_60, ann1.getQuorumHash(), ann1.getId(), ann1.getMsgHash()};
    BOOST_CHECK(node_state.CanCreateSessionFromAnn(ann1_refresh, max_sessions));
    node_state.GetOrCreateSessionFromAnn(ann1_refresh);
    BOOST_CHECK_EQUAL(node_state.GetSessionCount(), max_sessions);
}

BOOST_AUTO_TEST_CASE(sig_ses_ann_limit_is_per_llmq_type)
{
    CSigSharesNodeState node_state;

    constexpr size_t max_sessions{1};
    const CSigSesAnn ann1{MakeAnn(1, 1)};
    const CSigSesAnn ann2{MakeAnn(2, 2)};
    const CSigSesAnn other_type_ann{MakeAnn(3, 3, Consensus::LLMQType::LLMQ_400_60)};

    BOOST_CHECK(node_state.CanCreateSessionFromAnn(ann1, max_sessions));
    node_state.GetOrCreateSessionFromAnn(ann1);
    BOOST_CHECK_EQUAL(node_state.GetSessionCount(), 1U);
    BOOST_CHECK_EQUAL(node_state.GetSessionCount(Consensus::LLMQType::LLMQ_50_60), 1U);

    BOOST_CHECK(!node_state.CanCreateSessionFromAnn(ann2, max_sessions));
    BOOST_CHECK(node_state.CanCreateSessionFromAnn(other_type_ann, max_sessions));
    node_state.GetOrCreateSessionFromAnn(other_type_ann);
    BOOST_CHECK_EQUAL(node_state.GetSessionCount(), 2U);
    BOOST_CHECK_EQUAL(node_state.GetSessionCount(Consensus::LLMQType::LLMQ_400_60), 1U);
}

BOOST_AUTO_TEST_SUITE_END()
