// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <masternode/payments.h>

#include <chain.h>
#include <evo/chainhelper.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <validation.h>

#include <test/util/setup_common.h>

#include <vector>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(masternode_payments_tests)

namespace {
CTxOut MakeOut(CAmount value, uint8_t script_byte)
{
    CScript script;
    script << OP_RETURN << std::vector<uint8_t>{script_byte};
    return CTxOut{value, script};
}
} // namespace

BOOST_AUTO_TEST_CASE(strict_matches_simple)
{
    const std::vector<CTxOut> expected{MakeOut(100, 0x01), MakeOut(200, 0x02)};
    const std::vector<CTxOut> actual{MakeOut(100, 0x01), MakeOut(200, 0x02), MakeOut(50, 0x03)};

    BOOST_CHECK_EQUAL(FindUnmatchedMasternodePayment(expected, actual, /*strict_multiplicity=*/true), -1);
    BOOST_CHECK_EQUAL(FindUnmatchedMasternodePayment(expected, actual, /*strict_multiplicity=*/false), -1);
}

BOOST_AUTO_TEST_CASE(strict_missing_output_fails)
{
    const std::vector<CTxOut> expected{MakeOut(100, 0x01), MakeOut(200, 0x02)};
    const std::vector<CTxOut> actual{MakeOut(100, 0x01)};

    BOOST_CHECK_EQUAL(FindUnmatchedMasternodePayment(expected, actual, /*strict_multiplicity=*/true), 1);
    BOOST_CHECK_EQUAL(FindUnmatchedMasternodePayment(expected, actual, /*strict_multiplicity=*/false), 1);
}

// Regression: with two identical expected masternode payments, strict multiplicity
// matching must require two identical coinbase outputs. A single output should
// NOT satisfy both expected outputs (which was the bug pre-v24).
BOOST_AUTO_TEST_CASE(strict_duplicate_expected_requires_duplicate_actual)
{
    const std::vector<CTxOut> expected{MakeOut(100, 0x01), MakeOut(100, 0x01)};

    // Only one matching actual output: strict must reject, legacy must accept.
    const std::vector<CTxOut> actual_one{MakeOut(100, 0x01), MakeOut(50, 0x02)};
    BOOST_CHECK_EQUAL(FindUnmatchedMasternodePayment(expected, actual_one, /*strict_multiplicity=*/true), 1);
    BOOST_CHECK_EQUAL(FindUnmatchedMasternodePayment(expected, actual_one, /*strict_multiplicity=*/false), -1);

    // Two matching actual outputs: both modes accept.
    const std::vector<CTxOut> actual_two{MakeOut(100, 0x01), MakeOut(100, 0x01), MakeOut(50, 0x02)};
    BOOST_CHECK_EQUAL(FindUnmatchedMasternodePayment(expected, actual_two, /*strict_multiplicity=*/true), -1);
    BOOST_CHECK_EQUAL(FindUnmatchedMasternodePayment(expected, actual_two, /*strict_multiplicity=*/false), -1);
}

// A masternode payment and a superblock payment with the same script and amount
// must be paid by two outputs; each category's own check would accept one.
BOOST_AUTO_TEST_CASE(required_payments_across_categories_need_distinct_outputs)
{
    const std::vector<CTxOut> masternode{MakeOut(50, 0x03), MakeOut(100, 0x01)};
    const std::vector<CTxOut> superblock{MakeOut(100, 0x01)};

    const std::vector<CTxOut> shared{MakeOut(50, 0x03), MakeOut(100, 0x01)};
    BOOST_CHECK_EQUAL(FindUnmatchedMasternodePayment(masternode, shared, /*strict_multiplicity=*/true), -1);
    BOOST_CHECK_EQUAL(FindUnmatchedMasternodePayment(superblock, shared, /*strict_multiplicity=*/true), -1);
    BOOST_CHECK(!HasDistinctRequiredPayments(masternode, superblock, shared));

    const std::vector<CTxOut> distinct{MakeOut(100, 0x01), MakeOut(50, 0x03), MakeOut(100, 0x01)};
    BOOST_CHECK(HasDistinctRequiredPayments(masternode, superblock, distinct));

    // Different amount or script is a different obligation
    BOOST_CHECK(HasDistinctRequiredPayments(masternode, {MakeOut(101, 0x01)},
                                            {MakeOut(50, 0x03), MakeOut(100, 0x01), MakeOut(101, 0x01)}));
    BOOST_CHECK(!HasDistinctRequiredPayments(masternode, {MakeOut(100, 0x02)}, shared));

    // No masternode payments (before DIP3 enforcement or unknown payee) leaves the superblock check alone
    BOOST_CHECK(HasDistinctRequiredPayments({}, superblock, shared));
}

// Pre-v24 path retains the old existence-only behaviour: a single actual output
// can satisfy any number of identical expected outputs.
BOOST_AUTO_TEST_CASE(legacy_existence_only_matches_duplicates)
{
    const std::vector<CTxOut> expected{MakeOut(100, 0x01), MakeOut(100, 0x01), MakeOut(100, 0x01)};
    const std::vector<CTxOut> actual{MakeOut(100, 0x01)};

    BOOST_CHECK_EQUAL(FindUnmatchedMasternodePayment(expected, actual, /*strict_multiplicity=*/false), -1);
    BOOST_CHECK_EQUAL(FindUnmatchedMasternodePayment(expected, actual, /*strict_multiplicity=*/true), 1);
}

BOOST_AUTO_TEST_CASE(empty_expected_is_trivially_matched)
{
    const std::vector<CTxOut> expected{};
    const std::vector<CTxOut> actual{MakeOut(100, 0x01)};
    BOOST_CHECK_EQUAL(FindUnmatchedMasternodePayment(expected, actual, /*strict_multiplicity=*/true), -1);
    BOOST_CHECK_EQUAL(FindUnmatchedMasternodePayment(expected, actual, /*strict_multiplicity=*/false), -1);
}

BOOST_AUTO_TEST_CASE(strict_amount_must_match_exactly)
{
    const std::vector<CTxOut> expected{MakeOut(100, 0x01)};
    const std::vector<CTxOut> actual{MakeOut(99, 0x01)};
    BOOST_CHECK_EQUAL(FindUnmatchedMasternodePayment(expected, actual, /*strict_multiplicity=*/true), 0);
    BOOST_CHECK_EQUAL(FindUnmatchedMasternodePayment(expected, actual, /*strict_multiplicity=*/false), 0);
}

// Regression: mainnet block 332320 sits inside an old-budget cycle window and pays
// out a budget on top of the block reward. The old budget data is long gone, so a
// node that is not synced yet (SuperBlockCheckType::NoCheck) has no way to validate
// such a block and must accept it, otherwise it can never sync past that height.
BOOST_FIXTURE_TEST_CASE(old_budget_window_accepted_while_unsynced, TestingSetup)
{
    const Consensus::Params& consensus{m_node.chainman->GetConsensus()};
    constexpr int nBlockHeight{332320};
    BOOST_REQUIRE(nBlockHeight >= consensus.nBudgetPaymentsStartBlock);
    BOOST_REQUIRE(nBlockHeight < consensus.nSuperblockStartBlock);
    BOOST_REQUIRE(nBlockHeight % consensus.nBudgetPaymentsCycleBlocks < consensus.nBudgetPaymentsWindowBlocks);

    CBlockIndex pindexPrev;
    pindexPrev.nHeight = nBlockHeight - 1;

    constexpr CAmount blockReward{508031847};
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vout = {MakeOut(blockReward, 0x01), MakeOut(117600000000, 0x02)};

    CBlock block;
    block.vtx.push_back(MakeTransactionRef(coinbase));

    auto& mn_payments{*Assert(m_node.chain_helper)->mn_payments};
    LOCK(cs_main);
    const CChain& active_chain{m_node.chainman->ActiveChain()};

    std::string strError;
    BOOST_CHECK(mn_payments.IsBlockValueValid(active_chain, block, &pindexPrev, blockReward, strError,
                                              SuperBlockCheckType::NoCheck));
    BOOST_CHECK(strError.empty());

    // The enforcing branch is only reached by a node that is synced and has no
    // chainlock at that height, and it still rejects the over-reward block.
    BOOST_CHECK(!mn_payments.IsBlockValueValid(active_chain, block, &pindexPrev, blockReward, strError,
                                               SuperBlockCheckType::AllowDuplicates));
    BOOST_CHECK_EQUAL(strError, "coinbase pays too much at height 332320 (actual=118108031847 vs limit=508031847), "
                                "exceeded block reward, old budgets are disabled");
}

BOOST_AUTO_TEST_SUITE_END()
