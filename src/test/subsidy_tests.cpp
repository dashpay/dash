// Copyright (c) 2014-2018 The Waggox Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "chainparams.h"
#include "validation.h"

#include "test/test_waggox.h"

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(subsidy_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(block_subsidy_test)
{
    const Consensus::Params& consensusParams = Params(CBaseChainParams::MAIN).GetConsensus();

    int32_t nPrevHeight;
    CAmount nSubsidy;

    // details for block 4249 (subsidy returned will be for block 4250)
    nPrevHeight = 4249;
    nSubsidy = GetBlockSubsidy(nPrevHeight, consensusParams, false);
    BOOST_CHECK_EQUAL(nSubsidy, 1000000000ULL);

    // details for block 4501 (subsidy returned will be for block 4502)
    nPrevHeight = 4501;
    nSubsidy = GetBlockSubsidy(nPrevHeight, consensusParams, false);
    BOOST_CHECK_EQUAL(nSubsidy, 1000000000ULL);

    // details for block 5464 (subsidy returned will be for block 5465)
    nPrevHeight = 5464;
    nSubsidy = GetBlockSubsidy(nPrevHeight, consensusParams, false);
    BOOST_CHECK_EQUAL(nSubsidy, 1000000000ULL);

    // details for block 5465 (subsidy returned will be for block 5466)
    nPrevHeight = 5465;
    nSubsidy = GetBlockSubsidy(nPrevHeight, consensusParams, false);
    BOOST_CHECK_EQUAL(nSubsidy, 1000000000ULL);

    // details for block 17588 (subsidy returned will be for block 17589)
    nPrevHeight = 17588;
    nSubsidy = GetBlockSubsidy(nPrevHeight, consensusParams, false);
    BOOST_CHECK_EQUAL(nSubsidy, 2000000000ULL);

    // details for block 241269 (subsidy returned will be for block 100000)
    nPrevHeight = 241269;
    nSubsidy = GetBlockSubsidy(nPrevHeight, consensusParams, false);
    BOOST_CHECK_EQUAL(nSubsidy, 2000000000ULL);

    // details for block 210239 (subsidy returned will be for block 210240)
    nPrevHeight = 210239;
    nSubsidy = GetBlockSubsidy(nPrevHeight, consensusParams, false);
    BOOST_CHECK_EQUAL(nSubsidy, 2000000000ULL);

    // 1st subsidy reduction happens here

    // details for block 210240 (subsidy returned will be for block 210241)
    nPrevHeight = 210240;
    nSubsidy = GetBlockSubsidy(nPrevHeight, consensusParams, false);
    BOOST_CHECK_EQUAL(nSubsidy, 1600000000ULL);
}

BOOST_AUTO_TEST_SUITE_END()
