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

static CCoinJoinQueue MakeQueue(int denom, NodeClock::time_point time, bool fReady, const COutPoint& outpoint)
{
    return CCoinJoinQueue{denom, outpoint, uint256::ONE, time, fReady};
}

BOOST_AUTO_TEST_CASE(queue_sign_and_verify)
{
    // Build active MN manager with operator key using node context wiring
    CActiveMasternodeManager mn_activeman(*Assert(m_node.connman), *Assert(m_node.dmnman), MakeSecretKey());

    auto q{MakeQueue(CoinJoin::AmountToDenomination(CoinJoin::GetSmallestDenomination()),
                     GetAdjustedTime(), /*fReady=*/false, COutPoint{uint256S("aa"), 1})};

    // Sign and verify with corresponding pubkey
    q.vchSig = mn_activeman.SignBasic(q.GetSignatureHash());
    const CBLSPublicKey pub = mn_activeman.GetPubKey();
    BOOST_CHECK(q.CheckSignature(pub));
}

BOOST_AUTO_TEST_CASE(queue_hashes_and_equality)
{
    const auto a{MakeQueue(CoinJoin::AmountToDenomination(CoinJoin::GetSmallestDenomination()),
                           GetAdjustedTime(), /*fReady=*/true, COutPoint{uint256S("bb"), 2})};
    const auto b{a}; // NOLINT(performance-unnecessary-copy-initialization): the copy is under test

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
    const int denom{CoinJoin::AmountToDenomination(CoinJoin::GetSmallestDenomination())};
    const COutPoint outpoint{uint256S("cc"), 3};
    const auto current_time{std::chrono::time_point_cast<std::chrono::seconds>(GetAdjustedTime())};

    // Test valid timestamp (current time)
    auto q{MakeQueue(denom, current_time, /*fReady=*/false, outpoint)};
    BOOST_CHECK(!q.IsTimeOutOfBounds(current_time));

    // Test timestamp slightly in future (within COINJOIN_QUEUE_TIMEOUT = 30)
    q = MakeQueue(denom, current_time + 15s, /*fReady=*/false, outpoint);
    BOOST_CHECK(!q.IsTimeOutOfBounds(current_time));

    // Test timestamp slightly in past (within COINJOIN_QUEUE_TIMEOUT = 30)
    q = MakeQueue(denom, current_time - 15s, /*fReady=*/false, outpoint);
    BOOST_CHECK(!q.IsTimeOutOfBounds(current_time));

    // Test timestamp too far in future (outside COINJOIN_QUEUE_TIMEOUT = 30)
    q = MakeQueue(denom, current_time + 60s, /*fReady=*/false, outpoint);
    BOOST_CHECK(q.IsTimeOutOfBounds(current_time));

    // Test timestamp too far in past (outside COINJOIN_QUEUE_TIMEOUT = 30)
    q = MakeQueue(denom, current_time - 60s, /*fReady=*/false, outpoint);
    BOOST_CHECK(q.IsTimeOutOfBounds(current_time));
}

BOOST_AUTO_TEST_CASE(queue_timestamp_extreme_values)
{
    CCoinJoinQueue q;
    q.nDenom = CoinJoin::AmountToDenomination(CoinJoin::GetSmallestDenomination());
    q.m_protxHash = uint256::ONE;

    // Negative timestamps are rejected by the guard
    q.nTime = INT64_MIN;
    BOOST_CHECK(q.IsTimeOutOfBounds(NodeSeconds{std::chrono::seconds{INT64_MAX}}));

    q.nTime = INT64_MAX;
    BOOST_CHECK(q.IsTimeOutOfBounds(NodeSeconds{std::chrono::seconds{INT64_MIN}}));

    q.nTime = INT64_MIN;
    BOOST_CHECK(q.IsTimeOutOfBounds(NodeSeconds{std::chrono::seconds{INT64_MIN}}));

    // Large positive timestamp with same value: zero diff, in bounds
    q.nTime = INT64_MAX;
    BOOST_CHECK(!q.IsTimeOutOfBounds(NodeSeconds{std::chrono::seconds{INT64_MAX}}));

    // Zero vs extreme positive: huge gap, out of bounds
    q.nTime = 0;
    BOOST_CHECK(q.IsTimeOutOfBounds(NodeSeconds{std::chrono::seconds{INT64_MAX}}));

    // Zero vs negative: rejected by guard
    q.nTime = 0;
    BOOST_CHECK(q.IsTimeOutOfBounds(NodeSeconds{std::chrono::seconds{INT64_MIN}}));
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

BOOST_AUTO_TEST_CASE(queuemanager_checkqueue_removes_timeouts)
{
    CoinJoinQueueManager man;
    const int denom = CoinJoin::AmountToDenomination(CoinJoin::GetSmallestDenomination());
    const auto now{GetAdjustedTime()};
    // Non-expired
    man.AddQueue(MakeQueue(denom, now, false, COutPoint(uint256S("11"), 0)));
    // Expired (too old)
    man.AddQueue(MakeQueue(denom, now - std::chrono::seconds{COINJOIN_QUEUE_TIMEOUT + 1}, false, COutPoint(uint256S("12"), 0)));

    BOOST_CHECK_EQUAL(man.GetQueueSize(), 2);
    man.CheckQueue();
    // One should be removed
    BOOST_CHECK_EQUAL(man.GetQueueSize(), 1);
}

BOOST_AUTO_TEST_CASE(queuemanager_getqueueitem_marks_tried_once)
{
    CoinJoinQueueManager man;
    const int denom = CoinJoin::AmountToDenomination(CoinJoin::GetSmallestDenomination());
    const auto now{GetAdjustedTime()};
    CCoinJoinQueue dsq = MakeQueue(denom, now, false, COutPoint(uint256S("21"), 0));
    man.AddQueue(dsq);

    CCoinJoinQueue picked;
    // First retrieval should succeed
    BOOST_CHECK(man.GetQueueItemAndTry(picked));
    // No other items left to try (picked is marked tried inside)
    CCoinJoinQueue picked2;
    BOOST_CHECK(!man.GetQueueItemAndTry(picked2));
}

BOOST_AUTO_TEST_CASE(queuemanager_getqueueitem_denom_filter_leaves_others_untried)
{
    CoinJoinQueueManager man;
    const int denom_small = CoinJoin::AmountToDenomination(CoinJoin::GetSmallestDenomination());
    const int denom_other = CoinJoin::AmountToDenomination(CoinJoin::GetSmallestDenomination() * 10);
    BOOST_REQUIRE(CoinJoin::IsValidDenomination(denom_small));
    BOOST_REQUIRE(CoinJoin::IsValidDenomination(denom_other));

    const auto now{GetAdjustedTime()};
    man.AddQueue(MakeQueue(denom_other, now, /*fReady=*/false, COutPoint(uint256S("51"), 0)));
    man.AddQueue(MakeQueue(denom_small, now, /*fReady=*/false, COutPoint(uint256S("52"), 0)));

    // A filtered search returns a queue of the requested denomination
    CCoinJoinQueue picked;
    BOOST_REQUIRE(man.GetQueueItemAndTry(picked, denom_small));
    BOOST_CHECK_EQUAL(picked.nDenom, denom_small);

    // ...which is now tried, so asking again for it finds nothing
    CCoinJoinQueue picked_again;
    BOOST_CHECK(!man.GetQueueItemAndTry(picked_again, denom_small));

    // The queue the filter skipped was left untried, so an unfiltered search still finds it.
    // Marking it tried here would starve standard mixing of every announcement a rebalance
    // attempt looked past.
    CCoinJoinQueue picked_unfiltered;
    BOOST_REQUIRE(man.GetQueueItemAndTry(picked_unfiltered));
    BOOST_CHECK_EQUAL(picked_unfiltered.nDenom, denom_other);

    // Both have been tried by now
    CCoinJoinQueue picked_none;
    BOOST_CHECK(!man.GetQueueItemAndTry(picked_none));
}

BOOST_AUTO_TEST_CASE(queuemanager_has_queue_from_masternode_readiness)
{
    CoinJoinQueueManager man;
    const int denom = CoinJoin::AmountToDenomination(CoinJoin::GetSmallestDenomination());
    const auto now{GetAdjustedTime()};
    const COutPoint mn(uint256S("31"), 0);
    // Single queue from `mn` with fReady=false.
    man.AddQueue(MakeQueue(denom, now, /*fReady=*/false, mn));

    // Readiness-aware lookup honors fReady.
    BOOST_CHECK(man.HasQueueFromMasternode(mn, /*fReady=*/false));
    BOOST_CHECK(!man.HasQueueFromMasternode(mn, /*fReady=*/true));

    // Unrelated masternode is never matched.
    const COutPoint other(uint256S("32"), 0);
    BOOST_CHECK(!man.HasQueueFromMasternode(other, /*fReady=*/false));
    BOOST_CHECK(!man.HasQueueFromMasternode(other, /*fReady=*/true));

    // TRY variant ignores readiness, finds by outpoint only.
    auto try_present = man.TryHasQueueFromMasternode(mn);
    BOOST_REQUIRE(try_present.has_value());
    BOOST_CHECK(*try_present);

    auto try_absent = man.TryHasQueueFromMasternode(other);
    BOOST_REQUIRE(try_absent.has_value());
    BOOST_CHECK(!*try_absent);
}

BOOST_AUTO_TEST_CASE(queuemanager_try_check_duplicate)
{
    CoinJoinQueueManager man;
    const int denom = CoinJoin::AmountToDenomination(CoinJoin::GetSmallestDenomination());
    const auto now{GetAdjustedTime()};
    const COutPoint mn(uint256S("41"), 0);
    const COutPoint other_mn(uint256S("42"), 0);

    const auto seed = MakeQueue(denom, now, /*fReady=*/false, mn);
    man.AddQueue(seed);

    auto is_dup = [&](const CCoinJoinQueue& q) {
        auto r = man.TryCheckDuplicate(q);
        BOOST_REQUIRE(r.has_value());
        return *r;
    };

    // Exact duplicate.
    BOOST_CHECK(is_dup(seed));
    // Same masternode + same readiness, different nTime: the "too many dsqs" guard still flags it.
    BOOST_CHECK(is_dup(MakeQueue(denom, now + 1s, /*fReady=*/false, mn)));
    // Same masternode but different readiness: allowed.
    BOOST_CHECK(!is_dup(MakeQueue(denom, now, /*fReady=*/true, mn)));
    // Different masternode: allowed.
    BOOST_CHECK(!is_dup(MakeQueue(denom, now, /*fReady=*/false, other_mn)));
}

BOOST_AUTO_TEST_SUITE_END()
