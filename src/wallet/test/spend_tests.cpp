// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <policy/fees.h>
#include <test/util/wallet.h>
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
// inputs (via CCoinControl::fRequireAllInputs) and checks transaction creation with either a
// change output or an exact no-change target.
//
// UTXOs are funded in chunks of at most 100 per transaction (well under the CompactSize
// boundary being tested) so that setup itself never has to cross a CompactSize boundary on
// the *output* side, which would otherwise obscure the input-count regression under test.
static void CheckManyRequiredInputs(TestChain100Setup& setup, size_t input_count, bool exact_no_change = false)
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

        {
            LOCK(wallet->cs_wallet);
            for (size_t i = 0; i < chunk; ++i) {
                outpoints.emplace_back(fund_tx->GetHash(), i);
                wallet->LockCoin(outpoints.back());
            }
        }
    }

    CCoinControl coin_control;
    coin_control.m_feerate.emplace(10000);
    coin_control.fOverrideFeeRate = true;
    coin_control.fRequireAllInputs = true;
    coin_control.m_allow_other_inputs = false;
    for (const auto& outpoint : outpoints) coin_control.Select(outpoint);

    const CAmount total{CAmount(input_count) * CENT};
    const CScript recipient_script{GetScriptForDestination(getNewDestination(*wallet))};
    CAmount recipient_amount{total - CENT};
    if (exact_no_change) {
        CAmount selected_effective_value{0};
        {
            LOCK(wallet->cs_wallet);
            for (const auto& outpoint : outpoints) {
                const CWalletTx* wtx{wallet->GetWalletTx(outpoint.hash)};
                BOOST_REQUIRE(wtx);
                const CTxOut& txout{wtx->tx->vout.at(outpoint.n)};
                const int64_t input_size{CalculateMaximumSignedInputSize(txout, wallet.get(), &coin_control)};
                BOOST_REQUIRE_NE(input_size, -1);
                selected_effective_value += txout.nValue - coin_control.m_feerate->GetFee(input_size);
            }
        }
        const CTxOut recipient_out{/*nValueIn=*/0, recipient_script};
        const size_t noinputs_size{9 + GetSizeOfCompactSize(/*nSize=*/1) +
                                   GetSerializeSize(recipient_out) + GetSizeOfCompactSize(input_count) - 1};
        recipient_amount = selected_effective_value - coin_control.m_feerate->GetFee(noinputs_size);
    }

    CRecipient recipient{recipient_script, recipient_amount, /*subtract fee=*/false};
    auto res = CreateTransaction(*wallet, {recipient}, RANDOM_CHANGE_POSITION, coin_control);
    BOOST_REQUIRE(res);
    BOOST_CHECK_EQUAL(res->tx->vout.size(), exact_no_change ? 1 : 2);
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

// The exact-target path has no change output to absorb a fee shortfall, so coin selection must
// include the wider vin-count prefix in its target before finalizing the input set.
BOOST_FIXTURE_TEST_CASE(CompactSizeInputCountAtBoundaryNoChange, TestChain100Setup)
{
    CheckManyRequiredInputs(*this, 253, /*exact_no_change=*/true);
}

// Adding change to 252 recipient outputs crosses the vout-count CompactSize boundary. Coin
// selection must include the wider prefix in its target before finalizing the transaction.
BOOST_FIXTURE_TEST_CASE(CompactSizeOutputCountAtBoundary, TestChain100Setup)
{
    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    auto wallet = CreateSyncedWallet(*m_node.chain, *m_node.coinjoin_loader, *Assert(m_node.chainman), m_args, coinbaseKey);

    const CScript recipient_script{GetScriptForDestination(getNewDestination(*wallet))};
    const CRecipient recipient{recipient_script, CENT, /*subtract fee=*/false};
    const std::vector<CRecipient> recipients(/*count=*/252, recipient);

    CCoinControl coin_control;
    coin_control.m_feerate.emplace(10000);
    coin_control.fOverrideFeeRate = true;
    auto res = CreateTransaction(*wallet, recipients, RANDOM_CHANGE_POSITION, coin_control);
    BOOST_REQUIRE(res);
    BOOST_CHECK_EQUAL(res->tx->vout.size(), 253);
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

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
