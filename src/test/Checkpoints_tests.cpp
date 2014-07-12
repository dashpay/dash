//
// Unit tests for block-chain checkpoints
//
#include <boost/assign/list_of.hpp> // for 'map_list_of()'
#include <boost/test/unit_test.hpp>
#include <boost/foreach.hpp>

#include "../checkpoints.h"
#include "../util.h"

using namespace std;

BOOST_AUTO_TEST_SUITE(Checkpoints_tests)

BOOST_AUTO_TEST_CASE(sanity)
{
    uint256 p1500 = uint256("0x000000aaf0300f59f49bc3e970bad15c11f961fe2347accffff19d96ec9778e3");
    uint256 p88805 = uint256("0x00000000001392f1652e9bf45cd8bc79dc60fe935277cd11538565b4a94fa85f");
    BOOST_CHECK(Checkpoints::CheckBlock(1500, p1500));
    BOOST_CHECK(Checkpoints::CheckBlock(88805, p88805));


    // Wrong hashes at checkpoints should fail:
    BOOST_CHECK(!Checkpoints::CheckBlock(1500, p88805));
    BOOST_CHECK(!Checkpoints::CheckBlock(88805, p1500));

    // ... but any hash not at a checkpoint should succeed:
    BOOST_CHECK(Checkpoints::CheckBlock(1500+1, p88805));
    BOOST_CHECK(Checkpoints::CheckBlock(88805+1, p1500));

    BOOST_CHECK(Checkpoints::GetTotalBlocksEstimate() >= 88805);
}

BOOST_AUTO_TEST_SUITE_END()
