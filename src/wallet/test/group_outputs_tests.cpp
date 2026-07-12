// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <wallet/coinselection.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>

namespace wallet {
BOOST_FIXTURE_TEST_SUITE(group_outputs_tests, TestingSetup)

static int nextLockTime = 0;

static std::shared_ptr<CWallet> NewWallet(const node::NodeContext& m_node, const ArgsManager& args)
{
    std::unique_ptr<CWallet> wallet = std::make_unique<CWallet>(m_node.chain.get(), /*coinjoin_loader=*/nullptr, "", args, CreateMockWalletDatabase());
    wallet->LoadWallet();
    LOCK(wallet->cs_wallet);
    wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
    wallet->SetupDescriptorScriptPubKeyMans("", "");
    return wallet;
}

// Dash: only OutputType::LEGACY is supported, so coins always land in CoinsResult::legacy
// (there is no per-OutputType map to key into, unlike upstream).
static void addCoin(CoinsResult& coins,
                     CWallet& wallet,
                     const CTxDestination& dest,
                     const CAmount& nValue,
                     bool is_from_me,
                     CFeeRate fee_rate = CFeeRate(0),
                     int depth = 6)
{
    CMutableTransaction tx;
    tx.nLockTime = nextLockTime++;        // so all transactions get different hashes
    tx.vout.resize(1);
    tx.vout[0].nValue = nValue;
    tx.vout[0].scriptPubKey = GetScriptForDestination(dest);

    const uint256& txid = tx.GetHash();
    LOCK(wallet.cs_wallet);
    auto ret = wallet.mapWallet.emplace(std::piecewise_construct, std::forward_as_tuple(txid), std::forward_as_tuple(MakeTransactionRef(std::move(tx)), TxStateInactive{}));
    assert(ret.second);
    CWalletTx& wtx = (*ret.first).second;
    const auto& txout = wtx.tx->vout.at(0);
    coins.legacy.emplace_back(COutPoint(wtx.GetHash(), 0),
                               txout,
                               depth,
                               CalculateMaximumSignedInputSize(txout, &wallet, /*coin_control=*/nullptr),
                               /*spendable=*/ true,
                               /*solvable=*/ true,
                               /*safe=*/ true,
                               wtx.GetTxTime(),
                               is_from_me,
                               fee_rate);
}

 CoinSelectionParams makeSelectionParams(FastRandomContext& rand, bool avoid_partial_spends)
{
    return CoinSelectionParams{
            rand,
            /*change_output_size=*/ 0,
            /*change_spend_size=*/ 0,
            /*min_change_target=*/ CENT,
            /*effective_feerate=*/ CFeeRate(0),
            /*long_term_feerate=*/ CFeeRate(0),
            /*discard_feerate=*/ CFeeRate(0),
            /*tx_noinputs_size=*/ 0,
            /*avoid_partial=*/ avoid_partial_spends,
    };
}

class GroupVerifier
{
public:
    std::shared_ptr<CWallet> wallet{nullptr};
    CoinsResult coins_pool;
    FastRandomContext rand;

    void GroupVerify(const CoinEligibilityFilter& filter,
                     bool avoid_partial_spends,
                     bool positive_only,
                     int expected_size)
    {
        Groups groups = GroupOutputs(*wallet,
                                     coins_pool.all(),
                                     makeSelectionParams(rand, avoid_partial_spends),
                                     filter);
        const std::vector<OutputGroup>& groups_out = positive_only ? groups.positive_group : groups.mixed_group;
        BOOST_CHECK_EQUAL(groups_out.size(), expected_size);
    }

    void GroupAndVerify(const CoinEligibilityFilter& filter,
                        int expected_with_partial_spends_size,
                        int expected_without_partial_spends_size,
                        bool positive_only)
    {
        // First avoid partial spends
        GroupVerify(filter, /*avoid_partial_spends=*/false, positive_only,  expected_with_partial_spends_size);
        // Second don't avoid partial spends
        GroupVerify(filter, /*avoid_partial_spends=*/true, positive_only, expected_without_partial_spends_size);
    }
};

BOOST_AUTO_TEST_CASE(outputs_grouping_tests)
{
    const auto& wallet = NewWallet(m_node, m_args);
    GroupVerifier group_verifier;
    group_verifier.wallet = wallet;

    const CoinEligibilityFilter& BASIC_FILTER{1, 6, 0};

    // #################################################################################
    // 10 outputs from different txs going to the same script
    // 1) if partial spends is enabled --> must not be grouped
    // 2) if partial spends is not enabled --> must be grouped into a single OutputGroup
    // #################################################################################

    unsigned long GROUP_SIZE = 10;
    const CTxDestination dest = *Assert(wallet->GetNewDestination(""));
    for (unsigned long i = 0; i < GROUP_SIZE; i++) {
        addCoin(group_verifier.coins_pool, *wallet, dest, 10 * COIN, /*is_from_me=*/true);
    }

    group_verifier.GroupAndVerify(BASIC_FILTER,
                                  /*expected_with_partial_spends_size=*/ GROUP_SIZE,
                                  /*expected_without_partial_spends_size=*/ 1,
                                  /*positive_only=*/ true);

    // ####################################################################################
    // 3) 10 more UTXO are added with a different script --> must be grouped into a single
    //    group for avoid partial spends and 10 different output groups for partial spends
    // ####################################################################################

    const CTxDestination dest2 = *Assert(wallet->GetNewDestination(""));
    for (unsigned long i = 0; i < GROUP_SIZE; i++) {
        addCoin(group_verifier.coins_pool, *wallet, dest2, 5 * COIN, /*is_from_me=*/true);
    }

    group_verifier.GroupAndVerify(BASIC_FILTER,
            /*expected_with_partial_spends_size=*/ GROUP_SIZE * 2,
            /*expected_without_partial_spends_size=*/ 2,
            /*positive_only=*/ true);

    // ################################################################################
    // 4) Now add a negative output --> which will be skipped if "positive_only" is set
    // ################################################################################

    const CTxDestination dest3 = *Assert(wallet->GetNewDestination(""));
    addCoin(group_verifier.coins_pool, *wallet, dest3, 1, true, CFeeRate(100));
    BOOST_CHECK(group_verifier.coins_pool.legacy.back().GetEffectiveValue() <= 0);

    // First expect no changes with "positive_only" enabled
    group_verifier.GroupAndVerify(BASIC_FILTER,
            /*expected_with_partial_spends_size=*/ GROUP_SIZE * 2,
            /*expected_without_partial_spends_size=*/ 2,
            /*positive_only=*/ true);

    // Then expect changes with "positive_only" disabled
    group_verifier.GroupAndVerify(BASIC_FILTER,
            /*expected_with_partial_spends_size=*/ GROUP_SIZE * 2 + 1,
            /*expected_without_partial_spends_size=*/ 3,
            /*positive_only=*/ false);


    // ##############################################################################
    // 5) Try to add a non-eligible UTXO (due not fulfilling the min depth target for
    //    "not mine" UTXOs) --> it must not be added to any group
    // ##############################################################################

    const CTxDestination dest4 = *Assert(wallet->GetNewDestination(""));
    addCoin(group_verifier.coins_pool, *wallet, dest4, 6 * COIN,
            /*is_from_me=*/false, CFeeRate(0), /*depth=*/5);

    // Expect no changes from this round and the previous one (point 4)
    group_verifier.GroupAndVerify(BASIC_FILTER,
            /*expected_with_partial_spends_size=*/ GROUP_SIZE * 2 + 1,
            /*expected_without_partial_spends_size=*/ 3,
            /*positive_only=*/ false);


    // ##############################################################################
    // 6) Try to add a non-eligible UTXO (due not fulfilling the min depth target for
    //    "mine" UTXOs) --> it must not be added to any group
    // ##############################################################################

    const CTxDestination dest5 = *Assert(wallet->GetNewDestination(""));
    addCoin(group_verifier.coins_pool, *wallet, dest5, 6 * COIN,
            /*is_from_me=*/true, CFeeRate(0), /*depth=*/0);

    // Expect no changes from this round and the previous one (point 5)
    group_verifier.GroupAndVerify(BASIC_FILTER,
            /*expected_with_partial_spends_size=*/ GROUP_SIZE * 2 + 1,
            /*expected_without_partial_spends_size=*/ 3,
            /*positive_only=*/ false);

    // ###########################################################################################
    // 7) Surpass the OUTPUT_GROUP_MAX_ENTRIES and verify that a second partial group gets created
    // ###########################################################################################

    const CTxDestination dest7 = *Assert(wallet->GetNewDestination(""));
    uint16_t NUM_SINGLE_ENTRIES = 101;
    for (unsigned long i = 0; i < NUM_SINGLE_ENTRIES; i++) { // OUTPUT_GROUP_MAX_ENTRIES{100}
        addCoin(group_verifier.coins_pool, *wallet, dest7, 9 * COIN, /*is_from_me=*/true);
    }

    // Exclude partial groups only adds one more group to the previous test case (point 6)
    int PREVIOUS_ROUND_COUNT = GROUP_SIZE * 2 + 1;
    group_verifier.GroupAndVerify(BASIC_FILTER,
            /*expected_with_partial_spends_size=*/ PREVIOUS_ROUND_COUNT + NUM_SINGLE_ENTRIES,
            /*expected_without_partial_spends_size=*/ 4,
            /*positive_only=*/ false);

    // Include partial groups should add one more group inside the "avoid partial spends" count
    const CoinEligibilityFilter& avoid_partial_groups_filter{1, 6, 0, 0, /*include_partial=*/ true};
    group_verifier.GroupAndVerify(avoid_partial_groups_filter,
            /*expected_with_partial_spends_size=*/ PREVIOUS_ROUND_COUNT + NUM_SINGLE_ENTRIES,
            /*expected_without_partial_spends_size=*/ 5,
            /*positive_only=*/ false);
}

// Verifies the filter-list GroupOutputs overload (used by AutomaticCoinSelection to compute
// groups for every ordered eligibility filter once, ahead of time, instead of once per filter
// inside AttemptSelection): for a mixed pool of eligible and ineligible coins, grouping several
// filters together in one pass must produce, for each filter, the exact same positive/mixed
// groups that calling the single-filter overload with that filter alone would produce.
BOOST_AUTO_TEST_CASE(filtered_output_groups_match_per_filter_calls)
{
    const auto& wallet = NewWallet(m_node, m_args);
    CoinsResult coins_pool;
    FastRandomContext rand;

    // Coins spanning a mix of confirmation depths, so different filters below select different
    // subsets of them.
    const CTxDestination dest = *Assert(wallet->GetNewDestination(""));
    for (int depth : {0, 1, 2, 6, 10}) {
        addCoin(coins_pool, *wallet, dest, 10 * COIN, /*is_from_me=*/true, CFeeRate(0), depth);
    }
    // A handful of externally-received (not from us) coins, only eligible under looser filters.
    const CTxDestination dest2 = *Assert(wallet->GetNewDestination(""));
    for (int depth : {1, 6}) {
        addCoin(coins_pool, *wallet, dest2, 3 * COIN, /*is_from_me=*/false, CFeeRate(0), depth);
    }

    // Mirrors the shape (not the exact values) of AutomaticCoinSelection's ordered_filters walk:
    // a strict filter first, progressively looser ones after.
    const std::vector<SelectionFilter> filters{
        {CoinEligibilityFilter(1, 6, 0), /*allow_mixed_output_types=*/false},
        {CoinEligibilityFilter(1, 1, 0)},
        {CoinEligibilityFilter(0, 1, 2)},
        {CoinEligibilityFilter(0, 1, std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint64_t>::max(), /*include_partial=*/true)},
    };

    for (bool avoid_partial_spends : {false, true}) {
        const CoinSelectionParams params = makeSelectionParams(rand, avoid_partial_spends);

        const FilteredOutputGroups batched = GroupOutputs(*wallet, coins_pool.legacy, params, filters);

        for (const auto& sel_filter : filters) {
            const Groups single = GroupOutputs(*wallet, coins_pool.legacy, params, sel_filter.filter);

            auto it = batched.find(sel_filter.filter);
            const bool has_batched_entry = it != batched.end();
            // A filter with no eligible coins simply has no entry in the batched map; the
            // single-filter call correctly returns an empty Groups for the same filter.
            BOOST_CHECK_EQUAL(has_batched_entry, !(single.positive_group.empty() && single.mixed_group.empty()));
            if (!has_batched_entry) continue;

            BOOST_CHECK_EQUAL(it->second.positive_group.size(), single.positive_group.size());
            BOOST_CHECK_EQUAL(it->second.mixed_group.size(), single.mixed_group.size());
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
} // end namespace wallet
