// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/masternode.h>
#include <test/util/setup_common.h>

#include <bls/bls.h>
#include <chainparams.h>
#include <evo/chainhelper.h>
#include <evo/deterministicmns.h>
#include <governance/governance.h>
#include <governance/object.h>
#include <masternode/sync.h>
#include <netfulfilledman.h>
#include <script/standard.h>
#include <util/std23.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <string>
#include <vector>

// Fixture: a chain with DIP3 active, one registered masternode whose operator BLS
// key we hold, a governance manager wired the way production init does, and
// CMasternodeSync driven to FINISHED (MasternodeRateCheck is a no-op until then).
//
// The DIP3 activation height is pushed to 109 so TestChainSetup's fixed-checkpoint
// assert still succeeds while the early coinbases are mature.
struct FailedTriggerRateSetup : public TestChainSetup {
    COutPoint mn_outpoint;
    CBLSSecretKey operator_key;

    FailedTriggerRateSetup() :
        TestChainSetup(/*num_blocks=*/107, CBaseChainParams::REGTEST, {"-dip3params=109:500"})
    {
        auto& chainman = *Assert(m_node.chainman.get());
        auto& dmnman = *Assert(m_node.dmnman);
        const CScript coinbase_pk = GetScriptForRawPubKey(coinbaseKey.GetPubKey());

        // Activate DIP3 (fixture tip is one block before activation height 109).
        // Enforcement is independent of activation; ProRegTx only needs activation.
        CreateAndProcessBlock({}, coinbase_pk);
        BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainman.ActiveChain().Height()), 108);

        auto utxos = BuildSimpleUtxoMap(m_coinbase_txns);
        CKey owner_key;
        auto proreg_tx = CreateProRegTx(chainman, utxos, /*port=*/1, GenerateRandomAddress(), coinbaseKey, owner_key,
                                        operator_key);
        CreateAndProcessBlock({proreg_tx}, coinbase_pk);
        dmnman.UpdatedBlockTip(WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()));

        mn_outpoint = COutPoint{proreg_tx.GetHash(), 0};
        const auto dmn = dmnman.GetListAtChainTip().GetMNByCollateral(mn_outpoint);
        BOOST_REQUIRE(dmn);
        BOOST_REQUIRE(dmn->pdmnState->pubKeyOperator.Get() == operator_key.GetPublicKey());

        BOOST_REQUIRE(m_node.mn_metaman);
        BOOST_REQUIRE(m_node.mn_sync);
        BOOST_REQUIRE(m_node.chain_helper);
        BOOST_REQUIRE(m_node.chain_helper->superblocks);
        m_node.govman = std::make_unique<CGovernanceManager>(*m_node.mn_metaman, *m_node.chainman,
                                                             *m_node.chain_helper->superblocks, *m_node.dmnman,
                                                             *m_node.mn_sync);
        BOOST_REQUIRE(m_node.govman->LoadCache(/*load_cache=*/false));

        // Advance BLOCKCHAIN -> GOVERNANCE -> FINISHED. SyncFinished requires a
        // loaded netfulfilledman.
        BOOST_REQUIRE(m_node.netfulfilledman);
        BOOST_REQUIRE(m_node.netfulfilledman->LoadCache(/*load_cache=*/false));
        m_node.mn_sync->SwitchToNextAsset();
        m_node.mn_sync->SwitchToNextAsset();
        BOOST_REQUIRE(m_node.mn_sync->IsSynced());
    }

    ~FailedTriggerRateSetup()
    {
        // chain_helper (owner of the SuperblockManager govman clears in its dtor)
        // must outlive govman -- see PrepareShutdown in init.cpp.
        m_node.govman.reset();
    }

    // Malformed trigger JSON: type=TRIGGER so LoadData/IsValidLocally accept it, but
    // SuperblockManager::AddTrigger fails while constructing CSuperblock (missing
    // event_block_height / payment fields). That is the failed-trigger path.
    CGovernanceObject MakeFailedTrigger(int64_t creation_time, int salt) const
    {
        const std::string data = strprintf(R"({"type":2,"salt":%d})", salt);
        CGovernanceObject govobj{uint256{}, /*revision=*/1, creation_time, uint256{}, HexStr(data)};
        BOOST_REQUIRE_EQUAL(std23::to_underlying(govobj.GetObjectType()),
                            std23::to_underlying(GovernanceObject::TRIGGER));
        govobj.SetMasternodeOutpoint(mn_outpoint);
        const CBLSSignature sig = operator_key.Sign(govobj.GetSignatureHash(), /*specificLegacyScheme=*/false);
        BOOST_REQUIRE(sig.IsValid());
        govobj.SetSignature(sig.ToByteVector(/*specificLegacyScheme=*/false));
        return govobj;
    }
};

BOOST_FIXTURE_TEST_SUITE(governance_failed_trigger_rate_tests, FailedTriggerRateSetup)

// Regression: AddGovernanceObjectInternal used to return early on
// AddTrigger failure without calling MasternodeRateUpdate. Because
// MasternodeRateCheck short-circuits when mapLastMasternodeObject has no entry,
// a single operator key could flood mapObjects with unparseable triggers.
// After the fix, failed triggers still advance the per-masternode rate buffer
// so the flood is throttled.
BOOST_AUTO_TEST_CASE(failed_trigger_path_advances_masternode_rate_limit)
{
    // Deterministic clock inside the allowed rate-check timestamp window.
    SetMockTime(1'700'000'000s);
    const int64_t base_time = GetTime<std::chrono::seconds>().count();

    // Submit more failed triggers than RATE_BUFFER_SIZE from the same MN.
    // Pre-fix the rate buffer never fills, so every object lands in mapObjects.
    // Post-fix the buffer fills and further submissions are rate-rejected.
    constexpr int flood_count = 12;
    int objects_seen = 0;
    for (int i = 0; i < flood_count; ++i) {
        CGovernanceObject govobj = MakeFailedTrigger(base_time + i, /*salt=*/i);
        const uint256 hash = govobj.GetHash();
        // Unique per-object: salt + time vary both the data and the creation time.
        BOOST_REQUIRE(!m_node.govman->HaveObjectForHash(hash));

        LOCK(::cs_main);
        // ProcessObject returns true for both accepted and rate-rejected objects
        // (rate rejection is not a peer-misbehaviour event).
        BOOST_CHECK(m_node.govman->ProcessObject(/*peer_str=*/"test-peer", hash, govobj));

        if (m_node.govman->HaveObjectForHash(hash)) {
            ++objects_seen;
            // Failed triggers are accepted into mapObjects then marked for deletion.
            const auto stored = m_node.govman->FindGovernanceObject(hash);
            BOOST_REQUIRE(stored);
            BOOST_CHECK(stored->IsSetCachedDelete());
        }
    }

    // With the rate buffer size of 5, a fixed limiter records the first five
    // failed attempts and rejects the rest before insertion. Without the fix
    // every object is inserted (objects_seen == flood_count).
    BOOST_CHECK_LT(objects_seen, flood_count);
    BOOST_CHECK_LE(objects_seen, RATE_BUFFER_SIZE + 1);

    const UniValue stats = m_node.govman->ToJson();
    BOOST_CHECK_LT(stats["objects_total"].getInt<int>(), flood_count);
    BOOST_CHECK_EQUAL(stats["objects_total"].getInt<int>(), objects_seen);

    // A trigger we undid must never be announced to peers.
    BOOST_CHECK(m_node.govman->FetchRelayInventory().empty());
}

// A failed trigger must not be scheduled for the deferred "additional relay"
// pass. That bookkeeping (setAdditionalRelayObjects) fires for creation times
// close to MAX_TIME_FUTURE_DEVIATION, and used to live inside
// MasternodeRateUpdate() -- so counting the failed attempt against the rate
// buffer would otherwise make the node re-announce, and serve on GETDATA, an
// object it had just rejected and marked for deletion. One signature-valid
// message would then fan out to every peer, which in turn re-announce it.
BOOST_AUTO_TEST_CASE(failed_trigger_is_not_scheduled_for_additional_relay)
{
    SetMockTime(1'700'000'000s);
    const int64_t now = GetTime<std::chrono::seconds>().count();

    // Inside (now + MAX_TIME_FUTURE_DEVIATION - RELIABLE_PROPAGATION_TIME,
    // now + MAX_TIME_FUTURE_DEVIATION], i.e. accepted by MasternodeRateCheck but
    // "too new to propagate reliably", which is what arms the additional relay.
    const int64_t near_future = now + 3550;
    CGovernanceObject govobj = MakeFailedTrigger(near_future, /*salt=*/0);
    const uint256 hash = govobj.GetHash();

    WITH_LOCK(::cs_main, BOOST_CHECK(m_node.govman->ProcessObject(/*peer_str=*/"test-peer", hash, govobj)));

    // The object is stored (AddTrigger failure happens after insertion) but is
    // immediately marked for deletion, so it is not syncable...
    BOOST_REQUIRE(m_node.govman->HaveObjectForHash(hash));
    BOOST_CHECK(!m_node.govman->HaveSyncableObjectForHash(hash));
    // ...and nothing was queued for relay on the accept path either.
    BOOST_CHECK(m_node.govman->FetchRelayInventory().empty());

    // Move past RELIABLE_PROPAGATION_TIME so the deferred relay would be "ready",
    // then run the pass. UpdatedBlockTip -> CheckPostponedObjects drains
    // setAdditionalRelayObjects.
    SetMockTime(std::chrono::seconds{now + 120});
    m_node.govman->UpdatedBlockTip(WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip()));

    const auto invs = m_node.govman->FetchRelayInventory();
    BOOST_CHECK(std::ranges::none_of(invs, [&](const CInv& inv) { return inv.hash == hash; }));
    BOOST_CHECK(invs.empty());
}

BOOST_AUTO_TEST_SUITE_END()
