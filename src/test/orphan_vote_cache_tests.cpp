// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <governance/governance.h>
#include <governance/vote.h>
#include <primitives/transaction.h>
#include <uint256.h>
#include <util/time.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <vector>

using namespace std::chrono_literals;

namespace {
governance::OrphanVote OrphanFor(const uint256& parent_hash, const COutPoint& mn_outpoint)
{
    return {CGovernanceVote{mn_outpoint, parent_hash, VOTE_SIGNAL_FUNDING, VOTE_OUTCOME_YES}, NodeSeconds{9999s}};
}
} // namespace

// OrphanVoteCache accounting, exercised directly: none of these paths verify signatures, and
// small bounds keep the eviction cases readable.
BOOST_FIXTURE_TEST_SUITE(orphan_vote_cache_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(per_masternode_share_is_enforced_and_freed_on_erase)
{
    using Result = governance::OrphanVoteCache::InsertResult;
    governance::OrphanVoteCache cache{/*max_total=*/10, /*max_per_mn=*/2};
    const COutPoint mn{uint256S("aa"), 0};
    const COutPoint other_mn{uint256S("bb"), 0};

    BOOST_CHECK(cache.Insert(uint256S("a1"), OrphanFor(uint256S("a1"), mn)) == Result::OK);
    BOOST_CHECK(cache.Insert(uint256S("a1"), OrphanFor(uint256S("a1"), mn)) == Result::DUPLICATE);
    BOOST_CHECK(cache.Insert(uint256S("a2"), OrphanFor(uint256S("a2"), mn)) == Result::OK);
    BOOST_CHECK(cache.Insert(uint256S("a3"), OrphanFor(uint256S("a3"), mn)) == Result::MN_LIMIT);
    // One masternode at its share must not affect another.
    BOOST_CHECK(cache.Insert(uint256S("a3"), OrphanFor(uint256S("a3"), other_mn)) == Result::OK);
    BOOST_CHECK_EQUAL(cache.GetSize(), 3U);

    // Erasing one of the capped masternode's entries frees its slot; a duplicate insert must not
    // have double-counted it.
    cache.Erase(uint256S("a1"), OrphanFor(uint256S("a1"), mn));
    BOOST_CHECK(cache.Insert(uint256S("a4"), OrphanFor(uint256S("a4"), mn)) == Result::OK);
    BOOST_CHECK(cache.Insert(uint256S("a5"), OrphanFor(uint256S("a5"), mn)) == Result::MN_LIMIT);
}

// A vote we already hold costs no capacity, so a masternode at its share must still have repeats
// reported as duplicates: the caller needs that answer to register the relaying peer as a source
// for the missing parent, and MN_LIMIT would suppress it.
BOOST_AUTO_TEST_CASE(duplicates_are_recognized_at_the_per_masternode_share)
{
    using Result = governance::OrphanVoteCache::InsertResult;
    governance::OrphanVoteCache cache{/*max_total=*/10, /*max_per_mn=*/2};
    const COutPoint mn{uint256S("ee"), 0};

    BOOST_CHECK(cache.Insert(uint256S("d1"), OrphanFor(uint256S("d1"), mn)) == Result::OK);
    BOOST_CHECK(cache.Insert(uint256S("d2"), OrphanFor(uint256S("d2"), mn)) == Result::OK);
    // Exactly at its share now: a new parent is refused, but either cached vote repeats as one.
    BOOST_CHECK(cache.Insert(uint256S("d3"), OrphanFor(uint256S("d3"), mn)) == Result::MN_LIMIT);
    BOOST_CHECK(cache.Insert(uint256S("d1"), OrphanFor(uint256S("d1"), mn)) == Result::DUPLICATE);
    BOOST_CHECK(cache.Insert(uint256S("d2"), OrphanFor(uint256S("d2"), mn)) == Result::DUPLICATE);
    // The repeats changed nothing.
    BOOST_CHECK_EQUAL(cache.GetSize(), 2U);
}

BOOST_AUTO_TEST_CASE(global_eviction_releases_the_evicted_masternodes_slot)
{
    using Result = governance::OrphanVoteCache::InsertResult;
    governance::OrphanVoteCache cache{/*max_total=*/2, /*max_per_mn=*/2};
    const COutPoint mn_a{uint256S("aa"), 0};
    const COutPoint mn_b{uint256S("bb"), 0};

    BOOST_CHECK(cache.Insert(uint256S("f1"), OrphanFor(uint256S("f1"), mn_a)) == Result::OK);
    BOOST_CHECK(cache.Insert(uint256S("f2"), OrphanFor(uint256S("f2"), mn_a)) == Result::OK);
    // The cache is full: the next insert evicts the oldest entry (f1), which must release mn_a's
    // slot along with it.
    BOOST_CHECK(cache.Insert(uint256S("f3"), OrphanFor(uint256S("f3"), mn_b)) == Result::OK);
    BOOST_CHECK_EQUAL(cache.GetSize(), 2U);
    std::vector<governance::OrphanVote> votes;
    BOOST_CHECK(!cache.GetAll(uint256S("f1"), votes));
    // mn_a was at its share; without the release this would be MN_LIMIT.
    BOOST_CHECK(cache.Insert(uint256S("f4"), OrphanFor(uint256S("f4"), mn_a)) == Result::OK);
}

BOOST_AUTO_TEST_CASE(erase_all_for_masternode_and_clear_reset_the_accounting)
{
    using Result = governance::OrphanVoteCache::InsertResult;
    governance::OrphanVoteCache cache{/*max_total=*/10, /*max_per_mn=*/2};
    const COutPoint mn{uint256S("cc"), 0};
    const COutPoint bystander{uint256S("dd"), 0};

    BOOST_CHECK(cache.Insert(uint256S("e1"), OrphanFor(uint256S("e1"), mn)) == Result::OK);
    BOOST_CHECK(cache.Insert(uint256S("e2"), OrphanFor(uint256S("e2"), mn)) == Result::OK);
    BOOST_CHECK(cache.Insert(uint256S("e3"), OrphanFor(uint256S("e3"), bystander)) == Result::OK);

    BOOST_CHECK_EQUAL(cache.EraseAllForMasternode(mn), 2U);
    BOOST_CHECK_EQUAL(cache.GetSize(), 1U);
    BOOST_CHECK_EQUAL(cache.EraseAllForMasternode(mn), 0U);
    BOOST_CHECK(cache.Insert(uint256S("e4"), OrphanFor(uint256S("e4"), mn)) == Result::OK);

    cache.Clear();
    BOOST_CHECK_EQUAL(cache.GetSize(), 0U);
    BOOST_CHECK(cache.Insert(uint256S("e5"), OrphanFor(uint256S("e5"), bystander)) == Result::OK);
}

BOOST_AUTO_TEST_SUITE_END()
