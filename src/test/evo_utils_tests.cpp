// Copyright (c) 2022 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <llmq/utils.h>
#include <llmq/params.h>

#include <chainparams.h>

#include <validation.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(evo_utils_tests)

BOOST_FIXTURE_TEST_CASE(utils_IsQuorumTypeEnabled_tests, TestChainDIP3Setup)
{
    auto params = Params().GetConsensus();
    using namespace llmq;

    // We can't use Params().GetConsensus().llmqTypeDIP0024InstantSend / llmqTypeInstantSend since on regtest,
    // Consensus::LLMQType::LLMQ_TEST doesn't get disabled
    BOOST_CHECK_EQUAL(CLLMQUtils::IsQuorumTypeEnabledInternal(Consensus::LLMQType::LLMQ_50_60, nullptr, false, false), true);
    BOOST_CHECK_EQUAL(CLLMQUtils::IsQuorumTypeEnabledInternal(Consensus::LLMQType::LLMQ_50_60, nullptr, true, false), true);
    BOOST_CHECK_EQUAL(CLLMQUtils::IsQuorumTypeEnabledInternal(Consensus::LLMQType::LLMQ_50_60, nullptr, true, true), false);

    BOOST_CHECK_EQUAL(CLLMQUtils::IsQuorumTypeEnabledInternal(Consensus::LLMQType::LLMQ_60_75, nullptr, false, false), false);
    BOOST_CHECK_EQUAL(CLLMQUtils::IsQuorumTypeEnabledInternal(Consensus::LLMQType::LLMQ_60_75, nullptr, true, false), true);
    BOOST_CHECK_EQUAL(CLLMQUtils::IsQuorumTypeEnabledInternal(Consensus::LLMQType::LLMQ_60_75, nullptr, true, true), true);
    BOOST_CHECK_EQUAL(CLLMQUtils::IsQuorumTypeEnabledInternal(Consensus::LLMQType::LLMQ_TEST_DIP0024, nullptr, false, false), false);
    BOOST_CHECK_EQUAL(CLLMQUtils::IsQuorumTypeEnabledInternal(Consensus::LLMQType::LLMQ_TEST_DIP0024, nullptr, true, false), true);
    BOOST_CHECK_EQUAL(CLLMQUtils::IsQuorumTypeEnabledInternal(Consensus::LLMQType::LLMQ_TEST_DIP0024, nullptr, true, true), true);
}

BOOST_AUTO_TEST_SUITE_END()
