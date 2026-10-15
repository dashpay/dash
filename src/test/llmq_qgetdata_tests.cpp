// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <evo/evodb.h>
#include <llmq/context.h>
#include <llmq/net_quorum.h>
#include <llmq/options_args.h>
#include <llmq/quorums.h>
#include <llmq/quorumsman.h>
#include <net.h>
#include <net_processing.h>
#include <node/connection_types.h>
#include <protocol.h>
#include <streams.h>
#include <uint256.h>
#include <validation.h>
#include <version.h>

#include <test/util/net.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <span>

using namespace llmq;

namespace {

//! Minimal QuorumRole so NetQuorum accepts QGETDATA as if we were a masternode.
struct MockMasternodeRole final : public QuorumRole {
    explicit MockMasternodeRole(CQuorumManager& qman) : QuorumRole(qman) {}
    bool IsMasternode() const override { return true; }
    bool IsWatching() const override { return false; }
    bool SetQuorumSecretKeyShare(CQuorum& /*quorum*/, Span<CBLSSecretKey> /*skContributions*/) const override
    {
        return false;
    }
};

struct QGetDataSetup : public TestingSetup {
    MockMasternodeRole m_role;
    std::unique_ptr<NetQuorum> m_net_quorum;

    QGetDataSetup() :
        TestingSetup{ChainType::REGTEST},
        m_role{*m_node.llmq_ctx->qman}
    {
        BOOST_REQUIRE(m_node.connman);
        BOOST_REQUIRE(m_node.peerman);
        BOOST_REQUIRE(m_node.dmnman);
        BOOST_REQUIRE(m_node.llmq_ctx);
        BOOST_REQUIRE(m_node.mn_sync);
        BOOST_REQUIRE(m_node.sporkman);
        BOOST_REQUIRE(m_node.chainman);

        // Mirror init.cpp: TestingSetup does not register NetQuorum, so install
        // one with a masternode role so the QGETDATA gate opens for qwatch peers.
        m_net_quorum = std::make_unique<NetQuorum>(
            m_node.peerman.get(), *m_node.llmq_ctx->bls_worker, *m_node.connman, *m_node.dmnman,
            *m_node.llmq_ctx->qman, *m_node.llmq_ctx->qsnapman, *m_node.chainman, *m_node.mn_sync,
            *m_node.sporkman, &m_role, /*nodeman=*/nullptr, DEFAULT_WORKER_COUNT, QvvecSyncModeMap{},
            /*quorums_recovery=*/false);
    }

    ~QGetDataSetup()
    {
        m_net_quorum.reset();
    }
};

std::unique_ptr<CNode> MakePeer(NodeId id)
{
    in_addr peer_in_addr{};
    peer_in_addr.s_addr = htonl(0x0a000001 + static_cast<uint32_t>(id));
    auto peer{std::make_unique<CNode>(id,
                                      /*sock=*/nullptr,
                                      /*addrIn=*/CAddress{CService{peer_in_addr, 9999}, NODE_NETWORK},
                                      /*nKeyedNetGroupIn=*/0,
                                      /*nLocalHostNonceIn=*/0,
                                      /*addrBindIn=*/CAddress{},
                                      /*addrNameIn=*/std::string{},
                                      /*conn_type_in=*/ConnectionType::INBOUND,
                                      /*inbound_onion=*/false)};
    peer->nVersion = PROTOCOL_VERSION;
    peer->SetCommonVersion(PROTOCOL_VERSION);
    peer->fSuccessfullyConnected = true;
    // Unauthenticated QWATCH path: any peer can set this flag and then send QGETDATA.
    peer->qwatch = true;
    return peer;
}

CDataStream MakeQGetDataStream(Consensus::LLMQType llmq_type, const uint256& quorum_hash, uint16_t data_mask,
                               const uint256& protx_hash)
{
    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    stream << static_cast<uint8_t>(llmq_type);
    stream << quorum_hash;
    stream << data_mask;
    stream << protx_hash;
    return stream;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(llmq_qgetdata_tests, QGetDataSetup)

// An unknown block hash can mean the requester is ahead of us or on another fork, so it
// is answered with QUORUM_BLOCK_NOT_FOUND without scoring — but it must never create a
// tracking-map entry, or distinct hashes would grow the map without bound.
BOOST_AUTO_TEST_CASE(qgetdata_unknown_block_not_scored_not_tracked)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);

    auto peer{MakePeer(/*id=*/2)};
    m_node.peerman->InitializeNode(*peer, NODE_NETWORK);
    BOOST_CHECK_EQUAL(MisbehaviorScore(*m_node.peerman, *peer), 0);

    const uint256 protx_hash{uint256S("0x44")};
    for (const auto& hash_str : {"0x33", "0x55"}) {
        const uint256 quorum_hash{uint256S(hash_str)};
        auto stream = MakeQGetDataStream(Consensus::LLMQType::LLMQ_TEST, quorum_hash,
                                         CQuorumDataRequest::QUORUM_VERIFICATION_VECTOR, protx_hash);
        m_net_quorum->ProcessMessage(*peer, NetMsgType::QGETDATA, stream);
        BOOST_CHECK_EQUAL(MisbehaviorScore(*m_node.peerman, *peer), 0);
        BOOST_CHECK(!m_node.llmq_ctx->qman->IsDataRequestPending(peer->GetVerifiedProRegTxHash(),
                                                                 /*we_requested=*/false, quorum_hash,
                                                                 Consensus::LLMQType::LLMQ_TEST));
    }
}

// An active-chain block without a mined commitment is answered with QUORUM_NOT_FOUND and
// the first request is not scored: the commitment is mined after the quorum base block,
// so we can know the base block while still lagging behind the commitment the requester
// saw. Unlike the unknown-block miss, the key is bounded by real chain blocks, so the
// request is registered BEFORE the commitment lookup: the miss is an uncached EvoDB read,
// and repeats within the expiry window must be rate-limited and scored instead of
// re-running it for free.
BOOST_AUTO_TEST_CASE(qgetdata_active_non_quorum_unscored_once_then_rate_limited)
{
    const ActiveEvoViewScope turn{m_node.evodb.get()};
    LOCK(NetEventsInterface::g_msgproc_mutex);

    auto peer{MakePeer(/*id=*/3)};
    m_node.peerman->InitializeNode(*peer, NODE_NETWORK);
    BOOST_CHECK_EQUAL(MisbehaviorScore(*m_node.peerman, *peer), 0);

    const uint256 quorum_hash{
        WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Genesis()->GetBlockHash())};
    const uint256 protx_hash{uint256S("0x66")};

    auto send_request = [&]() {
        auto stream = MakeQGetDataStream(Consensus::LLMQType::LLMQ_TEST, quorum_hash,
                                         CQuorumDataRequest::QUORUM_VERIFICATION_VECTOR, protx_hash);
        m_net_quorum->ProcessMessage(*peer, NetMsgType::QGETDATA, stream);
    };

    // First miss: registered, answered, unscored.
    send_request();
    BOOST_CHECK_EQUAL(MisbehaviorScore(*m_node.peerman, *peer), 0);
    BOOST_CHECK(m_node.llmq_ctx->qman->IsDataRequestPending(peer->GetVerifiedProRegTxHash(), /*we_requested=*/false,
                                                            quorum_hash, Consensus::LLMQType::LLMQ_TEST));

    // Repeats hit the registered entry and score like any other rate-limited request.
    for (int score = 25; score <= 75; score += 25) {
        send_request();
        BOOST_CHECK_EQUAL(MisbehaviorScore(*m_node.peerman, *peer), score);
        BOOST_CHECK(m_node.llmq_ctx->qman->IsDataRequestPending(peer->GetVerifiedProRegTxHash(),
                                                                /*we_requested=*/false, quorum_hash,
                                                                Consensus::LLMQType::LLMQ_TEST));
    }
}

BOOST_AUTO_TEST_CASE(qgetdata_request_tracking_is_bounded)
{
    const uint256 requester{uint256S("0x77")};
    SetMockTime(100000);

    for (size_t i = 0; i < MAX_INBOUND_DATA_REQUESTS_PER_REQUESTER; ++i) {
        const uint256 hash{ArithToUint256(arith_uint256{i + 1})};
        const CQuorumDataRequestKey key{requester, /*we_requested=*/false, hash, Consensus::LLMQType::LLMQ_TEST};
        const CQuorumDataRequest request{Consensus::LLMQType::LLMQ_TEST, hash,
                                         CQuorumDataRequest::QUORUM_VERIFICATION_VECTOR};
        BOOST_CHECK(m_node.llmq_ctx->qman->RegisterDataRequest(key, request, /*add_expiry_bias=*/false) ==
                    DataRequestRegistration::Accepted);
    }

    const uint256 over_hash{uint256S("0xdead")};
    const CQuorumDataRequestKey over_key{requester, /*we_requested=*/false, over_hash, Consensus::LLMQType::LLMQ_TEST};
    const CQuorumDataRequest over_request{Consensus::LLMQType::LLMQ_TEST, over_hash,
                                          CQuorumDataRequest::QUORUM_VERIFICATION_VECTOR};
    BOOST_CHECK(m_node.llmq_ctx->qman->RegisterDataRequest(over_key, over_request, /*add_expiry_bias=*/false) ==
                DataRequestRegistration::RequesterLimitExceeded);
    BOOST_CHECK(!m_node.llmq_ctx->qman->IsDataRequestPending(requester, /*we_requested=*/false, over_hash,
                                                             Consensus::LLMQType::LLMQ_TEST));

    // Timer-driven cleanup can recover capacity even when no block arrives.
    SetMockTime(100000 + 361);
    m_node.llmq_ctx->qman->CleanupExpiredDataRequests();
    BOOST_CHECK(m_node.llmq_ctx->qman->RegisterDataRequest(over_key, over_request, /*add_expiry_bias=*/false) ==
                DataRequestRegistration::Accepted);
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(qgetdata_global_capacity_does_not_limit_outbound_requests)
{
    for (size_t i = 0; i < MAX_INBOUND_DATA_REQUESTS; ++i) {
        const uint256 requester{ArithToUint256(arith_uint256{i / MAX_INBOUND_DATA_REQUESTS_PER_REQUESTER + 1})};
        const uint256 hash{ArithToUint256(arith_uint256{i + 1})};
        const CQuorumDataRequestKey key{requester, /*we_requested=*/false, hash, Consensus::LLMQType::LLMQ_TEST};
        const CQuorumDataRequest request{Consensus::LLMQType::LLMQ_TEST, hash,
                                         CQuorumDataRequest::QUORUM_VERIFICATION_VECTOR};
        BOOST_REQUIRE(m_node.llmq_ctx->qman->RegisterDataRequest(key, request, /*add_expiry_bias=*/false) ==
                      DataRequestRegistration::Accepted);
    }

    const uint256 extra_requester{uint256S("0xbeef")};
    const uint256 extra_hash{uint256S("0xfeed")};
    const CQuorumDataRequest extra_request{Consensus::LLMQType::LLMQ_TEST, extra_hash,
                                           CQuorumDataRequest::QUORUM_VERIFICATION_VECTOR};
    BOOST_CHECK(m_node.llmq_ctx->qman->RegisterDataRequest({extra_requester, /*we_requested=*/false, extra_hash,
                                                            Consensus::LLMQType::LLMQ_TEST},
                                                           extra_request, /*add_expiry_bias=*/false) ==
                DataRequestRegistration::CapacityExhausted);
    BOOST_CHECK(!m_node.llmq_ctx->qman->IsDataRequestPending(extra_requester, /*we_requested=*/false, extra_hash,
                                                             Consensus::LLMQType::LLMQ_TEST));
    BOOST_CHECK(m_node.llmq_ctx->qman->RegisterDataRequest({extra_requester, /*we_requested=*/true, extra_hash,
                                                            Consensus::LLMQType::LLMQ_TEST},
                                                           extra_request, /*add_expiry_bias=*/false) ==
                DataRequestRegistration::Accepted);
}

BOOST_AUTO_TEST_SUITE_END()
