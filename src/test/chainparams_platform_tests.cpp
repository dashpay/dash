// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <chainparamsbase.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <utility>

BOOST_FIXTURE_TEST_SUITE(chainparams_platform_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(platform_chain_id)
{
    // Must match the Tenderdash genesis chain_id in dashmate's mainnet/testnet config defaults
    const std::pair<std::string, std::string> expected[]{
        {CBaseChainParams::MAIN, "evo1"},
        {CBaseChainParams::TESTNET, "dash-testnet-51"},
        {CBaseChainParams::DEVNET, ""},
        {CBaseChainParams::REGTEST, ""},
    };
    for (const auto& [chain, chain_id] : expected) {
        BOOST_CHECK_EQUAL(CreateChainParams(*m_node.args, chain)->PlatformChainId(), chain_id);
    }
}

BOOST_AUTO_TEST_SUITE_END()
