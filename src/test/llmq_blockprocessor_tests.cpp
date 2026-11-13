// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/llmq_tests.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>

#include <chainparams.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <evo/deterministicmns.h>
#include <hash.h>
#include <masternode/meta.h>
#include <net.h>
#include <net_processing.h>
#include <streams.h>
#include <util/time.h>
#include <validation.h>
#include <version.h>

#include <llmq/blockprocessor.h>
#include <llmq/commitment.h>
#include <llmq/context.h>
#include <msg_result.h>
#include <protocol.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <map>

using namespace llmq;
using namespace llmq::testutils;

BOOST_AUTO_TEST_SUITE(llmq_blockprocessor_tests)

namespace {
CDataStream Serialized(const CFinalCommitment& qc)
{
    CDataStream payload{SER_NETWORK, PROTOCOL_VERSION};
    payload << qc;
    return payload;
}

struct SigningHistorySetup : TestChainV19Setup {
    CDeterministicMNManager dmnman{*m_node.evodb, *m_node.mn_metaman};
    CQuorumBlockProcessor blockprocessor{*m_node.chainman, dmnman, *m_node.evodb, *m_node.llmq_ctx->qsnapman, 0};

    SigningHistorySetup()
    {
        mineBlocks(132);
        CDeterministicMNList mn_list;
        for (int i = 0; i < 8; ++i) {
            auto mn = std::make_shared<CDeterministicMN>(i);
            mn->proTxHash = GetTestQuorumHash(i + 1);
            mn->collateralOutpoint = COutPoint(mn->proTxHash, 0);
            auto state = std::make_shared<CDeterministicMNState>();
            state->nVersion = ProTxVersion::BasicBLS;
            state->netInfo = NetInfoInterface::MakeNetInfo(state->nVersion);
            state->keyIDOwner.begin()[0] = i + 1;
            state->nRegisteredHeight = 100;
            state->UpdateConfirmedHash(mn->proTxHash, GetTestBlockHash(i + 1));
            mn->pdmnState = state;
            mn_list.AddMN(mn);
        }
        // Read persisted lists with a fresh manager instead of replacing the node's cached lists.
        for (int height = 100; height <= 240; ++height) {
            mn_list.SetHeight(height);
            mn_list.SetBlockHash(Index(height)->GetBlockHash());
            m_node.evodb->Write(std::make_pair(std::string{"dmn_S3"}, mn_list.GetBlockHash()), mn_list);
        }
    }

    CBlockIndex* Index(int height) const
    {
        return WITH_LOCK(cs_main, return m_node.chainman->ActiveChain()[height]);
    }

    std::vector<CDeterministicMNCPtr> Members(Consensus::LLMQType type, int height)
    {
        return utils::GetAllQuorumMembers(type, {dmnman, *m_node.llmq_ctx->qsnapman, *m_node.chainman, Index(height)},
                                          /*reset_cache=*/true);
    }

    CFinalCommitment Commitment(Consensus::LLMQType type, int height, const std::vector<size_t>& invalid_members = {},
                                const std::vector<size_t>& non_signers = {})
    {
        const auto params = Params().GetLLMQ(type).value();
        BOOST_REQUIRE_EQUAL(Members(type, height).size(), static_cast<size_t>(params.size));
        CFinalCommitment qc{params, Index(height)->GetBlockHash()};
        qc.nVersion = CFinalCommitment::GetVersion(IsQuorumRotationEnabled(params, Index(height)), true);
        qc.quorumIndex = height % params.dkgInterval;
        qc.validMembers.assign(params.size, true);
        qc.signers.assign(params.size, true);
        for (const auto i : invalid_members)
            qc.validMembers[i] = false;
        for (const auto i : non_signers)
            qc.signers[i] = false;
        return qc;
    }

    void Store(const CFinalCommitment& qc, int mined_height, int base_height)
    {
        if (IsQuorumRotationEnabled(Params().GetLLMQ(qc.llmqType).value(), Index(base_height))) {
            m_node.evodb->Write(std::make_pair(std::string{"q_mc"}, std::make_pair(qc.llmqType, qc.quorumHash)),
                                std::make_pair(qc, Index(mined_height)->GetBlockHash()));
            m_node.evodb->Write(std::make_tuple(std::string{"q_mcihi"}, qc.llmqType, int{qc.quorumIndex},
                                                htobe32_internal(UINT32_MAX - mined_height)),
                                base_height);
        } else {
            WriteMinedCommitment(*m_node.evodb, qc, Index(mined_height)->GetBlockHash(), mined_height, base_height);
        }
    }
};

using NormalizedSigningRecords = std::map<uint256, std::vector<std::pair<int, bool>>>;

NormalizedSigningRecords Normalize(const SigningRecords& records)
{
    NormalizedSigningRecords ret;
    for (const auto& [protx, history] : records) {
        auto& entries = ret[protx];
        for (const auto& record : history) {
            entries.emplace_back(record.quorum_height, record.signed_commitment);
        }
        std::sort(entries.begin(), entries.end());
    }
    return ret;
}
} // namespace

// A QFCOMMITMENT is only ever sent in reply to a GETDATA, so one the peer was never asked for must
// be dropped before CQuorumBlockProcessor does any lookup work on its behalf. The block processor
// holds no PeerManagerInternal -- net_processing already includes its header, so a reference would
// be circular -- and reaches the in-flight check through a predicate injected at the call site.
// Everything below therefore goes through PeerManagerImpl::ProcessMessage: calling the handler
// directly would test the gate but not the injection, which is the part that has no compiler to
// keep it honest.
BOOST_FIXTURE_TEST_CASE(unrequested_qfcommit_is_dropped_and_scored, TestChain100Setup)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);

    // INV announcements for non-spork objects are only tracked outside IBD; the 100 mined blocks
    // of this fixture already take us out of it.
    BOOST_REQUIRE(!m_node.chainman->IsInitialBlockDownload());

    // Every Dash-specific message is offered to CMNAuth first, which asserts a loaded metadata
    // manager. The fixture leaves it unloaded, so initialise an empty cache here.
    BOOST_REQUIRE(m_node.mn_metaman->LoadCache(/*load_cache=*/false));

    auto& blockprocessor = *m_node.llmq_ctx->quorum_block_processor;
    const auto& llmq_params = GetLLMQParams(Consensus::LLMQType::LLMQ_TEST_V17);
    BOOST_REQUIRE(Params().GetLLMQ(llmq_params.type).has_value());

    auto unsolicited_peer{MakeTestPeer(/*id=*/51)};
    auto announcing_peer{MakeTestPeer(/*id=*/52)};
    m_node.peerman->InitializeNode(*unsolicited_peer, NODE_NETWORK);
    m_node.peerman->InitializeNode(*announcing_peer, NODE_NETWORK);

    // quorumHash names no block we know of. That is the one rejection below the gate that
    // deliberately carries no penalty -- we may simply be behind or on another chain -- so any
    // score this payload collects can only have come from the gate, and the checks that do score
    // cannot be mistaken for it.
    const auto unsolicited_qc = CreateValidCommitment(llmq_params, GetTestBlockHash(51));
    const uint256 unsolicited_hash = ::SerializeHash(unsolicited_qc);

    // Sent twice on purpose: an unsolicited peer must not be able to repeat the block lookups and
    // mineable-commitment probes for free, so both copies have to cost it.
    for (int i = 0; i < 2; ++i) {
        SendMessage(*m_node.peerman, *unsolicited_peer, NetMsgType::QFCOMMITMENT, Serialized(unsolicited_qc));
    }
    BOOST_CHECK_EQUAL(MisbehaviorScore(*m_node.peerman, *unsolicited_peer),
                      2 * UNREQUESTED_OBJECT_MISBEHAVIOR_SCORE);
    // Nothing below the gate ran: had the commitment been processed it could only have ended up
    // here or been rejected, and this is the observable half of that.
    BOOST_CHECK(!blockprocessor.HasMineableCommitment(unsolicited_hash));

    // The gate also runs ahead of the null-commitment check, which scores 100. A null payload from
    // a peer we never asked must still cost exactly the unsolicited price -- reaching the 100 would
    // mean the gate had let it through.
    {
        const int score_before_null = MisbehaviorScore(*m_node.peerman, *unsolicited_peer);
        SendMessage(*m_node.peerman, *unsolicited_peer, NetMsgType::QFCOMMITMENT, Serialized(CFinalCommitment{}));
        BOOST_CHECK_EQUAL(MisbehaviorScore(*m_node.peerman, *unsolicited_peer),
                          score_before_null + UNREQUESTED_OBJECT_MISBEHAVIOR_SCORE);
    }

    // Announcing the commitment is NOT enough to authorise it. An INV creates a candidate
    // immediately, but the GETDATA only goes out later from SendMessages, so accepting on the
    // announcement alone would let a peer authorise its own payload by racing INV and payload back
    // to back -- which costs it nothing and defeats the gate entirely.
    const auto announced_qc = CreateValidCommitment(llmq_params, GetTestBlockHash(52));
    const CInv announced_inv{MSG_QUORUM_FINAL_COMMITMENT, ::SerializeHash(announced_qc)};

    AnnounceInv(*m_node.peerman, *announcing_peer, announced_inv);
    {
        const int score_before_race = MisbehaviorScore(*m_node.peerman, *announcing_peer);
        SendMessage(*m_node.peerman, *announcing_peer, NetMsgType::QFCOMMITMENT, Serialized(announced_qc));
        BOOST_CHECK_EQUAL(MisbehaviorScore(*m_node.peerman, *announcing_peer),
                          score_before_race + UNREQUESTED_OBJECT_MISBEHAVIOR_SCORE);
    }

    // Once SendMessages has actually issued the GETDATA the same payload is authorised. The
    // rejection above must not have consumed the candidate, or no GETDATA would go out at all.
    SetMockTime(GetTime<std::chrono::seconds>() + 61s);
    m_node.peerman->SendMessages(announcing_peer.get());
    const int score_before = MisbehaviorScore(*m_node.peerman, *announcing_peer);

    SendMessage(*m_node.peerman, *announcing_peer, NetMsgType::QFCOMMITMENT, Serialized(announced_qc));

    // Unchanged, because the unknown quorum block this commitment names is rejected without any
    // penalty -- which is what proves the message got past the gate. Asserting the total exactly is
    // what would catch the gate also charging a peer we did ask.
    BOOST_CHECK_EQUAL(MisbehaviorScore(*m_node.peerman, *announcing_peer), score_before);

    // One GETDATA authorises exactly one answer. Both the in-flight request and the late-answer
    // grace are spent, so a replay of the very payload we asked for is unsolicited again -- a peer
    // must not be able to induce one request and then repeat the payload for free.
    SendMessage(*m_node.peerman, *announcing_peer, NetMsgType::QFCOMMITMENT, Serialized(announced_qc));
    BOOST_CHECK_EQUAL(MisbehaviorScore(*m_node.peerman, *announcing_peer),
                      score_before + UNREQUESTED_OBJECT_MISBEHAVIOR_SCORE);

    m_node.peerman->FinalizeNode(*unsolicited_peer);
    m_node.peerman->FinalizeNode(*announcing_peer);
    SetMockTime(0s);
}

// A relayed commitment for a quorum we already hold a mineable commitment for is only worth
// verifying, and relaying, when it has more signers than ours.
BOOST_FIXTURE_TEST_CASE(qfcommit_with_more_signers_replaces_stored_one, TestChain100Setup)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);
    BOOST_REQUIRE(!m_node.chainman->IsInitialBlockDownload());
    BOOST_REQUIRE(m_node.mn_metaman->LoadCache(/*load_cache=*/false));

    auto& blockprocessor = *m_node.llmq_ctx->quorum_block_processor;
    const auto& llmq_params = GetLLMQParams(Consensus::LLMQType::LLMQ_TEST_V17);
    BOOST_REQUIRE(Params().GetLLMQ(llmq_params.type).has_value());
    BOOST_REQUIRE_EQUAL(llmq_params.size, 3);

    const CBlockIndex* base_index{WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip())};
    base_index = base_index->GetAncestor(base_index->nHeight - (base_index->nHeight % llmq_params.dkgInterval));

    auto with_signers = [&](const CFinalCommitment& qc, size_t count) {
        CFinalCommitment ret{qc};
        for (size_t i = 0; i < ret.signers.size(); ++i) {
            ret.signers[i] = i < count;
        }
        return ret;
    };
    const auto base_qc = CreateValidCommitment(llmq_params, base_index->GetBlockHash());
    const auto qc_one = with_signers(base_qc, 1);
    const auto qc_two = with_signers(base_qc, 2);
    const auto qc_three = with_signers(base_qc, 3);
    auto qc_two_other{qc_three};
    qc_two_other.signers[0] = false;

    BOOST_REQUIRE(blockprocessor.AddMineableCommitment(qc_two).has_value());

    NodeId next_id{61};
    // Returns the misbehavior score the peer collected. The payloads carry random signatures, so a
    // commitment that reaches verification is rejected with 100, while one dropped by the
    // signer-count pre-check costs nothing.
    auto deliver = [&](const CFinalCommitment& qc) EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex) {
        auto peer{MakeTestPeer(next_id++)};
        m_node.peerman->InitializeNode(*peer, NODE_NETWORK);
        AnnounceInv(*m_node.peerman, *peer, CInv{MSG_QUORUM_FINAL_COMMITMENT, ::SerializeHash(qc)});
        SetMockTime(GetTime<std::chrono::seconds>() + 61s);
        m_node.peerman->SendMessages(peer.get());
        SendMessage(*m_node.peerman, *peer, NetMsgType::QFCOMMITMENT, Serialized(qc));
        const int score{MisbehaviorScore(*m_node.peerman, *peer)};
        m_node.peerman->FinalizeNode(*peer);
        return score;
    };

    BOOST_CHECK_EQUAL(deliver(qc_one), 0);
    BOOST_CHECK_EQUAL(deliver(qc_two_other), 0);
    BOOST_CHECK_EQUAL(deliver(qc_three), 100);

    BOOST_CHECK(!blockprocessor.AddMineableCommitment(qc_one).has_value());
    BOOST_CHECK(blockprocessor.HasMineableCommitment(::SerializeHash(qc_two)));

    const auto inv = blockprocessor.AddMineableCommitment(qc_three);
    BOOST_REQUIRE(inv.has_value());
    BOOST_CHECK(inv->hash == ::SerializeHash(qc_three));
    BOOST_CHECK(blockprocessor.HasMineableCommitment(::SerializeHash(qc_three)));
    BOOST_CHECK(!blockprocessor.HasMineableCommitment(::SerializeHash(qc_two)));

    SetMockTime(0s);
}

BOOST_FIXTURE_TEST_CASE(signing_records_window_mining_height_and_rotation, SigningHistorySetup)
{
    constexpr auto type{Consensus::LLMQType::LLMQ_TEST};
    constexpr auto rotated_type{Consensus::LLMQType::LLMQ_TEST_DIP0024};
    for (int height : {120, 144, 168}) {
        Members(rotated_type, height);
    }
    BOOST_REQUIRE(IsQuorumRotationEnabled(Params().GetLLMQ(rotated_type).value(), Index(228)));
    Store(Commitment(type, 192), 202, 192);
    Store(Commitment(type, 216, /*invalid_members=*/{2}, /*non_signers=*/{1}), 226, 216);
    Store(Commitment(rotated_type, 192), 204, 192);
    Store(Commitment(rotated_type, 193), 205, 193);
    Store(Commitment(rotated_type, 216), 228, 216);
    Store(Commitment(rotated_type, 217), 229, 217);

    NormalizedSigningRecords expected;
    const auto non_rotated_members = Members(type, 216);
    const auto inside_members = Members(rotated_type, 193);
    for (const auto& mn : inside_members) {
        expected[mn->proTxHash].emplace_back(193, true);
    }
    const auto expected_before_non_rotated = expected;
    expected[non_rotated_members[0]->proTxHash].emplace_back(216, true);
    expected[non_rotated_members[1]->proTxHash].emplace_back(216, false);
    const auto expected_before_mining = expected;
    const auto rotated_members = Members(rotated_type, 216);
    for (const auto& mn : rotated_members) {
        expected[mn->proTxHash].emplace_back(216, true);
    }
    for (auto& [_, history] : expected) {
        std::sort(history.begin(), history.end());
    }

    BOOST_CHECK(Normalize(*blockprocessor.GetSigningRecords(Index(228), 36)) == expected);

    BOOST_CHECK(Normalize(*blockprocessor.GetSigningRecords(Index(225), 33)) == expected_before_non_rotated);
    const auto earlier = Normalize(*blockprocessor.GetSigningRecords(Index(227), 35));
    BOOST_CHECK(earlier == expected_before_mining);
    const auto later = Normalize(*blockprocessor.GetSigningRecords(Index(229), 37));
    for (const auto& mn : Members(rotated_type, 217)) {
        const auto& history = later.at(mn->proTxHash);
        BOOST_CHECK(std::find(history.begin(), history.end(), std::make_pair(217, true)) != history.end());
    }
}

BOOST_FIXTURE_TEST_CASE(signing_records_unavailable_for_inconsistent_view, SigningHistorySetup)
{
    constexpr auto type{Consensus::LLMQType::LLMQ_TEST};
    CEvoDB evo_db{util::DbWrapperParams{.path = m_args.GetDataDirNet() / "signing-history", .memory = true, .wipe = true}};
    CQuorumBlockProcessor processor{*m_node.chainman, dmnman, evo_db, *m_node.llmq_ctx->qsnapman, 0};
    const auto qc = Commitment(type, 216, /*invalid_members=*/{2}, /*non_signers=*/{1});
    const auto key = std::make_pair(std::string{"q_mc"}, std::make_pair(type, qc.quorumHash));
    const auto mined_hash = Index(226)->GetBlockHash();
    WriteMinedCommitment(evo_db, qc, mined_hash, 226, 216);
    auto* gathering = Index(240);
    NormalizedSigningRecords expected;
    const auto members = Members(type, 216);
    expected[members[0]->proTxHash].emplace_back(216, true);
    expected[members[1]->proTxHash].emplace_back(216, false);

    auto& chainstate = m_node.chainman->ActiveChainstate();
    BlockValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, gathering));
    BOOST_REQUIRE(Index(240) == nullptr);
    BOOST_CHECK(!processor.GetSigningRecords(gathering, 48));
    WITH_LOCK(cs_main, chainstate.ResetBlockFailureFlags(gathering));
    BOOST_REQUIRE_MESSAGE(chainstate.ActivateBestChain(state), state.ToString());
    BOOST_REQUIRE(Index(240) == gathering);

    evo_db.Erase(key);
    BOOST_CHECK(!processor.GetSigningRecords(gathering, 48));

    // Mined in a block that is not an ancestor of the gathering block
    evo_db.Write(key, std::make_pair(qc, gathering->GetBlockHash()));
    BOOST_CHECK(!processor.GetSigningRecords(Index(230), 38));

    WriteMinedCommitment(evo_db, qc, mined_hash, 226, 216);
    for (const auto& [pindex, window] : {std::pair{gathering, 48}, std::pair{Index(230), 38}}) {
        const auto records = processor.GetSigningRecords(pindex, window);
        BOOST_REQUIRE(records);
        BOOST_CHECK(Normalize(*records) == expected);
    }
}

BOOST_AUTO_TEST_SUITE_END()
