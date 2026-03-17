// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <coinjoin/coinjoin.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(coinjoin_basemanager_tests, BasicTestingSetup)

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

BOOST_AUTO_TEST_CASE(checkqueue_removes_timeouts)
{
    CCoinJoinBaseManager man;
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

BOOST_AUTO_TEST_CASE(getqueueitem_marks_tried_once)
{
    CCoinJoinBaseManager man;
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
