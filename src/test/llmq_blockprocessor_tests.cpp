// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/llmq_tests.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>

#include <chainparams.h>
#include <consensus/params.h>
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
#include <random.h>

#include <boost/test/unit_test.hpp>

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
    BOOST_REQUIRE(!m_node.chainman->ActiveChainstate().IsInitialBlockDownload());

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


// The ordinary scanner over a complete history, written out the way
// CQuorumBlockProcessor walks its index: newest first without rotation;
// with it, the cycle-th newest record of every index in ascending order, one
// cycle after another, until a cycle yields nothing.
std::vector<const CBlockIndex*> OrdinaryScan(bool indexed,
                                             const std::vector<std::vector<std::pair<int, const CBlockIndex*>>>& history,
                                             int cutoff, size_t count)
{
    std::vector<const CBlockIndex*> ret;
    const auto nth_eligible = [&](size_t index, size_t nth) -> const CBlockIndex* {
        for (const auto& [mined, block] : history[index]) {
            if (mined > cutoff) continue;
            if (nth == 0) return block;
            --nth;
        }
        return nullptr;
    };
    if (!indexed) {
        for (size_t nth{0}; ret.size() < count; ++nth) {
            const CBlockIndex* block{nth_eligible(0, nth)};
            if (block == nullptr) break;
            ret.push_back(block);
        }
        return ret;
    }
    for (size_t cycle{0}; ret.size() < count; ++cycle) {
        std::vector<const CBlockIndex*> cycle_ret;
        for (size_t index{0}; index < history.size(); ++index) {
            if (const CBlockIndex * block{nth_eligible(index, cycle)}) cycle_ret.push_back(block);
        }
        if (cycle_ret.empty()) break;
        const size_t need{count - ret.size()};
        ret.insert(ret.end(), cycle_ret.begin(), cycle_ret.begin() + std::min(need, cycle_ret.size()));
    }
    return ret;
}

// A snapshot carries the canonical selection of a scan's history: the newest
// K records without rotation, the newest two of each quorum index with it,
// marked END when it holds fewer. On small generated histories, a scan the
// carried records prove must return exactly what the ordinary scanner returns
// over the complete history, at every cutoff and count, and coverage never
// shrinks as the cutoff rises within one fixed context.
BOOST_AUTO_TEST_CASE(scan_proofs_agree_with_full_scans)
{
    FastRandomContext rng{/*fDeterministic=*/true};
    size_t proven_count{0};
    size_t refused_count{0};
    size_t multi_rank_proofs{0};
    for (int round{0}; round < 300; ++round) {
        const bool indexed{round % 2 == 1};
        const size_t sequence_count{indexed ? 2 + rng.randrange(3) : 1};
        const size_t selected{indexed ? 2 : 1 + rng.randrange(4)};
        const int base{10 + static_cast<int>(rng.randrange(30))};
        // Distinct stand-in quorum blocks for every record of this round.
        std::vector<CBlockIndex> blocks(sequence_count * static_cast<size_t>(base));
        size_t next_block{0};
        // Mined heights per sequence, newest first.
        std::vector<std::vector<std::pair<int, const CBlockIndex*>>> history(sequence_count);
        for (auto& sequence : history) {
            for (int height{1}; height <= base; ++height) {
                if (rng.randrange(3) == 0) sequence.emplace_back(height, &blocks[next_block++]);
            }
            std::reverse(sequence.begin(), sequence.end());
        }
        std::vector<std::vector<std::pair<int, const CBlockIndex*>>> carried(sequence_count);
        std::vector<ScanSequenceEnd> ends;
        for (size_t i{0}; i < sequence_count; ++i) {
            for (const auto& record : history[i]) {
                if (carried[i].size() < selected) carried[i].push_back(record);
            }
            ends.push_back(carried[i].size() < selected ? ScanSequenceEnd::END : ScanSequenceEnd::UNKNOWN);
        }
        for (size_t count{1}; count <= 3 * sequence_count + 1; ++count) {
            bool was_covered{false};
            for (int cutoff{0}; cutoff <= base; ++cutoff) {
                std::vector<ScanSequence> proof;
                for (size_t i{0}; i < sequence_count; ++i) {
                    ScanSequence sequence{{}, ends[i]};
                    for (const auto& [mined, block] : carried[i]) {
                        if (mined <= cutoff) sequence.records.push_back(block);
                    }
                    proof.push_back(std::move(sequence));
                }
                const auto expected{OrdinaryScan(indexed, history, cutoff, count)};
                const auto proven{TraverseScanSequences(indexed, proof, count)};
                if (proven) {
                    BOOST_CHECK(*proven == expected);
                    ++proven_count;
                    const auto populated{std::count_if(proof.begin(), proof.end(), [](const auto& sequence) {
                        return sequence.records.size() >= 2;
                    })};
                    if (indexed && populated >= 2 && expected.size() > sequence_count) ++multi_rank_proofs;
                } else {
                    BOOST_CHECK(!was_covered);
                    ++refused_count;
                }
                was_covered = was_covered || proven.has_value();
            }
        }
    }
    BOOST_CHECK_GT(proven_count, 0U);
    BOOST_CHECK_GT(refused_count, 0U);
    BOOST_CHECK_GT(multi_rank_proofs, 0U);
}

// The rotating counterexample: four quorum indexes, index 0 never formed,
// indexes 1-3 carrying [new, old] with older ones omitted, and a cutoff
// between new and old. Rank 0 yields the three old records; rank 1 needs index
// 1's next one, which is not carried, so the scan is not proven rather than
// returning three (or the wrong four).
BOOST_AUTO_TEST_CASE(rotating_scan_counterexample_is_not_covered)
{
    std::vector<CBlockIndex> blocks(6);
    std::vector<ScanSequence> sequences{{{}, ScanSequenceEnd::END}};
    for (size_t i{0}; i < 3; ++i) {
        // After the cutoff drops each index's newest record, its old one is
        // eligible and the carried sequence goes on beyond it.
        sequences.push_back({{&blocks[2 * i + 1]}, ScanSequenceEnd::UNKNOWN});
    }
    BOOST_CHECK(!TraverseScanSequences(/*indexed=*/true, sequences, 4).has_value());
    const auto three{TraverseScanSequences(/*indexed=*/true, sequences, 3)};
    BOOST_REQUIRE(three.has_value());
    BOOST_CHECK(*three == (std::vector<const CBlockIndex*>{&blocks[1], &blocks[3], &blocks[5]}));
}

BOOST_AUTO_TEST_SUITE_END()
