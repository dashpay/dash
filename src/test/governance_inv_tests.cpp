// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls/bls.h>
#include <chainparams.h>
#include <common/bloom.h>
#include <evo/chainhelper.h>
#include <evo/deterministicmns.h>
#include <governance/governance.h>
#include <governance/net_governance.h>
#include <governance/object.h>
#include <masternode/meta.h>
#include <masternode/sync.h>
#include <net.h>
#include <net_processing.h>
#include <netfulfilledman.h>
#include <node/connection_types.h>
#include <protocol.h>
#include <script/standard.h>
#include <streams.h>
#include <uint256.h>
#include <util/std23.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>
#include <version.h>

#include <test/util/masternode.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {
// Unified governance unit-test fixture. The DIP3 / ProRegTx path is required for
// signed-trigger rate regressions, and it also satisfies the lighter INV/vote
// authorization tests (govman + NetGovernance handler + mn_sync).
//
// DIP3 activation is pushed to 109 so TestChainSetup's fixed-checkpoint assert
// still succeeds while the early coinbases are mature.
struct GovernanceInvSetup : public TestChainSetup {
    COutPoint mn_outpoint;
    CBLSSecretKey operator_key;

    GovernanceInvSetup() :
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
        // Note: mn_metaman is left unloaded. Reaching CGovernanceObject::ProcessVote asserts
        // metaman.IsValid(), so a test that delivers a vote whose parent object exists must
        // load it first (see invalid_vote_is_scored_alike_with_and_without_a_parent_object).
        BOOST_REQUIRE(m_node.mn_sync);
        BOOST_REQUIRE(m_node.chain_helper);
        BOOST_REQUIRE(m_node.chain_helper->superblocks);
        m_node.govman = std::make_unique<CGovernanceManager>(*m_node.mn_metaman, *m_node.chainman,
                                                             *m_node.chain_helper->superblocks, *m_node.dmnman,
                                                             *m_node.mn_sync);
        // Match runtime preconditions: NetGovernance::AlreadyHave claims we
        // already have the inv when governance isn't loaded (e.g.
        // -disablegovernance), so ConfirmInventoryRequest would never run.
        BOOST_REQUIRE(m_node.govman->LoadCache(/*load_cache=*/false));

        BOOST_REQUIRE(m_node.netfulfilledman);
        // Loaded before advancing sync: SyncFinished asserts netfulfilledman.IsValid().
        BOOST_REQUIRE(m_node.netfulfilledman->LoadCache(/*load_cache=*/false));
        BOOST_REQUIRE(m_node.connman);
        BOOST_REQUIRE(m_node.peerman);

        // Advance BLOCKCHAIN -> GOVERNANCE -> FINISHED. Rate checks are a no-op until
        // IsSynced(); INV/vote tests also tolerate a fully-synced start state.
        m_node.mn_sync->SwitchToNextAsset();
        m_node.mn_sync->SwitchToNextAsset();
        BOOST_REQUIRE(m_node.mn_sync->IsSynced());

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

        // Deterministic clock for object/vote timestamps and rate-check windows.
        SetMockTime(0s);
    }

    ~GovernanceInvSetup()
    {
        // govman holds a reference to chain_helper->superblocks, so it must be reset
        // before chain_helper (matches PrepareShutdown ordering in init.cpp).
        m_node.peerman->RemoveHandlers();
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

size_t CountQueuedMessages(const CNode& peer, const std::string& msg_type)
{
    LOCK(peer.cs_vSend);
    size_t count{0};
    for (const auto& msg : peer.vSendMsg) {
        if (msg.m_type == msg_type) {
            ++count;
        }
    }
    return count;
}

size_t CountQueuedInventory(const CNode& peer, const CInv& expected_inv)
{
    LOCK(peer.cs_vSend);
    size_t count{0};
    for (const auto& msg : peer.vSendMsg) {
        if (msg.m_type != NetMsgType::INV) {
            continue;
        }
        CDataStream stream{msg.data, SER_NETWORK, PROTOCOL_VERSION};
        std::vector<CInv> invs;
        stream >> invs;
        for (const auto& inv : invs) {
            if (inv.type == expected_inv.type && inv.hash == expected_inv.hash) {
                ++count;
            }
        }
    }
    return count;
}

std::unique_ptr<CNode> MakeGovernanceInvPeer(NodeId id)
{
    in_addr peer_in_addr{};
    peer_in_addr.s_addr = htonl(0x01020305 + id);
    auto peer{std::make_unique<CNode>(id,
                                      /*sock=*/nullptr,
                                      /*addrIn=*/CAddress{CService{peer_in_addr, 8333}, NODE_NETWORK},
                                      /*nKeyedNetGroupIn=*/0,
                                      /*nLocalHostNonceIn=*/0,
                                      /*addrBindIn=*/CAddress{},
                                      /*addrNameIn=*/std::string{},
                                      /*conn_type_in=*/ConnectionType::INBOUND,
                                      /*inbound_onion=*/false)};
    peer->nVersion = PROTOCOL_VERSION;
    peer->SetCommonVersion(PROTOCOL_VERSION);
    peer->fSuccessfullyConnected = true;
    return peer;
}

void ProcessInv(PeerManager& peerman, CNode& peer, const CInv& inv)
    EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex)
{
    CDataStream inv_stream{SER_NETWORK, PROTOCOL_VERSION};
    inv_stream << std::vector<CInv>{inv};
    std::atomic<bool> interrupt_dummy{false};
    peerman.ProcessMessage(peer, NetMsgType::INV, inv_stream, GetTime<std::chrono::microseconds>(), interrupt_dummy);
}

CGovernanceObject MakeGovernanceObject(int64_t creation_time, const uint256& collateral_hash)
{
    const std::string data{
        R"({"type":1,"name":"proposal","start_epoch":1700000000,"end_epoch":1700100000,"payment_amount":1.0,"payment_address":"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwV","url":"https://dash.org"})"};
    return CGovernanceObject{uint256{}, /*revision=*/1, creation_time, collateral_hash, HexStr(data)};
}

void ProcessGovernanceObject(NetGovernance& net_gov, CNode& peer, const CGovernanceObject& govobj)
{
    CDataStream object_stream{SER_NETWORK, PROTOCOL_VERSION};
    object_stream << govobj;
    net_gov.ProcessMessage(peer, NetMsgType::MNGOVERNANCEOBJECT, object_stream);
}

CGovernanceVote MakeGovernanceVote(const uint256& parent_hash)
{
    CGovernanceVote vote{COutPoint{uint256S("11"), 1}, parent_hash, VOTE_SIGNAL_FUNDING, VOTE_OUTCOME_YES};
    vote.SetTime(GetTime<std::chrono::seconds>().count());
    vote.SetSignature(std::vector<unsigned char>(CGovernanceVote::COMPACT_SIG_SIZE));
    return vote;
}

void ProcessGovernanceVote(NetGovernance& net_gov, CNode& peer, const CGovernanceVote& vote)
{
    CDataStream vote_stream{SER_NETWORK, PROTOCOL_VERSION};
    vote_stream << vote;
    net_gov.ProcessMessage(peer, NetMsgType::MNGOVERNANCEOBJECTVOTE, vote_stream);
}

void AssertMisbehaviorScore(PeerManager& peerman, const CNode& peer, int expected)
{
    CNodeStateStats stats;
    BOOST_REQUIRE(peerman.GetNodeStateStats(peer.GetId(), stats));
    BOOST_CHECK_EQUAL(stats.m_misbehavior_score, expected);
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(governance_inv_tests, GovernanceInvSetup)

BOOST_AUTO_TEST_CASE(per_object_vote_sync_is_fulfilled_request_limited)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);

    // NetGovernance::ProcessMessage ignores MNGOVERNANCESYNC until sync is
    // fully finished; the fixture already advances to FINISHED.
    BOOST_REQUIRE(m_node.mn_sync->IsSynced());

    auto peer{MakeGovernanceInvPeer(/*id=*/1)};
    m_node.peerman->InitializeNode(*peer, NODE_NETWORK);

    auto make_request_stream = [](const uint256& object_hash, const CBloomFilter& filter) {
        CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
        stream << object_hash << filter;
        return stream;
    };

    NetGovernance net_gov(m_node.peerman.get(), *m_node.govman, *m_node.mn_sync,
                          *m_node.netfulfilledman, *m_node.connman);
    auto& connman = static_cast<ConnmanTestMsg&>(*m_node.connman);
    AssertMisbehaviorScore(*m_node.peerman, *peer, 0);

    const CBloomFilter vote_filter{1, 0.001, 0, BLOOM_UPDATE_NONE};
    CGovernanceObject postponed_object{uint256(), /*revision=*/1, GetTime(), uint256::ONE, /*data=*/{}};
    m_node.govman->AddPostponedObject(postponed_object);

    const uint256 postponed_object_hash{postponed_object.GetHash()};
    const std::string postponed_vote_sync_request{strprintf("%s-votes-%s", NetMsgType::MNGOVERNANCESYNC,
                                                            postponed_object_hash.ToString())};
    auto postponed_stream = make_request_stream(postponed_object_hash, vote_filter);
    net_gov.ProcessMessage(*peer, NetMsgType::MNGOVERNANCESYNC, postponed_stream);

    BOOST_CHECK(!m_node.netfulfilledman->HasFulfilledRequest(peer->addr, postponed_vote_sync_request));
    AssertMisbehaviorScore(*m_node.peerman, *peer, 0);

    auto duplicate_postponed_stream = make_request_stream(postponed_object_hash, vote_filter);
    net_gov.ProcessMessage(*peer, NetMsgType::MNGOVERNANCESYNC, duplicate_postponed_stream);

    AssertMisbehaviorScore(*m_node.peerman, *peer, 0);

    CGovernanceObject syncable_object{uint256(), /*revision=*/1, GetTime() + 1, uint256S("02"), /*data=*/{}};
    m_node.govman->AddGovernanceObjectForTesting(syncable_object);

    const uint256 syncable_object_hash{syncable_object.GetHash()};
    const std::string syncable_vote_sync_request{strprintf("%s-votes-%s", NetMsgType::MNGOVERNANCESYNC,
                                                           syncable_object_hash.ToString())};
    BOOST_CHECK(!m_node.netfulfilledman->HasFulfilledRequest(peer->addr, syncable_vote_sync_request));

    connman.FlushSendBuffer(*peer);
    auto syncable_object_fetch_stream = make_request_stream(syncable_object_hash, CBloomFilter{});
    net_gov.ProcessMessage(*peer, NetMsgType::MNGOVERNANCESYNC, syncable_object_fetch_stream);

    BOOST_CHECK_EQUAL(CountQueuedInventory(*peer, CInv{MSG_GOVERNANCE_OBJECT, syncable_object_hash}), 1U);
    BOOST_CHECK(!m_node.netfulfilledman->HasFulfilledRequest(peer->addr, syncable_vote_sync_request));
    BOOST_CHECK_EQUAL(CountQueuedMessages(*peer, NetMsgType::SYNCSTATUSCOUNT), 0U);
    AssertMisbehaviorScore(*m_node.peerman, *peer, 0);

    connman.FlushSendBuffer(*peer);
    auto duplicate_syncable_object_fetch_stream = make_request_stream(syncable_object_hash, CBloomFilter{});
    net_gov.ProcessMessage(*peer, NetMsgType::MNGOVERNANCESYNC, duplicate_syncable_object_fetch_stream);

    BOOST_CHECK_EQUAL(CountQueuedInventory(*peer, CInv{MSG_GOVERNANCE_OBJECT, syncable_object_hash}), 1U);
    BOOST_CHECK(!m_node.netfulfilledman->HasFulfilledRequest(peer->addr, syncable_vote_sync_request));
    BOOST_CHECK_EQUAL(CountQueuedMessages(*peer, NetMsgType::SYNCSTATUSCOUNT), 0U);
    AssertMisbehaviorScore(*m_node.peerman, *peer, 0);

    connman.FlushSendBuffer(*peer);
    auto syncable_stream = make_request_stream(syncable_object_hash, vote_filter);
    net_gov.ProcessMessage(*peer, NetMsgType::MNGOVERNANCESYNC, syncable_stream);

    BOOST_CHECK(m_node.netfulfilledman->HasFulfilledRequest(peer->addr, syncable_vote_sync_request));
    BOOST_CHECK_EQUAL(CountQueuedMessages(*peer, NetMsgType::SYNCSTATUSCOUNT), 1U);
    AssertMisbehaviorScore(*m_node.peerman, *peer, 0);

    auto duplicate_syncable_stream = make_request_stream(syncable_object_hash, vote_filter);
    net_gov.ProcessMessage(*peer, NetMsgType::MNGOVERNANCESYNC, duplicate_syncable_stream);

    AssertMisbehaviorScore(*m_node.peerman, *peer, 20);

    connman.FlushSendBuffer(*peer);
    CGovernanceObject empty_filter_object{uint256(), /*revision=*/1, GetTime() + 2, uint256S("03"), /*data=*/{}};
    m_node.govman->AddPostponedObject(empty_filter_object);
    const uint256 empty_filter_object_hash{empty_filter_object.GetHash()};
    const std::string empty_filter_vote_request{strprintf("%s-votes-%s", NetMsgType::MNGOVERNANCESYNC,
                                                          empty_filter_object_hash.ToString())};
    auto empty_filter_stream = make_request_stream(empty_filter_object_hash, CBloomFilter{});
    net_gov.ProcessMessage(*peer, NetMsgType::MNGOVERNANCESYNC, empty_filter_stream);

    BOOST_CHECK_EQUAL(CountQueuedInventory(*peer, CInv{MSG_GOVERNANCE_OBJECT, empty_filter_object_hash}), 1U);
    BOOST_CHECK(!m_node.netfulfilledman->HasFulfilledRequest(peer->addr, empty_filter_vote_request));
    BOOST_CHECK_EQUAL(CountQueuedMessages(*peer, NetMsgType::SYNCSTATUSCOUNT), 0U);
    AssertMisbehaviorScore(*m_node.peerman, *peer, 20);

    connman.FlushSendBuffer(*peer);
    auto duplicate_empty_filter_stream = make_request_stream(empty_filter_object_hash, CBloomFilter{});
    net_gov.ProcessMessage(*peer, NetMsgType::MNGOVERNANCESYNC, duplicate_empty_filter_stream);

    BOOST_CHECK_EQUAL(CountQueuedInventory(*peer, CInv{MSG_GOVERNANCE_OBJECT, empty_filter_object_hash}), 1U);
    BOOST_CHECK(!m_node.netfulfilledman->HasFulfilledRequest(peer->addr, empty_filter_vote_request));
    BOOST_CHECK_EQUAL(CountQueuedMessages(*peer, NetMsgType::SYNCSTATUSCOUNT), 0U);
    AssertMisbehaviorScore(*m_node.peerman, *peer, 20);

    const uint256 unknown_vote_hash{uint256S("09")};
    const std::string unknown_vote_request{strprintf("%s-votes-%s", NetMsgType::MNGOVERNANCESYNC,
                                                     unknown_vote_hash.ToString())};
    auto unknown_vote_stream = make_request_stream(unknown_vote_hash, vote_filter);
    net_gov.ProcessMessage(*peer, NetMsgType::MNGOVERNANCESYNC, unknown_vote_stream);

    BOOST_CHECK(!m_node.netfulfilledman->HasFulfilledRequest(peer->addr, unknown_vote_request));
    AssertMisbehaviorScore(*m_node.peerman, *peer, 20);

    auto duplicate_unknown_vote_stream = make_request_stream(unknown_vote_hash, vote_filter);
    net_gov.ProcessMessage(*peer, NetMsgType::MNGOVERNANCESYNC, duplicate_unknown_vote_stream);

    BOOST_CHECK(!m_node.netfulfilledman->HasFulfilledRequest(peer->addr, unknown_vote_request));
    AssertMisbehaviorScore(*m_node.peerman, *peer, 20);

    const uint256 object_fetch_hash{uint256S("0a")};
    const std::string object_fetch_request{strprintf("%s-votes-%s", NetMsgType::MNGOVERNANCESYNC,
                                                     object_fetch_hash.ToString())};
    auto object_fetch_stream = make_request_stream(object_fetch_hash, CBloomFilter{});
    net_gov.ProcessMessage(*peer, NetMsgType::MNGOVERNANCESYNC, object_fetch_stream);

    BOOST_CHECK(!m_node.netfulfilledman->HasFulfilledRequest(peer->addr, object_fetch_request));
    AssertMisbehaviorScore(*m_node.peerman, *peer, 20);

    auto duplicate_object_fetch_stream = make_request_stream(object_fetch_hash, CBloomFilter{});
    net_gov.ProcessMessage(*peer, NetMsgType::MNGOVERNANCESYNC, duplicate_object_fetch_stream);

    BOOST_CHECK(!m_node.netfulfilledman->HasFulfilledRequest(peer->addr, object_fetch_request));
    AssertMisbehaviorScore(*m_node.peerman, *peer, 20);

    m_node.peerman->FinalizeNode(*peer);
}

BOOST_AUTO_TEST_CASE(governance_objects_require_peer_announcement_or_request)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);

    NetGovernance net_gov(m_node.peerman.get(), *m_node.govman, *m_node.mn_sync,
                          *m_node.netfulfilledman, *m_node.connman);

    auto announcing_peer{MakeGovernanceInvPeer(/*id=*/11)};
    auto second_announcing_peer{MakeGovernanceInvPeer(/*id=*/12)};
    auto unsolicited_peer{MakeGovernanceInvPeer(/*id=*/13)};
    m_node.peerman->InitializeNode(*announcing_peer, NODE_NETWORK);
    m_node.peerman->InitializeNode(*second_announcing_peer, NODE_NETWORK);
    m_node.peerman->InitializeNode(*unsolicited_peer, NODE_NETWORK);

    AssertMisbehaviorScore(*m_node.peerman, *announcing_peer, 0);

    const CGovernanceObject govobj{MakeGovernanceObject(GetTime<std::chrono::seconds>().count(), uint256S("21"))};
    const CInv object_inv{MSG_GOVERNANCE_OBJECT, govobj.GetHash()};

    ProcessInv(*m_node.peerman, *announcing_peer, object_inv);
    ProcessInv(*m_node.peerman, *second_announcing_peer, object_inv);
    // Pre-seed the object so ProcessObject short-circuits on "already seen" and the
    // acceptance gate is exercised in isolation from collateral/chain validation. Both
    // ProcessInv calls ran first, so the object was still absent at INV time and both
    // peers registered a real pending request in the net-layer tracker.
    m_node.govman->AddPostponedObject(govobj);

    // Announcing peer: the gate accepts (it announced the inv) and consumes its per-peer
    // request entry. Checking the entry is gone proves the gate actually consulted the
    // net-layer tracker, independent of the pre-seed above (a rejected object would leave
    // the entry untouched).
    ProcessGovernanceObject(net_gov, *announcing_peer, govobj);
    BOOST_CHECK(
        !WITH_LOCK(::cs_main, return m_node.peerman->PeerConsumeObjectRequest(announcing_peer->GetId(), object_inv)));
    AssertMisbehaviorScore(*m_node.peerman, *announcing_peer, 0);

    ProcessGovernanceObject(net_gov, *second_announcing_peer, govobj);
    // Consumption is per-peer: the second announcer's own entry is accepted and consumed,
    // independent of the first peer's already-consumed entry.
    BOOST_CHECK(!WITH_LOCK(::cs_main,
                           return m_node.peerman->PeerConsumeObjectRequest(second_announcing_peer->GetId(), object_inv)));
    AssertMisbehaviorScore(*m_node.peerman, *second_announcing_peer, 0);

    const CGovernanceObject unsolicited_govobj{MakeGovernanceObject(GetTime<std::chrono::seconds>().count() + 1, uint256S("22"))};
    ProcessGovernanceObject(net_gov, *unsolicited_peer, unsolicited_govobj);
    BOOST_CHECK(!m_node.govman->HaveObjectForHash(unsolicited_govobj.GetHash()));
    AssertMisbehaviorScore(*m_node.peerman, *unsolicited_peer, 0);

    m_node.peerman->FinalizeNode(*announcing_peer);
    m_node.peerman->FinalizeNode(*second_announcing_peer);
    m_node.peerman->FinalizeNode(*unsolicited_peer);
}

BOOST_AUTO_TEST_CASE(governance_votes_require_peer_announcement_or_request)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);

    NetGovernance net_gov(m_node.peerman.get(), *m_node.govman, *m_node.mn_sync,
                          *m_node.netfulfilledman, *m_node.connman);

    auto announcing_peer{MakeGovernanceInvPeer(/*id=*/21)};
    auto second_announcing_peer{MakeGovernanceInvPeer(/*id=*/22)};
    auto unsolicited_peer{MakeGovernanceInvPeer(/*id=*/23)};
    m_node.peerman->InitializeNode(*announcing_peer, NODE_NETWORK);
    m_node.peerman->InitializeNode(*second_announcing_peer, NODE_NETWORK);
    m_node.peerman->InitializeNode(*unsolicited_peer, NODE_NETWORK);

    // The vote below carries an unknown masternode outpoint, so CGovernanceManager::ProcessVote
    // rejects it with a penalty of 20. Only a peer that passes the announce-then-request gate
    // reaches ProcessVote at all, which makes the score the observable for the gate itself.
    // Penalties are applied only once fully synced.
    m_node.mn_sync->SwitchToNextAsset();
    BOOST_REQUIRE(m_node.mn_sync->IsSynced());

    const CGovernanceVote vote{MakeGovernanceVote(uint256S("31"))};
    const CInv vote_inv{MSG_GOVERNANCE_OBJECT_VOTE, vote.GetHash()};

    ProcessGovernanceVote(net_gov, *unsolicited_peer, vote);
    BOOST_CHECK_EQUAL(CountQueuedMessages(*unsolicited_peer, NetMsgType::MNGOVERNANCESYNC), 0U);
    AssertMisbehaviorScore(*m_node.peerman, *unsolicited_peer, 0);

    ProcessInv(*m_node.peerman, *announcing_peer, vote_inv);
    ProcessInv(*m_node.peerman, *second_announcing_peer, vote_inv);

    ProcessGovernanceVote(net_gov, *announcing_peer, vote);
    BOOST_CHECK(
        !WITH_LOCK(::cs_main, return m_node.peerman->PeerConsumeObjectRequest(announcing_peer->GetId(), vote_inv)));
    AssertMisbehaviorScore(*m_node.peerman, *announcing_peer, 20);

    ProcessGovernanceVote(net_gov, *second_announcing_peer, vote);
    // Consumption is per-peer: the second announcer's own entry is accepted and consumed,
    // independent of the first peer's already-consumed entry.
    BOOST_CHECK(!WITH_LOCK(::cs_main,
                           return m_node.peerman->PeerConsumeObjectRequest(second_announcing_peer->GetId(), vote_inv)));
    AssertMisbehaviorScore(*m_node.peerman, *second_announcing_peer, 20);

    m_node.peerman->FinalizeNode(*announcing_peer);
    m_node.peerman->FinalizeNode(*second_announcing_peer);
    m_node.peerman->FinalizeNode(*unsolicited_peer);
}

// A message received while not blockchain-synced is dropped, but the per-peer authorization must
// survive so a retransmit after sync is still accepted (the consume happens after the sync gate).
BOOST_AUTO_TEST_CASE(governance_vote_authorization_survives_unsynced_drop)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);

    NetGovernance net_gov(m_node.peerman.get(), *m_node.govman, *m_node.mn_sync,
                          *m_node.netfulfilledman, *m_node.connman);

    auto peer{MakeGovernanceInvPeer(/*id=*/31)};
    m_node.peerman->InitializeNode(*peer, NODE_NETWORK);

    const CGovernanceVote vote{MakeGovernanceVote(uint256S("41"))};
    const CInv vote_inv{MSG_GOVERNANCE_OBJECT_VOTE, vote.GetHash()};

    // Synced: announce the vote so we hold a per-peer request for it.
    BOOST_REQUIRE(m_node.mn_sync->IsBlockchainSynced());
    ProcessInv(*m_node.peerman, *peer, vote_inv);

    // Not synced: delivering the vote is dropped at the sync gate and must NOT consume the request.
    m_node.mn_sync->Reset(/*fForce=*/true, /*fNotifyReset=*/false);
    BOOST_REQUIRE(!m_node.mn_sync->IsBlockchainSynced());
    ProcessGovernanceVote(net_gov, *peer, vote);
    AssertMisbehaviorScore(*m_node.peerman, *peer, 0);

    // Back in sync, the retransmit is still authorized and reaches ProcessVote, which rejects the
    // unknown masternode outpoint with a penalty of 20. Had the unsynced drop consumed the request,
    // the gate would now reject the vote as unrequested and return before ProcessVote, leaving the
    // score at 0.
    while (!m_node.mn_sync->IsSynced()) {
        m_node.mn_sync->SwitchToNextAsset();
    }
    ProcessGovernanceVote(net_gov, *peer, vote);
    AssertMisbehaviorScore(*m_node.peerman, *peer, 20);

    m_node.peerman->FinalizeNode(*peer);
}

// A vote whose parent object is unknown must prove masternode authorship before it is cached.
BOOST_AUTO_TEST_CASE(orphan_votes_require_a_valid_masternode_signature)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);

    // Penalties are applied only once fully synced.
    BOOST_REQUIRE(m_node.mn_sync->IsSynced());

    NetGovernance net_gov(m_node.peerman.get(), *m_node.govman, *m_node.mn_sync,
                          *m_node.netfulfilledman, *m_node.connman);

    auto peer{MakeGovernanceInvPeer(/*id=*/41)};
    m_node.peerman->InitializeNode(*peer, NODE_NETWORK);
    auto& connman = static_cast<ConnmanTestMsg&>(*m_node.connman);

    // The tip masternode list is empty in this setup, so no vote can name a known collateral.
    const CGovernanceVote vote{MakeGovernanceVote(uint256S("51"))};
    const CInv vote_inv{MSG_GOVERNANCE_OBJECT_VOTE, vote.GetHash()};

    ProcessInv(*m_node.peerman, *peer, vote_inv);
    connman.FlushSendBuffer(*peer);
    ProcessGovernanceVote(net_gov, *peer, vote);

    BOOST_CHECK(m_node.govman->GetOrphanVoteObjectHashes().empty());
    BOOST_CHECK_EQUAL(CountQueuedMessages(*peer, NetMsgType::MNGOVERNANCESYNC), 0U);
    AssertMisbehaviorScore(*m_node.peerman, *peer, 20);

    m_node.peerman->FinalizeNode(*peer);
}

// The same unauthenticated vote must cost the sender the same whether or not its parent object
// happens to have arrived first.
BOOST_AUTO_TEST_CASE(invalid_vote_is_scored_alike_with_and_without_a_parent_object)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);

    BOOST_REQUIRE(m_node.mn_sync->IsSynced());
    // The known-object path runs CGovernanceObject::ProcessVote, which asserts metaman.IsValid().
    BOOST_REQUIRE(m_node.mn_metaman->LoadCache(/*load_cache=*/false));

    NetGovernance net_gov(m_node.peerman.get(), *m_node.govman, *m_node.mn_sync,
                          *m_node.netfulfilledman, *m_node.connman);

    auto orphan_peer{MakeGovernanceInvPeer(/*id=*/51)};
    auto known_parent_peer{MakeGovernanceInvPeer(/*id=*/52)};
    m_node.peerman->InitializeNode(*orphan_peer, NODE_NETWORK);
    m_node.peerman->InitializeNode(*known_parent_peer, NODE_NETWORK);

    const CGovernanceObject govobj{MakeGovernanceObject(GetTime<std::chrono::seconds>().count(), uint256S("61"))};
    const CGovernanceVote orphan_vote{MakeGovernanceVote(uint256S("62"))};
    const CGovernanceVote known_parent_vote{MakeGovernanceVote(govobj.GetHash())};

    ProcessInv(*m_node.peerman, *orphan_peer, CInv{MSG_GOVERNANCE_OBJECT_VOTE, orphan_vote.GetHash()});
    ProcessGovernanceVote(net_gov, *orphan_peer, orphan_vote);
    AssertMisbehaviorScore(*m_node.peerman, *orphan_peer, 20);

    m_node.govman->AddGovernanceObjectForTesting(govobj);
    ProcessInv(*m_node.peerman, *known_parent_peer, CInv{MSG_GOVERNANCE_OBJECT_VOTE, known_parent_vote.GetHash()});
    ProcessGovernanceVote(net_gov, *known_parent_peer, known_parent_vote);
    AssertMisbehaviorScore(*m_node.peerman, *known_parent_peer, 20);

    m_node.peerman->FinalizeNode(*orphan_peer);
    m_node.peerman->FinalizeNode(*known_parent_peer);
}

// Regression: AddGovernanceObjectInternal used to return early on
// AddTrigger failure without calling MasternodeRateUpdate. Because
// MasternodeRateCheck short-circuits when mapLastMasternodeObject has no entry,
// a single operator key could flood mapObjects with unparseable triggers.
// After the fix, failed triggers still advance the per-masternode rate buffer
// so the flood is throttled.
BOOST_AUTO_TEST_CASE(failed_trigger_path_advances_masternode_rate_limit)
{
    // Deterministic clock inside the allowed rate-check timestamp window.
    SetMockTime(0s);
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

// A failed trigger must not be scheduled for the deferred trigger-relay pass.
// That bookkeeping (setAdditionalRelayObjects) fires for creation times close
// to MAX_TIME_FUTURE_DEVIATION, and used to live inside MasternodeRateUpdate()
// -- so counting the failed attempt against the rate buffer would otherwise
// make the node re-announce, and serve on GETDATA, an object it had just
// rejected and marked for deletion. One signature-valid message would then fan
// out to every peer, which in turn re-announce it.
BOOST_AUTO_TEST_CASE(failed_trigger_is_not_scheduled_for_additional_relay)
{
    SetMockTime(0s);
    const int64_t now = GetTime<std::chrono::seconds>().count();

    // Inside (now + MAX_TIME_FUTURE_DEVIATION - RELIABLE_PROPAGATION_TIME,
    // now + MAX_TIME_FUTURE_DEVIATION], i.e. accepted by MasternodeRateCheck but
    // "too new to propagate reliably", which is what arms the trigger relay.
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
