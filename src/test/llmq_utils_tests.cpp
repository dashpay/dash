// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/llmq_tests.h>
#include <test/util/masternode.h>
#include <test/util/setup_common.h>

#include <bls/bls.h>
#include <chainparams.h>
#include <consensus/params.h>
#include <dbwrapper.h>
#include <deploymentstatus.h>
#include <evo/cbtx.h>
#include <evo/deterministicmns.h>
#include <evo/evodb.h>
#include <evo/providertx.h>
#include <evo/specialtx.h>
#include <llmq/context.h>
#include <llmq/net_signing.h>
#include <llmq/params.h>
#include <llmq/signhash.h>
#include <llmq/signing.h>
#include <llmq/signing_shares.h>
#include <llmq/snapshot.h>
#include <llmq/utils.h>
#include <netaddress.h>
#include <random.h>
#include <streams.h>
#include <util/strencodings.h>
#include <validation.h>
#include <versionbits.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

using namespace llmq;
using namespace llmq::testutils;

BOOST_FIXTURE_TEST_SUITE(llmq_utils_tests, BasicTestingSetup)

BOOST_FIXTURE_TEST_CASE(platform_recovered_sigs_preserve_each_message, TestingSetup)
{
    auto& sigman = *m_node.llmq_ctx->sigman;
    const auto type = Params().GetConsensus().llmqTypePlatform;
    const auto id = GetTestQuorumHash(1);
    const auto quorum = GetTestQuorumHash(2);
    CBLSSecretKey key;
    key.MakeNewKey();
    const auto make_sig = [&](uint32_t message) {
        const auto msg = GetTestQuorumHash(message);
        return std::make_shared<const CRecoveredSig>(type, quorum, id, msg,
                                                     key.Sign(SignHash(type, quorum, id, msg).Get(),
                                                              bls::bls_legacy_scheme.load()));
    };
    const auto current = make_sig(4);
    const auto earlier = make_sig(3);
    BOOST_CHECK(sigman.ProcessRecoveredSig(current));
    BOOST_CHECK(!sigman.HasRecoveredSigForSigning(type, id, earlier->getMsgHash()));
    BOOST_CHECK(sigman.ProcessRecoveredSig(earlier));
    BOOST_CHECK(!sigman.ProcessRecoveredSig(current));
    for (const auto& sig : {current, earlier}) {
        CRecoveredSig stored;
        BOOST_REQUIRE(sigman.GetRecoveredSig(type, id, sig->getMsgHash(), stored));
        BOOST_CHECK(stored.GetHash() == sig->GetHash());
        BOOST_CHECK(sigman.HasRecoveredSigForSigning(type, id, sig->getMsgHash()));
    }
}

BOOST_AUTO_TEST_CASE(platform_recovered_sigs_persistence_and_expiry)
{
    const auto type = Params().GetConsensus().llmqTypePlatform;
    const auto id = GetTestQuorumHash(1);
    const auto quorum = GetTestQuorumHash(2);
    CBLSSecretKey key;
    key.MakeNewKey();
    const auto make_sig = [&](uint32_t message) {
        const auto msg = GetTestQuorumHash(message);
        return CRecoveredSig(type, quorum, id, msg,
                             key.Sign(SignHash(type, quorum, id, msg).Get(), bls::bls_legacy_scheme.load()));
    };
    const auto first = make_sig(3);
    const auto second = make_sig(4);
    const auto path = m_args.GetDataDirBase() / "platform_sigs";
    const int64_t now = GetTime();
    {
        CRecoveredSigsDb db({.path = path, .wipe = true});
        SetMockTime(now);
        db.WriteRecoveredSig(first);
        SetMockTime(now + 100);
        db.WriteRecoveredSig(second);
    }
    {
        CRecoveredSigsDb db({.path = path});
        for (const auto* sig : {&first, &second}) {
            CRecoveredSig stored;
            BOOST_REQUIRE(db.GetRecoveredSig(type, id, sig->getMsgHash(), stored));
            BOOST_CHECK(stored.GetHash() == sig->GetHash());
            BOOST_REQUIRE(db.GetRecoveredSigByHash(sig->GetHash(), stored));
            BOOST_CHECK(stored.GetHash() == sig->GetHash());
            BOOST_CHECK(db.HasRecoveredSigForHash(sig->GetHash()));
            BOOST_CHECK(db.HasRecoveredSigForSession(sig->buildSignHash().Get()));
        }
        SetMockTime(now + 151);
        db.CleanupOldRecoveredSigs(100);
        CRecoveredSig stored;
        BOOST_CHECK(!db.GetRecoveredSig(type, id, first.getMsgHash(), stored));
        BOOST_CHECK(!db.GetRecoveredSigByHash(first.GetHash(), stored));
        BOOST_CHECK(!db.HasRecoveredSig(type, id, first.getMsgHash()));
        BOOST_CHECK(!db.HasRecoveredSigForHash(first.GetHash()));
        BOOST_CHECK(!db.HasRecoveredSigForSession(first.buildSignHash().Get()));
        BOOST_REQUIRE(db.GetRecoveredSig(type, id, second.getMsgHash(), stored));
        BOOST_CHECK(stored.GetHash() == second.GetHash());
        SetMockTime(now + 201);
        db.CleanupOldRecoveredSigs(100);
        BOOST_CHECK(!db.GetRecoveredSig(type, id, second.getMsgHash(), stored));
        BOOST_CHECK(!db.HasRecoveredSigForHash(second.GetHash()));
        BOOST_CHECK(!db.HasRecoveredSigForSession(second.buildSignHash().Get()));
    }
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(truncated_recovered_sig_markers_expire_after_reopen)
{
    const auto type = Params().GetConsensus().llmqTypeChainLocks;
    const auto id = GetTestQuorumHash(1);
    const auto quorum = GetTestQuorumHash(2);
    const auto msg = GetTestQuorumHash(3);
    CBLSSecretKey key;
    key.MakeNewKey();
    const CRecoveredSig sig{type, quorum, id, msg,
                            key.Sign(SignHash{type, quorum, id, msg}.Get(), bls::bls_legacy_scheme.load())};
    const auto path = m_args.GetDataDirBase() / "truncated_sigs";
    const auto now = GetTime();
    SetMockTime(now);
    {
        CRecoveredSigsDb db({.path = path, .wipe = true});
        db.WriteRecoveredSig(sig);
        db.TruncateRecoveredSig(type, id);
        BOOST_CHECK(!db.HasRecoveredSigForId(type, id));
        BOOST_CHECK(db.HasRecoveredSigForHash(sig.GetHash()));
        BOOST_CHECK(db.HasRecoveredSigForSession(sig.buildSignHash().Get()));
    }
    {
        auto legacy_reader = util::MakeDbWrapper({.path = path / "llmq" / "recsigdb"});
        std::pair<Consensus::LLMQType, uint256> owner;
        BOOST_REQUIRE(legacy_reader->Read(std::make_pair(std::string("rs_h"), sig.GetHash()), owner));
        BOOST_CHECK(owner == std::make_pair(type, id));
        BOOST_CHECK(legacy_reader->Exists(std::make_pair(std::string("rs_s"), sig.buildSignHash().Get())));
    }
    {
        CRecoveredSigsDb db({.path = path});
        BOOST_CHECK(db.HasRecoveredSigForHash(sig.GetHash()));
        BOOST_CHECK(db.HasRecoveredSigForSession(sig.buildSignHash().Get()));
        SetMockTime(now + 99);
        db.CleanupOldRecoveredSigs(100);
        BOOST_CHECK(db.HasRecoveredSigForHash(sig.GetHash()));
        BOOST_CHECK(db.HasRecoveredSigForSession(sig.buildSignHash().Get()));
        SetMockTime(now + 101);
        db.CleanupOldRecoveredSigs(100);
        BOOST_CHECK(!db.HasRecoveredSigForHash(sig.GetHash()));
        BOOST_CHECK(!db.HasRecoveredSigForSession(sig.buildSignHash().Get()));
    }
    {
        CRecoveredSigsDb db({.path = path});
        BOOST_CHECK(!db.HasRecoveredSigForHash(sig.GetHash()));
        BOOST_CHECK(!db.HasRecoveredSigForSession(sig.buildSignHash().Get()));
    }
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(recovered_sig_reinsertion_survives_stale_expiry)
{
    const auto type = Params().GetConsensus().llmqTypeChainLocks;
    const auto id = GetTestQuorumHash(1);
    const auto quorum = GetTestQuorumHash(2);
    CBLSSecretKey key;
    key.MakeNewKey();
    const auto make_sig = [&](uint32_t message) {
        const auto msg = GetTestQuorumHash(message);
        return CRecoveredSig{type, quorum, id, msg,
                             key.Sign(SignHash{type, quorum, id, msg}.Get(), bls::bls_legacy_scheme.load())};
    };
    const auto now = GetTime();
    for (const bool same_message : {false, true}) {
        const auto first = make_sig(3);
        const auto second = make_sig(same_message ? 3 : 4);
        const auto path = m_args.GetDataDirBase() / (same_message ? "refreshed_sigs" : "reinserted_sigs");
        {
            CRecoveredSigsDb db({.path = path, .wipe = true});
            SetMockTime(now);
            db.WriteRecoveredSig(first);
            db.TruncateRecoveredSig(type, id);
            SetMockTime(now + 60);
            db.WriteRecoveredSig(second);
        }
        {
            CRecoveredSigsDb db({.path = path});
            SetMockTime(now + 101);
            db.CleanupOldRecoveredSigs(100);
            CRecoveredSig stored;
            BOOST_REQUIRE(db.GetRecoveredSigById(type, id, stored));
            BOOST_CHECK(stored.GetHash() == second.GetHash());
            BOOST_CHECK(db.HasRecoveredSigForHash(second.GetHash()));
            BOOST_CHECK(db.HasRecoveredSigForSession(second.buildSignHash().Get()));
            if (!same_message) {
                BOOST_CHECK(!db.HasRecoveredSigForHash(first.GetHash()));
                BOOST_CHECK(!db.HasRecoveredSigForSession(first.buildSignHash().Get()));
            }
            SetMockTime(now + 161);
            db.CleanupOldRecoveredSigs(100);
            BOOST_CHECK(!db.HasRecoveredSigForId(type, id));
            BOOST_CHECK(!db.HasRecoveredSigForHash(second.GetHash()));
            BOOST_CHECK(!db.HasRecoveredSigForSession(second.buildSignHash().Get()));
        }
    }
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(legacy_recovered_sig_markers_migrate_with_bounded_grace)
{
    const auto type = Params().GetConsensus().llmqTypeChainLocks;
    const auto path = m_args.GetDataDirBase() / "legacy_markers";
    const auto db_path = path / "llmq" / "recsigdb";
    const auto now = GetTime();
    // Serialized legacy index records can outlive both the signature and its old rs_t entry.
    {
        auto legacy = util::MakeDbWrapper({.path = db_path, .wipe = true});
        CDBBatch batch{*legacy};
        for (uint32_t i = 1; i <= 3; ++i) {
            const auto hash = GetTestQuorumHash(i);
            batch.Write(std::make_pair(std::string("rs_h"), hash), std::make_pair(type, hash));
            batch.Write(std::make_pair(std::string("rs_s"), hash), uint8_t{1});
        }
        BOOST_REQUIRE(legacy->WriteBatch(batch));
    }
    {
        CRecoveredSigsDb db({.path = path});
        SetMockTime(now);
        db.CleanupOldRecoveredSigs(100);
        for (uint32_t i = 1; i <= 3; ++i) {
            BOOST_CHECK(db.HasRecoveredSigForHash(GetTestQuorumHash(i)));
            BOOST_CHECK(db.HasRecoveredSigForSession(GetTestQuorumHash(i)));
        }
        SetMockTime(now + 101);
        db.CleanupOldRecoveredSigs(100);
        for (uint32_t i = 1; i <= 3; ++i) {
            BOOST_CHECK(!db.HasRecoveredSigForHash(GetTestQuorumHash(i)));
            BOOST_CHECK(!db.HasRecoveredSigForSession(GetTestQuorumHash(i)));
        }
    }
    // A later legacy writer must be adopted again, even after an earlier sweep completed.
    const auto later = GetTestQuorumHash(2000);
    {
        auto legacy = util::MakeDbWrapper({.path = db_path});
        legacy->Write(std::make_pair(std::string("rs_h"), later), std::make_pair(type, later));
        legacy->Write(std::make_pair(std::string("rs_s"), later), uint8_t{1});
    }
    {
        CRecoveredSigsDb db({.path = path});
        SetMockTime(now + 300);
        db.CleanupOldRecoveredSigs(100);
        BOOST_CHECK(db.HasRecoveredSigForHash(later));
        BOOST_CHECK(db.HasRecoveredSigForSession(later));
        SetMockTime(now + 401);
        db.CleanupOldRecoveredSigs(100);
        BOOST_CHECK(!db.HasRecoveredSigForHash(later));
        BOOST_CHECK(!db.HasRecoveredSigForSession(later));
    }
    SetMockTime(0);
}

static CSigSesAnn MakeSigSesAnn(uint32_t session_id, uint32_t nonce, Consensus::LLMQType llmq_type = Consensus::LLMQType::LLMQ_50_60)
{
    return CSigSesAnn{session_id, llmq_type, GetTestQuorumHash(1), GetTestQuorumHash(2), GetTestQuorumHash(nonce)};
}

static CSigShare MakeSigShare(uint32_t nonce, Consensus::LLMQType llmq_type = Consensus::LLMQType::LLMQ_50_60)
{
    CSigShare sig_share{llmq_type, GetTestQuorumHash(1), GetTestQuorumHash(2), GetTestQuorumHash(nonce), 1, CBLSLazySignature{}};
    sig_share.UpdateKey();
    return sig_share;
}

BOOST_AUTO_TEST_CASE(sig_ses_ann_respects_session_limit_but_allows_refresh)
{
    CSigSharesNodeState node_state;

    const CSigSesAnn ann1{MakeSigSesAnn(1, 1)};
    const CSigSesAnn ann2{MakeSigSesAnn(2, 2)};
    const CSigSesAnn ann3{MakeSigSesAnn(3, 3)};
    constexpr size_t max_sessions{2};

    BOOST_CHECK(node_state.CanCreateSessionFromAnn(ann1, max_sessions));
    node_state.GetOrCreateSessionFromAnn(ann1);
    BOOST_CHECK_EQUAL(node_state.GetSessionCount(), 1U);
    BOOST_CHECK_EQUAL(node_state.GetAnnouncementSessionCount(Consensus::LLMQType::LLMQ_50_60), 1U);

    BOOST_CHECK(node_state.CanCreateSessionFromAnn(ann2, max_sessions));
    node_state.GetOrCreateSessionFromAnn(ann2);
    BOOST_CHECK_EQUAL(node_state.GetSessionCount(), max_sessions);
    BOOST_CHECK_EQUAL(node_state.GetAnnouncementSessionCount(Consensus::LLMQType::LLMQ_50_60), max_sessions);

    BOOST_CHECK(!node_state.CanCreateSessionFromAnn(ann3, max_sessions));

    const CSigSesAnn ann1_refresh{4, Consensus::LLMQType::LLMQ_50_60, ann1.getQuorumHash(), ann1.getId(), ann1.getMsgHash()};
    BOOST_CHECK(node_state.CanCreateSessionFromAnn(ann1_refresh, max_sessions));
    node_state.GetOrCreateSessionFromAnn(ann1_refresh);
    BOOST_CHECK_EQUAL(node_state.GetSessionCount(), max_sessions);
    BOOST_CHECK_EQUAL(node_state.GetAnnouncementSessionCount(Consensus::LLMQType::LLMQ_50_60), max_sessions);
}

BOOST_AUTO_TEST_CASE(sig_ses_ann_limit_ignores_send_only_sessions)
{
    CSigSharesNodeState node_state;

    constexpr size_t max_sessions{1};
    const CSigShare sig_share{MakeSigShare(1)};
    const CSigSesAnn ann{MakeSigSesAnn(1, 2)};

    node_state.GetOrCreateSessionFromShare(sig_share);
    BOOST_CHECK_EQUAL(node_state.GetSessionCount(Consensus::LLMQType::LLMQ_50_60), 1U);
    BOOST_CHECK_EQUAL(node_state.GetAnnouncementSessionCount(Consensus::LLMQType::LLMQ_50_60), 0U);

    BOOST_CHECK(node_state.CanCreateSessionFromAnn(ann, max_sessions));
    node_state.GetOrCreateSessionFromAnn(ann);
    BOOST_CHECK_EQUAL(node_state.GetSessionCount(Consensus::LLMQType::LLMQ_50_60), 2U);
    BOOST_CHECK_EQUAL(node_state.GetAnnouncementSessionCount(Consensus::LLMQType::LLMQ_50_60), 1U);
}

BOOST_AUTO_TEST_CASE(sig_ses_ann_limit_is_per_llmq_type)
{
    CSigSharesNodeState node_state;

    constexpr size_t max_sessions{1};
    const CSigSesAnn ann1{MakeSigSesAnn(1, 1)};
    const CSigSesAnn ann2{MakeSigSesAnn(2, 2)};
    const CSigSesAnn other_type_ann{MakeSigSesAnn(3, 3, Consensus::LLMQType::LLMQ_400_60)};

    BOOST_CHECK(node_state.CanCreateSessionFromAnn(ann1, max_sessions));
    node_state.GetOrCreateSessionFromAnn(ann1);
    BOOST_CHECK_EQUAL(node_state.GetSessionCount(), 1U);
    BOOST_CHECK_EQUAL(node_state.GetSessionCount(Consensus::LLMQType::LLMQ_50_60), 1U);

    BOOST_CHECK(!node_state.CanCreateSessionFromAnn(ann2, max_sessions));
    BOOST_CHECK(node_state.CanCreateSessionFromAnn(other_type_ann, max_sessions));
    node_state.GetOrCreateSessionFromAnn(other_type_ann);
    BOOST_CHECK_EQUAL(node_state.GetSessionCount(), 2U);
    BOOST_CHECK_EQUAL(node_state.GetSessionCount(Consensus::LLMQType::LLMQ_400_60), 1U);
}

BOOST_AUTO_TEST_CASE(sig_share_map_size_tracks_mutations)
{
    SigShareMap<CSigShare> sig_share_map;
    const CSigShare sig_share1{MakeSigShare(1)};
    const CSigShare sig_share2{MakeSigShare(2)};

    BOOST_CHECK(sig_share_map.Add(sig_share1.GetKey(), sig_share1));
    BOOST_CHECK(!sig_share_map.Add(sig_share1.GetKey(), sig_share1));
    BOOST_CHECK(sig_share_map.Add(sig_share2.GetKey(), sig_share2));
    BOOST_CHECK_EQUAL(sig_share_map.Size(), 2U);

    sig_share_map.Erase(sig_share1.GetKey());
    sig_share_map.Erase(sig_share1.GetKey());
    BOOST_CHECK_EQUAL(sig_share_map.Size(), 1U);

    sig_share_map.EraseAllForSignHash(sig_share2.GetSignHash());
    sig_share_map.EraseAllForSignHash(sig_share2.GetSignHash());
    BOOST_CHECK_EQUAL(sig_share_map.Size(), 0U);
    BOOST_CHECK(sig_share_map.Empty());

    BOOST_CHECK(sig_share_map.Add(sig_share1.GetKey(), sig_share1));
    BOOST_CHECK(sig_share_map.Add(sig_share2.GetKey(), sig_share2));
    sig_share_map.EraseIf([&](const SigShareKey& k, const CSigShare&) { return k == sig_share1.GetKey(); });
    BOOST_CHECK_EQUAL(sig_share_map.Size(), 1U);

    sig_share_map.Clear();
    BOOST_CHECK_EQUAL(sig_share_map.Size(), 0U);
}

BOOST_AUTO_TEST_CASE(sig_share_map_bucket_erase_updates_size)
{
    SigShareMap<CSigShare> sig_share_map;
    const auto sign_hash = MakeSigShare(1).GetSignHash();

    for (uint16_t member = 0; member < 5; ++member) {
        CSigShare s{Consensus::LLMQType::LLMQ_50_60, GetTestQuorumHash(1), GetTestQuorumHash(2), GetTestQuorumHash(1),
                    member, CBLSLazySignature{}};
        s.UpdateKey();
        BOOST_CHECK_EQUAL(s.GetSignHash(), sign_hash);
        BOOST_CHECK(sig_share_map.Add(s.GetKey(), s));
    }
    BOOST_CHECK_EQUAL(sig_share_map.Size(), 5U);

    sig_share_map.EraseAllForSignHash(sign_hash);
    BOOST_CHECK(sig_share_map.Empty());
}

BOOST_AUTO_TEST_CASE(pending_sig_shares_session_removal_updates_count)
{
    CSigSharesNodeState node_state;
    const CSigShare sig_share1{MakeSigShare(1)};
    const CSigShare sig_share2{MakeSigShare(2)};

    BOOST_CHECK(node_state.pendingIncomingSigShares.Add(sig_share1.GetKey(), sig_share1));
    BOOST_CHECK(node_state.pendingIncomingSigShares.Add(sig_share2.GetKey(), sig_share2));
    BOOST_CHECK_EQUAL(node_state.pendingIncomingSigShares.Size(), 2U);

    node_state.RemoveSession(sig_share1.GetSignHash());
    BOOST_CHECK_EQUAL(node_state.pendingIncomingSigShares.Size(), 1U);
    BOOST_CHECK(!node_state.pendingIncomingSigShares.Has(sig_share1.GetKey()));
    BOOST_CHECK(node_state.pendingIncomingSigShares.Has(sig_share2.GetKey()));

    // Removing the same session twice, or a session with no pending shares, is a no-op.
    node_state.RemoveSession(sig_share1.GetSignHash());
    node_state.RemoveSession(MakeSigShare(3).GetSignHash());
    BOOST_CHECK_EQUAL(node_state.pendingIncomingSigShares.Size(), 1U);
}

BOOST_AUTO_TEST_CASE(sig_share_batch_verifier_judges_each_sender_by_its_own_share)
{
    const bool legacy_scheme{bls::bls_legacy_scheme.load()};
    const CSigShare sig_share{MakeSigShare(1)};

    CBLSSecretKey sk_share;
    sk_share.MakeNewKey();
    const CBLSPublicKey pk_share{sk_share.GetPublicKey()};
    const CBLSSignature valid_sig{sk_share.Sign(sig_share.GetSignHash(), legacy_scheme)};

    CBLSSecretKey sk_forger;
    sk_forger.MakeNewKey();
    const CBLSSignature forged_sig{sk_forger.Sign(sig_share.GetSignHash(), legacy_scheme)};

    constexpr NodeId honest{1};
    constexpr NodeId attacker{2};
    constexpr NodeId other_honest{3};

    struct Push {
        NodeId from;
        CBLSSignature sig;
    };
    const std::vector<std::pair<std::vector<Push>, std::set<NodeId>>> cases{
        // Honest share first: the forged copy of the same key must still be checked, not shadowed.
        {{{honest, valid_sig}, {attacker, forged_sig}}, {attacker}},
        // Forged share first: the honest sender must be judged by its own share, not the attacker's.
        {{{attacker, forged_sig}, {honest, valid_sig}}, {attacker}},
        // The same valid share relayed by two peers is not misbehavior.
        {{{honest, valid_sig}, {other_honest, valid_sig}}, {}},
    };

    for (const auto& [pushes, expected_bad_sources] : cases) {
        SigShareBatchVerifier verifier(/*_secureVerification=*/false, /*_perMessageFallback=*/true);
        for (const auto& push : pushes) {
            PushSigShareToBatchVerifier(verifier, push.from, sig_share, push.sig, pk_share);
        }
        verifier.Verify();
        BOOST_CHECK(verifier.badSources == expected_bad_sources);
    }
}

BOOST_AUTO_TEST_CASE(batched_sig_shares_rejects_oversized_inner_vector)
{
    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    stream << VARINT(uint32_t{1});
    WriteCompactSize(stream, MAX_MSGS_TOTAL_BATCHED_SIGS + 1);

    CBatchedSigShares batched_sig_shares;
    BOOST_CHECK_THROW(stream >> batched_sig_shares, std::ios_base::failure);
    BOOST_CHECK(batched_sig_shares.sigShares.empty());
}

BOOST_AUTO_TEST_CASE(batched_sig_shares_accepts_max_inner_vector)
{
    CBatchedSigShares batched;
    batched.sessionId = 1;
    batched.sigShares.resize(MAX_MSGS_TOTAL_BATCHED_SIGS); // exactly the cap must be accepted

    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    stream << batched;

    CBatchedSigShares roundtripped;
    BOOST_CHECK_NO_THROW(stream >> roundtripped);
    BOOST_CHECK_EQUAL(roundtripped.sigShares.size(), MAX_MSGS_TOTAL_BATCHED_SIGS);
}

static CBatchedSigShares MakeBatch(uint32_t session_id, size_t share_count)
{
    CBatchedSigShares batch;
    batch.sessionId = session_id;
    batch.sigShares.resize(share_count); // default pairs are enough to exercise the count invariant
    return batch;
}

// Exercise the production QBSIGSHARES decoder directly: the outer batch count is
// bounded regardless of the inner contents. Every batch here is empty, so the
// aggregate running-total guard never fires; only the outer-count guard can
// reject the stream, which keeps this case a genuine test of that guard rather
// than an end-of-stream artifact.
BOOST_AUTO_TEST_CASE(qbsigshares_rejects_oversized_batch_count)
{
    std::vector<CBatchedSigShares> msgs(MAX_MSGS_TOTAL_BATCHED_SIGS + 1); // count over cap, 0 inner shares each

    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    stream << msgs;

    BOOST_CHECK_THROW(UnserializeBatchedSigShares(stream), std::ios_base::failure);
}

// The regression this targets: each batch is individually within the per-batch
// LIMITED_VECTOR cap, but their running aggregate exceeds MAX_MSGS_TOTAL_BATCHED_SIGS,
// so the decoder must abort mid-stream rather than accept the cross product.
BOOST_AUTO_TEST_CASE(qbsigshares_rejects_oversized_aggregate_total)
{
    std::vector<CBatchedSigShares> msgs;
    msgs.push_back(MakeBatch(1, 300));
    msgs.push_back(MakeBatch(2, 200)); // 300 + 200 = 500 > 400, though each batch is <= the cap

    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    stream << msgs;

    BOOST_CHECK_THROW(UnserializeBatchedSigShares(stream), std::ios_base::failure);
}

// Batches whose aggregate is exactly at the cap must be accepted and decoded intact.
BOOST_AUTO_TEST_CASE(qbsigshares_accepts_aggregate_at_cap)
{
    std::vector<CBatchedSigShares> msgs;
    msgs.push_back(MakeBatch(1, 200));
    msgs.push_back(MakeBatch(2, MAX_MSGS_TOTAL_BATCHED_SIGS - 200)); // aggregate == cap

    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    stream << msgs;

    std::vector<CBatchedSigShares> decoded;
    BOOST_CHECK_NO_THROW(decoded = UnserializeBatchedSigShares(stream));
    BOOST_CHECK_EQUAL(decoded.size(), 2U);
    BOOST_CHECK_EQUAL(decoded[0].sigShares.size(), 200U);
    BOOST_CHECK_EQUAL(decoded[1].sigShares.size(), MAX_MSGS_TOTAL_BATCHED_SIGS - 200);
}

BOOST_AUTO_TEST_CASE(deterministic_outbound_connection_test)
{
    // Test deterministic behavior
    // DeterministicOutboundConnection returns one of the two input hashes based on a deterministic calculation
    uint256 proTxHash1 = GetTestQuorumHash(1);
    uint256 proTxHash2 = GetTestQuorumHash(2);

    // Same inputs should produce same output
    uint256 conn1a = llmq::utils::DeterministicOutboundConnection(proTxHash1, proTxHash2);
    uint256 conn1b = llmq::utils::DeterministicOutboundConnection(proTxHash1, proTxHash2);
    BOOST_CHECK(conn1a == conn1b);
    // Result should be one of the input hashes
    BOOST_CHECK(conn1a == proTxHash1 || conn1a == proTxHash2);

    // Swapped inputs should produce the same result (commutative)
    // The function deterministically selects which node initiates the connection
    uint256 conn2 = llmq::utils::DeterministicOutboundConnection(proTxHash2, proTxHash1);
    BOOST_CHECK(conn1a == conn2);

    // The result should consistently be the same node regardless of order
    BOOST_CHECK(llmq::utils::DeterministicOutboundConnection(proTxHash1, proTxHash2) ==
                llmq::utils::DeterministicOutboundConnection(proTxHash2, proTxHash1));
}

BOOST_AUTO_TEST_CASE(deterministic_outbound_connection_edge_cases_test)
{
    // Test with null hashes
    uint256 nullHash;
    uint256 validHash = GetTestQuorumHash(1);

    // DeterministicOutboundConnection returns one of the input hashes
    uint256 conn1 = llmq::utils::DeterministicOutboundConnection(nullHash, validHash);
    uint256 conn2 = llmq::utils::DeterministicOutboundConnection(validHash, nullHash);
    uint256 conn3 = llmq::utils::DeterministicOutboundConnection(nullHash, nullHash);

    // With null and valid hash, should return one of them
    BOOST_CHECK(conn1 == nullHash || conn1 == validHash);
    BOOST_CHECK(conn2 == nullHash || conn2 == validHash);
    // Since the function is order-independent, conn1 and conn2 should be the same
    BOOST_CHECK(conn1 == conn2);

    // With two null hashes, should return null
    BOOST_CHECK(conn3 == nullHash);

    // Test with same source and destination
    uint256 sameHash = GetTestQuorumHash(42);
    uint256 connSame = llmq::utils::DeterministicOutboundConnection(sameHash, sameHash);
    // Should return the same hash
    BOOST_CHECK(connSame == sameHash);
    BOOST_CHECK(!connSame.IsNull());
}

// Note: CalcDeterministicWatchConnections requires CBlockIndex which is complex to mock
// Testing is deferred to functional tests

BOOST_AUTO_TEST_CASE(deterministic_connection_symmetry_test)
{
    // Test interesting properties of DeterministicOutboundConnection
    uint256 proTxHash1 = GetTestQuorumHash(1);
    uint256 proTxHash2 = GetTestQuorumHash(2);
    uint256 proTxHash3 = GetTestQuorumHash(3);

    // Create a "network" of connections
    // DeterministicOutboundConnection is symmetric - order doesn't matter
    uint256 conn12 = llmq::utils::DeterministicOutboundConnection(proTxHash1, proTxHash2);
    uint256 conn21 = llmq::utils::DeterministicOutboundConnection(proTxHash2, proTxHash1);
    uint256 conn13 = llmq::utils::DeterministicOutboundConnection(proTxHash1, proTxHash3);
    uint256 conn31 = llmq::utils::DeterministicOutboundConnection(proTxHash3, proTxHash1);
    uint256 conn23 = llmq::utils::DeterministicOutboundConnection(proTxHash2, proTxHash3);
    uint256 conn32 = llmq::utils::DeterministicOutboundConnection(proTxHash3, proTxHash2);

    // Verify symmetry - swapped inputs produce same output
    BOOST_CHECK(conn12 == conn21);
    BOOST_CHECK(conn13 == conn31);
    BOOST_CHECK(conn23 == conn32);

    // Each connection returns one of the two nodes
    BOOST_CHECK(conn12 == proTxHash1 || conn12 == proTxHash2);
    BOOST_CHECK(conn13 == proTxHash1 || conn13 == proTxHash3);
    BOOST_CHECK(conn23 == proTxHash2 || conn23 == proTxHash3);

    // The function deterministically picks which node initiates the connection
    // Verify we get consistent results for each pair
    std::set<uint256> uniqueResults;
    uniqueResults.insert(conn12);
    uniqueResults.insert(conn13);
    uniqueResults.insert(conn23);
    // Each pair should produce one of its members, but pairs may have overlapping results
    BOOST_CHECK(uniqueResults.size() >= 2 && uniqueResults.size() <= 3);
}

BOOST_AUTO_TEST_CASE(legacy_quarter_walk_pinned)
{
    using Positions = std::vector<std::vector<size_t>>;
    const auto walk = [](size_t list_size, size_t quorums, size_t quarter, const std::vector<size_t>& used,
                         const std::set<size_t>& rejected) {
        return llmq::utils::WalkLegacyQuarters(list_size, quorums, quarter, used,
                                               [&](size_t, size_t pos) { return rejected.count(pos) == 0; });
    };

    // (a) no skips
    auto res = walk(6, 2, 2, {0, 0}, {});
    BOOST_REQUIRE(res.has_value());
    BOOST_CHECK(res->quarters == (Positions{{0, 1}, {2, 3}}));
    BOOST_CHECK(res->skip_list.empty());
    BOOST_CHECK(llmq::utils::ReplayLegacyQuarters(5, 3, 2, {}) == (Positions{{0, 1}, {2, 3}, {4, 0}}));

    // (b) first skip at position 0 leaves firstSkippedIndex at 0, so the second entry stays absolute
    // and becomes the new base; the third is relative to that base
    res = walk(8, 1, 4, {0}, {0, 2, 5});
    BOOST_REQUIRE(res.has_value());
    BOOST_CHECK(res->quarters == (Positions{{1, 3, 4, 6}}));
    BOOST_CHECK((res->skip_list == std::vector<int>{0, 2, 3}));
    // the decoder has the same first_entry_index == 0 quirk and reproduces the positions
    BOOST_CHECK(llmq::utils::ReplayLegacyQuarters(8, 1, 4, res->skip_list) == res->quarters);

    // (c) the builder stops early once no unused candidate is left, the replay pads by wrapping
    res = walk(5, 2, 3, {0, 3}, {});
    BOOST_REQUIRE(res.has_value());
    BOOST_CHECK(res->quarters == (Positions{{0, 1, 2}, {3, 4}}));
    BOOST_CHECK(res->skip_list.empty());
    BOOST_CHECK(llmq::utils::ReplayLegacyQuarters(5, 2, 3, res->skip_list) == (Positions{{0, 1, 2}, {3, 4, 0}}));

    // a full pass without progress gives up
    BOOST_CHECK(!walk(3, 1, 2, {0}, {0, 1, 2}).has_value());

    // (d) serialized snapshot holding (b)'s skip list
    llmq::CQuorumSnapshot snapshot{{true, false, true, true, false, false, false, true},
                                   SnapshotSkipMode::MODE_SKIPPING_ENTRIES, {0, 2, 3}};
    DataStream ss{};
    ss << snapshot;
    BOOST_CHECK_EQUAL(HexStr(ss), "01000000" "08" "8d" "03" "00000000" "02000000" "03000000");
}

namespace {
using Positions = std::vector<std::vector<size_t>>;

// can_add that rejects positions already used by the index (and records additions) or failing for index 0
struct StepWalkFixture {
    std::vector<std::vector<bool>> used;
    std::set<size_t> failing_for_index0;

    StepWalkFixture(size_t list_size, size_t quorums) : used(quorums, std::vector<bool>(list_size, false)) {}

    bool CanAdd(size_t i, size_t pos)
    {
        if (used[i][pos] || (i == 0 && failing_for_index0.count(pos) != 0)) return false;
        used[i][pos] = true;
        return true;
    }
    std::optional<llmq::utils::QuarterWalkResult> Walk(size_t list_size, size_t quorums, size_t quarter)
    {
        return llmq::utils::WalkNewQuarters(list_size, quorums, quarter,
                                            [this](size_t i, size_t pos) { return CanAdd(i, pos); });
    }
};
} // namespace

BOOST_AUTO_TEST_CASE(quarter_walk_roundtrip_exhaustive)
{
    for (size_t list_size = 0; list_size <= 5; ++list_size) {
        for (size_t quorums = 1; quorums <= 3; ++quorums) {
            for (size_t quarter = 1; quarter <= 2; ++quarter) {
                const size_t used_combos = size_t{1} << (list_size * quorums);
                for (size_t used_mask = 0; used_mask < used_combos; ++used_mask) {
                    for (size_t fail_mask = 0; fail_mask < (size_t{1} << list_size); ++fail_mask) {
                        StepWalkFixture fx(list_size, quorums);
                        for (size_t i = 0; i < quorums; ++i) {
                            for (size_t p = 0; p < list_size; ++p) {
                                fx.used[i][p] = (used_mask >> (i * list_size + p)) & 1;
                            }
                        }
                        for (size_t p = 0; p < list_size; ++p) {
                            if ((fail_mask >> p) & 1) fx.failing_for_index0.insert(p);
                        }
                        const auto walk = fx.Walk(list_size, quorums, quarter);
                        BOOST_REQUIRE(walk.has_value());
                        const auto replayed = llmq::utils::ReplayNewQuarters(list_size, quorums, quarter, walk->skip_list);
                        BOOST_REQUIRE(replayed.has_value());
                        BOOST_CHECK(*replayed == walk->quarters);
                        BOOST_CHECK_LE(walk->skip_list.size(), 2 * list_size * quorums);
                        BOOST_REQUIRE_EQUAL(walk->quarters.size(), quorums);
                        for (size_t i = 0; i < quorums; ++i) {
                            const auto& q = walk->quarters[i];
                            BOOST_CHECK_LE(q.size(), quarter);
                            BOOST_CHECK_EQUAL(std::set<size_t>(q.begin(), q.end()).size(), q.size());
                            if (q.size() < quarter) {
                                for (size_t p = 0; p < list_size; ++p) {
                                    // fx.used now includes the additions
                                    BOOST_CHECK(fx.used[i][p] || (i == 0 && fx.failing_for_index0.count(p) != 0));
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(quarter_walk_collision_case)
{
    // the legacy encoding records skips [0] for both of these
    StepWalkFixture index1_has_0(1, 2);
    index1_has_0.used[1][0] = true;
    const auto a = index1_has_0.Walk(1, 2, 1);
    BOOST_REQUIRE(a.has_value());
    BOOST_CHECK(a->quarters == (Positions{{0}, {}}));

    StepWalkFixture index0_has_0(1, 2);
    index0_has_0.used[0][0] = true;
    const auto b = index0_has_0.Walk(1, 2, 1);
    BOOST_REQUIRE(b.has_value());
    BOOST_CHECK(b->quarters == (Positions{{}, {0}}));

    BOOST_CHECK(a->skip_list != b->skip_list);
    BOOST_CHECK(llmq::utils::ReplayNewQuarters(1, 2, 1, a->skip_list) == a->quarters);
    BOOST_CHECK(llmq::utils::ReplayNewQuarters(1, 2, 1, b->skip_list) == b->quarters);
}

BOOST_AUTO_TEST_CASE(quarter_walk_addmn_failure_short)
{
    StepWalkFixture fx(5, 1);
    fx.failing_for_index0 = {1, 2, 3};
    const auto walk = fx.Walk(5, 1, 3);
    BOOST_REQUIRE(walk.has_value());
    BOOST_CHECK(walk->quarters == (Positions{{0, 4}}));
    BOOST_CHECK(llmq::utils::ReplayNewQuarters(5, 1, 3, walk->skip_list) == walk->quarters);
}

BOOST_AUTO_TEST_CASE(quarter_walk_empty_list)
{
    StepWalkFixture fx(0, 3);
    const auto walk = fx.Walk(0, 3, 2);
    BOOST_REQUIRE(walk.has_value());
    BOOST_CHECK(walk->quarters == (Positions{{}, {}, {}}));
    BOOST_CHECK(walk->skip_list.empty());
    BOOST_CHECK(llmq::utils::ReplayNewQuarters(0, 3, 2, {}) == walk->quarters);
}

BOOST_AUTO_TEST_CASE(quarter_walk_replay_rejects_malformed)
{
    const auto replay = [](const std::vector<int>& skips) {
        return llmq::utils::ReplayNewQuarters(4, 2, 2, skips);
    };
    BOOST_CHECK(!replay({-1}).has_value());
    BOOST_CHECK(!replay({0, 0}).has_value());
    BOOST_CHECK(!replay({3, -1}).has_value());
    BOOST_CHECK(!replay({0, std::numeric_limits<int>::max(), 1}).has_value());

    // a valid list with one extra trailing entry is not fully consumed
    BOOST_CHECK(replay({1}).has_value());
    BOOST_CHECK(!replay({1, 1, 1, 1, 1, 1, 1, 1, 1, 1}).has_value());
    StepWalkFixture fx(4, 2);
    fx.failing_for_index0 = {1};
    const auto walk = fx.Walk(4, 2, 2);
    BOOST_REQUIRE(walk.has_value());
    BOOST_REQUIRE(!walk->skip_list.empty());
    BOOST_CHECK(llmq::utils::ReplayNewQuarters(4, 2, 2, walk->skip_list).has_value());
    auto extended = walk->skip_list;
    extended.push_back(1000); // a skip step the walk never reaches
    BOOST_CHECK(!llmq::utils::ReplayNewQuarters(4, 2, 2, extended).has_value());

    // wrapping around within one quarter would repeat a position, which the walk never does
    BOOST_CHECK(!llmq::utils::ReplayNewQuarters(2, 1, 3, {}).has_value());
    BOOST_CHECK(!llmq::utils::ReplayNewQuarters(3, 1, 3, std::vector<int>{1}).has_value());
    BOOST_CHECK(llmq::utils::ReplayNewQuarters(2, 1, 2, {}) == (Positions{{0, 1}}));
}

BOOST_AUTO_TEST_CASE(quarter_walk_snapshot_roundtrip)
{
    const auto check = [](StepWalkFixture& fx, size_t list_size, size_t quorums, size_t quarter) {
        const auto walk = fx.Walk(list_size, quorums, quarter);
        BOOST_REQUIRE(walk.has_value());
        BOOST_REQUIRE(!walk->skip_list.empty());
        llmq::CQuorumSnapshot snapshot{std::vector<bool>(list_size, true), SnapshotSkipMode::MODE_SKIPPING_ENTRIES,
                                       walk->skip_list};
        DataStream ss{};
        ss << snapshot;
        llmq::CQuorumSnapshot decoded;
        ss >> decoded;
        BOOST_CHECK(decoded.mnSkipListMode == SnapshotSkipMode::MODE_SKIPPING_ENTRIES);
        const auto replayed = llmq::utils::ReplayNewQuarters(list_size, quorums, quarter, decoded.mnSkipList);
        BOOST_REQUIRE(replayed.has_value());
        BOOST_CHECK(*replayed == walk->quarters);
    };

    StepWalkFixture collision(1, 2);
    collision.used[0][0] = true;
    check(collision, 1, 2, 1);

    StepWalkFixture failure(5, 1);
    failure.failing_for_index0 = {1, 2, 3};
    check(failure, 5, 1, 3);
}

BOOST_AUTO_TEST_CASE(quarter_steps_prediction_rule)
{
    using llmq::utils::QuarterStepsRuleForPrediction;
    constexpr std::optional<bool> active{true}, inactive{false}, unknown{std::nullopt};
    // {period, state after the work block, rule at the cycle base}: up to ceil(7 / period) periods start
    // between the block after the work block and the cycle base
    const std::vector<std::tuple<int, ThresholdState, std::optional<bool>>> cases{
        // longer than WORK_DIFF_DEPTH (mainnet 4032, testnet 100): at most one period starts in between
        {4032, ThresholdState::ACTIVE, active},
        {4032, ThresholdState::FAILED, inactive},
        {4032, ThresholdState::DEFINED, inactive},
        {4032, ThresholdState::STARTED, inactive},
        {4032, ThresholdState::LOCKED_IN, unknown},
        {7, ThresholdState::STARTED, inactive},
        // shorter (the 5-block window of feature_llmq_rotation_short_quarters.py): two may start
        {5, ThresholdState::ACTIVE, active},
        {5, ThresholdState::FAILED, inactive},
        {5, ThresholdState::DEFINED, inactive},
        {5, ThresholdState::STARTED, unknown},
        {5, ThresholdState::LOCKED_IN, unknown},
        {4, ThresholdState::DEFINED, inactive},
        // three or more: even DEFINED may reach ACTIVE
        {3, ThresholdState::DEFINED, unknown},
        {1, ThresholdState::DEFINED, unknown},
        {1, ThresholdState::FAILED, inactive},
        {1, ThresholdState::ACTIVE, active},
        // no overflow for the longest period -vbparams accepts
        {std::numeric_limits<int>::max(), ThresholdState::STARTED, inactive},
    };
    for (const auto& [period, state, expected] : cases) {
        BOOST_CHECK_MESSAGE(QuarterStepsRuleForPrediction(state, period) == expected,
                            strprintf("period %d state %d", period, static_cast<int>(state)));
    }
}

namespace {
// Regtest chain with v19/v20 active from height 109 and quarter_steps always active
struct QuarterStepsChainSetup : public TestChainSetup {
    QuarterStepsChainSetup() :
        TestChainSetup(103, CBaseChainParams::REGTEST,
                       {"-dip3params=100:500", "-testactivationheight=v19@109", "-testactivationheight=v20@109",
                        "-testactivationheight=mn_rr@109", "-vbparams=quarter_steps:-1:999999999999"}),
        chainman(*Assert(m_node.chainman)),
        dmnman(*Assert(m_node.dmnman)),
        qsnapman(*Assert(Assert(m_node.llmq_ctx)->qsnapman)),
        llmq_params(*Assert(Params().GetLLMQ(Consensus::LLMQType::LLMQ_TEST_DIP0024))),
        utxos(BuildSimpleUtxoMap(m_coinbase_txns)),
        coinbase_pk(GetScriptForRawPubKey(coinbaseKey.GetPubKey()))
    {
        MineTo(110);
    }

    const CBlockIndex* Tip() const { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()); }

    void Mine(const std::vector<CMutableTransaction>& txs = {})
    {
        CreateAndProcessBlock(txs, coinbase_pk);
        dmnman.UpdatedBlockTip(Tip());
    }

    void MineTo(int height)
    {
        while (Tip()->nHeight < height)
            Mine();
    }

    CMutableTransaction RegisterTx(int port, CBLSSecretKey& operator_key)
    {
        CKey owner_key;
        return CreateProRegTx(chainman, utxos, port, GenerateRandomAddress(), coinbaseKey, owner_key, operator_key);
    }

    CMutableTransaction RevokeTx(const uint256& pro_tx_hash, const CBLSSecretKey& operator_key)
    {
        CProUpRevTx pro_tx;
        pro_tx.nVersion = ProTxVersion::GetMax(!bls::bls_legacy_scheme, /*is_extended_addr=*/false);
        pro_tx.proTxHash = pro_tx_hash;
        CMutableTransaction tx;
        tx.nVersion = 3;
        tx.nType = TRANSACTION_PROVIDER_UPDATE_REVOKE;
        const auto spent = FundTransaction(chainman, tx, utxos, coinbase_pk, 1 * COIN);
        pro_tx.inputsHash = CalcTxInputsHash(CTransaction(tx));
        pro_tx.sig = operator_key.Sign(::SerializeHash(pro_tx), bls::bls_legacy_scheme);
        SetTxPayload(tx, pro_tx);
        SignTransaction(tx, spent, coinbaseKey);
        return tx;
    }

    CMutableTransaction UpServTx(const uint256& pro_tx_hash, const CBLSSecretKey& operator_key, int port)
    {
        CProUpServTx pro_tx;
        pro_tx.nVersion = ProTxVersion::GetMax(!bls::bls_legacy_scheme, /*is_extended_addr=*/false);
        pro_tx.netInfo = NetInfoInterface::MakeNetInfo(pro_tx.nVersion);
        pro_tx.proTxHash = pro_tx_hash;
        BOOST_REQUIRE(pro_tx.netInfo->AddEntry(NetInfoPurpose::CORE_P2P, strprintf("1.1.1.1:%d", port)) ==
                      NetInfoStatus::Success);
        CMutableTransaction tx;
        tx.nVersion = 3;
        tx.nType = TRANSACTION_PROVIDER_UPDATE_SERVICE;
        const auto spent = FundTransaction(chainman, tx, utxos, coinbase_pk, 1 * COIN);
        pro_tx.inputsHash = CalcTxInputsHash(CTransaction(tx));
        pro_tx.sig = operator_key.Sign(::SerializeHash(pro_tx), bls::bls_legacy_scheme);
        SetTxPayload(tx, pro_tx);
        SignTransaction(tx, spent, coinbaseKey);
        return tx;
    }

    std::vector<std::array<llmq::utils::QuorumMembers, 4>> Quarters(int cycle_height)
    {
        const CBlockIndex* base = Assert(Tip()->GetAncestor(cycle_height));
        const CBlockIndex* work = Assert(base->GetAncestor(cycle_height - llmq::WORK_DIFF_DEPTH));
        // v20 modifier of a work block without a coinbase chainlock
        BOOST_REQUIRE(!GetNonNullCoinbaseChainlock(work).has_value());
        const uint256 modifier = ::SerializeHash(std::make_pair(llmq_params.type, work->GetBlockHash()));
        return llmq::utils::ComputeRotationQuarters(llmq_params, {dmnman, qsnapman, chainman, base}, work, cycle_height,
                                                    modifier, /*predicting=*/false);
    }

    size_t CountStoredSnapshots()
    {
        const std::string prefix{"llmq_S"};
        std::unique_ptr<CDBIterator> it(Assert(m_node.evodb)->GetRawDB().NewIterator());
        size_t count{0};
        for (it->Seek(prefix); it->Valid(); it->Next()) {
            std::pair<std::string, uint256> key;
            if (!it->GetKey(key) || key.first != prefix) break;
            ++count;
        }
        return count;
    }

    ChainstateManager& chainman;
    CDeterministicMNManager& dmnman;
    llmq::CQuorumSnapshotManager& qsnapman;
    const Consensus::LLMQParams llmq_params;
    SimpleUTXOMap utxos;
    const CScript coinbase_pk;
};

std::vector<uint256> ProTxHashes(const llmq::utils::QuorumMembers& members)
{
    std::vector<uint256> ret;
    for (const auto& dmn : members)
        ret.push_back(dmn->proTxHash);
    return ret;
}
} // namespace

BOOST_FIXTURE_TEST_CASE(quarter_steps_builder_matches_rebuild, QuarterStepsChainSetup)
{
    // Two masternodes for quarters of one member each, first eligible at cycle 144: by its third cycle
    // every index has used both
    CBLSSecretKey op1, op2;
    Mine({RegisterTx(1, op1), RegisterTx(2, op2)});
    const int cycle{192};
    MineTo(cycle);

    const size_t quarter_size{static_cast<size_t>(llmq_params.size / 4)};
    const auto built = Quarters(cycle);
    BOOST_REQUIRE_EQUAL(built.size(), static_cast<size_t>(llmq_params.signingActiveQuorumCount));
    BOOST_REQUIRE(std::ranges::any_of(built, [&](const auto& q) { return q[3].size() < quarter_size; }));

    MineTo(cycle + llmq_params.dkgInterval);
    const auto next = Quarters(cycle + llmq_params.dkgInterval);
    BOOST_REQUIRE_EQUAL(next.size(), built.size());
    for (size_t i = 0; i < built.size(); ++i) {
        const auto expected = ProTxHashes(built[i][3]);
        const auto rebuilt = ProTxHashes(next[i][2]);
        BOOST_CHECK_EQUAL_COLLECTIONS(rebuilt.begin(), rebuilt.end(), expected.begin(), expected.end());
    }
}

namespace {
using CycleQuarters = std::vector<std::array<llmq::utils::QuorumMembers, 4>>;

// Mines to cycle and builds its quarters; no index may repeat a masternode, and the new quarters of the previous
// cycle, if built, must be rebuilt unchanged
void BuildAndCheckCycle(QuarterStepsChainSetup& setup, std::map<int, CycleQuarters>& built, int cycle)
{
    setup.MineTo(cycle);
    auto& quarters = built[cycle] = setup.Quarters(cycle);
    BOOST_REQUIRE_EQUAL(quarters.size(), static_cast<size_t>(setup.llmq_params.signingActiveQuorumCount));
    for (const auto& q : quarters) {
        std::vector<uint256> composition;
        for (const auto& quarter : q) {
            const auto hashes = ProTxHashes(quarter);
            composition.insert(composition.end(), hashes.begin(), hashes.end());
        }
        BOOST_CHECK_EQUAL(std::set<uint256>(composition.begin(), composition.end()).size(), composition.size());
    }
    if (const auto prev = built.find(cycle - setup.llmq_params.dkgInterval); prev != built.end()) {
        for (size_t i = 0; i < quarters.size(); ++i) {
            const auto expected = ProTxHashes(prev->second[i][3]);
            const auto rebuilt = ProTxHashes(quarters[i][2]);
            BOOST_CHECK_EQUAL_COLLECTIONS(rebuilt.begin(), rebuilt.end(), expected.begin(), expected.end());
        }
    }
}
} // namespace

BOOST_FIXTURE_TEST_CASE(quarter_steps_excludes_previous_members_by_protxhash, QuarterStepsChainSetup)
{
    // Two masternodes for quarters of one member each: cycle 144 gives each index one of them, cycle 168 the
    // other. Between their work blocks (136 and 160) the first moves to a new address and the second takes its
    // former one, so from cycle 192 one index has two previous members whose historical states share an
    // address. Both must stay excluded for that index.
    CBLSSecretKey op1, op2;
    const auto reg1 = RegisterTx(1, op1);
    const auto reg2 = RegisterTx(2, op2);
    Mine({reg1, reg2});
    MineTo(144);
    Mine({UpServTx(reg1.GetHash(), op1, 3)});
    Mine({UpServTx(reg2.GetHash(), op2, 1)});

    std::map<int, CycleQuarters> built;
    for (const int cycle : {168, 192, 216}) {
        BuildAndCheckCycle(*this, built, cycle);
    }

    size_t conflicting{0};
    for (const auto& q : built[192]) {
        BOOST_REQUIRE_EQUAL(q[1].size(), 1U);
        BOOST_REQUIRE_EQUAL(q[2].size(), 1U);
        CDeterministicMNList history;
        history.AddMN(q[1][0]);
        try {
            history.AddMN(q[2][0]);
        } catch (const std::runtime_error&) {
            ++conflicting;
        }
        BOOST_CHECK(q[3].empty());
    }
    BOOST_CHECK_EQUAL(conflicting, 1U);
}

BOOST_FIXTURE_TEST_CASE(quarter_steps_candidate_ignores_previous_members_properties, QuarterStepsChainSetup)
{
    // Three masternodes for quarters of one member each: cycle 144 leaves the third unused. Before cycle 168's
    // work block, index 0's member moves to a new address and the unused one takes its former address. The
    // unused candidate is visited first at 168 and must join index 0 despite that index's previous member's
    // historical address.
    std::map<uint256, std::pair<int, CBLSSecretKey>> mns;
    std::vector<CMutableTransaction> regs;
    for (const int port : {1, 2, 3}) {
        CBLSSecretKey op;
        regs.push_back(RegisterTx(port, op));
        mns.emplace(regs.back().GetHash(), std::make_pair(port, op));
    }
    Mine(regs);

    std::map<int, CycleQuarters> built;
    BuildAndCheckCycle(*this, built, 144);
    std::set<uint256> unused;
    for (const auto& [hash, _] : mns) {
        unused.insert(hash);
    }
    for (const auto& q : built[144]) {
        BOOST_REQUIRE_EQUAL(q[3].size(), 1U);
        unused.erase(q[3][0]->proTxHash);
    }
    BOOST_REQUIRE_EQUAL(unused.size(), 1U);
    const uint256 moved{built[144][0][3][0]->proTxHash};
    const uint256 newcomer{*unused.begin()};
    Mine({UpServTx(moved, mns.at(moved).second, 10)});
    Mine({UpServTx(newcomer, mns.at(newcomer).second, mns.at(moved).first)});

    BuildAndCheckCycle(*this, built, 168);
    BOOST_REQUIRE_EQUAL(built[168][0][3].size(), 1U);
    BOOST_CHECK(built[168][0][3][0]->proTxHash == newcomer);
    BuildAndCheckCycle(*this, built, 192);
}

BOOST_FIXTURE_TEST_CASE(quarter_steps_zero_candidates_store_snapshot, QuarterStepsChainSetup)
{
    CBLSSecretKey op;
    const auto reg = RegisterTx(1, op);
    Mine({reg});
    Mine({RevokeTx(reg.GetHash(), op)});
    const int cycle{120};
    MineTo(cycle);

    const CBlockIndex* base = Tip()->GetAncestor(cycle);
    const auto work_list = dmnman.GetListForBlock(base->GetAncestor(cycle - llmq::WORK_DIFF_DEPTH));
    BOOST_REQUIRE_EQUAL(work_list.GetCounts().total(), 1U);
    BOOST_REQUIRE_EQUAL(work_list.GetCounts().enabled(), 0U);

    const auto quarters = Quarters(cycle);
    BOOST_CHECK(qsnapman.GetSnapshotForBlock(llmq_params.type, base).has_value());
    BOOST_REQUIRE_EQUAL(quarters.size(), static_cast<size_t>(llmq_params.signingActiveQuorumCount));
    for (const auto& q : quarters) {
        for (const auto& quarter : q) {
            BOOST_CHECK(quarter.empty());
        }
    }
}

BOOST_FIXTURE_TEST_CASE(quarter_steps_prediction_writes_no_snapshot, QuarterStepsChainSetup)
{
    CBLSSecretKey op1, op2;
    Mine({RegisterTx(1, op1), RegisterTx(2, op2)});
    const int cycle{192};
    MineTo(cycle - llmq::WORK_DIFF_DEPTH);

    const size_t before{CountStoredSnapshots()};
    BOOST_REQUIRE_GT(before, 0U);
    const CBlockIndex* work = Tip();
    const auto members = llmq::utils::ComputeQuorumMembersFromWorkBlock(llmq_params.type,
                                                                        {dmnman, qsnapman, chainman, work}, work, cycle);
    BOOST_REQUIRE(members.has_value());
    BOOST_CHECK_EQUAL(CountStoredSnapshots(), before);
}

namespace {
// quarter_steps signalled by miners in 20-block windows, locked in at 40 and held until height 80
struct QuarterStepsSignalSetup : public TestChainSetup {
    QuarterStepsSignalSetup() :
        TestChainSetup(18, CBaseChainParams::REGTEST, {"-vbparams=quarter_steps:0:9223372036854775807:80:20:16:16:5:0"})
    {
    }
};
} // namespace

BOOST_FIXTURE_TEST_CASE(quarter_steps_activation_boundary, QuarterStepsSignalSetup)
{
    const int activation_height{80};
    mineBlocks(activation_height + 1 - 18);
    auto& chainman = *Assert(m_node.chainman);
    const CBlockIndex* tip = WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip());
    BOOST_REQUIRE_EQUAL(tip->nHeight, activation_height + 1);
    const CBlockIndex* before = Assert(tip->GetAncestor(activation_height - 1));
    const CBlockIndex* at = Assert(tip->GetAncestor(activation_height));

    BOOST_CHECK(!llmq::utils::QuarterStepsActive(*before, chainman));
    BOOST_CHECK(llmq::utils::QuarterStepsActive(*at, chainman));
    BOOST_CHECK(DeploymentActiveAfter(before, chainman, Consensus::DEPLOYMENT_QUARTER_STEPS));
}

BOOST_AUTO_TEST_SUITE_END()
