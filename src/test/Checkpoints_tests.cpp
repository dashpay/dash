// Copyright (c) 2011-2013 The Bitcoin Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//
// Unit tests for block-chain checkpoints
//

#include "checkpoints.h"

#include "uint256.h"

#include <boost/test/unit_test.hpp>

using namespace std;

BOOST_AUTO_TEST_SUITE(Checkpoints_tests)

BOOST_AUTO_TEST_CASE(sanity)
{
    uint256 p_1 = uint256("0x000000aaf0300f59f49bc3e970bad15c11f961fe2347accffff19d96ec9778e3");
    uint256 p_2 = uint256("0x000000003b01809551952460744d5dbb8fcbd6cbae3c220267bf7fa43f837367");
    BOOST_CHECK(Checkpoints::CheckBlock(-1, p_1));
    BOOST_CHECK(Checkpoints::CheckBlock(-2, p_2));

    
    // Wrong hashes at checkpoints should fail:
    BOOST_CHECK(!Checkpoints::CheckBlock(-1, p_2));
    BOOST_CHECK(!Checkpoints::CheckBlock(-2, p_1));

    // ... but any hash not at a checkpoint should succeed:
    BOOST_CHECK(Checkpoints::CheckBlock(-1+1, p_2));
    BOOST_CHECK(Checkpoints::CheckBlock(-2+1, p_1));

    BOOST_CHECK(Checkpoints::GetTotalBlocksEstimate() >= -2);
}    

BOOST_AUTO_TEST_SUITE_END()
