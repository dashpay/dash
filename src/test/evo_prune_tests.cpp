// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <chain.h>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <evo/creditpool.h>
#include <evo/specialtxman.h>
#include <llmq/snapshot.h>
#include <util/system.h>
#include <validation.h>

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

// Link blocks into a chain of block indexes from first_height on, all with data
static void MakeChain(std::vector<CBlockIndex>& blocks, int first_height) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    for (size_t i{0}; i < blocks.size(); ++i) {
        blocks[i].nHeight = first_height + static_cast<int>(i);
        blocks[i].pprev = i > 0 ? &blocks[i - 1] : nullptr;
        blocks[i].nStatus = BLOCK_HAVE_DATA;
    }
}

// The height of the block FindMissingEvoBlockData() reports for the last block when only the block at missing_height
// has no data, or -1 for none
static int FindMissingWith(const CChainParams& params, std::vector<CBlockIndex>& blocks, int missing_height)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    CBlockIndex& missing{blocks.at(missing_height - blocks.front().nHeight)};
    missing.nStatus &= ~BLOCK_HAVE_DATA;
    const CBlockIndex* found{FindMissingEvoBlockData(params, &blocks.back())};
    missing.nStatus |= BLOCK_HAVE_DATA;
    return found ? found->nHeight : -1;
}

BOOST_AUTO_TEST_CASE(evo_prune_find_missing_block_data)
{
    LOCK(cs_main);
    const auto params{CreateChainParams(ArgsManager{}, CBaseChainParams::MAIN)};
    const auto llmq_params{params->GetLLMQ(Consensus::LLMQType::LLMQ_60_75)};
    BOOST_REQUIRE(llmq_params && llmq_params->useRotation);
    const int interval{llmq_params->dkgInterval};
    const int period{CCreditPoolManager::DISK_SNAPSHOT_PERIOD};
    // A cycle of rotated quorums that starts at a credit pool snapshot height, with a commitment mined on top of tip
    const int cycle_base{(params->GetConsensus().V20Height / period + 10) * period};
    BOOST_REQUIRE_EQUAL(cycle_base % interval, 0);
    const int tip{cycle_base + 40};
    // Its members are built from the work block of the third cycle before it, which is older than any block that the
    // credit pool reads for the next block (from the snapshot of cycle_base on)
    const int work_block{cycle_base - 3 * interval - llmq::WORK_DIFF_DEPTH};
    BOOST_REQUIRE_LT(work_block, cycle_base + 1 - params->CreditPoolPeriodBlocks());

    const int height_first{GetEvoPruneLockHeight(*params, tip)};
    BOOST_CHECK_EQUAL(height_first, tip - (6 * 288 + 50 + 8));
    // The lock keeps more blocks below them for a reorg, which a rewound tip may have used up, so these aren't checked
    const int lock_first{GetEvoPruneLockHeightForTip(*params, tip)};
    BOOST_CHECK_EQUAL(lock_first, height_first - static_cast<int>(MIN_BLOCKS_TO_KEEP));
    std::vector<CBlockIndex> blocks(tip - (lock_first - 10) + 1);
    MakeChain(blocks, lock_first - 10);
    BOOST_CHECK(FindMissingEvoBlockData(*params, &blocks.back()) == nullptr);
    BOOST_CHECK_EQUAL(FindMissingWith(*params, blocks, work_block), work_block);
    BOOST_CHECK_EQUAL(FindMissingWith(*params, blocks, tip), tip);
    BOOST_CHECK_EQUAL(FindMissingWith(*params, blocks, height_first), height_first);
    BOOST_CHECK_EQUAL(FindMissingWith(*params, blocks, height_first - 1), -1);
    BOOST_CHECK_EQUAL(FindMissingWith(*params, blocks, lock_first), -1);
}

BOOST_AUTO_TEST_CASE(evo_prune_find_missing_block_data_before_v20)
{
    LOCK(cs_main);
    const auto params{CreateChainParams(ArgsManager{}, CBaseChainParams::REGTEST)};
    // Nothing before the credit pool window of the V20 activation block is kept or checked
    const int floor{params->GetConsensus().V20Height - params->CreditPoolPeriodBlocks()};
    std::vector<CBlockIndex> blocks(floor + 11);
    MakeChain(blocks, 0);
    BOOST_CHECK_EQUAL(FindMissingWith(*params, blocks, floor), floor);
    BOOST_CHECK_EQUAL(FindMissingWith(*params, blocks, floor - 1), -1);
    // A chain that ends below it isn't checked at all
    std::vector<CBlockIndex> short_blocks(floor);
    MakeChain(short_blocks, 0);
    BOOST_CHECK_EQUAL(FindMissingWith(*params, short_blocks, floor - 1), -1);
}

BOOST_AUTO_TEST_SUITE_END()
