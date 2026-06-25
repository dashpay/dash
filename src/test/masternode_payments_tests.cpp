// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <masternode/payments.h>

#include <primitives/transaction.h>
#include <script/script.h>

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

BOOST_AUTO_TEST_SUITE_END()
