// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <active/masternode.h>
#include <bls/bls.h>
#include <coinjoin/coinjoin.h>
#include <coinjoin/common.h>
#include <consensus/amount.h>

#include <uint256.h>

#include <climits>
#include <cstdint>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(coinjoin_queue_tests, TestingSetup)

static CBLSSecretKey MakeSecretKey()
{
    // Generate a dummy operator key pair for signing
    CBLSSecretKey sk;
    sk.MakeNewKey();
    return sk;
}

BOOST_AUTO_TEST_CASE(queue_sign_and_verify)
{
    // Build active MN manager with operator key using node context wiring
    CActiveMasternodeManager mn_activeman(*Assert(m_node.connman), *Assert(m_node.dmnman), MakeSecretKey());

    CCoinJoinQueue q;
    q.nDenom = CoinJoin::AmountToDenomination(CoinJoin::GetSmallestDenomination());
    q.masternodeOutpoint = COutPoint(uint256S("aa"), 1);
    q.m_protxHash = uint256::ONE;
    q.nTime = GetAdjustedTime();
    q.fReady = false;

    // Sign and verify with corresponding pubkey
    q.vchSig = mn_activeman.SignBasic(q.GetSignatureHash());
    const CBLSPublicKey pub = mn_activeman.GetPubKey();
    BOOST_CHECK(q.CheckSignature(pub));
}

BOOST_AUTO_TEST_CASE(queue_hashes_and_equality)
{
    CCoinJoinQueue a, b;
    a.nDenom = b.nDenom = CoinJoin::AmountToDenomination(CoinJoin::GetSmallestDenomination());
    a.masternodeOutpoint = b.masternodeOutpoint = COutPoint(uint256S("bb"), 2);
    a.m_protxHash = b.m_protxHash = uint256::ONE;
    a.nTime = b.nTime = GetAdjustedTime();
    a.fReady = b.fReady = true;

    BOOST_CHECK(a == b);
    BOOST_CHECK(a.GetHash() == b.GetHash());
    BOOST_CHECK(a.GetSignatureHash() == b.GetSignatureHash());
}

BOOST_AUTO_TEST_CASE(queue_denomination_validation)
{
    // Test that valid denominations pass
    int validDenom = CoinJoin::AmountToDenomination(CoinJoin::GetSmallestDenomination());
    BOOST_CHECK(CoinJoin::IsValidDenomination(validDenom));

    // Test that invalid denominations fail
    BOOST_CHECK(!CoinJoin::IsValidDenomination(0));     // Zero
    BOOST_CHECK(!CoinJoin::IsValidDenomination(-1));    // Negative
    BOOST_CHECK(!CoinJoin::IsValidDenomination(999));   // Invalid value
}

BOOST_AUTO_TEST_CASE(queue_timestamp_validation)
{
    CCoinJoinQueue q;
    q.nDenom = CoinJoin::AmountToDenomination(CoinJoin::GetSmallestDenomination());
    q.masternodeOutpoint = COutPoint(uint256S("cc"), 3);
    q.m_protxHash = uint256::ONE;

    int64_t current_time = GetAdjustedTime();

    // Test valid timestamp (current time)
    q.nTime = current_time;
    BOOST_CHECK(!q.IsTimeOutOfBounds(current_time));

    // Test timestamp slightly in future (within COINJOIN_QUEUE_TIMEOUT = 30)
    q.nTime = current_time + 15; // 15 seconds in future
    BOOST_CHECK(!q.IsTimeOutOfBounds(current_time));

    // Test timestamp slightly in past (within COINJOIN_QUEUE_TIMEOUT = 30)
    q.nTime = current_time - 15; // 15 seconds ago
    BOOST_CHECK(!q.IsTimeOutOfBounds(current_time));

    // Test timestamp too far in future (outside COINJOIN_QUEUE_TIMEOUT = 30)
    q.nTime = current_time + 60; // 60 seconds in future
    BOOST_CHECK(q.IsTimeOutOfBounds(current_time));

    // Test timestamp too far in past (outside COINJOIN_QUEUE_TIMEOUT = 30)
    q.nTime = current_time - 60; // 60 seconds ago
    BOOST_CHECK(q.IsTimeOutOfBounds(current_time));
}

BOOST_AUTO_TEST_CASE(queue_timestamp_extreme_values)
{
    CCoinJoinQueue q;
    q.nDenom = CoinJoin::AmountToDenomination(CoinJoin::GetSmallestDenomination());
    q.m_protxHash = uint256::ONE;

    // Extreme deltas that would overflow with naive (current_time - nTime)
    q.nTime = INT64_MIN;
    BOOST_CHECK(q.IsTimeOutOfBounds(INT64_MAX));

    q.nTime = INT64_MAX;
    BOOST_CHECK(q.IsTimeOutOfBounds(INT64_MIN));

    // Same extreme time on both sides → difference is zero → in bounds
    q.nTime = INT64_MAX;
    BOOST_CHECK(!q.IsTimeOutOfBounds(INT64_MAX));

    q.nTime = INT64_MIN;
    BOOST_CHECK(!q.IsTimeOutOfBounds(INT64_MIN));

    // Zero vs extreme values → huge gap → out of bounds
    q.nTime = 0;
    BOOST_CHECK(q.IsTimeOutOfBounds(INT64_MAX));

    q.nTime = 0;
    BOOST_CHECK(q.IsTimeOutOfBounds(INT64_MIN));
}

static_assert(CoinJoin::CalculateAmountPriority(MAX_MONEY) == -(MAX_MONEY / COIN));
static_assert(CoinJoin::CalculateAmountPriority(static_cast<CAmount>(INT64_MAX)) == std::numeric_limits<int>::min());

BOOST_AUTO_TEST_CASE(calculate_amount_priority_clamp)
{
    // Realistic amount: MAX_MONEY (21 million DASH)
    BOOST_CHECK_EQUAL(CoinJoin::CalculateAmountPriority(MAX_MONEY), -(MAX_MONEY / COIN));

    // Extreme amount that would overflow int without clamping
    BOOST_CHECK_EQUAL(CoinJoin::CalculateAmountPriority(static_cast<CAmount>(INT64_MAX)), std::numeric_limits<int>::min());

    // Edge: exactly INT_MAX coins → -(INT_MAX) fits in int, no clamping needed
    BOOST_CHECK_EQUAL(CoinJoin::CalculateAmountPriority(static_cast<CAmount>(INT_MAX) * COIN), -INT_MAX);
}

BOOST_AUTO_TEST_SUITE_END()
