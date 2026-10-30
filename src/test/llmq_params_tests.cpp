// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/llmq_tests.h>
#include <test/util/setup_common.h>

#include <chainparams.h>
#include <chainparamsbase.h>
#include <consensus/params.h>
#include <llmq/params.h>
#include <util/system.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <string>

using namespace llmq;
using namespace llmq::testutils;
using namespace Consensus;

BOOST_FIXTURE_TEST_SUITE(llmq_params_tests, BasicTestingSetup)

// Get test params for use in tests
static const Consensus::LLMQParams& TEST_PARAMS_BASE = GetLLMQParams(Consensus::LLMQType::LLMQ_TEST_V17);

BOOST_AUTO_TEST_CASE(llmq_params_max_cycles_test)
{
    // Test non-rotated quorum
    LLMQParams nonRotated = TEST_PARAMS_BASE;
    nonRotated.useRotation = false;
    nonRotated.signingActiveQuorumCount = 2;

    // For non-rotated: max_cycles = quorums_count
    BOOST_CHECK_EQUAL(nonRotated.max_cycles(10), 10);
    BOOST_CHECK_EQUAL(nonRotated.max_cycles(100), 100);
    BOOST_CHECK_EQUAL(nonRotated.max_cycles(1), 1);
    BOOST_CHECK_EQUAL(nonRotated.max_cycles(0), 0);

    // Test rotated quorum
    LLMQParams rotated = TEST_PARAMS_BASE;
    rotated.useRotation = true;
    rotated.signingActiveQuorumCount = 2;

    // For rotated: max_cycles = quorums_count / signingActiveQuorumCount
    BOOST_CHECK_EQUAL(rotated.max_cycles(10), 5);
    BOOST_CHECK_EQUAL(rotated.max_cycles(100), 50);
    BOOST_CHECK_EQUAL(rotated.max_cycles(1), 0); // Integer division
    BOOST_CHECK_EQUAL(rotated.max_cycles(0), 0);

    // Test with different signingActiveQuorumCount
    rotated.signingActiveQuorumCount = 4;
    BOOST_CHECK_EQUAL(rotated.max_cycles(100), 25);
    BOOST_CHECK_EQUAL(rotated.max_cycles(16), 4);
    BOOST_CHECK_EQUAL(rotated.max_cycles(15), 3); // Integer division
}

BOOST_AUTO_TEST_CASE(llmq_params_max_store_depth_test)
{
    LLMQParams params = TEST_PARAMS_BASE;
    params.dkgInterval = 24;
    params.signingActiveQuorumCount = 2;
    params.keepOldKeys = 10;

    // max_store_depth = max_cycles(keepOldKeys) * dkgInterval

    // Test non-rotated
    params.useRotation = false;
    // max_cycles(10) = 10 for non-rotated
    BOOST_CHECK_EQUAL(params.max_store_depth(), 10 * 24); // 240

    // Test rotated
    params.useRotation = true;
    // max_cycles(10) = 10/2 = 5 for rotated
    BOOST_CHECK_EQUAL(params.max_store_depth(), 5 * 24); // 120

    // Test with different values
    params.keepOldKeys = 20;
    params.dkgInterval = 48;
    params.signingActiveQuorumCount = 4;
    // max_cycles(20) = 20/4 = 5 for rotated
    BOOST_CHECK_EQUAL(params.max_store_depth(), 5 * 48); // 240

    // Test edge cases
    params.keepOldKeys = 0;
    BOOST_CHECK_EQUAL(params.max_store_depth(), 0);

    params.keepOldKeys = 1;
    params.dkgInterval = 1;
    params.signingActiveQuorumCount = 1;
    BOOST_CHECK_EQUAL(params.max_store_depth(), 1);
}

BOOST_AUTO_TEST_CASE(llmq_params_validation_test)
{
    // Test parameter constraints and relationships
    LLMQParams params = TEST_PARAMS_BASE;

    // minSize should be less than or equal to size
    BOOST_CHECK_LE(params.minSize, params.size);

    // threshold should be less than or equal to size
    BOOST_CHECK_LE(params.threshold, params.size);

    // threshold should typically be > 50% of size for security
    BOOST_CHECK_GT(params.threshold * 2, params.size);

    // dkgMiningWindowStart should be after DKG phases complete
    // Typically should be >= 5 * dkgPhaseBlocks
    BOOST_CHECK_GE(params.dkgMiningWindowStart, 5 * params.dkgPhaseBlocks);

    // dkgMiningWindowEnd should be after dkgMiningWindowStart
    BOOST_CHECK_GT(params.dkgMiningWindowEnd, params.dkgMiningWindowStart);

    // dkgMiningWindowEnd should be within dkgInterval
    BOOST_CHECK_LT(params.dkgMiningWindowEnd, params.dkgInterval);
}

BOOST_AUTO_TEST_CASE(llmq_params_types_test)
{
    // Test that LLMQ types are properly defined
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(LLMQType::LLMQ_NONE), 0xff);
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(LLMQType::LLMQ_50_60), 1);
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(LLMQType::LLMQ_400_60), 2);
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(LLMQType::LLMQ_400_85), 3);
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(LLMQType::LLMQ_100_67), 4);
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(LLMQType::LLMQ_60_75), 5);
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(LLMQType::LLMQ_25_67), 6);

    // Test special types
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(LLMQType::LLMQ_TEST), 100);
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(LLMQType::LLMQ_DEVNET), 101);
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(LLMQType::LLMQ_TEST_V17), 102);
}

BOOST_AUTO_TEST_CASE(llmq_params_edge_calculations_test)
{
    LLMQParams params;

    // Test with maximum values
    params.useRotation = true;
    params.signingActiveQuorumCount = 1;
    params.keepOldKeys = std::numeric_limits<int>::max() / 2; // Avoid overflow
    params.dkgInterval = 2;

    // Should not overflow
    int depth = params.max_store_depth();
    BOOST_CHECK_GT(depth, 0);

    // Test with all zeros
    params.useRotation = false;
    params.keepOldKeys = 0;
    params.dkgInterval = 0;
    BOOST_CHECK_EQUAL(params.max_store_depth(), 0);
}

BOOST_AUTO_TEST_CASE(llmq_params_calculations_overflow_test)
{
    LLMQParams params;
    params.useRotation = false;
    params.signingActiveQuorumCount = 1;

    // Test max_cycles with large values
    int largeQuorumCount = std::numeric_limits<int>::max();
    int cycles = params.max_cycles(largeQuorumCount);
    BOOST_CHECK_EQUAL(cycles, largeQuorumCount); // Non-rotated returns input

    // Test potential overflow in max_store_depth
    params.keepOldKeys = std::numeric_limits<int>::max() / 100;
    params.dkgInterval = 100;

    // This should not crash or overflow
    int depth = params.max_store_depth();
    BOOST_CHECK_GE(depth, 0); // Result should be valid

    // Test with rotation and potential division issues
    params.useRotation = true;
    params.signingActiveQuorumCount = std::numeric_limits<int>::max();
    cycles = params.max_cycles(1000);
    BOOST_CHECK_EQUAL(cycles, 0); // 1000 / max_int = 0
}

//! Build chain params for `chain` with a single LLMQ size/threshold override applied.
static std::unique_ptr<const CChainParams> ChainParamsWithLLMQOverride(const std::string& chain,
                                                                       const std::string& arg,
                                                                       int size, int threshold)
{
    ArgsManager args;
    args.ForceSetArg(arg, strprintf("%d:%d", size, threshold));
    return CreateChainParams(args, chain);
}

static int LLMQSize(const CChainParams& params, LLMQType type)
{
    const auto& llmqs = params.GetConsensus().llmqs;
    const auto it = std::ranges::find_if(llmqs, [type](const auto& llmq) { return llmq.type == type; });
    BOOST_REQUIRE(it != llmqs.end());
    return it->size;
}

// Quorum sizes above MAX_LLMQ_SIZE cannot sign end-to-end: signing session inventories and
// batched sig-share messages are bounded by that same maximum, so a node running such a quorum
// would drop its peers' sig shares. Reject the override at chain-parameter construction instead.
BOOST_AUTO_TEST_CASE(llmq_params_size_override_bounds_test)
{
    constexpr int kMax = Consensus::MAX_LLMQ_SIZE;

    const std::pair<std::string, LLMQType> regtest_overrides[]{
        {"-llmqtestparams", LLMQType::LLMQ_TEST},
        {"-llmqtestinstantsendparams", LLMQType::LLMQ_TEST_INSTANTSEND},
        {"-llmqtestplatformparams", LLMQType::LLMQ_TEST_PLATFORM},
    };

    for (const auto& [arg, type] : regtest_overrides) {
        // Normal custom values keep working
        const auto small = ChainParamsWithLLMQOverride(CBaseChainParams::REGTEST, arg, 5, 3);
        BOOST_CHECK_EQUAL(LLMQSize(*small, type), 5);

        // Exactly the maximum is accepted
        const auto at_max = ChainParamsWithLLMQOverride(CBaseChainParams::REGTEST, arg, kMax, kMax * 2 / 3);
        BOOST_CHECK_EQUAL(LLMQSize(*at_max, type), kMax);

        // One above the maximum is rejected
        BOOST_CHECK_THROW(ChainParamsWithLLMQOverride(CBaseChainParams::REGTEST, arg, kMax + 1, kMax * 2 / 3),
                          std::runtime_error);
    }

    // LLMQ_TEST_DIP0024 rotates in quarters, so its size must be a positive multiple of 4
    {
        const auto params = ChainParamsWithLLMQOverride(CBaseChainParams::REGTEST, "-llmqtestdip0024params", 8, 2);
        const auto& llmqs = params->GetConsensus().llmqs;
        const auto it = std::ranges::find_if(llmqs, [](const auto& llmq) { return llmq.type == LLMQType::LLMQ_TEST_DIP0024; });
        BOOST_REQUIRE(it != llmqs.end());
        BOOST_CHECK_EQUAL(it->size, 8);
        BOOST_CHECK_EQUAL(it->minSize, 2);
        BOOST_CHECK_EQUAL(it->threshold, 2);
        BOOST_CHECK_EQUAL(it->dkgBadVotesThreshold, 2);

        ArgsManager malformed;
        malformed.ForceSetArg("-llmqtestdip0024params", "8");
        BOOST_CHECK_THROW(CreateChainParams(malformed, CBaseChainParams::REGTEST), std::runtime_error);

        for (const int bad_size : {0, -4, 6, kMax + 4}) {
            BOOST_CHECK_THROW(ChainParamsWithLLMQOverride(CBaseChainParams::REGTEST, "-llmqtestdip0024params", bad_size, 1),
                              std::runtime_error);
        }
    }

    // Same bounds for the devnet override
    const auto devnet_small = ChainParamsWithLLMQOverride(CBaseChainParams::DEVNET, "-llmqdevnetparams", 12, 6);
    BOOST_CHECK_EQUAL(LLMQSize(*devnet_small, LLMQType::LLMQ_DEVNET), 12);

    const auto devnet_at_max = ChainParamsWithLLMQOverride(CBaseChainParams::DEVNET, "-llmqdevnetparams", kMax, kMax / 2);
    BOOST_CHECK_EQUAL(LLMQSize(*devnet_at_max, LLMQType::LLMQ_DEVNET), kMax);

    BOOST_CHECK_THROW(ChainParamsWithLLMQOverride(CBaseChainParams::DEVNET, "-llmqdevnetparams", kMax + 1, kMax / 2),
                      std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()
