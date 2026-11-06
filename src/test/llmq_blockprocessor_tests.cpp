// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/llmq_tests.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>

#include <chain.h>
#include <chainparams.h>
#include <consensus/params.h>
#include <evo/evodb.h>
#include <hash.h>
#include <masternode/meta.h>
#include <net.h>
#include <net_processing.h>
#include <streams.h>
#include <util/fs.h>
#include <util/std23.h>
#include <util/time.h>
#include <validation.h>
#include <version.h>

#include <llmq/blockprocessor.h>
#include <llmq/commitment.h>
#include <llmq/context.h>
#include <msg_result.h>
#include <protocol.h>

#include <boost/test/unit_test.hpp>

#include <vector>

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

namespace {
std::vector<int> BaseHeights(const MinedCommitmentScan& scan)
{
    std::vector<int> heights;
    for (const auto* base : scan.bases)
        heights.push_back(base->nHeight);
    return heights;
}
} // namespace

// A scan of mined commitments may only be read as "nothing older exists" when it filled the requested
// window or walked off the end of the type's index. Entries are installed directly in EvoDB.
BOOST_FIXTURE_TEST_CASE(scan_mined_commitments_reports_coverage, TestChain100Setup)
{
    const auto& qbp = *m_node.llmq_ctx->quorum_block_processor;
    constexpr auto type = Consensus::LLMQType::LLMQ_TEST;
    LOCK(::cs_main);
    const CChain& active = m_node.chainman->ActiveChain();
    {
        auto db_tx = m_node.evodb->BeginTransaction();
        for (const int base : {10, 20, 30, 40, 50}) {
            WriteMinedCommitment(*m_node.evodb, CreateValidCommitment(GetLLMQParams(type), active[base]->GetBlockHash()),
                                 active[base + 1]->GetBlockHash(), /*mined_height=*/base + 1, /*quorum_height=*/base);
        }
        db_tx->Commit();
    }
    // The key right after this type's range is no index key at all and holds no height. The walk must
    // end at the prefix without parsing it.
    const auto next_type = static_cast<Consensus::LLMQType>(std23::to_underlying(type) + 1);
    BOOST_REQUIRE(
        m_node.evodb->GetRawDB().Write(std::make_pair(DB_MINED_COMMITMENT_BY_INVERSED_HEIGHT, next_type), uint8_t{0}));

    auto scan = qbp.ScanMinedCommitmentsUntilBlock(type, active.Tip(), 4);
    BOOST_CHECK(BaseHeights(scan) == std::vector<int>({50, 40, 30, 20}));
    BOOST_CHECK(scan.complete);
    BOOST_CHECK(scan.bases == qbp.GetMinedCommitmentsUntilBlock(type, active.Tip(), 4));

    // Young chain: fewer entries than requested, and complete because the walk left the type's range.
    scan = qbp.ScanMinedCommitmentsUntilBlock(type, active[25], 4);
    BOOST_CHECK(BaseHeights(scan) == std::vector<int>({20, 10}));
    BOOST_CHECK(scan.complete);

    // An index value that does not deserialize, at mined height 35.
    BOOST_REQUIRE(m_node.evodb->GetRawDB().Write(BuildInversedHeightKey(type, 35), uint8_t{0}));
    scan = qbp.ScanMinedCommitmentsUntilBlock(type, active.Tip(), 4);
    BOOST_CHECK(BaseHeights(scan) == std::vector<int>({50, 40}));
    BOOST_CHECK(!scan.complete);
    // A window filled before the corrupt entry, or a walk starting below it, never reads it.
    scan = qbp.ScanMinedCommitmentsUntilBlock(type, active.Tip(), 2);
    BOOST_CHECK(BaseHeights(scan) == std::vector<int>({50, 40}));
    BOOST_CHECK(scan.complete);
    scan = qbp.ScanMinedCommitmentsUntilBlock(type, active[34], 4);
    BOOST_CHECK(BaseHeights(scan) == std::vector<int>({30, 20, 10}));
    BOOST_CHECK(scan.complete);

    // An entry whose quorum base is not below the block that mined it.
    BOOST_REQUIRE(m_node.evodb->GetRawDB().Write(BuildInversedHeightKey(type, 15), 15));
    scan = qbp.ScanMinedCommitmentsUntilBlock(type, active[34], 4);
    BOOST_CHECK(BaseHeights(scan) == std::vector<int>({30, 20}));
    BOOST_CHECK(!scan.complete);

    // A commitment is mined within its own quorum's DKG interval, so the quorum bases strictly
    // decrease along the index. A second entry for base 50, or an entry for an older base above
    // base 50's, contradicts the next one, and either of the two may be the wrong one.
    BOOST_REQUIRE(m_node.evodb->GetRawDB().Write(BuildInversedHeightKey(type, 55), 50));
    scan = qbp.ScanMinedCommitmentsUntilBlock(type, active.Tip(), 2);
    BOOST_CHECK(BaseHeights(scan).empty());
    BOOST_CHECK(!scan.complete);
    BOOST_REQUIRE(m_node.evodb->GetRawDB().Write(BuildInversedHeightKey(type, 55), 45));
    scan = qbp.ScanMinedCommitmentsUntilBlock(type, active.Tip(), 2);
    BOOST_CHECK(BaseHeights(scan).empty());
    BOOST_CHECK(!scan.complete);
    // An entry before the contradicting pair is kept.
    BOOST_REQUIRE(m_node.evodb->GetRawDB().Write(BuildInversedHeightKey(type, 58), 52));
    scan = qbp.ScanMinedCommitmentsUntilBlock(type, active.Tip(), 3);
    BOOST_CHECK(BaseHeights(scan) == std::vector<int>({52}));
    BOOST_CHECK(!scan.complete);
}

// LevelDB steps over a table block that fails its checksum and keeps iterating. The entries after
// such a block are not the next newest ones, and the window is not complete. The EvoDB here is on
// disk, closed, corrupted and reopened.
BOOST_FIXTURE_TEST_CASE(scan_mined_commitments_stops_at_corrupt_block, TestChain100Setup)
{
    constexpr auto type = Consensus::LLMQType::LLMQ_TEST;
    const fs::path path{m_args.GetDataDirNet() / "corrupt_index"};
    {
        CEvoDB db{util::DbWrapperParams{.path = path, .memory = false, .wipe = true}};
        for (const int base : {20, 30, 40, 50}) {
            BOOST_REQUIRE(db.GetRawDB().Write(BuildInversedHeightKey(type, base + 1), PaddedIndexValue(base)));
        }
        db.GetRawDB().CompactFull();
    }
    // Each padded entry ends its table block, so base 40's entry has a block of its own.
    BOOST_REQUIRE(CorruptTableBlock(path / "evodb", std::vector<uint8_t>(64, 40)));

    CEvoDB db{util::DbWrapperParams{.path = path, .memory = false, .wipe = false}};
    const CQuorumBlockProcessor qbp{*m_node.chainman, *m_node.dmnman, db, *m_node.llmq_ctx->qsnapman,
                                    /*bls_threads=*/0};
    LOCK(::cs_main);
    const CChain& active = m_node.chainman->ActiveChain();
    auto scan = qbp.ScanMinedCommitmentsUntilBlock(type, active.Tip(), 4);
    BOOST_CHECK(BaseHeights(scan) == std::vector<int>({50}));
    BOOST_CHECK(!scan.complete);
    // A window filled before the corrupt block does not depend on it, and a walk starting below it
    // never reaches it.
    scan = qbp.ScanMinedCommitmentsUntilBlock(type, active.Tip(), 1);
    BOOST_CHECK(BaseHeights(scan) == std::vector<int>({50}));
    BOOST_CHECK(scan.complete);
    scan = qbp.ScanMinedCommitmentsUntilBlock(type, active[35], 4);
    BOOST_CHECK(BaseHeights(scan) == std::vector<int>({30, 20}));
    BOOST_CHECK(scan.complete);
}

// Rotation-enabled types are read cycle by cycle, quorum index by quorum index, as
// GetMinedCommitmentsIndexedUntilBlock does.
BOOST_FIXTURE_TEST_CASE(scan_mined_commitments_indexed_reports_coverage, TestChain100Setup)
{
    const auto& qbp = *m_node.llmq_ctx->quorum_block_processor;
    constexpr auto type = Consensus::LLMQType::LLMQ_TEST_DIP0024;
    BOOST_REQUIRE(Params().GetLLMQ(type).has_value());
    BOOST_REQUIRE_EQUAL(GetLLMQParams(type).signingActiveQuorumCount, 2);
    LOCK(::cs_main);
    const CChain& active = m_node.chainman->ActiveChain();
    {
        // Cycles at 24, 48 and 72: quorum index i has its base at cycle + i and is mined at cycle + 12 + i.
        auto db_tx = m_node.evodb->BeginTransaction();
        for (const int cycle : {24, 48, 72}) {
            for (const int quorum_index : {0, 1}) {
                m_node.evodb->Write(BuildInversedHeightKeyIndexed(type, cycle + 12 + quorum_index, quorum_index),
                                    cycle + quorum_index);
            }
        }
        db_tx->Commit();
    }

    auto scan = qbp.ScanMinedCommitmentsIndexedUntilBlock(type, active.Tip(), 4);
    BOOST_CHECK(BaseHeights(scan) == std::vector<int>({72, 73, 48, 49}));
    BOOST_CHECK(scan.complete);
    BOOST_CHECK(scan.bases == qbp.GetMinedCommitmentsIndexedUntilBlock(type, active.Tip(), 4));

    scan = qbp.ScanMinedCommitmentsIndexedUntilBlock(type, active.Tip(), 10);
    BOOST_CHECK(BaseHeights(scan) == std::vector<int>({72, 73, 48, 49, 24, 25}));
    BOOST_CHECK(scan.complete);
    scan = qbp.ScanMinedCommitmentsIndexedUntilBlock(type, active[40], 4);
    BOOST_CHECK(BaseHeights(scan) == std::vector<int>({24, 25}));
    BOOST_CHECK(scan.complete);

    // Quorum index 0's newest entry names base 40, older than its next entry's base 48. The first
    // cycle takes base 40; the second meets the contradiction and drops it again.
    m_node.evodb->Write(BuildInversedHeightKeyIndexed(type, 84, 0), 40);
    scan = qbp.ScanMinedCommitmentsIndexedUntilBlock(type, active.Tip(), 4);
    BOOST_CHECK(BaseHeights(scan) == std::vector<int>({73}));
    BOOST_CHECK(!scan.complete);
    m_node.evodb->Write(BuildInversedHeightKeyIndexed(type, 84, 0), 72);

    // Quorum index 1 has an unreadable entry at mined height 62, met in the second cycle.
    BOOST_REQUIRE(m_node.evodb->GetRawDB().Write(BuildInversedHeightKeyIndexed(type, 62, 1), uint8_t{0}));
    scan = qbp.ScanMinedCommitmentsIndexedUntilBlock(type, active.Tip(), 4);
    BOOST_CHECK(BaseHeights(scan) == std::vector<int>({72, 73, 48}));
    BOOST_CHECK(!scan.complete);
    scan = qbp.ScanMinedCommitmentsIndexedUntilBlock(type, active.Tip(), 2);
    BOOST_CHECK(BaseHeights(scan) == std::vector<int>({72, 73}));
    BOOST_CHECK(scan.complete);

    // Quorum indexes never share a base: quorum index 1's newest entry names index 0's base 72, and
    // neither entry is kept.
    BOOST_REQUIRE(m_node.evodb->GetRawDB().Write(BuildInversedHeightKeyIndexed(type, 86, 1), 72));
    scan = qbp.ScanMinedCommitmentsIndexedUntilBlock(type, active.Tip(), 2);
    BOOST_CHECK(BaseHeights(scan).empty());
    BOOST_CHECK(!scan.complete);
}

BOOST_AUTO_TEST_SUITE_END()
