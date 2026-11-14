// Copyright (c) 2019-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls/bls.h>
#include <bls/bls_batchverifier.h>
#include <bls/bls_worker.h>
#include <chainparams.h>
#include <clientversion.h>
#include <consensus/validation.h>
#include <crypto/common.h>
#include <deploymentstatus.h>
#include <random.h>
#include <script/standard.h>
#include <streams.h>
#include <test/util/bls.h>
#include <test/util/setup_common.h>
#include <timedata.h>
#include <util/helpers.h>
#include <util/strencodings.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <type_traits>

BOOST_AUTO_TEST_SUITE(bls_tests)

void FuncSign(const BLSScheme scheme)
{
    CBLSSecretKey sk1, sk2;
    sk1.MakeNewKey();
    sk2.MakeNewKey();

    uint256 msgHash1 = uint256::ONE;
    uint256 msgHash2 = uint256::TWO;

    auto sig1 = SignBLS(sk1, msgHash1, scheme);
    auto sig2 = SignBLS(sk2, msgHash1, scheme);
    BOOST_CHECK(sig1.VerifyInsecure(sk1.GetPublicKey(), msgHash1, scheme));
    BOOST_CHECK(!sig1.VerifyInsecure(sk1.GetPublicKey(), msgHash2, scheme));
    BOOST_CHECK(!sig2.VerifyInsecure(sk1.GetPublicKey(), msgHash1, scheme));
    BOOST_CHECK(!sig2.VerifyInsecure(sk2.GetPublicKey(), msgHash2, scheme));
    BOOST_CHECK(sig2.VerifyInsecure(sk2.GetPublicKey(), msgHash1, scheme));

    return;
}

void FuncSerialize(const BLSScheme scheme)
{
    CBLSSecretKey sk;
    CDataStream ds2(SER_DISK, CLIENT_VERSION), ds3(SER_DISK, CLIENT_VERSION);
    uint256 msgHash = uint256::ONE;

    sk.MakeNewKey();
    CBLSSignature sig1 = SignBLS(sk, msgHash, scheme);
    const BLSScheme opposite{scheme == BLSScheme::Legacy ? BLSScheme::Basic : BLSScheme::Legacy};
    ds2 << BLSEncoded(sig1, scheme);
    ds3 << BLSEncoded(sig1, opposite);

    CBLSSignature sig2;
    ds2 >> BLSEncoded(sig2, scheme);
    BOOST_CHECK(sig1 == sig2);

    CBLSSignature sig3;
    ds3 >> BLSEncoded(sig3, opposite);
    BOOST_CHECK(sig1 == sig3);
}

void FuncSetHexStr()
{
    // Secret keys are not scheme-dependent. SetHexStr rejects a scalar that does not decode.
    CBLSSecretKey sk;
    std::string strValidSecret = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    // Note: invalid string passed to SetHexStr() should cause it to fail and reset key internal data
    BOOST_CHECK(sk.SetHexStr(strValidSecret));
    BOOST_CHECK(!sk.SetHexStr("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1g")); // non-hex
    BOOST_CHECK(!sk.IsValid());
    BOOST_CHECK(sk == CBLSSecretKey());
    // Try few more invalid strings
    BOOST_CHECK(sk.SetHexStr(strValidSecret));
    BOOST_CHECK(!sk.SetHexStr("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e")); // hex but too short
    BOOST_CHECK(!sk.IsValid());
    BOOST_CHECK(sk.SetHexStr(strValidSecret));
    BOOST_CHECK(!sk.SetHexStr("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20")); // hex but too long
    BOOST_CHECK(!sk.IsValid());
}

void FuncKeyAgg(const BLSScheme scheme)
{
    CBLSSecretKey sk1, sk2;
    sk1.MakeNewKey();
    sk2.MakeNewKey();

    CBLSPublicKey ag_pk = sk1.GetPublicKey();
    ag_pk.AggregateInsecure(sk2.GetPublicKey());

    CBLSSecretKey ag_sk = sk1;
    ag_sk.AggregateInsecure(sk2);

    BOOST_CHECK(ag_pk == ag_sk.GetPublicKey());

    uint256 msgHash1 = uint256::ONE;
    uint256 msgHash2 = uint256::TWO;

    auto sig = SignBLS(ag_sk, msgHash1, scheme);
    BOOST_CHECK(sig.VerifyInsecure(ag_pk, msgHash1, scheme));
    BOOST_CHECK(!sig.VerifyInsecure(ag_pk, msgHash2, scheme));
}

void FuncKeyAggVec(const BLSScheme scheme)
{

    std::vector<CBLSSecretKey> vec_sk;
    std::vector<CBLSPublicKey> vec_pk;

    {
        auto ret = CBLSSecretKey::AggregateInsecure(vec_sk);
        BOOST_CHECK(ret == CBLSSecretKey());
    }
    {
        auto ret = CBLSPublicKey::AggregateInsecure(vec_pk);
        BOOST_CHECK(ret == CBLSPublicKey());
    }

    // In practice, we only aggregate 400 key shares at any given time, something substantially larger than that should
    // be good. Plus this is very very fast, so who cares!
    int key_count = 10000;
    vec_sk.reserve(key_count);
    vec_pk.reserve(key_count);
    CBLSSecretKey sk;
    for (int i = 0; i < key_count; i++) {
        sk.MakeNewKey();
        vec_sk.push_back(sk);
        vec_pk.push_back(sk.GetPublicKey());
    }

    CBLSSecretKey ag_sk = CBLSSecretKey::AggregateInsecure(vec_sk);
    CBLSPublicKey ag_pk = CBLSPublicKey::AggregateInsecure(vec_pk);

    BOOST_CHECK(ag_sk.IsValid());
    BOOST_CHECK(ag_pk.IsValid());

    uint256 msgHash1 = uint256::ONE;
    uint256 msgHash2 = uint256::TWO;

    auto sig = SignBLS(ag_sk, msgHash1, scheme);
    BOOST_CHECK(sig.VerifyInsecure(ag_pk, msgHash1, scheme));
    BOOST_CHECK(!sig.VerifyInsecure(ag_pk, msgHash2, scheme));
}


void FuncSigAggSub()
{

    int count = 20;
    std::vector<CBLSPublicKey> vec_pks;
    std::vector<uint256> vec_hashes;
    std::vector<CBLSSignature> vec_sigs;
    vec_pks.reserve(count);
    vec_hashes.reserve(count);
    vec_sigs.reserve(count);

    CBLSSignature sig;
    auto sk = CBLSSecretKey();
    uint256 hash;
    for (int i = 0; i < count; i++) {
        sk.MakeNewKey();
        vec_pks.push_back(sk.GetPublicKey());
        hash = GetRandHash();
        vec_hashes.push_back(hash);
        CBLSSignature sig_i = sk.Sign(hash);
        vec_sigs.push_back(sig_i);
        if (i == 0) {
            // first sig is assigned directly
            sig = sig_i;
        } else {
            // all other sigs are aggregated into the previously computed/stored sig
            sig.AggregateInsecure(sig_i);
        }
        BOOST_CHECK(sig.VerifyInsecureAggregated(vec_pks, vec_hashes));
    }
    // Create an aggregated signature from the vector of individual signatures
    auto vecSig = CBLSSignature::AggregateInsecure(vec_sigs);
    BOOST_CHECK(vecSig.VerifyInsecureAggregated(vec_pks, vec_hashes));
    // Check that these two signatures are equal
    BOOST_CHECK(sig == vecSig);

    // Test that the sig continues to be valid when subtracting sigs via `SubInsecure`

    for (int i = 0; i < count - 1; i++) {
        auto top_sig = vec_sigs.back();
        vec_pks.pop_back();
        vec_hashes.pop_back();
        BOOST_CHECK(!sig.VerifyInsecureAggregated(vec_pks, vec_hashes));
        sig.SubInsecure(top_sig);
        BOOST_CHECK(sig.VerifyInsecureAggregated(vec_pks, vec_hashes));
        vec_sigs.pop_back();
    }
    // Check that the final left-over sig validates
    BOOST_CHECK_EQUAL(vec_sigs.size(), 1);
    BOOST_CHECK_EQUAL(vec_pks.size(), 1);
    BOOST_CHECK_EQUAL(vec_hashes.size(), 1);
    BOOST_CHECK(vec_sigs[0].VerifyInsecure(vec_pks[0], vec_hashes[0]));
}


void FuncSigAggSecure(const BLSScheme scheme)
{
    int count = 10;

    uint256 hash = GetRandHash();

    std::vector<CBLSSignature> vec_sigs;
    std::vector<CBLSPublicKey> vec_pks;

    CBLSSecretKey sk;
    for (int i = 0; i < count; i++) {
        sk.MakeNewKey();
        vec_pks.push_back(sk.GetPublicKey());
        vec_sigs.push_back(SignBLS(sk, hash, scheme));
    }

    auto sec_agg_sig = AggregateSecureBLS(vec_sigs, vec_pks, hash, scheme);
    BOOST_CHECK(sec_agg_sig.IsValid());
    BOOST_CHECK(sec_agg_sig.VerifySecureAggregated(vec_pks, hash, scheme));
    const BLSScheme other{scheme == BLSScheme::Legacy ? BLSScheme::Basic : BLSScheme::Legacy};
    BOOST_CHECK(!sec_agg_sig.VerifySecureAggregated(vec_pks, hash, other));
    if (scheme == BLSScheme::Basic) {
        BOOST_CHECK(sec_agg_sig == CBLSSignature::AggregateSecure(vec_sigs, vec_pks, hash));
    }
}

void FuncDHExchange()
{

    CBLSSecretKey sk1, sk2;
    sk1.MakeNewKey();
    sk2.MakeNewKey();

    CBLSPublicKey pk1, pk2;
    pk1 = sk1.GetPublicKey();
    pk2 = sk2.GetPublicKey();

    // Perform diffie-helman exchange
    CBLSPublicKey pke1, pke2;
    pke1.DHKeyExchange(sk1, pk2);
    pke2.DHKeyExchange(sk2, pk1);

    BOOST_CHECK(pke1.IsValid());
    BOOST_CHECK(pke2.IsValid());
    BOOST_CHECK(pke1 == pke2);
}

struct Message
{
    uint32_t sourceId;
    uint32_t msgId;
    uint256 msgHash;
    CBLSSecretKey sk;
    CBLSPublicKey pk;
    CBLSSignature sig;
    bool valid;
};

static void AddMessage(std::vector<Message>& vec, uint32_t sourceId, uint32_t msgId, uint8_t msgHash, bool valid)
{
    Message m;
    m.sourceId = sourceId;
    m.msgId = msgId;
    m.msgHash = uint256(msgHash);
    m.sk.MakeNewKey();
    m.pk = m.sk.GetPublicKey();
    m.sig = m.sk.Sign(m.msgHash);
    m.valid = valid;

    if (!valid) {
        CBLSSecretKey tmp;
        tmp.MakeNewKey();
        m.sig = tmp.Sign(m.msgHash);
    }

    vec.emplace_back(m);
}

// Adds two wrong signatures whose errors cancel out when both are aggregated unweighted
static void AddCancellingMessages(std::vector<Message>& vec, uint32_t sourceId1, uint32_t msgId1, uint8_t msgHash1,
                                  uint32_t sourceId2, uint32_t msgId2, uint8_t msgHash2, bool sameKey)
{
    AddMessage(vec, sourceId1, msgId1, msgHash1, true);
    AddMessage(vec, sourceId2, msgId2, msgHash2, true);
    auto& m1 = vec[vec.size() - 2];
    auto& m2 = vec[vec.size() - 1];
    if (sameKey) {
        m2.sk = m1.sk;
        m2.pk = m1.pk;
        m2.sig = m2.sk.Sign(m2.msgHash);
    }

    CBLSSecretKey sk_error;
    sk_error.MakeNewKey();
    const CBLSSignature error{sk_error.Sign(uint256::ONE)};
    m1.sig.AggregateInsecure(error);
    m2.sig.SubInsecure(error);
    m1.valid = false;
    m2.valid = false;
}

static void Verify(std::vector<Message>& vec, bool secureVerification, bool perMessageFallback)
{
    CBLSBatchVerifier<uint32_t, uint32_t> batchVerifier(secureVerification, perMessageFallback);

    std::set<uint32_t> expectedBadMessages;
    std::set<uint32_t> expectedBadSources;
    for (auto& m : vec) {
        if (!m.valid) {
            expectedBadMessages.emplace(m.msgId);
            expectedBadSources.emplace(m.sourceId);
        }

        batchVerifier.PushMessage(m.sourceId, m.msgId, m.msgHash, m.sig, m.pk);
    }

    batchVerifier.Verify();

    BOOST_CHECK(batchVerifier.badSources == expectedBadSources);

    if (perMessageFallback) {
        BOOST_CHECK(batchVerifier.badMessages == expectedBadMessages);
    } else {
        BOOST_CHECK(batchVerifier.badMessages.empty());
    }
}

static void Verify(std::vector<Message>& vec)
{
    Verify(vec, false, false);
    Verify(vec, true, false);
    Verify(vec, false, true);
    Verify(vec, true, true);
}

void FuncBatchVerifier()
{
    std::vector<Message> msgs;

    // distinct messages from distinct sources
    AddMessage(msgs, 1, 1, 1, true);
    AddMessage(msgs, 2, 2, 2, true);
    AddMessage(msgs, 3, 3, 3, true);
    Verify(msgs);

    // distinct messages from same source
    AddMessage(msgs, 4, 4, 4, true);
    AddMessage(msgs, 4, 5, 5, true);
    AddMessage(msgs, 4, 6, 6, true);
    Verify(msgs);

    // invalid sig
    AddMessage(msgs, 7, 7, 7, false);
    Verify(msgs);

    // same message as before, but from another source and with valid sig
    AddMessage(msgs, 8, 8, 7, true);
    Verify(msgs);

    // same message as before, but from another source and signed with another key
    AddMessage(msgs, 9, 9, 7, true);
    Verify(msgs);

    msgs.clear();
    // same message, signed by multiple keys
    AddMessage(msgs, 1, 1, 1, true);
    AddMessage(msgs, 1, 2, 1, true);
    AddMessage(msgs, 1, 3, 1, true);
    AddMessage(msgs, 2, 4, 1, true);
    AddMessage(msgs, 2, 5, 1, true);
    AddMessage(msgs, 2, 6, 1, true);
    Verify(msgs);

    // last message invalid from one source
    AddMessage(msgs, 1, 7, 1, false);
    Verify(msgs);

    msgs.clear();
    // errors cancelling out between distinct messages from one source
    AddMessage(msgs, 1, 1, 1, true);
    AddCancellingMessages(msgs, 2, 2, 2, 2, 3, 3, /*sameKey=*/false);
    Verify(msgs);

    // errors cancelling out between distinct sources signing the same message with the same key
    msgs.clear();
    AddMessage(msgs, 1, 1, 1, true);
    AddCancellingMessages(msgs, 2, 2, 2, 3, 3, 2, /*sameKey=*/true);
    Verify(msgs);
}

void FuncThresholdSignature(const BLSScheme scheme)
{

    [[maybe_unused]] const uint256 hash = GetRandHash();

    constexpr size_t m_size = 20;
    constexpr size_t m_threshold = 15;

    std::vector<CBLSSecretKey> v_threshold_sks;
    std::vector<CBLSPublicKey> v_threshold_pks;
    for ([[maybe_unused]] const auto i : util::irange(m_threshold)) {
        CBLSSecretKey sk;
        sk.MakeNewKey();
        v_threshold_sks.push_back(sk);
        v_threshold_pks.emplace_back(sk.GetPublicKey());
    }

    CBLSSecretKey thr_sk = v_threshold_sks[0];
    CBLSPublicKey thr_pk = v_threshold_pks[0];
    CBLSSignature thr_sig = SignBLS(thr_sk, hash, scheme);

    std::vector<CBLSId> v_size_ids;
    std::vector<CBLSSecretKey> v_size_sk_shares;
    std::vector<CBLSPublicKey> v_size_pk_shares;
    for ([[maybe_unused]] const auto m_shares : util::irange(m_size)) {
        v_size_ids.emplace_back(GetRandHash());
        CBLSSecretKey sk;
        BOOST_CHECK(sk.SecretKeyShare(v_threshold_sks, v_size_ids[m_shares]));
        v_size_sk_shares.push_back(sk);
        CBLSPublicKey pk;
        BOOST_CHECK(pk.PublicKeyShare(v_threshold_pks, v_size_ids[m_shares]));
        v_size_pk_shares.push_back(pk);

        std::vector<CBLSSignature> v_share_sigs;
        std::vector<CBLSId> v_share_ids;
        for ([[maybe_unused]] const auto j : util::irange(m_shares)) {
            v_share_sigs.emplace_back(SignBLS(v_size_sk_shares[j], hash, scheme));
            BOOST_CHECK(v_share_sigs.back().VerifyInsecure(v_size_pk_shares[j], hash, scheme));
            v_share_ids.push_back(v_size_ids[j]);
        }

        CBLSSignature rec_share_sig;
        BOOST_CHECK_EQUAL(rec_share_sig.Recover(v_share_sigs, v_share_ids), m_shares >= 2);
        BOOST_CHECK_EQUAL(rec_share_sig.IsValid(), m_shares >= 2);
        BOOST_CHECK_EQUAL(rec_share_sig == thr_sig, m_shares >= m_threshold);
        BOOST_CHECK_EQUAL(rec_share_sig.VerifyInsecure(thr_pk, hash, scheme), m_shares >= m_threshold);
    }
}

BOOST_AUTO_TEST_CASE(bls_sethexstr_tests) { FuncSetHexStr(); }

BOOST_AUTO_TEST_CASE(bls_serialize_tests)
{
    FuncSerialize(BLSScheme::Legacy);
    FuncSerialize(BLSScheme::Basic);
}

BOOST_AUTO_TEST_CASE(bls_sig_tests)
{
    FuncSign(BLSScheme::Legacy);
    FuncSign(BLSScheme::Basic);
}

BOOST_AUTO_TEST_CASE(bls_key_agg_tests)
{
    FuncKeyAgg(BLSScheme::Legacy);
    FuncKeyAgg(BLSScheme::Basic);
}

BOOST_AUTO_TEST_CASE(bls_key_agg_vec_tests)
{
    FuncKeyAggVec(BLSScheme::Legacy);
    FuncKeyAggVec(BLSScheme::Basic);
}

BOOST_AUTO_TEST_CASE(bls_sig_agg_sub_tests) { FuncSigAggSub(); }

BOOST_AUTO_TEST_CASE(bls_sig_agg_secure_tests)
{
    FuncSigAggSecure(BLSScheme::Legacy);
    FuncSigAggSecure(BLSScheme::Basic);
}

BOOST_AUTO_TEST_CASE(bls_dh_exchange_tests) { FuncDHExchange(); }

BOOST_AUTO_TEST_CASE(batch_verifier_tests) { FuncBatchVerifier(); }

BOOST_AUTO_TEST_CASE(bls_threshold_signature_tests)
{
    FuncThresholdSignature(BLSScheme::Legacy);
    FuncThresholdSignature(BLSScheme::Basic);
}

// Regression test: a DKG justification path can reach
// CBLSWorker::AsyncVerifyContributionShare() with a null verification vector when
// the corresponding member's contribution was never received (e.g. on a non-member
// observer). The singular overload dereferenced *vvec with no null check and aborted
// the process; it must instead treat a missing vvec as a failed verification, like
// the plural VerifyVerificationVectors() path. A *valid* id is required so the guard
// does not short-circuit before reaching the (previously) unguarded dereference.
void FuncVerifyContributionShareNullVvec()
{

    CBLSWorker worker;
    worker.Start(1);

    const CBLSId id{uint256::ONE};
    BOOST_REQUIRE(id.IsValid());

    CBLSSecretKey sk;
    sk.MakeNewKey();

    const BLSVerificationVectorPtr null_vvec; // default-constructed null shared_ptr
    BOOST_REQUIRE(null_vvec == nullptr);

    // Must return false (verification failed) instead of dereferencing the null vvec.
    BOOST_CHECK(worker.AsyncVerifyContributionShare(id, null_vvec, sk).get() == false);

    worker.Stop();
}

BOOST_AUTO_TEST_CASE(bls_verify_contribution_share_null_vvec_tests) { FuncVerifyContributionShareNullVvec(); }

// Regression: an empty verification-vector set schedules no work at all, so
// before the guard nothing ever invoked doneCallback and .get() threw
// std::future_error(broken_promise). In production that throw escapes
// VerifyPendingContributions on the DKG phase-handler thread, which only catches
// AbortPhaseException, and std::terminate aborts dashd.
// Contract: return an empty result vector, on both the aggregated and the
// one-by-one path.
void FuncVerifyContributionSharesEmptyVvecs()
{

    CBLSWorker worker;
    worker.Start(1);

    const CBLSId id{uint256::ONE};
    BOOST_REQUIRE(id.IsValid());

    std::vector<BLSVerificationVectorPtr> vvecs; // empty
    std::vector<CBLSSecretKey> skShares;         // empty

    // Must return gracefully (empty result) instead of destroying the promise.
    const auto result = worker.VerifyContributionShares(id, vvecs, skShares);
    BOOST_CHECK(result.empty());

    const auto result_no_agg = worker.VerifyContributionShares(id, vvecs, skShares,
                                                               /*parallel=*/true,
                                                               /*aggregated=*/false);
    BOOST_CHECK(result_no_agg.empty());

    worker.Stop();
}

BOOST_AUTO_TEST_CASE(bls_verify_contribution_shares_empty_vvecs_tests) { FuncVerifyContributionSharesEmptyVvecs(); }

// Positive control for the empty-input guard: it must not change which
// contributions are accepted. Accepting an invalid share would corrupt the
// recovered quorum key; rejecting an honest one would stall DKG. This property
// was previously only covered by src/bench/bls_dkg.cpp, which does not run under
// `make check`.
//
// QUORUM_SIZE exceeds CONTRIBUTION_VERIFY_BATCH_SIZE so the aggregated path
// builds more than one batch. Corrupting a share in the first batch must then
// fail that batch, fall back to per-contribution verification, blame exactly the
// corrupted index, and leave the second batch accepted wholesale - an invalid
// share must never be masked by the valid shares it is aggregated with.
void FuncVerifyContributionSharesHonest()
{

    constexpr size_t QUORUM_SIZE{CBLSWorker::CONTRIBUTION_VERIFY_BATCH_SIZE + 2};
    constexpr int THRESHOLD{6};

    CBLSWorker worker;
    worker.Start(4);

    std::vector<CBLSId> ids;
    ids.reserve(QUORUM_SIZE);
    for (const size_t i : util::irange(QUORUM_SIZE)) {
        uint256 id;
        WriteLE64(id.begin(), i + 1);
        ids.emplace_back(id);
    }

    // Every member runs GenerateContributions and broadcasts its vvec; member i's
    // share for us (member 0) is skShares[i][0].
    std::vector<BLSVerificationVectorPtr> vvecs;
    std::vector<CBLSSecretKey> skContributions;
    vvecs.reserve(QUORUM_SIZE);
    skContributions.reserve(QUORUM_SIZE);
    for ([[maybe_unused]] const size_t i : util::irange(QUORUM_SIZE)) {
        BLSVerificationVectorPtr vvec;
        std::vector<CBLSSecretKey> skShares;
        BOOST_REQUIRE(worker.GenerateContributions(THRESHOLD, ids, vvec, skShares));
        BOOST_REQUIRE(vvec != nullptr);
        BOOST_REQUIRE_EQUAL(vvec->size(), size_t(THRESHOLD));
        BOOST_REQUIRE_EQUAL(skShares.size(), QUORUM_SIZE);
        vvecs.emplace_back(std::move(vvec));
        skContributions.emplace_back(skShares[0]); // the share addressed to ids[0]
    }

    const CBLSId& myId = ids[0];

    // 1. All-honest input must be accepted in full, on both code paths.
    for (const bool aggregated : {true, false}) {
        const auto result = worker.VerifyContributionShares(myId, vvecs, skContributions,
                                                            /*parallel=*/true, aggregated);
        BOOST_REQUIRE_EQUAL(result.size(), QUORUM_SIZE);
        for (const size_t i : util::irange(QUORUM_SIZE)) {
            BOOST_CHECK_MESSAGE(result[i], "honest contribution " << i << " rejected (aggregated="
                                                                  << aggregated << ")");
        }
    }

    // 2. Corrupt one share and expect exactly that index to be blamed, nobody else.
    //    With aggregated=true the corrupted index sits in the first batch, so that
    //    batch must fail and fall back to per-contribution verification while the
    //    second batch stays accepted wholesale. With aggregated=false there is a
    //    single batch verified one-by-one, so only the blame-exactness holds.
    constexpr size_t BAD_IDX{3};
    static_assert(BAD_IDX < CBLSWorker::CONTRIBUTION_VERIFY_BATCH_SIZE,
                  "corrupted index must land in the first batch");
    auto corrupted = skContributions;
    corrupted[BAD_IDX].MakeNewKey(); // valid key, wrong value -> pk mismatch

    for (const bool aggregated : {true, false}) {
        const auto result = worker.VerifyContributionShares(myId, vvecs, corrupted,
                                                            /*parallel=*/true, aggregated);
        BOOST_REQUIRE_EQUAL(result.size(), QUORUM_SIZE);
        for (const size_t i : util::irange(QUORUM_SIZE)) {
            const bool expected{i != BAD_IDX};
            BOOST_CHECK_MESSAGE(result[i] == expected,
                                "index " << i << " expected " << expected << " got " << result[i]
                                         << " (aggregated=" << aggregated << ")");
        }
    }

    // 3. With the defaults the DKG uses, only the dealers of wrong shares are blamed:
    //    - two colluding dealers in the same batch swap their shares: each one is wrong, but their sum is not, so the
    //      errors cancel in an unweighted aggregate;
    //    - a missing (invalid) share fails its batch and is blamed alone.
    auto swapped = skContributions;
    std::swap(swapped[1], swapped[2]);
    auto missing = skContributions;
    missing[5] = CBLSSecretKey{};
    std::vector<std::pair<std::vector<CBLSSecretKey>, std::set<size_t>>> cases{
        {swapped, {1, 2}},
        {missing, {5}},
    };
    for (auto& [shares, bad] : cases) {
        for (const bool parallel : {true, false}) {
            const auto result = worker.VerifyContributionShares(myId, vvecs, shares, parallel);
            BOOST_REQUIRE_EQUAL(result.size(), QUORUM_SIZE);
            for (const size_t i : util::irange(QUORUM_SIZE)) {
                const bool expected{bad.count(i) == 0};
                BOOST_CHECK_MESSAGE(result[i] == expected, "index " << i << " expected " << expected << " got "
                                                                    << result[i] << " (parallel=" << parallel << ")");
            }
        }
    }

    worker.Stop();
}

BOOST_AUTO_TEST_CASE(bls_verify_contribution_shares_honest_tests) { FuncVerifyContributionSharesHonest(); }

// A duplicate coefficient point between two members' verification vectors must not fail the
// other members' contributions. CBLSWorker::VerifyContributionShares() batches the members'
// vvecs, and the batched VerifyVerificationVectors() check rejects a coefficient shared
// between two vvecs. Failing the whole batch on that let one member, by copying another
// member's coefficient into its own vvec, mark every honest member in the batch invalid.
// The fix verifies each contribution against its own vvec, so only the member whose share no
// longer matches its own vvec fails. Regression: on the old all-or-nothing code every entry
// came back false, so the honest members verifying true is what distinguishes fixed from broken.
void FuncVerifyContributionSharesDuplicateCoefficient()
{

    CBLSWorker worker;
    worker.Start(4);

    const int threshold = 2;
    std::vector<CBLSId> ids;
    for (int i = 0; i < 4; i++) {
        uint256 id;
        WriteLE64(id.begin(), i + 1);
        ids.emplace_back(id);
    }

    // Three dealers each generate a verification vector and their secret-key shares to each id.
    std::vector<BLSVerificationVectorPtr> vvecs(3);
    std::vector<std::vector<CBLSSecretKey>> skShares(3);
    for (int d = 0; d < 3; d++) {
        BOOST_REQUIRE(worker.GenerateContributions(threshold, ids, vvecs[d], skShares[d]));
    }

    // Receiver R collects each dealer's share to it.
    const size_t R = 0;
    const CBLSId& recvId = ids[R];
    std::vector<CBLSSecretKey> recvShares{skShares[0][R], skShares[1][R], skShares[2][R]};

    // All honest: every contribution verifies.
    std::vector<bool> ok = worker.VerifyContributionShares(recvId, vvecs, recvShares);
    BOOST_REQUIRE_EQUAL(ok.size(), 3U);
    BOOST_CHECK(ok[0] && ok[1] && ok[2]);

    // Dealer 0 copies dealer 1's leading coefficient into its own vvec (a cross-member
    // duplicate) without touching its share, making the batch vvec check fail.
    vvecs[0]->at(0) = vvecs[1]->at(0);
    std::vector<bool> res = worker.VerifyContributionShares(recvId, vvecs, recvShares);
    BOOST_REQUIRE_EQUAL(res.size(), 3U);
    BOOST_CHECK(!res[0]);  // the member that planted the duplicate is caught alone
    BOOST_CHECK(res[1]);   // the honest member whose coefficient was copied is unaffected
    BOOST_CHECK(res[2]);   // the other honest member is unaffected
    BOOST_CHECK(CBLSWorker::FindVerificationVectorsWithSharedCoefficients(vvecs) == (std::set<size_t>{0, 1}));

    // Dealer 0 copies dealer 1's whole contribution, vvec and share alike: every share verifies,
    // but the quorum verification vector can't be built until both copies are dropped.
    vvecs[0] = std::make_shared<std::vector<CBLSPublicKey>>(*vvecs[1]);
    recvShares[0] = skShares[1][R];
    res = worker.VerifyContributionShares(recvId, vvecs, recvShares);
    BOOST_REQUIRE_EQUAL(res.size(), 3U);
    BOOST_CHECK(res[0] && res[1] && res[2]);
    BOOST_CHECK(worker.BuildQuorumVerificationVector(vvecs) == nullptr);
    BOOST_CHECK(CBLSWorker::FindVerificationVectorsWithSharedCoefficients(vvecs) == (std::set<size_t>{0, 1}));
    std::vector<BLSVerificationVectorPtr> remaining{vvecs[2]};
    BOOST_CHECK(worker.BuildQuorumVerificationVector(remaining) != nullptr);

    // Distinct vvecs share nothing, and a missing vvec is skipped.
    std::vector<BLSVerificationVectorPtr> distinct{vvecs[1], nullptr, vvecs[2]};
    BOOST_CHECK(CBLSWorker::FindVerificationVectorsWithSharedCoefficients(distinct).empty());

    worker.Stop();
}

BOOST_AUTO_TEST_CASE(bls_verify_contribution_shares_duplicate_coefficient_tests)
{
    FuncVerifyContributionSharesDuplicateCoefficient();
}

void FuncAggregateCollidingContributionShares()
{

    CBLSWorker worker;
    worker.Start(4);

    // The receiver's id is -1 mod the group order, so a degree-1 polynomial b0 + b1 * x takes the value b0 - b1 there
    uint256 recvIdHash;
    const auto minusOne{ParseHex("73eda753299d7d483339d80809a1d80553bda402fffe5bfeffffffff00000000")};
    std::copy(minusOne.begin(), minusOne.end(), recvIdHash.begin());
    const CBLSId recvId{recvIdHash};

    // Dealer 0 is honest. Dealers 1 and 2 collude: their polynomials differ, but agree at the receiver's id
    const auto makeKey = [] { CBLSSecretKey sk; sk.MakeNewKey(); return sk; };
    std::vector<std::vector<CBLSSecretKey>> polys{{makeKey(), makeKey()}, {makeKey(), makeKey()}, {}};
    CBLSSecretKey share1;
    BOOST_REQUIRE(share1.SecretKeyShare(polys[1], recvId));
    const CBLSSecretKey b1{makeKey()};
    CBLSSecretKey b0{share1};
    b0.AggregateInsecure(b1);
    polys[2] = {b0, b1};

    std::vector<BLSVerificationVectorPtr> vvecs;
    std::vector<CBLSSecretKey> recvShares;
    for (auto& poly : polys) {
        auto vvec = std::make_shared<std::vector<CBLSPublicKey>>();
        for (const auto& coefficient : poly) {
            vvec->emplace_back(coefficient.GetPublicKey());
        }
        vvecs.emplace_back(vvec);
        BOOST_REQUIRE(recvShares.emplace_back().SecretKeyShare(poly, recvId));
    }
    BOOST_REQUIRE(recvShares[1] == recvShares[2]);

    // Every contribution verifies and nothing in the vvecs collides
    const std::vector<bool> ok{worker.VerifyContributionShares(recvId, vvecs, recvShares)};
    BOOST_CHECK(ok == std::vector<bool>(3, true));
    const BLSVerificationVectorPtr quorumVvec{worker.BuildQuorumVerificationVector(vvecs)};
    BOOST_REQUIRE(quorumVvec != nullptr);

    // so the receiver must still get the quorum secret key share matching the quorum vvec
    const CBLSSecretKey skShare{worker.AggregateSecretKeys(recvShares)};
    BOOST_REQUIRE(skShare.IsValid());
    BOOST_CHECK(skShare.GetPublicKey() == CBLSWorker::BuildPubKeyShare(quorumVvec, recvId));

    worker.Stop();
}

BOOST_AUTO_TEST_CASE(bls_aggregate_colliding_contribution_shares_tests) { FuncAggregateCollidingContributionShares(); }

// A dummy BLS object that satisfies the minimal interface expected by CBLSLazyWrapper.
class DummyBLS
{
public:
    // Define a fixed serialization size (for testing purposes).
    static const size_t SerSize = 4;
    std::array<uint8_t, SerSize> data{};

    DummyBLS() { data.fill(0); }

    // A dummy validity check: valid if any byte is non-zero.
    bool IsValid() const
    {
        return std::any_of(data.begin(), data.end(), [](uint8_t c) { return c != 0; });
    }

    // The dummy object has one encoding; the scheme argument is unused.
    std::array<uint8_t, SerSize> ToBytes(BLSScheme /*scheme*/) const { return data; }

    void SetBytes(const std::array<uint8_t, SerSize>& bytes, BLSScheme /*scheme*/) { data = bytes; }

    // A dummy malleability check: simply compares the stored data to the given bytes.
    bool CheckMalleable(const std::array<uint8_t, SerSize>& bytes, BLSScheme /*scheme*/) const { return data == bytes; }

    // Reset the object to an "empty" state.
    void Reset() { data.fill(0); }

    // Produce a string representation.
    std::string ToString(BLSScheme /*scheme*/) const { return HexStr(data); }

    // Equality operator.
    bool operator==(const DummyBLS& other) const { return data == other.data; }
};

// The scheme-taking operations of the lazy wrapper are protected; the production
// subclasses publish the ones they allow. This test subclass publishes all of them.
class LazyDummyBLS : public CBLSLazyWrapper<DummyBLS>
{
public:
    using CBLSLazyWrapper<DummyBLS>::SerializeAs;
    using CBLSLazyWrapper<DummyBLS>::UnserializeAs;
    using CBLSLazyWrapper<DummyBLS>::GetHashAs;
};

// Test 1: Two default (unset) wrappers should compare equal.
BOOST_AUTO_TEST_CASE(test_default_equality)
{
    LazyDummyBLS lazy1;
    LazyDummyBLS lazy2;
    // Neither instance has been set, so they represent the default/null object.
    BOOST_CHECK(lazy1 == lazy2);
}

// Test 2: A default wrapper and one initialized with a nonzero DummyBLS should compare unequal.
BOOST_AUTO_TEST_CASE(test_non_default_vs_default)
{
    LazyDummyBLS lazy_default;
    LazyDummyBLS lazy_set;
    DummyBLS obj;
    obj.data = {1, 0, 0, 0}; // nonzero data makes the object valid
    lazy_set.Set(obj);
    BOOST_CHECK(!(lazy_default == lazy_set));
    BOOST_CHECK(lazy_default != lazy_set);
}

// Test 2: A default wrapper and one initialized with a nonzero DummyBLS should compare unequal.
BOOST_AUTO_TEST_CASE(test_non_default_vs_different)
{
    LazyDummyBLS lazy_a;
    LazyDummyBLS lazy_b;
    DummyBLS obj;
    obj.data = {1, 2, 3, 4}; // nonzero data makes the object valid
    lazy_a.Set(obj);
    obj.data = {2, 2, 3, 4}; // nonzero data makes the object valid
    lazy_b.Set(obj);
    BOOST_CHECK(lazy_a != lazy_b);
}

// Test 3: Two wrappers set with the same underlying DummyBLS value compare equal.
BOOST_AUTO_TEST_CASE(test_equality_same_value)
{
    LazyDummyBLS lazy1;
    LazyDummyBLS lazy2;
    BOOST_CHECK(lazy1 == lazy2);
    DummyBLS obj;
    obj.data = {5, 6, 7, 8};
    lazy1.Set(obj);
    BOOST_CHECK(lazy1 != lazy2);
    lazy2.Set(obj);
    BOOST_CHECK(lazy1 == lazy2);
}

// Test 4: Serialization and unserialization preserve the wrapped value.
BOOST_AUTO_TEST_CASE(test_serialization_unserialization)
{
    LazyDummyBLS lazy1;
    DummyBLS obj;
    obj.data = {9, 10, 11, 12};
    lazy1.Set(obj);

    // Serialize the lazy object into a data stream.
    CDataStream ds(SER_DISK, CLIENT_VERSION);
    lazy1.SerializeAs(ds, BLSScheme::Legacy);

    // Create a new instance and unserialize the data into it.
    LazyDummyBLS lazy2;
    lazy2.UnserializeAs(ds, BLSScheme::Legacy);
    BOOST_CHECK(lazy1 == lazy2);
    BOOST_CHECK(lazy2.Get() == obj);
}

// Test 5: Two wrappers wrapping the same object should have the same hash.
BOOST_AUTO_TEST_CASE(test_get_hash_consistency)
{
    LazyDummyBLS lazy1;
    LazyDummyBLS lazy2;
    DummyBLS obj;
    obj.data = {13, 14, 15, 16};
    lazy1.Set(obj);
    lazy2.Set(obj);
    uint256 hash1 = lazy1.GetHashAs(BLSScheme::Basic);
    uint256 hash2 = lazy2.GetHashAs(BLSScheme::Basic);
    BOOST_CHECK(hash1 == hash2);
}

namespace {

enum class DecodeKind {
    Invalid,
    Throws,
    Valid
};

struct DecodeRow {
    const char* name;
    const char* hex;
    BLSScheme scheme;
    DecodeKind kind;
    // Re-encodings of the decoded point. Null unless kind == Valid.
    const char* point_legacy;
    const char* point_basic;
};

std::string RepeatByteHex(uint8_t byte, size_t size) { return HexStr(std::vector<uint8_t>(size, byte)); }

// Both schemes serialize the identity as 0xc0 followed by zeros.
std::string InfinityHex(size_t size)
{
    std::vector<uint8_t> bytes(size, 0);
    bytes[0] = 0xc0;
    return HexStr(bytes);
}

template <typename T>
void CheckDecode(const DecodeRow& row)
{
    const auto bytes = ParseHex(row.hex);
    BOOST_REQUIRE_EQUAL(bytes.size(), T::SerSize);
    T obj;
    CDataStream ds(bytes, SER_NETWORK, PROTOCOL_VERSION);
    bool threw = false;
    std::string what;
    try {
        obj.Unserialize(ds, row.scheme);
    } catch (const std::ios_base::failure& e) {
        threw = true;
        what = e.what();
    }
    if (row.kind == DecodeKind::Throws) {
        BOOST_CHECK(threw);
        BOOST_CHECK(what.find("malleable BLS object") != std::string::npos);
        return;
    }
    BOOST_CHECK(!threw);
    if (row.kind == DecodeKind::Invalid) {
        BOOST_CHECK(!obj.IsValid());
        return;
    }
    BOOST_REQUIRE(obj.IsValid());
    BOOST_CHECK_EQUAL(obj.ToString(BLSScheme::Legacy), row.point_legacy);
    BOOST_CHECK_EQUAL(obj.ToString(BLSScheme::Basic), row.point_basic);
}

// The different point PK1's basic encoding decodes to under legacy.
const char* PK1_BAS_AS_LEG_BAS = "b7f1d3a73197d7942695638c4fa9ac0fc3688c4f9774b905a14e3a3f171bac586c55e83ff97a1aeffb3af"
                                 "00adb22c6bb";
// Public key of secret scalar 2, and the different point its legacy encoding decodes to under basic.
const char* PK2_LEG = "8572cbea904d67468808c8eb50a9450c9721db309128012543902d0ac358a62ae28f75bb8f1c7c42c39a8c5529bf0f4"
                      "e";
const char* PK2_BAS = "a572cbea904d67468808c8eb50a9450c9721db309128012543902d0ac358a62ae28f75bb8f1c7c42c39a8c5529bf0f4"
                      "e";
const char* PK2_LEG_AS_BAS_LEG = "0572cbea904d67468808c8eb50a9450c9721db309128012543902d0ac358a62ae28f75bb8f1c7c42c39a8"
                                 "c5529bf0f4e";
// Legacy signature of scalar 1 over uint256::ONE. Its legacy encoding is not canonical under basic, and its basic encoding is not canonical under legacy.
const char* SIG1_LEG = "8f8cde907769bb0b753efe4dd1cfddd85d8ac1878dc724884347acea5dd5d8f92bbd42a8bd34ecc134951435231d6c1"
                       "9184450feca2ff2fb723308f7d5c64d3234e64bf508158f99144bed1ed20d34b2db63baaef121b29143102882b6d13a"
                       "97";
const char* SIG1_BAS = "b84450feca2ff2fb723308f7d5c64d3234e64bf508158f99144bed1ed20d34b2db63baaef121b29143102882b6d13a9"
                       "70f8cde907769bb0b753efe4dd1cfddd85d8ac1878dc724884347acea5dd5d8f92bbd42a8bd34ecc134951435231d6c"
                       "19";
// Legacy signature of scalar 6 over uint256::ONE. Its basic encoding is canonical under both schemes and decodes to two different points.
const char* SIG6_LEG = "029305a30c9830695358432443b30d29ac40cc60aba2df4df195c4144521613840b984e0ebd5e934d2b00d13b07e813"
                       "212d170dd5a79b4b1a30b5e345f8a1d14472d40d885c8e55c9406159f5de8909e0b85f923b6290dac61bd6cafcc7f9d"
                       "ba";
const char* SIG6_BAS = "92d170dd5a79b4b1a30b5e345f8a1d14472d40d885c8e55c9406159f5de8909e0b85f923b6290dac61bd6cafcc7f9db"
                       "a029305a30c9830695358432443b30d29ac40cc60aba2df4df195c4144521613840b984e0ebd5e934d2b00d13b07e81"
                       "32";
const char* SIG6_BAS_AS_LEG_BAS = "a29305a30c9830695358432443b30d29ac40cc60aba2df4df195c4144521613840b984e0ebd5e934d2b0"
                                  "0d13b07e813212d170dd5a79b4b1a30b5e345f8a1d14472d40d885c8e55c9406159f5de8909e0b85f923"
                                  "b6290dac61bd6cafcc7f9dba";

template <typename Lazy, typename Object>
void CheckLazy(const Object& point, const Object& other)
{
    constexpr bool is_pubkey = std::is_same_v<Lazy, CBLSLazyPublicKey>;
    const auto legacy_arr = point.ToBytes(BLSScheme::Legacy);
    const auto basic_arr = point.ToBytes(BLSScheme::Basic);
    const auto other_arr = other.ToBytes(is_pubkey ? BLSScheme::Legacy : BLSScheme::Basic);
    const std::vector<uint8_t> legacy_bytes(legacy_arr.begin(), legacy_arr.end());
    const std::vector<uint8_t> basic_bytes(basic_arr.begin(), basic_arr.end());
    const std::vector<uint8_t> other_bytes(other_arr.begin(), other_arr.end());
    const std::vector<uint8_t> garbage(Object::SerSize, 0xff);

    auto hash_of = [](const std::vector<uint8_t>& bytes) {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss.write(MakeByteSpan(bytes));
        return ss.GetHash();
    };

    if constexpr (is_pubkey) {
        for (const BLSScheme scheme : {BLSScheme::Legacy, BLSScheme::Basic}) {
            Lazy lazy;
            CDataStream in(garbage, SER_DISK, CLIENT_VERSION);
            lazy.Unserialize(in, scheme);
            std::vector<unsigned char> out;
            CVectorWriter writer(SER_DISK, CLIENT_VERSION, out, 0);
            BOOST_CHECK_NO_THROW(lazy.Serialize(writer, scheme));
            BOOST_CHECK(out == garbage);

            Lazy invalid;
            CDataStream invalid_in(garbage, SER_DISK, CLIENT_VERSION);
            invalid.Unserialize(invalid_in, scheme);
            const Object* decoded = nullptr;
            BOOST_CHECK_NO_THROW(decoded = &invalid.Get());
            BOOST_REQUIRE(decoded != nullptr);
            BOOST_CHECK(!decoded->IsValid());
        }

        Lazy legacy_wrapped;
        CDataStream legacy_in(legacy_bytes, SER_DISK, CLIENT_VERSION);
        legacy_wrapped.Unserialize(legacy_in, BLSScheme::Legacy);
        Lazy basic_wrapped;
        CDataStream basic_in(basic_bytes, SER_DISK, CLIENT_VERSION);
        basic_wrapped.Unserialize(basic_in, BLSScheme::Basic);
        BOOST_CHECK(legacy_wrapped == basic_wrapped);

        Lazy other_wrapped;
        CDataStream other_in(other_bytes, SER_DISK, CLIENT_VERSION);
        other_wrapped.Unserialize(other_in, BLSScheme::Legacy);
        BOOST_CHECK(legacy_wrapped != other_wrapped);

        Lazy unset_a;
        Lazy unset_b;
        BOOST_CHECK(unset_a == unset_b);

        BOOST_CHECK(legacy_wrapped.GetHash(BLSScheme::Legacy) == hash_of(legacy_bytes));
        BOOST_CHECK(basic_wrapped.GetHash(BLSScheme::Basic) == hash_of(basic_bytes));

        for (const BLSScheme from_scheme : {BLSScheme::Legacy, BLSScheme::Basic}) {
            Lazy raw;
            const auto& bytes = from_scheme == BLSScheme::Legacy ? legacy_bytes : basic_bytes;
            const BLSScheme other_scheme{from_scheme == BLSScheme::Legacy ? BLSScheme::Basic : BLSScheme::Legacy};
            CDataStream raw_in(bytes, SER_DISK, CLIENT_VERSION);
            raw.Unserialize(raw_in, from_scheme);
            std::vector<unsigned char> raw_out;
            CVectorWriter raw_writer(SER_DISK, CLIENT_VERSION, raw_out, 0);
            // An undecoded point serialized under the other scheme is re-encoded. Zeros are written
            // only when the cached bytes do not decode.
            BOOST_CHECK_NO_THROW(raw.Serialize(raw_writer, other_scheme));
            const auto expected{point.ToBytes(other_scheme)};
            BOOST_CHECK(raw_out == std::vector<unsigned char>(expected.begin(), expected.end()));

            // Serializing under another scheme leaves the cached bytes and the point intact.
            std::vector<unsigned char> again;
            CVectorWriter again_writer(SER_DISK, CLIENT_VERSION, again, 0);
            raw.Serialize(again_writer, from_scheme);
            BOOST_CHECK(again == bytes);
            BOOST_CHECK(raw.Get() == point);
        }

        Lazy undecodable;
        CDataStream garbage_in(garbage, SER_DISK, CLIENT_VERSION);
        undecodable.Unserialize(garbage_in, BLSScheme::Legacy);
        std::vector<unsigned char> zeros_out;
        CVectorWriter zeros_writer(SER_DISK, CLIENT_VERSION, zeros_out, 0);
        BOOST_CHECK_NO_THROW(undecodable.Serialize(zeros_writer, BLSScheme::Basic));
        const std::vector<unsigned char> zeros(Object::SerSize, 0);
        BOOST_CHECK(zeros_out == zeros);
    } else {
        Lazy lazy;
        CDataStream in(garbage, SER_DISK, CLIENT_VERSION);
        lazy.Unserialize(in);
        std::vector<unsigned char> out;
        CVectorWriter writer(SER_DISK, CLIENT_VERSION, out, 0);
        BOOST_CHECK_NO_THROW(lazy.Serialize(writer));
        BOOST_CHECK(out == garbage);

        Lazy invalid;
        CDataStream invalid_in(garbage, SER_DISK, CLIENT_VERSION);
        invalid.Unserialize(invalid_in);
        const Object* decoded = nullptr;
        BOOST_CHECK_NO_THROW(decoded = &invalid.Get());
        BOOST_REQUIRE(decoded != nullptr);
        BOOST_CHECK(!decoded->IsValid());

        Lazy basic_wrapped;
        CDataStream basic_in(basic_bytes, SER_DISK, CLIENT_VERSION);
        basic_wrapped.Unserialize(basic_in);
        Lazy other_wrapped;
        CDataStream other_in(other_bytes, SER_DISK, CLIENT_VERSION);
        other_wrapped.Unserialize(other_in);
        BOOST_CHECK(basic_wrapped != other_wrapped);
        BOOST_CHECK(basic_wrapped.Get() == point);
        BOOST_CHECK(basic_wrapped.GetHash() == hash_of(basic_bytes));

        Lazy unset_a;
        Lazy unset_b;
        BOOST_CHECK(unset_a == unset_b);
    }
}

} // namespace

BOOST_AUTO_TEST_CASE(bls_scheme_decode_matrix)
{
    const std::string pk_zero = RepeatByteHex(0x00, CBLSPublicKey::SerSize);
    const std::string pk_inf = InfinityHex(CBLSPublicKey::SerSize);
    const std::string pk_ff = RepeatByteHex(0xff, CBLSPublicKey::SerSize);
    const DecodeRow pk_rows[]{
        {"pk all-zero", pk_zero.c_str(), BLSScheme::Legacy, DecodeKind::Invalid, nullptr, nullptr},
        {"pk all-zero", pk_zero.c_str(), BLSScheme::Basic, DecodeKind::Invalid, nullptr, nullptr},
        {"pk infinity", pk_inf.c_str(), BLSScheme::Legacy, DecodeKind::Throws, nullptr, nullptr},
        {"pk infinity", pk_inf.c_str(), BLSScheme::Basic, DecodeKind::Throws, nullptr, nullptr},
        {"pk garbage", pk_ff.c_str(), BLSScheme::Legacy, DecodeKind::Throws, nullptr, nullptr},
        {"pk garbage", pk_ff.c_str(), BLSScheme::Basic, DecodeKind::Throws, nullptr, nullptr},
        {"pk1 legacy encoding", PK1_LEG, BLSScheme::Legacy, DecodeKind::Valid, PK1_LEG, PK1_BAS},
        {"pk1 legacy encoding", PK1_LEG, BLSScheme::Basic, DecodeKind::Invalid, nullptr, nullptr},
        {"pk1 basic encoding", PK1_BAS, BLSScheme::Basic, DecodeKind::Valid, PK1_LEG, PK1_BAS},
        {"pk1 basic encoding", PK1_BAS, BLSScheme::Legacy, DecodeKind::Valid, PK1_BAS, PK1_BAS_AS_LEG_BAS},
        {"pk2 legacy encoding", PK2_LEG, BLSScheme::Legacy, DecodeKind::Valid, PK2_LEG, PK2_BAS},
        {"pk2 legacy encoding", PK2_LEG, BLSScheme::Basic, DecodeKind::Valid, PK2_LEG_AS_BAS_LEG, PK2_LEG},
        {"pk2 basic encoding", PK2_BAS, BLSScheme::Basic, DecodeKind::Valid, PK2_LEG, PK2_BAS},
        {"pk2 basic encoding", PK2_BAS, BLSScheme::Legacy, DecodeKind::Invalid, nullptr, nullptr},
    };
    for (const auto& row : pk_rows) {
        BOOST_TEST_CONTEXT(row.name << " scheme=" << SchemeName(row.scheme)) { CheckDecode<CBLSPublicKey>(row); }
    }

    const std::string sig_zero = RepeatByteHex(0x00, CBLSSignature::SerSize);
    const std::string sig_inf = InfinityHex(CBLSSignature::SerSize);
    const std::string sig_ff = RepeatByteHex(0xff, CBLSSignature::SerSize);
    const DecodeRow sig_rows[]{
        {"sig all-zero", sig_zero.c_str(), BLSScheme::Legacy, DecodeKind::Invalid, nullptr, nullptr},
        {"sig all-zero", sig_zero.c_str(), BLSScheme::Basic, DecodeKind::Invalid, nullptr, nullptr},
        {"sig infinity", sig_inf.c_str(), BLSScheme::Legacy, DecodeKind::Throws, nullptr, nullptr},
        {"sig infinity", sig_inf.c_str(), BLSScheme::Basic, DecodeKind::Throws, nullptr, nullptr},
        {"sig garbage", sig_ff.c_str(), BLSScheme::Legacy, DecodeKind::Throws, nullptr, nullptr},
        {"sig garbage", sig_ff.c_str(), BLSScheme::Basic, DecodeKind::Throws, nullptr, nullptr},
        {"sig1 legacy encoding", SIG1_LEG, BLSScheme::Legacy, DecodeKind::Valid, SIG1_LEG, SIG1_BAS},
        {"sig1 legacy encoding", SIG1_LEG, BLSScheme::Basic, DecodeKind::Invalid, nullptr, nullptr},
        {"sig1 basic encoding", SIG1_BAS, BLSScheme::Basic, DecodeKind::Valid, SIG1_LEG, SIG1_BAS},
        {"sig1 basic encoding", SIG1_BAS, BLSScheme::Legacy, DecodeKind::Invalid, nullptr, nullptr},
        {"sig6 legacy encoding", SIG6_LEG, BLSScheme::Legacy, DecodeKind::Valid, SIG6_LEG, SIG6_BAS},
        {"sig6 legacy encoding", SIG6_LEG, BLSScheme::Basic, DecodeKind::Invalid, nullptr, nullptr},
        {"sig6 basic encoding", SIG6_BAS, BLSScheme::Basic, DecodeKind::Valid, SIG6_LEG, SIG6_BAS},
        {"sig6 basic encoding", SIG6_BAS, BLSScheme::Legacy, DecodeKind::Valid, SIG6_BAS, SIG6_BAS_AS_LEG_BAS},
    };
    for (const auto& row : sig_rows) {
        BOOST_TEST_CONTEXT(row.name << " scheme=" << SchemeName(row.scheme)) { CheckDecode<CBLSSignature>(row); }
    }
}

BOOST_AUTO_TEST_CASE(bls_plain_aggregation_scheme_independent)
{
    const auto sk1 = SecretFromScalar(1);
    const auto sk2 = SecretFromScalar(2);
    const auto pk1 = sk1.GetPublicKey();
    const auto pk2 = sk2.GetPublicKey();
    std::vector<CBLSSecretKey> sks{sk1, sk2};
    const CBLSSecretKey sk_agg{CBLSSecretKey::AggregateInsecure(sks)};
    const CBLSPublicKey pk_from_agg{sk_agg.GetPublicKey()};

    CBLSPublicKey pk_member = pk1;
    pk_member.AggregateInsecure(pk2);
    std::vector<CBLSPublicKey> pks{pk1, pk2};
    const CBLSPublicKey pk_span{CBLSPublicKey::AggregateInsecure(pks)};
    BOOST_CHECK(pk_member.IsValid());
    BOOST_CHECK(pk_member == pk_from_agg);
    BOOST_CHECK(pk_span == pk_from_agg);

    const uint256 hash{uint256::ONE};
    const auto sig1_basic = sk1.Sign(hash);
    const auto sig2_basic = sk2.Sign(hash);
    const auto sig_agg_basic = sk_agg.Sign(hash);
    CBLSSignature sig_member_basic{sig1_basic};
    sig_member_basic.AggregateInsecure(sig2_basic);
    std::vector<CBLSSignature> sigs_basic{sig1_basic, sig2_basic};
    BOOST_CHECK(sig_member_basic.IsValid());
    BOOST_CHECK(sig_member_basic == sig_agg_basic);
    BOOST_CHECK(CBLSSignature::AggregateInsecure(sigs_basic) == sig_agg_basic);

    const auto sig1_legacy = SignBLS(sk1, hash, BLSScheme::Legacy);
    const auto sig2_legacy = SignBLS(sk2, hash, BLSScheme::Legacy);
    const auto sig_agg_legacy = SignBLS(sk_agg, hash, BLSScheme::Legacy);
    CBLSSignature sig_member_legacy{sig1_legacy};
    sig_member_legacy.AggregateInsecure(sig2_legacy);
    std::vector<CBLSSignature> sigs_legacy{sig1_legacy, sig2_legacy};
    BOOST_CHECK(sig_member_legacy.IsValid());
    BOOST_CHECK(sig_member_legacy == sig_agg_legacy);
    BOOST_CHECK(CBLSSignature::AggregateInsecure(sigs_legacy) == sig_agg_legacy);
}

BOOST_AUTO_TEST_CASE(bls_verify_insecure_scheme_dependent)
{
    const auto sk = SecretFromScalar(1);
    const auto pk = sk.GetPublicKey();
    const uint256 hash = uint256::ONE;
    const auto legacy_sig = SignBLS(sk, hash, BLSScheme::Legacy);
    const auto basic_sig = sk.Sign(hash);
    BOOST_CHECK(basic_sig == SignBLS(sk, hash, BLSScheme::Basic));
    BOOST_CHECK(legacy_sig.VerifyInsecure(pk, hash, BLSScheme::Legacy));
    BOOST_CHECK(!legacy_sig.VerifyInsecure(pk, hash, BLSScheme::Basic));
    BOOST_CHECK(basic_sig.VerifyInsecure(pk, hash, BLSScheme::Basic));
    BOOST_CHECK(!basic_sig.VerifyInsecure(pk, hash, BLSScheme::Legacy));
}

BOOST_AUTO_TEST_CASE(bls_lazy_wrapper_scheme_bytes)
{
    const auto sk1 = SecretFromScalar(1);
    const auto sk2 = SecretFromScalar(2);
    const uint256 hash = uint256::ONE;
    CheckLazy<CBLSLazyPublicKey>(sk1.GetPublicKey(), sk2.GetPublicKey());
    CheckLazy<CBLSLazySignature>(SignBLS(sk1, hash, BLSScheme::Legacy), SignBLS(sk2, hash, BLSScheme::Legacy));
}

BOOST_AUTO_TEST_SUITE_END()
