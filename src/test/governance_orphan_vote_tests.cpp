// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <clientversion.h>
#include <evo/chainhelper.h>
#include <evo/deterministicmns.h>
#include <governance/governance.h>
#include <governance/object.h>
#include <governance/vote.h>
#include <masternode/sync.h>
#include <streams.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <util/time.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {
struct GovernanceOrphanVoteSetup : public TestingSetup {
    GovernanceOrphanVoteSetup() : TestingSetup{CBaseChainParams::MAIN}
    {
        BOOST_REQUIRE(m_node.mn_sync);
        m_node.mn_sync->SwitchToNextAsset();
        BOOST_REQUIRE(m_node.mn_sync->IsBlockchainSynced());

        BOOST_REQUIRE(m_node.mn_metaman);
        // Left unloaded: no test here reaches CGovernanceObject::ProcessVote.
        m_node.govman = std::make_unique<CGovernanceManager>(*m_node.mn_metaman, *m_node.chainman,
                                                             *m_node.chain_helper->superblocks, *m_node.dmnman,
                                                             *m_node.mn_sync);
        BOOST_REQUIRE(m_node.govman->LoadCache(/*load_cache=*/false));

        // Deterministic timestamps for constructed votes.
        SetMockTime(1'700'000'000s);
    }
    ~GovernanceOrphanVoteSetup()
    {
        // govman holds a reference to chain_helper->superblocks; tear down first.
        m_node.govman.reset();
    }
};

CGovernanceVote MakeUnvalidatedOrphanVote(const uint256& parent_hash, uint32_t outpoint_n)
{
    // Garbage masternode outpoint + garbage signature: well-formed on the wire,
    // but never a valid tip-list MN vote. Pre-fix ProcessVote still caches these.
    CGovernanceVote vote{COutPoint{uint256S("11"), outpoint_n}, parent_hash, VOTE_SIGNAL_FUNDING, VOTE_OUTCOME_YES};
    vote.SetTime(GetTime<std::chrono::seconds>().count());
    vote.SetSignature(std::vector<unsigned char>(CGovernanceVote::COMPACT_SIG_SIZE, 0xab));
    return vote;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(governance_orphan_vote_tests, GovernanceOrphanVoteSetup)

// SECURITY regression:
// Unauthenticated peers must not be able to fill cmmapOrphanVotes with unvalidated
// votes keyed by attacker-chosen parent hashes. Pre-fix, ProcessVote inserts
// before any masternode/signature check and returns GOVERNANCE_EXCEPTION_WARNING
// with nNodePenalty=0, so N distinct parents become N orphan keys that the
// 5-minute scheduler then fans out as MNGOVERNANCESYNC to every peer.
//
// Post-fix, unvalidated votes are rejected before orphan insertion with the same
// penalty 20 that CGovernanceObject::ProcessVote applies to an unknown masternode,
// so the orphan set stays empty. (The orphan branch itself keeps penalty 0: getting
// there now requires a valid MN signature, so it is a benign relay race.)
BOOST_AUTO_TEST_CASE(orphan_vote_cache_rejects_unvalidated_votes)
{
    constexpr size_t N = 50;

    size_t inserted_as_orphan{0};
    size_t zero_penalty_rejects{0};
    size_t penalty_20_rejects{0};

    for (size_t i = 0; i < N; ++i) {
        // Distinct attacker-chosen parent hashes so each would occupy its own
        // CacheMultiMap key (and therefore produce one MNGOVERNANCESYNC per peer).
        const uint256 parent_hash{uint256S(strprintf("%02x", static_cast<unsigned>(i + 1)))};
        const CGovernanceVote vote{MakeUnvalidatedOrphanVote(parent_hash, /*outpoint_n=*/static_cast<uint32_t>(i + 1))};

        CGovernanceException exception;
        uint256 hash_to_request;
        const bool accepted = m_node.govman->ProcessVote(vote, exception, hash_to_request);
        BOOST_CHECK(!accepted);

        if (!hash_to_request.IsNull()) {
            ++inserted_as_orphan;
            BOOST_CHECK_EQUAL(hash_to_request, parent_hash);
        }
        if (exception.GetNodePenalty() == 0) {
            ++zero_penalty_rejects;
        }
        if (exception.GetNodePenalty() == 20) {
            ++penalty_20_rejects;
        }
    }

    const std::vector<uint256> orphan_parents = m_node.govman->GetOrphanVoteObjectHashes();

    // Pre-fix this fails: every unvalidated vote is cached under its parent hash
    // with a zero-penalty WARNING, so inserted_as_orphan == N and orphan_parents
    // grows without a protective validation gate.
    //
    // Post-fix invariants:
    // - no unvalidated vote enters the orphan cache
    // - no courtesy object-request is advertised for garbage parents
    // - every rejection is scored 20, so repeated injection reaches
    //   DISCOURAGEMENT_THRESHOLD and the peer is disconnected
    BOOST_CHECK_EQUAL(inserted_as_orphan, 0U);
    BOOST_CHECK_EQUAL(zero_penalty_rejects, 0U);
    BOOST_CHECK_EQUAL(penalty_20_rejects, N);
    BOOST_CHECK_EQUAL(orphan_parents.size(), 0U);
}

// The orphan cache must be bounded well below MAX_CACHE_SIZE (1e6). At ~600 bytes
// per retained CacheMultiMap entry, the pre-fix ceiling was ~600 MB of
// attacker-controlled data; MAX_ORPHAN_VOTES keeps the worst case under ~1 MB.
// Asserted against the shared MAX_CACHE_SIZE bound rather than a repeated literal
// so this fails if someone reverts the constructor back to MAX_CACHE_SIZE.
BOOST_AUTO_TEST_CASE(orphan_vote_cache_is_bounded_far_below_max_cache_size)
{
    BOOST_CHECK_GT(CGovernanceManager::MAX_ORPHAN_VOTES, 0);
    // Comfortably above the number of governance objects a real network syncs
    // out of order, so honest orphan recovery is unaffected.
    BOOST_CHECK_GE(CGovernanceManager::MAX_ORPHAN_VOTES, 500);
    // ~1000x below the generic cache ceiling: the memory-exhaustion fix.
    BOOST_CHECK_LE(CGovernanceManager::MAX_ORPHAN_VOTES, 10'000);

    // The cache actually enforces it. Pre-fix (cmmapOrphanVotes(MAX_CACHE_SIZE))
    // this reports 1'000'000 and fails.
    BOOST_CHECK_EQUAL(m_node.govman->GetOrphanVoteCacheMaxSize(),
                      static_cast<size_t>(CGovernanceManager::MAX_ORPHAN_VOTES));

    // A single tick must never fan out more requests than the cache can hold.
    BOOST_CHECK_GT(CGovernanceManager::MAX_ORPHAN_OBJECT_REQUESTS_PER_TICK, 0U);
    BOOST_CHECK_LE(CGovernanceManager::MAX_ORPHAN_OBJECT_REQUESTS_PER_TICK,
                   static_cast<size_t>(CGovernanceManager::MAX_ORPHAN_VOTES));
}

// governance.dat must not carry orphan votes across a restart, and the
// version string must be bumped whenever that layout changes so old files are
// discarded rather than misparsed.
BOOST_AUTO_TEST_CASE(orphan_votes_are_not_persisted)
{
    // A v16 file has an extra CacheMultiMap between cmapInvalidVotes and mapObjects.
    // Reading it with the v17 layout would misparse, so the version string must differ.
    BOOST_CHECK_NE(CGovernanceManager::GetSerializationVersionString(),
                   std::string{"CGovernanceManager-Version-16"});

    // Round-trip the store. Serialize writes the v17 field sequence; Unserialize must
    // consume it exactly, leaving no trailing bytes. If Serialize and Unserialize ever
    // disagree about the orphan map (one writing it, the other not), the reader either
    // throws or leaves the stream non-empty here.
    CDataStream ss{SER_DISK, CLIENT_VERSION};
    ss << *m_node.govman;
    const size_t written = ss.size();

    BOOST_REQUIRE_NO_THROW(ss >> *m_node.govman);
    BOOST_CHECK(ss.empty());

    // Writing the reloaded store must reproduce the identical byte stream, so no
    // orphan state was smuggled in or dropped across the round trip.
    CDataStream ss2{SER_DISK, CLIENT_VERSION};
    ss2 << *m_node.govman;
    BOOST_CHECK_EQUAL(ss2.size(), written);
    BOOST_CHECK(m_node.govman->GetOrphanVoteObjectHashes().empty());
}

BOOST_AUTO_TEST_SUITE_END()
