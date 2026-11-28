// Copyright (c) 2024-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/feefrac.h>
#include <random.h>

#include <compare>
#include <vector>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(feefrac_tests)

BOOST_AUTO_TEST_CASE(feefrac_operators)
{
    FeeFrac p1{1000, 100}, p2{500, 300};
    FeeFrac sum{1500, 400};
    FeeFrac diff{500, -200};
    FeeFrac empty{0, 0};
    FeeFrac zero_fee{0, 1}; // zero-fee allowed

    BOOST_CHECK(empty == FeeFrac{}); // same as no-args

    BOOST_CHECK(p1 == p1);
    BOOST_CHECK(p1 + p2 == sum);
    BOOST_CHECK(p1 - p2 == diff);

    FeeFrac p3{2000, 200};
    BOOST_CHECK(p1 != p3); // feefracs only equal if both fee and size are same
    BOOST_CHECK(p2 != p3);

    FeeFrac p4{3000, 300};
    BOOST_CHECK(p1 == p4-p3);
    BOOST_CHECK(p1 + p3 == p4);

    // Fee-rate comparison
    BOOST_CHECK(p1 > p2);
    BOOST_CHECK(p1 >= p2);
    BOOST_CHECK(p1 >= p4-p3);
    BOOST_CHECK(!(p1 >> p3)); // not strictly better
    BOOST_CHECK(p1 >> p2); // strictly greater feerate

    BOOST_CHECK(p2 < p1);
    BOOST_CHECK(p2 <= p1);
    BOOST_CHECK(p1 <= p4-p3);
    BOOST_CHECK(!(p3 << p1)); // not strictly worse
    BOOST_CHECK(p2 << p1); // strictly lower feerate

    // "empty" comparisons
    BOOST_CHECK(!(p1 >> empty)); // << will always result in false
    BOOST_CHECK(!(p1 << empty));
    BOOST_CHECK(!(empty >> empty));
    BOOST_CHECK(!(empty << empty));

    // empty is always bigger than everything else
    BOOST_CHECK(empty > p1);
    BOOST_CHECK(empty > p2);
    BOOST_CHECK(empty > p3);
    BOOST_CHECK(empty >= p1);
    BOOST_CHECK(empty >= p2);
    BOOST_CHECK(empty >= p3);

    // check "max" values for comparison
    FeeFrac oversized_1{4611686000000, 4000000};
    FeeFrac oversized_2{184467440000000, 100000};

    BOOST_CHECK(oversized_1 < oversized_2);
    BOOST_CHECK(oversized_1 <= oversized_2);
    BOOST_CHECK(oversized_1 << oversized_2);
    BOOST_CHECK(oversized_1 != oversized_2);

    // Tests paths that use double arithmetic
    FeeFrac busted{(static_cast<int64_t>(INT32_MAX)) + 1, INT32_MAX};
    BOOST_CHECK(!(busted < busted));

    FeeFrac max_fee{2100000000000000, INT32_MAX};
    BOOST_CHECK(!(max_fee < max_fee));
    BOOST_CHECK(!(max_fee > max_fee));
    BOOST_CHECK(max_fee <= max_fee);
    BOOST_CHECK(max_fee >= max_fee);

    FeeFrac max_fee2{1, 1};
    BOOST_CHECK(max_fee >= max_fee2);

}

BOOST_AUTO_TEST_CASE(feerate_chunks_utilities)
{
    // Sanity check the correctness of the feerate chunks comparison.

    // A strictly better case.
    std::vector<FeeFrac> old_chunks{{{950, 300}, {100, 100}}};
    std::vector<FeeFrac> new_chunks{{{1000, 300}, {50, 100}}};

    BOOST_CHECK(std::is_lt(CompareChunks(old_chunks, new_chunks)));
    BOOST_CHECK(std::is_gt(CompareChunks(new_chunks, old_chunks)));

    // Incomparable diagrams
    old_chunks = {{950, 300}, {100, 100}};
    new_chunks = {{1000, 300}, {0, 100}};

    BOOST_CHECK(CompareChunks(old_chunks, new_chunks) == std::partial_ordering::unordered);
    BOOST_CHECK(CompareChunks(new_chunks, old_chunks) == std::partial_ordering::unordered);

    // Strictly better but smaller size.
    old_chunks = {{950, 300}, {100, 100}};
    new_chunks = {{1100, 300}};

    BOOST_CHECK(std::is_lt(CompareChunks(old_chunks, new_chunks)));
    BOOST_CHECK(std::is_gt(CompareChunks(new_chunks, old_chunks)));

    // New diagram is strictly better due to the first chunk, even though
    // second chunk contributes no fees
    old_chunks = {{950, 300}, {100, 100}};
    new_chunks = {{1100, 100}, {0, 100}};

    BOOST_CHECK(std::is_lt(CompareChunks(old_chunks, new_chunks)));
    BOOST_CHECK(std::is_gt(CompareChunks(new_chunks, old_chunks)));

    // Feerate of first new chunk is better with, but second chunk is worse
    old_chunks = {{950, 300}, {100, 100}};
    new_chunks = {{750, 100}, {249, 250}, {151, 650}};

    BOOST_CHECK(CompareChunks(old_chunks, new_chunks) == std::partial_ordering::unordered);
    BOOST_CHECK(CompareChunks(new_chunks, old_chunks) == std::partial_ordering::unordered);

    // If we make the second chunk slightly better, the new diagram now wins.
    old_chunks = {{950, 300}, {100, 100}};
    new_chunks = {{750, 100}, {250, 250}, {150, 150}};

    BOOST_CHECK(std::is_lt(CompareChunks(old_chunks, new_chunks)));
    BOOST_CHECK(std::is_gt(CompareChunks(new_chunks, old_chunks)));

    // Identical diagrams, cannot be strictly better
    old_chunks = {{950, 300}, {100, 100}};
    new_chunks = {{950, 300}, {100, 100}};

    BOOST_CHECK(std::is_eq(CompareChunks(old_chunks, new_chunks)));
    BOOST_CHECK(std::is_eq(CompareChunks(new_chunks, old_chunks)));

    // Same aggregate fee, but different total size (trigger single tail fee check step)
    old_chunks = {{950, 300}, {100, 99}};
    new_chunks = {{950, 300}, {100, 100}};

    // No change in evaluation when tail check needed.
    BOOST_CHECK(std::is_gt(CompareChunks(old_chunks, new_chunks)));
    BOOST_CHECK(std::is_lt(CompareChunks(new_chunks, old_chunks)));

    // Trigger multiple tail fee check steps
    old_chunks = {{950, 300}, {100, 99}};
    new_chunks = {{950, 300}, {100, 100}, {0, 1}, {0, 1}};

    BOOST_CHECK(std::is_gt(CompareChunks(old_chunks, new_chunks)));
    BOOST_CHECK(std::is_lt(CompareChunks(new_chunks, old_chunks)));

    // Multiple tail fee check steps, unordered result
    new_chunks = {{950, 300}, {100, 100}, {0, 1}, {0, 1}, {1, 1}};
    BOOST_CHECK(CompareChunks(old_chunks, new_chunks) == std::partial_ordering::unordered);
    BOOST_CHECK(CompareChunks(new_chunks, old_chunks) == std::partial_ordering::unordered);
}

BOOST_AUTO_TEST_SUITE_END()
