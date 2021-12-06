// Copyright (c) 2021 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/setup_common.h>

#include <chainparams.h>
#include <consensus/validation.h>
#include <miner.h>
#include <script/interpreter.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

const auto deployment_id = Consensus::DEPLOYMENT_DIP0020;
const int window{100}, th_start{80}, th_end{60};

struct TestChain98Setup : public TestChainSetup
{
    TestChain98Setup() : TestChainSetup(98) {}
};

static int threshold(int attempt)
{
    // An implementation of VersionBitsConditionChecker::Threshold()
    int threshold_calc = th_start - attempt * attempt * window / 100 / 5;
    if (threshold_calc < th_end) {
        return th_end;
    }
    return threshold_calc;
};

BOOST_AUTO_TEST_SUITE(dynamic_activation_thresholds_tests)

BOOST_FIXTURE_TEST_CASE(activate_at_min_level, TestChain98Setup)
{
    const auto& consensus_params = Params().GetConsensus();
    CScript coinbasePubKey = CScript() <<  ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;

    auto signal = [&](int num_blocks, bool expected_lockin)
    {
        // Mine non-signalling blocks
        gArgs.ForceSetArg("-blockversion", "536870912");
        for (int i = 0; i < window - num_blocks; ++i) {
            CreateAndProcessBlock({}, coinbaseKey);
        }
        gArgs.ForceRemoveArg("-blockversion");
        if (num_blocks > 0) {
            // Mine signalling blocks
            for (int i = 0; i < num_blocks; ++i) {
                CreateAndProcessBlock({}, coinbaseKey);
            }
        }
        LOCK(cs_main);
        if (expected_lockin) {
            BOOST_CHECK_EQUAL(VersionBitsTipState(consensus_params, deployment_id), ThresholdState::LOCKED_IN);
        } else {
            BOOST_CHECK_EQUAL(VersionBitsTipState(consensus_params, deployment_id), ThresholdState::STARTED);
        }
    };

    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(::ChainActive().Height(), 98);
        BOOST_CHECK_EQUAL(VersionBitsTipState(consensus_params, deployment_id), ThresholdState::DEFINED);
    }

    CreateAndProcessBlock({}, coinbaseKey);

    {
        LOCK(cs_main);
        // Advance from DEFINED to STARTED at height = 99
        BOOST_CHECK_EQUAL(::ChainActive().Height(), 99);
        BOOST_CHECK_EQUAL(VersionBitsTipState(consensus_params, deployment_id), ThresholdState::STARTED);
        BOOST_CHECK_EQUAL(VersionBitsTipStatistics(consensus_params, deployment_id).threshold, threshold(0));
        // Next block should be signaling by default
        const auto pblocktemplate = BlockAssembler(Params()).CreateNewBlock(coinbasePubKey);
        BOOST_ASSERT(::ChainActive().Tip()->nVersion = 536870912);
        BOOST_ASSERT(pblocktemplate->block.nVersion != 536870912);
    }

    // Reach min level + 1 more to check it doesn't go lower than that
    for (int i = 0; i < 12; ++i) {
        signal(threshold(i) - 1, false); // 1 block short

        {
            // Still STARTED but with a (potentially) new threshold
            LOCK(cs_main);
            BOOST_CHECK_EQUAL(::ChainActive().Height(), window * (i + 2) - 1);
            BOOST_CHECK_EQUAL(VersionBitsTipState(consensus_params, deployment_id), ThresholdState::STARTED);
            BOOST_CHECK_EQUAL(VersionBitsTipStatistics(consensus_params, deployment_id).threshold, threshold(i + 1));
        }
    }
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(VersionBitsTipState(consensus_params, deployment_id), ThresholdState::STARTED);
        BOOST_CHECK_EQUAL(VersionBitsTipStatistics(consensus_params, deployment_id).threshold, th_end);
    }

    // activate
    signal(threshold(12), true);
    for (int i = 0; i < window; ++i) {
        CreateAndProcessBlock({}, coinbaseKey);
    }
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(VersionBitsTipState(consensus_params, deployment_id), ThresholdState::ACTIVE);
    }
}

BOOST_FIXTURE_TEST_CASE(activate_at_mid_level, TestChain98Setup)
{
    const auto& consensus_params = Params().GetConsensus();
    CScript coinbasePubKey = CScript() <<  ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;

    auto signal = [&](int num_blocks, bool expected_lockin)
    {
        // Mine non-signalling blocks
        gArgs.ForceSetArg("-blockversion", "536870912");
        for (int i = 0; i < window - num_blocks; ++i) {
            CreateAndProcessBlock({}, coinbaseKey);
        }
        gArgs.ForceRemoveArg("-blockversion");
        if (num_blocks > 0) {
            // Mine signalling blocks
            for (int i = 0; i < num_blocks; ++i) {
                CreateAndProcessBlock({}, coinbaseKey);
            }
        }
        LOCK(cs_main);
        if (expected_lockin) {
            BOOST_CHECK_EQUAL(VersionBitsTipState(consensus_params, deployment_id), ThresholdState::LOCKED_IN);
        } else {
            BOOST_CHECK_EQUAL(VersionBitsTipState(consensus_params, deployment_id), ThresholdState::STARTED);
        }
    };

    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(::ChainActive().Height(), 98);
        BOOST_CHECK_EQUAL(VersionBitsTipState(consensus_params, deployment_id), ThresholdState::DEFINED);
    }

    CreateAndProcessBlock({}, coinbaseKey);

    {
        LOCK(cs_main);
        // Advance from DEFINED to STARTED at height = 99
        BOOST_CHECK_EQUAL(::ChainActive().Height(), 99);
        BOOST_CHECK_EQUAL(VersionBitsTipState(consensus_params, deployment_id), ThresholdState::STARTED);
        BOOST_CHECK_EQUAL(VersionBitsTipStatistics(consensus_params, deployment_id).threshold, threshold(0));
        // Next block should be signaling by default
        const auto pblocktemplate = BlockAssembler(Params()).CreateNewBlock(coinbasePubKey);
        BOOST_ASSERT(::ChainActive().Tip()->nVersion = 536870912);
        BOOST_ASSERT(pblocktemplate->block.nVersion != 536870912);
    }

    // Reach mid level
    for (int i = 0; i < 6; ++i) {
        signal(threshold(i) - 1, false); // 1 block short

        {
            // Still STARTED but with a (potentially) new threshold
            LOCK(cs_main);
            BOOST_CHECK_EQUAL(::ChainActive().Height(), window * (i + 2) - 1);
            BOOST_CHECK_EQUAL(VersionBitsTipState(consensus_params, deployment_id), ThresholdState::STARTED);
            BOOST_CHECK_EQUAL(VersionBitsTipStatistics(consensus_params, deployment_id).threshold, threshold(i + 1));
        }
    }

    // activate
    signal(threshold(6), true);
    for (int i = 0; i < window; ++i) {
        CreateAndProcessBlock({}, coinbaseKey);
    }
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(VersionBitsTipState(consensus_params, deployment_id), ThresholdState::ACTIVE);
    }
}

BOOST_AUTO_TEST_SUITE_END()
