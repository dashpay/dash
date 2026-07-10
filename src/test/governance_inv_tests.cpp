// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <governance/governance.h>
#include <governance/net_governance.h>
#include <masternode/meta.h>
#include <masternode/sync.h>
#include <net.h>
#include <net_processing.h>
#include <netfulfilledman.h>
#include <node/connection_types.h>
#include <protocol.h>
#include <scheduler.h>
#include <streams.h>
#include <uint256.h>
#include <util/time.h>
#include <version.h>

#include <test/util/setup_common.h>

#include <arith_uint256.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
struct GovernanceInvSetup : public TestingSetup {
    GovernanceInvSetup() : TestingSetup{CBaseChainParams::MAIN}
    {
        // ConfirmInventoryRequest and CheckAndRemove short-circuit on
        // !IsBlockchainSynced(); CheckAndRemove also asserts metaman.IsValid().
        // NetGovernance::AlreadyHave gates on m_gov_manager.IsValid().
        BOOST_REQUIRE(m_node.mn_sync);
        m_node.mn_sync->SwitchToNextAsset();
        BOOST_REQUIRE(m_node.mn_sync->IsBlockchainSynced());

        BOOST_REQUIRE(m_node.mn_metaman);
        BOOST_REQUIRE(m_node.mn_metaman->LoadCache(/*load_cache=*/false));

        BOOST_REQUIRE(m_node.govman);
        // Match runtime preconditions: NetGovernance::AlreadyHave claims we
        // already have the inv when governance isn't loaded (e.g.
        // -disablegovernance), so ConfirmInventoryRequest would never run.
        BOOST_REQUIRE(m_node.govman->LoadCache(/*load_cache=*/false));

        BOOST_REQUIRE(m_node.netfulfilledman);
        // Loaded here for the later test that advances GOVERNANCE -> FINISHED;
        // the sync notifier asserts netfulfilledman.IsValid().
        BOOST_REQUIRE(m_node.netfulfilledman->LoadCache(/*load_cache=*/false));
        BOOST_REQUIRE(m_node.connman);
        BOOST_REQUIRE(m_node.peerman);

        // Intentional unit-test boundary: TestingSetup does not run the
        // init.cpp/AppInit startup path that registers the Dash-specific
        // handlers, so the INV branch in PeerManagerImpl::AlreadyHave would not
        // route MSG_GOVERNANCE_OBJECT[_VOTE] anywhere. Install the same
        // NetGovernance handler init.cpp registers so a real INV reaches
        // CGovernanceManager::ConfirmInventoryRequest; the startup registration
        // itself stays outside this unit test.
        m_node.peerman->AddExtraHandler(std::make_unique<NetGovernance>(
            m_node.peerman.get(), *m_node.govman, *m_node.mn_sync,
            *m_node.netfulfilledman, *m_node.connman));

        // Anchor the mocked clock so SetMockTime advances are deterministic.
        SetMockTime(1'700'000'000s);
    }
};

// Replaces the per-type loop in test/functional/p2p_governance_invs.py: an
// inv hash is recorded by ConfirmInventoryRequest, deduplicated while valid,
// and purged by CheckAndRemove only after the reliable propagation timeout.
void CheckInvExpirationCycle(CGovernanceManager& govman, const CInv& inv)
{
    BOOST_CHECK_EQUAL(govman.RequestedHashCacheSizeForTesting(), 0U);

    // First inv is recorded.
    BOOST_CHECK(govman.ConfirmInventoryRequest(inv));
    BOOST_CHECK_EQUAL(govman.RequestedHashCacheSizeForTesting(), 1U);

    // Duplicate inv before expiry does not re-insert.
    BOOST_CHECK(govman.ConfirmInventoryRequest(inv));
    BOOST_CHECK_EQUAL(govman.RequestedHashCacheSizeForTesting(), 1U);

    // Cleanup before the reliable propagation timeout must not expire the entry.
    govman.CheckAndRemove();
    BOOST_CHECK_EQUAL(govman.RequestedHashCacheSizeForTesting(), 1U);

    // Still recorded -> another inv for the same hash is treated as a duplicate.
    BOOST_CHECK(govman.ConfirmInventoryRequest(inv));
    BOOST_CHECK_EQUAL(govman.RequestedHashCacheSizeForTesting(), 1U);

    // Advance past the reliable propagation timeout and clean: the entry is evicted.
    SetMockTime(GetTime<std::chrono::seconds>() + governance::RELIABLE_PROPAGATION_TIME + 1s);
    govman.CheckAndRemove();
    BOOST_CHECK_EQUAL(govman.RequestedHashCacheSizeForTesting(), 0U);

    // After eviction the same inv must be accepted and recorded again.
    BOOST_CHECK(govman.ConfirmInventoryRequest(inv));
    BOOST_CHECK_EQUAL(govman.RequestedHashCacheSizeForTesting(), 1U);
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(governance_inv_tests, GovernanceInvSetup)

BOOST_AUTO_TEST_CASE(object_inv_request_expiration)
{
    CheckInvExpirationCycle(*m_node.govman, CInv{MSG_GOVERNANCE_OBJECT, uint256S("01")});
}

BOOST_AUTO_TEST_CASE(vote_inv_request_expiration)
{
    CheckInvExpirationCycle(*m_node.govman, CInv{MSG_GOVERNANCE_OBJECT_VOTE, uint256S("02")});
}

BOOST_AUTO_TEST_CASE(inv_request_cache_prunes_after_clock_rollback)
{
    const auto initial_time = GetTime<std::chrono::seconds>();
    const uint256 older_hash = uint256S("03");
    const uint256 newer_hash = uint256S("04");

    BOOST_REQUIRE(m_node.govman->ConfirmInventoryRequest(CInv{MSG_GOVERNANCE_OBJECT, older_hash}));

    // A request inserted after a wall-clock rollback expires before the older
    // insertion, so expiration order no longer matches CacheMap order.
    SetMockTime(initial_time - 30s);
    BOOST_REQUIRE(m_node.govman->ConfirmInventoryRequest(CInv{MSG_GOVERNANCE_OBJECT, newer_hash}));

    SetMockTime(initial_time + 31s);
    m_node.govman->CheckAndRemove();

    BOOST_CHECK_EQUAL(m_node.govman->RequestedHashCacheSizeForTesting(), 1U);
    BOOST_CHECK(m_node.govman->AcceptMessage(older_hash));
    BOOST_CHECK(!m_node.govman->AcceptMessage(newer_hash));
}

BOOST_AUTO_TEST_CASE(inv_request_cache_is_bounded)
{
    const size_t max_size = m_node.govman->RequestedHashCacheMaxSizeForTesting();
    BOOST_REQUIRE_GT(max_size, 0U);

    for (size_t i = 0; i < max_size; ++i) {
        const auto hash = ArithToUint256(arith_uint256{i + 100});
        BOOST_REQUIRE(m_node.govman->ConfirmInventoryRequest(CInv{MSG_GOVERNANCE_OBJECT, hash}));
    }
    BOOST_CHECK_EQUAL(m_node.govman->RequestedHashCacheSizeForTesting(), max_size);

    // Duplicate hash does not grow the cache.
    const CInv duplicate_inv{MSG_GOVERNANCE_OBJECT, ArithToUint256(arith_uint256{100})};
    BOOST_CHECK(m_node.govman->ConfirmInventoryRequest(duplicate_inv));
    BOOST_CHECK_EQUAL(m_node.govman->RequestedHashCacheSizeForTesting(), max_size);

    // Saturation must not cause AlreadyHave to suppress a new honest hash. The
    // over-limit hash is admitted (return true) and the oldest tracked hash is
    // evicted to keep the cache bounded at max_size.
    const CInv over_limit_inv{MSG_GOVERNANCE_OBJECT, ArithToUint256(arith_uint256{max_size + 100})};
    BOOST_CHECK(m_node.govman->ConfirmInventoryRequest(over_limit_inv));
    BOOST_CHECK_EQUAL(m_node.govman->RequestedHashCacheSizeForTesting(), max_size);

    SetMockTime(GetTime<std::chrono::seconds>() + governance::RELIABLE_PROPAGATION_TIME + 1s);

    const CInv after_expiry_inv{MSG_GOVERNANCE_OBJECT, ArithToUint256(arith_uint256{max_size + 101})};
    BOOST_CHECK(m_node.govman->ConfirmInventoryRequest(after_expiry_inv));
    BOOST_CHECK_EQUAL(m_node.govman->RequestedHashCacheSizeForTesting(), 1U);
}

// Guards against an eclipse where one peer saturates the request cache with
// 50k announcements and AlreadyHave would then suppress every subsequent
// honest INV. After the cache is full, a new hash must still be admitted
// (ConfirmInventoryRequest returns true -> AlreadyHave returns false), the
// oldest tracked hash is evicted, and total memory stays bounded at max_size.
BOOST_AUTO_TEST_CASE(inv_request_cache_preserves_liveness_under_saturation)
{
    const size_t max_size = m_node.govman->RequestedHashCacheMaxSizeForTesting();
    BOOST_REQUIRE_GT(max_size, 0U);

    // Simulate an attacker saturating the cache with fresh, distinct hashes.
    const uint256 oldest_hash = ArithToUint256(arith_uint256{1});
    BOOST_REQUIRE(m_node.govman->ConfirmInventoryRequest(CInv{MSG_GOVERNANCE_OBJECT, oldest_hash}));
    for (size_t i = 1; i < max_size; ++i) {
        const auto hash = ArithToUint256(arith_uint256{i + 1});
        BOOST_REQUIRE(m_node.govman->ConfirmInventoryRequest(CInv{MSG_GOVERNANCE_OBJECT, hash}));
    }
    BOOST_REQUIRE_EQUAL(m_node.govman->RequestedHashCacheSizeForTesting(), max_size);

    // Honest peer announces a brand new hash while the cache is saturated: it
    // must be admitted so we can request it from that peer, and the cache
    // must not exceed its bound.
    const CInv honest_inv{MSG_GOVERNANCE_OBJECT, ArithToUint256(arith_uint256{max_size + 1000})};
    BOOST_CHECK(m_node.govman->ConfirmInventoryRequest(honest_inv));
    BOOST_CHECK_EQUAL(m_node.govman->RequestedHashCacheSizeForTesting(), max_size);

    // AcceptMessage returns true iff the hash is currently tracked in the
    // request cache. Use it as an oracle to prove FIFO eviction actually
    // ran on the oldest entry and left the fresh honest entry intact.
    BOOST_CHECK(!m_node.govman->AcceptMessage(oldest_hash));
    BOOST_CHECK_EQUAL(m_node.govman->RequestedHashCacheSizeForTesting(), max_size);
    BOOST_CHECK(m_node.govman->AcceptMessage(honest_inv.hash));
    BOOST_CHECK_EQUAL(m_node.govman->RequestedHashCacheSizeForTesting(), max_size - 1);

    // Vote INVs must also remain admissible under object-INV saturation.
    const CInv honest_vote{MSG_GOVERNANCE_OBJECT_VOTE, ArithToUint256(arith_uint256{max_size + 2000})};
    BOOST_CHECK(m_node.govman->ConfirmInventoryRequest(honest_vote));
    BOOST_CHECK(m_node.govman->AcceptMessage(honest_vote.hash));
}

// Guards eviction ordering across an accept-then-reannounce sequence. A hash is
// tracked, consumed by AcceptMessage, then re-announced so it becomes one of the
// newest entries. Because CacheMap keeps its index and ordering list in lockstep
// on every erase, the re-announced entry sits at the front and a later eviction
// must drop a truly-oldest entry instead of the freshly re-announced one.
BOOST_AUTO_TEST_CASE(inv_request_cache_eviction_survives_accept_then_reannounce)
{
    const size_t max_size = m_node.govman->RequestedHashCacheMaxSizeForTesting();
    BOOST_REQUIRE_GT(max_size, 2U);

    // Track a hash; filling the rest below leaves it as the oldest entry.
    const uint256 reannounced = ArithToUint256(arith_uint256{7});
    BOOST_REQUIRE(m_node.govman->ConfirmInventoryRequest(CInv{MSG_GOVERNANCE_OBJECT, reannounced}));

    // Fill the rest of the cache so the next new hash triggers eviction.
    for (size_t i = 1; i < max_size; ++i) {
        BOOST_REQUIRE(m_node.govman->ConfirmInventoryRequest(CInv{MSG_GOVERNANCE_OBJECT,
            ArithToUint256(arith_uint256{i + 100})}));
    }
    BOOST_REQUIRE_EQUAL(m_node.govman->RequestedHashCacheSizeForTesting(), max_size);

    // AcceptMessage the hash (this is what NetGovernance does after successfully
    // receiving the object/vote). It removes the entry from index and order
    // together, so no stale ordering slot can linger.
    BOOST_REQUIRE(m_node.govman->AcceptMessage(reannounced));
    BOOST_REQUIRE_EQUAL(m_node.govman->RequestedHashCacheSizeForTesting(), max_size - 1);

    // Re-announcing `reannounced` re-inserts it as one of the newest entries
    // with a fresh valid_until.
    BOOST_CHECK(m_node.govman->ConfirmInventoryRequest(CInv{MSG_GOVERNANCE_OBJECT, reannounced}));
    BOOST_CHECK_EQUAL(m_node.govman->RequestedHashCacheSizeForTesting(), max_size);

    // A brand new hash now forces eviction. The truly-oldest tracked entry must
    // be evicted, leaving the freshly re-announced entry intact.
    const CInv new_inv{MSG_GOVERNANCE_OBJECT, ArithToUint256(arith_uint256{max_size + 500})};
    BOOST_CHECK(m_node.govman->ConfirmInventoryRequest(new_inv));
    BOOST_CHECK_EQUAL(m_node.govman->RequestedHashCacheSizeForTesting(), max_size);
    BOOST_CHECK(m_node.govman->AcceptMessage(reannounced));
    BOOST_CHECK(m_node.govman->AcceptMessage(new_inv.hash));
}

// Replaces the end-to-end check the old functional test performed via real P2P:
// a governance INV delivered to PeerManager::ProcessMessage must reach
// CGovernanceManager::ConfirmInventoryRequest through PeerManagerImpl::AlreadyHave
// and the registered NetGovernance handler. Exercising the full inbound INV path
// keeps the wiring from regressing if PeerManager's dispatch ever changes.
BOOST_AUTO_TEST_CASE(peerman_inv_routes_to_governance_request_cache)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);

    in_addr peer_in_addr{};
    peer_in_addr.s_addr = htonl(0x01020304);
    CNode peer{/*id=*/0,
               /*sock=*/nullptr,
               /*addrIn=*/CAddress{CService{peer_in_addr, 8333}, NODE_NETWORK},
               /*nKeyedNetGroupIn=*/0,
               /*nLocalHostNonceIn=*/0,
               /*addrBindIn=*/CAddress{},
               /*addrNameIn=*/std::string{},
               /*conn_type_in=*/ConnectionType::INBOUND,
               /*inbound_onion=*/false};
    peer.nVersion = PROTOCOL_VERSION;
    peer.SetCommonVersion(PROTOCOL_VERSION);
    m_node.peerman->InitializeNode(peer, NODE_NETWORK);
    peer.fSuccessfullyConnected = true;

    auto make_inv_stream = [](const CInv& inv) {
        CDataStream s{SER_NETWORK, PROTOCOL_VERSION};
        s << std::vector<CInv>{inv};
        return s;
    };

    BOOST_CHECK_EQUAL(m_node.govman->RequestedHashCacheSizeForTesting(), 0U);

    const std::atomic<bool> interrupt_dummy{false};

    // Object INV: PeerManager -> AlreadyHave -> NetGovernance -> ConfirmInventoryRequest.
    {
        const CInv inv{MSG_GOVERNANCE_OBJECT, uint256S("06")};
        auto stream = make_inv_stream(inv);
        m_node.peerman->ProcessMessage(peer, NetMsgType::INV, stream,
                                       /*time_received=*/std::chrono::microseconds{0},
                                       interrupt_dummy);
        BOOST_CHECK_EQUAL(m_node.govman->RequestedHashCacheSizeForTesting(), 1U);

        // Duplicate INV with the same hash must not grow the cache.
        auto dup_stream = make_inv_stream(inv);
        m_node.peerman->ProcessMessage(peer, NetMsgType::INV, dup_stream,
                                       std::chrono::microseconds{0}, interrupt_dummy);
        BOOST_CHECK_EQUAL(m_node.govman->RequestedHashCacheSizeForTesting(), 1U);
    }

    // Vote INV travels the same path and adds a separate entry.
    {
        const CInv vote_inv{MSG_GOVERNANCE_OBJECT_VOTE, uint256S("07")};
        auto stream = make_inv_stream(vote_inv);
        m_node.peerman->ProcessMessage(peer, NetMsgType::INV, stream,
                                       std::chrono::microseconds{0}, interrupt_dummy);
        BOOST_CHECK_EQUAL(m_node.govman->RequestedHashCacheSizeForTesting(), 2U);
    }

    m_node.peerman->FinalizeNode(peer);
}

// Pins the periodic-cleanup wiring the deleted functional test exercised via
// node.mockscheduler: NetGovernance::Schedule queues a task that calls
// CGovernanceManager::CheckAndRemove, so an expired inv request is purged
// without any manual CheckAndRemove call.
BOOST_AUTO_TEST_CASE(net_governance_schedule_drives_check_and_remove)
{
    // NetGovernance::Schedule's periodic callback short-circuits on
    // !m_node_sync.IsSynced(); advance from GOVERNANCE to FINISHED.
    m_node.mn_sync->SwitchToNextAsset();
    BOOST_REQUIRE(m_node.mn_sync->IsSynced());

    // Pre-load an entry that has already passed the reliable propagation timeout so
    // the very next CheckAndRemove evicts it.
    const CInv inv{MSG_GOVERNANCE_OBJECT, uint256S("05")};
    BOOST_REQUIRE(m_node.govman->ConfirmInventoryRequest(inv));
    BOOST_CHECK_EQUAL(m_node.govman->RequestedHashCacheSizeForTesting(), 1U);
    SetMockTime(GetTime<std::chrono::seconds>() + governance::RELIABLE_PROPAGATION_TIME + 1s);

    // Drive a dedicated scheduler so the assertion is independent of
    // m_node.scheduler's existing workload.
    CScheduler scheduler;
    NetGovernance net_gov(m_node.peerman.get(), *m_node.govman, *m_node.mn_sync,
                          *m_node.netfulfilledman, *m_node.connman);
    net_gov.Schedule(scheduler);
    std::thread worker([&] { scheduler.serviceQueue(); });

    // First periodic fire is at +5min; bump the clock so the queue is ready.
    scheduler.MockForward(std::chrono::minutes{5});

    // Queue a stop marker after the mocked-forward tasks; due cleanup runs first.
    scheduler.scheduleFromNow([&scheduler] { scheduler.stop(); }, 1ms);
    worker.join();

    BOOST_CHECK_EQUAL(m_node.govman->RequestedHashCacheSizeForTesting(), 0U);
}

BOOST_AUTO_TEST_SUITE_END()
