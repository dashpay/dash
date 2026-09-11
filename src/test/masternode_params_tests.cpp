// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <init.h>
#include <test/util/setup_common.h>
#include <util/system.h>

#include <boost/test/unit_test.hpp>

//! These tests cover the parameter interactions triggered by
//! -masternodeblsprivkey in InitParameterInteraction. Previously this was
//! exercised by test/functional/feature_masternode_params.py, which spun up
//! full nodes only to read back arg state.
BOOST_FIXTURE_TEST_SUITE(masternode_params_tests, BasicTestingSetup)

//! Without -masternodeblsprivkey, masternode-driven defaults must not fire.
BOOST_AUTO_TEST_CASE(no_masternode_key_leaves_defaults)
{
    ArgsManager args;
    InitParameterInteraction(args);

    BOOST_CHECK(!args.IsArgSet("-disablewallet"));
    BOOST_CHECK(!args.IsArgSet("-peerblockfilters"));
    BOOST_CHECK(!args.IsArgSet("-blockfilterindex"));
}

//! Setting -masternodeblsprivkey must auto-enable -disablewallet,
//! -peerblockfilters and -blockfilterindex=basic.
BOOST_AUTO_TEST_CASE(masternode_key_enables_filters_and_disables_wallet)
{
    ArgsManager args;
    args.ForceSetArg("-masternodeblsprivkey", "dummy");

    InitParameterInteraction(args);

    BOOST_CHECK(args.GetBoolArg("-disablewallet", false));
    BOOST_CHECK(args.GetBoolArg("-peerblockfilters", false));
    BOOST_CHECK_EQUAL(args.GetArg("-blockfilterindex", ""), "basic");
}

//! Explicit user overrides must win over the masternode defaults
//! (SoftSet semantics): the user can still disable filters and keep the
//! wallet on a masternode.
BOOST_AUTO_TEST_CASE(masternode_key_respects_user_overrides)
{
    ArgsManager args;
    args.ForceSetArg("-masternodeblsprivkey", "dummy");
    args.ForceSetArg("-disablewallet", "0");
    args.ForceSetArg("-peerblockfilters", "0");
    args.ForceSetArg("-blockfilterindex", "0");

    InitParameterInteraction(args);

    BOOST_CHECK(!args.GetBoolArg("-disablewallet", true));
    BOOST_CHECK(!args.GetBoolArg("-peerblockfilters", true));
    BOOST_CHECK_EQUAL(args.GetArg("-blockfilterindex", ""), "0");
}

BOOST_AUTO_TEST_SUITE_END()
