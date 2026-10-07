// Copyright (c) 2021-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/masternode.h>
#include <test/util/setup_common.h>

#include <bls/bls.h>
#include <evo/deterministicmns.h>
#include <masternode/payments.h>
#include <util/check.h>
#include <util/helpers.h>

#include <chainparams.h>
#include <deploymentstatus.h>
#include <node/miner.h>
#include <script/standard.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

using node::BlockAssembler;

struct TestChainBRRBeforeActivationSetup : public TestChainSetup
{
    // Force fast DIP3 activation
    TestChainBRRBeforeActivationSetup() :
        TestChainSetup(497, CBaseChainParams::REGTEST,
                       {"-dip3params=30:50", "-testactivationheight=brr@1000", "-testactivationheight=v20@1200", "-testactivationheight=mn_rr@2200"})
    {
    }
};

BOOST_AUTO_TEST_SUITE(block_reward_reallocation_tests)

BOOST_AUTO_TEST_CASE(simple_utxo_map_skips_unspendable_outputs)
{
    CMutableTransaction tx;
    tx.vout.emplace_back(/*nValueIn=*/1, CScript() << OP_TRUE);
    tx.vout.emplace_back(/*nValueIn=*/0, CScript() << OP_RETURN);
    const auto tx_ref = MakeTransactionRef(tx);

    const auto utxos = BuildSimpleUtxoMap({tx_ref});

    BOOST_REQUIRE_EQUAL(utxos.size(), 1);
    BOOST_CHECK(utxos.contains(COutPoint{tx_ref->GetHash(), 0}));
}

BOOST_FIXTURE_TEST_CASE(block_reward_reallocation, TestChainBRRBeforeActivationSetup)
{
    auto& dmnman = *Assert(m_node.dmnman);
    const auto& consensus_params = Params().GetConsensus();

    CScript coinbasePubKey = GetScriptForRawPubKey(coinbaseKey.GetPubKey());

    BOOST_REQUIRE(DeploymentDIP0003Enforced(WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height()),
                                            consensus_params));

    // Register one MN
    CKey ownerKey;
    CBLSSecretKey operatorKey;
    auto utxos = BuildSimpleUtxoMap(m_coinbase_txns);
    auto tx = CreateProRegTx(*m_node.chainman, utxos, 1, GenerateRandomAddress(), coinbaseKey, ownerKey, operatorKey);

    CreateAndProcessBlock({tx}, coinbasePubKey);

    {
        LOCK(cs_main);
        const CBlockIndex* const tip{m_node.chainman->ActiveChain().Tip()};
        dmnman.UpdatedBlockTip(tip);

        BOOST_REQUIRE(dmnman.GetListAtChainTip().HasMN(tx.GetHash()));

        BOOST_CHECK_EQUAL(tip->nHeight, 498);
        BOOST_CHECK(tip->nHeight < Params().GetConsensus().BRRHeight);
    }

    CreateAndProcessBlock({}, coinbasePubKey);

    {
        LOCK(cs_main);
        const CBlockIndex* const tip{m_node.chainman->ActiveChain().Tip()};
        BOOST_CHECK_EQUAL(tip->nHeight, 499);
        dmnman.UpdatedBlockTip(tip);
        BOOST_REQUIRE(dmnman.GetListAtChainTip().HasMN(tx.GetHash()));
        BOOST_CHECK(tip->nHeight < Params().GetConsensus().BRRHeight);
        // Creating blocks by different ways
        const auto pblocktemplate = BlockAssembler(m_node.chainman->ActiveChainstate(), m_node, m_node.mempool.get()).CreateNewBlock(coinbasePubKey);
    }
    for ([[maybe_unused]] auto _ : util::irange(499)) {
        CreateAndProcessBlock({}, coinbasePubKey);
        LOCK(cs_main);
        dmnman.UpdatedBlockTip(m_node.chainman->ActiveChain().Tip());
    }
    BOOST_CHECK(WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Height()) < Params().GetConsensus().BRRHeight);
    CreateAndProcessBlock({}, coinbasePubKey);

    {
        // Advance to ACTIVE at height = (BRRHeight - 1)
        LOCK(cs_main);
        const CBlockIndex* const tip{m_node.chainman->ActiveChain().Tip()};
        BOOST_CHECK_EQUAL(tip->nHeight, Params().GetConsensus().BRRHeight - 1);
        dmnman.UpdatedBlockTip(tip);
        BOOST_REQUIRE(dmnman.GetListAtChainTip().HasMN(tx.GetHash()));
    }

    {
        // Reward split should stay ~50/50 before the first superblock after activation.
        // This applies even if reallocation was activated right at superblock height like it does here.
        // next block should be signaling by default
        LOCK(cs_main);
        const CBlockIndex* const tip{Assert(m_node.chainman->ActiveChain().Tip())};
        const MnRewardEra era{GetMnRewardEraAfter(tip, *m_node.chainman)};
        const bool isV20Active{era != MnRewardEra::Classic};
        dmnman.UpdatedBlockTip(tip);
        BOOST_REQUIRE(dmnman.GetListAtChainTip().HasMN(tx.GetHash()));
        const CAmount block_subsidy = GetBlockSubsidyInner(tip->nBits, tip->nHeight, consensus_params, isV20Active);
        const CAmount masternode_payment = GetMasternodePayment(tip->nHeight, block_subsidy, consensus_params, era);
        const auto pblocktemplate = BlockAssembler(m_node.chainman->ActiveChainstate(), m_node, m_node.mempool.get()).CreateNewBlock(coinbasePubKey);
        BOOST_CHECK_EQUAL(pblocktemplate->voutMasternodePayments[0].nValue, masternode_payment);
    }

    for ([[maybe_unused]] auto _ : util::irange(consensus_params.nSuperblockCycle - 1)) {
        CreateAndProcessBlock({}, coinbasePubKey);
    }

    {
        LOCK(cs_main);
        const CBlockIndex* const tip{Assert(m_node.chainman->ActiveChain().Tip())};
        const MnRewardEra era{GetMnRewardEraAfter(tip, *m_node.chainman)};
        const bool isV20Active{era != MnRewardEra::Classic};
        const CAmount block_subsidy = GetBlockSubsidyInner(tip->nBits, tip->nHeight, consensus_params, isV20Active);
        const CAmount masternode_payment = GetMasternodePayment(tip->nHeight, block_subsidy, consensus_params, era);
        const auto pblocktemplate = BlockAssembler(m_node.chainman->ActiveChainstate(), m_node, m_node.mempool.get()).CreateNewBlock(coinbasePubKey);
        BOOST_CHECK_EQUAL(pblocktemplate->block.vtx[0]->GetValueOut(), 28847249686);
        BOOST_CHECK_EQUAL(pblocktemplate->voutMasternodePayments[0].nValue, masternode_payment);
        BOOST_CHECK_EQUAL(pblocktemplate->voutMasternodePayments[0].nValue, 14423624841); // 0.4999999999
    }

    // Reallocation should kick-in with the superblock after 19 adjustments, 3 superblocks long each
    for ([[maybe_unused]] auto i : util::irange(19)) {
        for ([[maybe_unused]] auto j : util::irange(3)) {
            for ([[maybe_unused]] auto k : util::irange(consensus_params.nSuperblockCycle)) {
                CreateAndProcessBlock({}, coinbasePubKey);
            }
            LOCK(cs_main);
            const CBlockIndex* const tip{Assert(m_node.chainman->ActiveChain().Tip())};
            const MnRewardEra era{GetMnRewardEraAfter(tip, *m_node.chainman)};
            const bool isV20Active{era != MnRewardEra::Classic};
            const CAmount block_subsidy = GetBlockSubsidyInner(tip->nBits, tip->nHeight, consensus_params, isV20Active);
            const CAmount masternode_payment = GetMasternodePayment(tip->nHeight, block_subsidy, consensus_params, era);
            const auto pblocktemplate = BlockAssembler(m_node.chainman->ActiveChainstate(), m_node, m_node.mempool.get()).CreateNewBlock(coinbasePubKey);
            BOOST_CHECK_EQUAL(pblocktemplate->voutMasternodePayments[0].nValue, masternode_payment);
        }
    }
    BOOST_CHECK(WITH_LOCK(::cs_main, return DeploymentActiveAfter(m_node.chainman->ActiveChain().Tip(), consensus_params, Consensus::DEPLOYMENT_V20)));
    // Allocation of block subsidy is 60% MN, 20% miners and 20% treasury
    {
        // Reward split should reach ~75/25 after reallocation is done
        LOCK(cs_main);
        const CBlockIndex* const tip{Assert(m_node.chainman->ActiveChain().Tip())};
        const MnRewardEra era{GetMnRewardEraAfter(tip, *m_node.chainman)};
        const bool isV20Active{era != MnRewardEra::Classic};
        const CAmount block_subsidy = GetBlockSubsidyInner(tip->nBits, tip->nHeight, consensus_params, isV20Active);
        const CAmount block_subsidy_sb = GetSuperblockSubsidyInner(tip->nBits, tip->nHeight, consensus_params, isV20Active);
        CAmount block_subsidy_potential = block_subsidy + block_subsidy_sb;
        BOOST_CHECK_EQUAL(block_subsidy_potential, 177167660);
        CAmount expected_block_reward = block_subsidy_potential - block_subsidy_potential / 5;

        const CAmount masternode_payment = GetMasternodePayment(tip->nHeight, block_subsidy, consensus_params, era);
        const auto pblocktemplate = BlockAssembler(m_node.chainman->ActiveChainstate(), m_node, m_node.mempool.get()).CreateNewBlock(coinbasePubKey);
        BOOST_CHECK_EQUAL(pblocktemplate->block.vtx[0]->GetValueOut(), expected_block_reward);
        BOOST_CHECK_EQUAL(pblocktemplate->block.vtx[0]->GetValueOut(), 141734128);
        BOOST_CHECK_EQUAL(pblocktemplate->voutMasternodePayments[0].nValue, masternode_payment);
        BOOST_CHECK_EQUAL(pblocktemplate->voutMasternodePayments[0].nValue, 106300596); // 0.75
    }
    BOOST_CHECK(WITH_LOCK(::cs_main, return !DeploymentActiveAfter(m_node.chainman->ActiveChain().Tip(), consensus_params, Consensus::DEPLOYMENT_MN_RR)));

    // Reward split should stay ~75/25 after reallocation is done,
    // check 10 next superblocks
    for ([[maybe_unused]] auto i : util::irange(10)) {
        for ([[maybe_unused]] auto k : util::irange(consensus_params.nSuperblockCycle)) {
            CreateAndProcessBlock({}, coinbasePubKey);
        }
        LOCK(cs_main);
        const CBlockIndex* const tip{Assert(m_node.chainman->ActiveChain().Tip())};
        const MnRewardEra era{GetMnRewardEraAfter(tip, *m_node.chainman)};
        const bool isV20Active{era != MnRewardEra::Classic};
        const bool isMNRewardReallocated{era == MnRewardEra::EvoReward};
        const CAmount block_subsidy = GetBlockSubsidyInner(tip->nBits, tip->nHeight, consensus_params, isV20Active);
        CAmount masternode_payment = GetMasternodePayment(tip->nHeight, block_subsidy, consensus_params, era);
        const auto pblocktemplate = BlockAssembler(m_node.chainman->ActiveChainstate(), m_node, m_node.mempool.get()).CreateNewBlock(coinbasePubKey);

        if (isMNRewardReallocated) {
            const CAmount platform_payment = PlatformShare(masternode_payment);
            masternode_payment -= platform_payment;
        }
        size_t payment_index = isMNRewardReallocated ? 1 : 0;

        BOOST_CHECK_EQUAL(pblocktemplate->voutMasternodePayments[payment_index].nValue, masternode_payment);
    }

    BOOST_CHECK(WITH_LOCK(::cs_main, return DeploymentActiveAfter(m_node.chainman->ActiveChain().Tip(), consensus_params, Consensus::DEPLOYMENT_MN_RR)));
    { // At this moment Masternode reward should be reallocated to platform
        // Allocation of block subsidy is 60% MN, 20% miners and 20% treasury
        LOCK(cs_main);
        const CBlockIndex* const tip{Assert(m_node.chainman->ActiveChain().Tip())};
        const MnRewardEra era{GetMnRewardEraAfter(tip, *m_node.chainman)};
        const bool isV20Active{era != MnRewardEra::Classic};
        const CAmount block_subsidy = GetBlockSubsidyInner(tip->nBits, tip->nHeight, consensus_params, isV20Active);
        const CAmount block_subsidy_sb = GetSuperblockSubsidyInner(tip->nBits, tip->nHeight, consensus_params, isV20Active);
        CAmount masternode_payment = GetMasternodePayment(tip->nHeight, block_subsidy, consensus_params, era);
        const CAmount platform_payment = PlatformShare(masternode_payment);
        masternode_payment -= platform_payment;
        const auto pblocktemplate = BlockAssembler(m_node.chainman->ActiveChainstate(), m_node, m_node.mempool.get()).CreateNewBlock(coinbasePubKey);

        CAmount block_subsidy_potential = block_subsidy + block_subsidy_sb;
        BOOST_CHECK_EQUAL(tip->nHeight, 2358);
        BOOST_CHECK_EQUAL(block_subsidy_potential, 164512828);
        // Treasury is 20% since MNRewardReallocation
        CAmount expected_block_reward = block_subsidy_potential - block_subsidy_potential / 5;
        // Since MNRewardReallocation, MN reward share is 75% of the block reward
        CAmount expected_masternode_reward = expected_block_reward * 3 / 4;
        CAmount expected_mn_platform_payment = PlatformShare(expected_masternode_reward);
        CAmount expected_mn_core_payment = expected_masternode_reward - expected_mn_platform_payment;

        BOOST_CHECK_EQUAL(pblocktemplate->block.vtx[0]->GetValueOut(), expected_block_reward);
        BOOST_CHECK_EQUAL(pblocktemplate->voutMasternodePayments[1].nValue, masternode_payment);
        BOOST_CHECK_EQUAL(pblocktemplate->voutMasternodePayments[1].nValue, expected_mn_core_payment);
    }
}

BOOST_AUTO_TEST_SUITE_END()
