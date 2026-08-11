// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <policy/fees.h>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <wallet/spend.h>
#include <wallet/test/util.h>
#include <wallet/test/wallet_test_fixture.h>

#include <boost/test/unit_test.hpp>

namespace wallet {
BOOST_FIXTURE_TEST_SUITE(spend_tests, WalletTestingSetup)

BOOST_FIXTURE_TEST_CASE(SubtractFee, TestChain100Setup)
{
    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    auto wallet = CreateSyncedWallet(*m_node.chain, *m_node.coinjoin_loader, *Assert(m_node.chainman), m_args, coinbaseKey);

    // Check that a subtract-from-recipient transaction slightly less than the
    // coinbase input amount does not create a change output (because it would
    // be uneconomical to add and spend the output), and make sure it pays the
    // leftover input amount which would have been change to the recipient
    // instead of the miner.
    auto check_tx = [&wallet](CAmount leftover_input_amount) {
        CRecipient recipient{GetScriptForRawPubKey({}), 500 * COIN - leftover_input_amount, true /* subtract fee */};
        bilingual_str error;
        CCoinControl coin_control;
        coin_control.m_feerate.emplace(10000);
        coin_control.fOverrideFeeRate = true;
        auto res = CreateTransaction(*wallet, {recipient}, RANDOM_CHANGE_POSITION, coin_control);
        BOOST_CHECK(res);
        const auto& txr = *res;
        BOOST_CHECK_EQUAL(txr.tx->vout.size(), 1);
        BOOST_CHECK_EQUAL(txr.tx->vout[0].nValue, recipient.nAmount + leftover_input_amount - txr.fee);
        BOOST_CHECK_GT(txr.fee, 0);
        return txr.fee;
    };

    // Send full input amount to recipient, check that only nonzero fee is
    // subtracted (to_reduce == fee).
    const CAmount fee{check_tx(0)};

    // Send slightly less than full input amount to recipient, check leftover
    // input amount is paid to recipient not the miner (to_reduce == fee - 123)
    BOOST_CHECK_EQUAL(fee, check_tx(123));

    // Send full input minus fee amount to recipient, check leftover input
    // amount is paid to recipient not the miner (to_reduce == 0)
    BOOST_CHECK_EQUAL(fee, check_tx(fee));

    // Send full input minus more than the fee amount to recipient, check
    // leftover input amount is paid to recipient not the miner (to_reduce ==
    // -123). This overpays the recipient instead of overpaying the miner more
    // than double the neccesary fee.
    BOOST_CHECK_EQUAL(fee, check_tx(fee + 123));
}

// Regression test for a Dash-specific bug: CreateTransactionInternal() estimates the
// non-input part of the transaction size (used to size coin selection's target and, via
// SelectionResult::m_target, the change amount) assuming the vin-count CompactSize prefix
// is always 1 byte. Once the final input count reaches 253 or more, the real prefix is 3
// bytes, so the fee actually collected falls a couple of duffs short of the fee needed for
// the accurately-measured final size, and CreateTransactionInternal() used to bail out with
// "Fee needed > fee paid" (see spend.cpp's tx_noinputs_size and the STR_INTERNAL_BUG check).
//
// Funds a wallet with `input_count` confirmed CENT UTXOs, then requires all of them as
// inputs (via CCoinControl::fRequireAllInputs) to a transaction that leaves enough value
// behind for a change output, and checks that transaction creation succeeds.
//
// UTXOs are funded in chunks of at most 100 per transaction (well under the CompactSize
// boundary being tested) so that setup itself never has to cross a CompactSize boundary on
// the *output* side, which would otherwise obscure the input-count regression under test.
static void CheckManyRequiredInputs(TestChain100Setup& setup, size_t input_count)
{
    setup.CreateAndProcessBlock({}, GetScriptForRawPubKey(setup.coinbaseKey.GetPubKey()));
    auto wallet = CreateSyncedWallet(*setup.m_node.chain, *setup.m_node.coinjoin_loader, *Assert(setup.m_node.chainman), setup.m_args, setup.coinbaseKey);

    std::vector<COutPoint> outpoints;
    outpoints.reserve(input_count);
    static constexpr size_t CHUNK_SIZE{100};
    for (size_t funded = 0; funded < input_count; funded += CHUNK_SIZE) {
        const size_t chunk = std::min(CHUNK_SIZE, input_count - funded);

        CCoinControl fund_control;
        fund_control.m_feerate.emplace(10000);
        fund_control.fOverrideFeeRate = true;
        std::vector<CRecipient> recipients;
        recipients.reserve(chunk);
        for (size_t i = 0; i < chunk; ++i) {
            recipients.push_back({GetScriptForDestination(getNewDestination(*wallet)), CENT, /*subtract fee=*/false});
        }
        auto fund_res = CreateTransaction(*wallet, recipients, RANDOM_CHANGE_POSITION, fund_control);
        BOOST_REQUIRE(fund_res);
        const CTransactionRef fund_tx = fund_res->tx;

        // Confirm the funding transaction, mirroring CreateTransactionTestSetup::GetCoins()
        // in wallet_tests.cpp: CreateSyncedWallet() does not register the wallet for chain
        // notifications, so the confirmation has to be reflected into mapWallet manually.
        wallet->CommitTransaction(fund_tx, {}, {});
        setup.CreateAndProcessBlock({CMutableTransaction(*fund_tx)}, GetScriptForRawPubKey(setup.coinbaseKey.GetPubKey()));
        const CBlockIndex* tip{WITH_LOCK(Assert(setup.m_node.chainman)->GetMutex(), return setup.m_node.chainman->ActiveChain().Tip())};
        {
            LOCK(wallet->cs_wallet);
            wallet->SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
            auto it = wallet->mapWallet.find(fund_tx->GetHash());
            BOOST_REQUIRE(it != wallet->mapWallet.end());
            it->second.m_state = TxStateConfirmed{tip->GetBlockHash(), tip->nHeight, /*index=*/1};
        }

        for (size_t i = 0; i < chunk; ++i) outpoints.emplace_back(fund_tx->GetHash(), i);
    }

    CCoinControl coin_control;
    coin_control.m_feerate.emplace(10000);
    coin_control.fOverrideFeeRate = true;
    coin_control.fRequireAllInputs = true;
    coin_control.m_allow_other_inputs = false;
    for (const auto& outpoint : outpoints) coin_control.Select(outpoint);

    // Send most of the selected value to a single recipient, leaving one CENT of slack for
    // the fee and a change output (not subtracting the fee from the recipient, so the "Fee
    // needed > fee paid" internal-bug check applies).
    const CAmount total{CAmount(input_count) * CENT};
    CRecipient recipient{GetScriptForDestination(getNewDestination(*wallet)), total - CENT, /*subtract fee=*/false};
    auto res = CreateTransaction(*wallet, {recipient}, RANDOM_CHANGE_POSITION, coin_control);
    BOOST_CHECK(res);
}

// Below the CompactSize boundary: the vin-count prefix really is 1 byte, so this must always
// succeed, with or without the fix.
BOOST_FIXTURE_TEST_CASE(CompactSizeInputCountBelowBoundary, TestChain100Setup)
{
    CheckManyRequiredInputs(*this, 252);
}

// At the CompactSize boundary: 253 inputs need a 3-byte vin-count prefix. This is the case
// that used to fail with "Fee needed > fee paid".
BOOST_FIXTURE_TEST_CASE(CompactSizeInputCountAtBoundary, TestChain100Setup)
{
    CheckManyRequiredInputs(*this, 253);
}

static void TestFillInputToWeight(int64_t additional_weight, int64_t expected_scriptsig_size)
{
    static const int64_t EMPTY_INPUT_WEIGHT = ::GetSerializeSize(CTxIn());

    CTxIn input;
    int64_t target_weight = EMPTY_INPUT_WEIGHT + additional_weight;
    BOOST_CHECK(FillInputToWeight(input, target_weight));

    BOOST_CHECK_LE(::GetSerializeSize(input), target_weight);
    BOOST_CHECK_GE(::GetSerializeSize(input), target_weight - 2);

    BOOST_CHECK_EQUAL(input.scriptSig.size(), expected_scriptsig_size);
}

BOOST_FIXTURE_TEST_CASE(FillInputToWeightTest, BasicTestingSetup)
{
    {
        // Less than or equal minimum of 41 should not add any witness data
        CTxIn input;
        BOOST_CHECK(!FillInputToWeight(input, -1));
        BOOST_CHECK_EQUAL(::GetSerializeSize(input), 41);
        BOOST_CHECK(!FillInputToWeight(input, 0));
        BOOST_CHECK_EQUAL(::GetSerializeSize(input), 41);
        BOOST_CHECK(!FillInputToWeight(input, 40));
        BOOST_CHECK_EQUAL(::GetSerializeSize(input), 41);
        BOOST_CHECK(FillInputToWeight(input, 41));
        BOOST_CHECK_EQUAL(::GetSerializeSize(input), 41);
    }

    // Make sure we can add at least one weight
    TestFillInputToWeight(1, 1);

    // 1 byte compact size uint boundary
    TestFillInputToWeight(252, 252);
    TestFillInputToWeight(253, 251);
    TestFillInputToWeight(262, 260);
    TestFillInputToWeight(263, 261);

    // 3 byte compact size uint boundary
    TestFillInputToWeight(65535, 65533);
    TestFillInputToWeight(65536, 65532);
    TestFillInputToWeight(65545, 65541);
    TestFillInputToWeight(65546, 65542);

    // Note: We don't test the next boundary because of memory allocation constraints.
}

BOOST_FIXTURE_TEST_CASE(wallet_duplicated_preset_inputs_test, TestChain100Setup)
{
    // Verify that the wallet's Coin Selection process does not include pre-selected inputs twice in a transaction.

    // Add 4 spendable UTXO, 500 DASH each, to the wallet (total balance 2000 DASH)
    for (int i = 0; i < 4; i++) CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    auto wallet = CreateSyncedWallet(*m_node.chain, *m_node.coinjoin_loader, *Assert(m_node.chainman), m_args, coinbaseKey);

    LOCK(wallet->cs_wallet);
    auto available_coins = AvailableCoins(*wallet);
    std::vector<COutput> coins = available_coins.all();
    // Preselect the first 3 UTXO (1500 DASH total)
    std::set<COutPoint> preset_inputs = {coins[0].outpoint, coins[1].outpoint, coins[2].outpoint};

    // Try to create a tx that spends more than what preset inputs + wallet selected inputs are covering for.
    // The wallet can cover up to 2000 DASH, and the tx target is 2990 DASH.
    std::vector<CRecipient> recipients = {{GetScriptForDestination(*Assert(wallet->GetNewDestination("dummy"))),
                                           /*nAmount=*/2990 * COIN, /*fSubtractFeeFromAmount=*/true}};
    CCoinControl coin_control;
    coin_control.m_allow_other_inputs = true;
    for (const auto& outpoint : preset_inputs) {
        coin_control.Select(outpoint);
    }

    // Attempt to send 2990 DASH from a wallet that only has 2000 DASH. The wallet should exclude
    // the preset inputs from the pool of available coins, realize that there is not enough
    // money to fund the 2990 DASH payment, and fail with "Insufficient funds".
    //
    // Even with SFFO, the wallet can only afford to send 2000 DASH.
    // If the wallet does not properly exclude preset inputs from the pool of available coins
    // prior to coin selection, it may create a transaction that does not fund the full payment
    // amount or, through SFFO, incorrectly reduce the recipient's amount by the difference
    // between the original target and the wrongly counted inputs (in this case 990 DASH)
    // so that the recipient's amount is no longer equal to the user's selected target of 2990 DASH.

    // First case, use 'subtract_fee_from_outputs=true'
    util::Result<CreatedTransactionResult> res_tx = CreateTransaction(*wallet, recipients, /*change_pos=*/-1, coin_control);
    BOOST_CHECK(!res_tx.has_value());

    // Second case, don't use 'subtract_fee_from_outputs'.
    recipients[0].fSubtractFeeFromAmount = false;
    res_tx = CreateTransaction(*wallet, recipients, /*change_pos=*/-1, coin_control);
    BOOST_CHECK(!res_tx.has_value());
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
