// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <kernel/mempool_options.h>
#include <node/mempool_args.h>
#include <util/system.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(masternode_params_tests, BasicTestingSetup)

static std::vector<std::string> StricterFor(const std::vector<std::string>& args)
{
    ArgsManager argsman;
    std::vector<const char*> argv{"dashd"};
    for (const auto& arg : args) argv.push_back(arg.c_str());
    std::string error;
    argsman.AddArg("-minrelaytxfee", "", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-incrementalrelayfee", "", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-dustrelayfee", "", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-datacarrier", "", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-datacarriersize", "", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-permitbaremultisig", "", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-limitancestorcount", "", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-limitancestorsize", "", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-limitdescendantcount", "", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-limitdescendantsize", "", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-maxmempool", "", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    BOOST_REQUIRE(argsman.ParseParameters(argv.size(), argv.data(), error));
    kernel::MemPoolOptions opts{};
    BOOST_REQUIRE(!ApplyArgsManOptions(argsman, Params(), opts));
    return GetStricterThanDefaultRelayPolicy(opts);
}

BOOST_AUTO_TEST_CASE(default_relay_policy_is_not_stricter)
{
    BOOST_CHECK(StricterFor({}).empty());
}

BOOST_AUTO_TEST_CASE(looser_relay_policy_is_not_stricter)
{
    BOOST_CHECK(
        StricterFor({"-minrelaytxfee=0.000001", "-limitancestorcount=50", "-datacarriersize=160", "-maxmempool=301"}).empty());
}

BOOST_AUTO_TEST_CASE(each_stricter_relay_option_is_reported)
{
    using V = std::vector<std::string>;
    BOOST_CHECK(StricterFor({"-minrelaytxfee=0.001"}) == V{"-minrelaytxfee"});
    BOOST_CHECK(StricterFor({"-incrementalrelayfee=0.001"}) == V{"-incrementalrelayfee"});
    BOOST_CHECK(StricterFor({"-dustrelayfee=0.001"}) == V{"-dustrelayfee"});
    BOOST_CHECK(StricterFor({"-datacarrier=0"}) == V{"-datacarrier/-datacarriersize"});
    // An asset lock carries a two-byte OP_RETURN, which a smaller limit makes nonstandard.
    BOOST_CHECK(StricterFor({"-datacarriersize=1"}) == V{"-datacarrier/-datacarriersize"});
    BOOST_CHECK(StricterFor({"-permitbaremultisig=0"}) == V{"-permitbaremultisig"});
    BOOST_CHECK(StricterFor({"-limitancestorcount=5"}) == V{"-limitancestorcount"});
    BOOST_CHECK(StricterFor({"-limitancestorsize=50"}) == V{"-limitancestorsize"});
    BOOST_CHECK(StricterFor({"-limitdescendantcount=5"}) == V{"-limitdescendantcount"});
    BOOST_CHECK(StricterFor({"-limitdescendantsize=50"}) == V{"-limitdescendantsize"});
    BOOST_CHECK(StricterFor({"-maxmempool=299"}) == V{"-maxmempool"});
}

BOOST_AUTO_TEST_SUITE_END()
