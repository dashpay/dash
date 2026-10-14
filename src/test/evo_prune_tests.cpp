// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <chainparams.h>
#include <chainparamsbase.h>
#include <evo/creditpool.h>
#include <evo/specialtxman.h>
#include <llmq/snapshot.h>
#include <util/system.h>

#include <boost/test/unit_test.hpp>

#include <string>

BOOST_FIXTURE_TEST_SUITE(evo_prune_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(evo_prune_lock_depth)
{
    // Mainnet, testnet and devnet: rebuilding LLMQ_60_75 members (dkgInterval 288, dkgMiningWindowEnd 50) dominates.
    // Regtest: rebuilding the credit pool (snapshot period 576, CreditPoolPeriodBlocks 100) dominates.
    const std::pair<std::string, int> expected_depths[]{
        {CBaseChainParams::MAIN, 6 * 288 + 50 + 8},
        {CBaseChainParams::TESTNET, 6 * 288 + 50 + 8},
        {CBaseChainParams::DEVNET, 6 * 288 + 50 + 8},
        {CBaseChainParams::REGTEST, 576 + 100},
    };
    for (const auto& [chain, depth] : expected_depths) {
        const auto params{CreateChainParams(ArgsManager{}, chain)};
        const int tip{params->GetConsensus().V20Height + 10'000};
        BOOST_CHECK_EQUAL(GetEvoPruneLockHeight(*params, tip), tip - depth);
    }
}

BOOST_AUTO_TEST_CASE(evo_prune_lock_keeps_rotated_quorum_work_blocks)
{
    for (const auto& chain :
         {CBaseChainParams::MAIN, CBaseChainParams::TESTNET, CBaseChainParams::DEVNET, CBaseChainParams::REGTEST}) {
        const auto params{CreateChainParams(ArgsManager{}, chain)};
        const auto& consensus{params->GetConsensus()};
        for (const auto& llmq_params : consensus.llmqs) {
            if (!llmq_params.useRotation) continue;
            const int interval{llmq_params.dkgInterval};
            const int cycle_base{(consensus.V20Height / interval + 10) * interval};
            // The last block that can carry a commitment of the cycle connects on top of this tip
            const int tip{cycle_base + llmq_params.dkgMiningWindowEnd - 1};
            // Its members are built from the work blocks of the cycle and of the three previous cycles, and the
            // members of the oldest of these cycles were built from the work blocks of the three cycles before it
            const int oldest_work_block{cycle_base - 6 * interval - llmq::WORK_DIFF_DEPTH};
            BOOST_CHECK_LE(GetEvoPruneLockHeight(*params, tip), oldest_work_block);
        }
    }
}

BOOST_AUTO_TEST_CASE(evo_prune_lock_keeps_credit_pool_blocks)
{
    for (const auto& chain :
         {CBaseChainParams::MAIN, CBaseChainParams::TESTNET, CBaseChainParams::DEVNET, CBaseChainParams::REGTEST}) {
        const auto params{CreateChainParams(ArgsManager{}, chain)};
        const int period{CCreditPoolManager::DISK_SNAPSHOT_PERIOD};
        // A rebuild on a cold cache starts furthest back when the tip is at a disk snapshot height, as the snapshot of
        // the tip is only written once the next block connects
        const int tip{(params->GetConsensus().V20Height / period + 10) * period};
        // The first block after the previous snapshot reads the block CreditPoolPeriodBlocks before it
        const int oldest_read{tip - period + 1 - params->CreditPoolPeriodBlocks()};
        BOOST_CHECK_LE(GetEvoPruneLockHeight(*params, tip), oldest_read);
    }
}

struct RegTestV20At2000Setup : public BasicTestingSetup {
    RegTestV20At2000Setup() :
        BasicTestingSetup{CBaseChainParams::REGTEST, {"-testactivationheight=v20@2000", "-testactivationheight=mn_rr@2000"}}
    {
    }
};

BOOST_FIXTURE_TEST_CASE(evo_prune_lock_before_v20, RegTestV20At2000Setup)
{
    const CChainParams& params{Params()};
    BOOST_REQUIRE_EQUAL(params.GetConsensus().V20Height, 2000);
    // Nothing before the credit pool window of the first V20 block is read
    BOOST_CHECK_EQUAL(GetEvoPruneLockHeight(params, 1000), 2000 - 100);
    BOOST_CHECK_EQUAL(GetEvoPruneLockHeight(params, 2000 + 576), 2000 - 100);
    BOOST_CHECK_EQUAL(GetEvoPruneLockHeight(params, 2000 + 576 + 1), 2000 - 100 + 1);
    BOOST_CHECK_EQUAL(GetEvoPruneLockHeight(params, 5000), 5000 - (576 + 100));
}

BOOST_AUTO_TEST_SUITE_END()
