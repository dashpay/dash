// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <key.h>
#include <key_io.h>
#include <script/standard.h>
#include <spork.h>
#include <timedata.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <limits>

BOOST_FIXTURE_TEST_SUITE(spork_tests, RegTestingSetup)

BOOST_AUTO_TEST_CASE(extreme_timestamps_are_handled_without_overflow)
{
    CSporkManager sporkman{m_args.GetDataDirNet()};
    CKey key;
    key.MakeNewKey(/*fCompressed=*/true);
    BOOST_REQUIRE(sporkman.SetSporkAddress(EncodeDestination(PKHash{key.GetPubKey()})));

    CSporkMessage future_spork;
    future_spork.nTimeSigned = std::numeric_limits<int64_t>::max();
    BOOST_REQUIRE(future_spork.Sign(key));
    BOOST_CHECK(!sporkman.IsValidSpork(future_spork));

    CSporkMessage inactive_spork{SPORK_2_INSTANTSEND_ENABLED, std::numeric_limits<int64_t>::max(),
                                 GetAdjustedTime()};
    BOOST_REQUIRE(inactive_spork.Sign(key));
    BOOST_REQUIRE(sporkman.IsValidSpork(inactive_spork));
    BOOST_REQUIRE(sporkman.ProcessSpork(inactive_spork));
    BOOST_CHECK(!sporkman.IsSporkActive(SPORK_2_INSTANTSEND_ENABLED));
}

BOOST_AUTO_TEST_CASE(sporks_signed_by_an_unknown_key_are_rejected)
{
    CSporkManager sporkman{m_args.GetDataDirNet()};
    CKey spork_key;
    spork_key.MakeNewKey(/*fCompressed=*/true);
    BOOST_REQUIRE(sporkman.SetSporkAddress(EncodeDestination(PKHash{spork_key.GetPubKey()})));

    CKey other_key;
    other_key.MakeNewKey(/*fCompressed=*/true);

    CSporkMessage spork{SPORK_2_INSTANTSEND_ENABLED, 1, GetAdjustedTime()};
    BOOST_REQUIRE(spork.Sign(other_key));
    BOOST_CHECK(!sporkman.IsValidSpork(spork));

    BOOST_REQUIRE(spork.Sign(spork_key));
    BOOST_CHECK(sporkman.IsValidSpork(spork));
}

BOOST_AUTO_TEST_CASE(check_and_remove_drops_sporks_not_signed_by_the_spork_key)
{
    CSporkManager sporkman{m_args.GetDataDirNet()};
    CKey key;
    key.MakeNewKey(/*fCompressed=*/true);
    BOOST_REQUIRE(sporkman.SetSporkAddress(EncodeDestination(PKHash{key.GetPubKey()})));

    const SporkValue default_value{sporkman.GetSporkValue(SPORK_2_INSTANTSEND_ENABLED)};
    BOOST_REQUIRE(default_value != 1);

    CSporkMessage spork{SPORK_2_INSTANTSEND_ENABLED, 1, GetAdjustedTime()};
    BOOST_REQUIRE(spork.Sign(key));
    BOOST_REQUIRE(sporkman.ProcessSpork(spork));
    BOOST_REQUIRE_EQUAL(sporkman.GetSporkValue(SPORK_2_INSTANTSEND_ENABLED), 1);
    BOOST_REQUIRE(sporkman.GetSporkByHash(spork.GetHash()).has_value());

    // Mimic a restart with a spork address the cached sporks were not signed by.
    CKey other_key;
    other_key.MakeNewKey(/*fCompressed=*/true);
    BOOST_REQUIRE(sporkman.SetSporkAddress(EncodeDestination(PKHash{other_key.GetPubKey()})));
    sporkman.CheckAndRemove();

    BOOST_CHECK_EQUAL(sporkman.GetSporkValue(SPORK_2_INSTANTSEND_ENABLED), default_value);
    BOOST_CHECK(!sporkman.GetSporkByHash(spork.GetHash()).has_value());
}

// "10020" + "1" + "1791370200" and "10020" + "11" + "791370200" are the same decimal text
BOOST_AUTO_TEST_CASE(spork_signature_binds_value_and_time)
{
    CKey key;
    key.MakeNewKey(/*fCompressed=*/true);
    CSporkMessage spork{SPORK_21_QUORUM_ALL_CONNECTED, 1, NodeClock::time_point{1791370200s}};
    BOOST_REQUIRE(spork.Sign(key));
    BOOST_CHECK(spork.CheckSignature(key.GetPubKey().GetID()));

    CSporkMessage reinterpreted{spork};
    reinterpreted.nValue = 11;
    reinterpreted.nTimeSigned = 791370200;
    BOOST_CHECK(!reinterpreted.CheckSignature(key.GetPubKey().GetID()));
}

BOOST_AUTO_TEST_SUITE_END()
