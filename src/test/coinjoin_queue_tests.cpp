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

    // Negative timestamps are rejected by the guard
    q.nTime = INT64_MIN;
    BOOST_CHECK(q.IsTimeOutOfBounds(INT64_MAX));

    q.nTime = INT64_MAX;
    BOOST_CHECK(q.IsTimeOutOfBounds(INT64_MIN));

    q.nTime = INT64_MIN;
    BOOST_CHECK(q.IsTimeOutOfBounds(INT64_MIN));

    // Large positive timestamp with same value: zero diff, in bounds
    q.nTime = INT64_MAX;
    BOOST_CHECK(!q.IsTimeOutOfBounds(INT64_MAX));

    // Zero vs extreme positive: huge gap, out of bounds
    q.nTime = 0;
    BOOST_CHECK(q.IsTimeOutOfBounds(INT64_MAX));

    // Zero vs negative: rejected by guard
    q.nTime = 0;
    BOOST_CHECK(q.IsTimeOutOfBounds(INT64_MIN));
}

static_assert(CoinJoin::CalculateAmountPriority(MAX_MONEY) == -(MAX_MONEY / COIN));
static_assert(CoinJoin::CalculateAmountPriority(static_cast<CAmount>(INT64_MAX)) == 0);
static_assert(CoinJoin::CalculateAmountPriority(static_cast<CAmount>(-1)) == 0);

BOOST_AUTO_TEST_CASE(calculate_amount_priority_guard)
{
    // Realistic amount: MAX_MONEY (21 million DASH)
    BOOST_CHECK_EQUAL(CoinJoin::CalculateAmountPriority(MAX_MONEY), -(MAX_MONEY / COIN));

    // Out-of-range amounts return 0
    BOOST_CHECK_EQUAL(CoinJoin::CalculateAmountPriority(static_cast<CAmount>(INT64_MAX)), 0);
    BOOST_CHECK_EQUAL(CoinJoin::CalculateAmountPriority(static_cast<CAmount>(-1)), 0);
    BOOST_CHECK_EQUAL(CoinJoin::CalculateAmountPriority(MAX_MONEY + 1), 0);
}

static CCoinJoinQueue MakeQueue(int denom, int64_t nTime, bool fReady, const COutPoint& outpoint)
{
    CCoinJoinQueue q;
    q.nDenom = denom;
    q.masternodeOutpoint = outpoint;
    q.m_protxHash = uint256::ONE;
    q.nTime = nTime;
    q.fReady = fReady;
    return q;
}

BOOST_AUTO_TEST_CASE(queuemanager_checkqueue_removes_timeouts)
{
    CoinJoinQueueManager man;
    const int denom = CoinJoin::AmountToDenomination(CoinJoin::GetSmallestDenomination());
    const int64_t now = GetAdjustedTime();
    // Non-expired
    man.AddQueue(MakeQueue(denom, now, false, COutPoint(uint256S("11"), 0)));
    // Expired (too old)
    man.AddQueue(MakeQueue(denom, now - COINJOIN_QUEUE_TIMEOUT - 1, false, COutPoint(uint256S("12"), 0)));

    BOOST_CHECK_EQUAL(man.GetQueueSize(), 2);
    man.CheckQueue();
    // One should be removed
    BOOST_CHECK_EQUAL(man.GetQueueSize(), 1);
}

BOOST_AUTO_TEST_CASE(queuemanager_getqueueitem_marks_tried_once)
{
    CoinJoinQueueManager man;
    const int denom = CoinJoin::AmountToDenomination(CoinJoin::GetSmallestDenomination());
    const int64_t now = GetAdjustedTime();
    CCoinJoinQueue dsq = MakeQueue(denom, now, false, COutPoint(uint256S("21"), 0));
    man.AddQueue(dsq);

    CCoinJoinQueue picked;
    // First retrieval should succeed
    BOOST_CHECK(man.GetQueueItemAndTry(picked));
    // No other items left to try (picked is marked tried inside)
    CCoinJoinQueue picked2;
    BOOST_CHECK(!man.GetQueueItemAndTry(picked2));
}

BOOST_AUTO_TEST_SUITE_END()
