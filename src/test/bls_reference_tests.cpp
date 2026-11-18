// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls/bls.h>
#include <bls/bls_ies.h>
#include <crypto/aes.h>
#include <streams.h>
#include <test/util/bls.h>
#include <test/data/bls_reference_vectors.txt.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <version.h>

#include <blst.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

//! Known answers recorded from the library before any of its internals were
//! replaced. Consensus depends on these bytes and on these accept/reject
//! decisions, so nothing here may change when the implementation does.

namespace {
constexpr BLSScheme LEGACY{BLSScheme::Legacy};
constexpr BLSScheme BASIC{BLSScheme::Basic};

const std::map<std::string, std::string>& Entries()
{
    static const std::map<std::string, std::string> entries = [] {
        std::map<std::string, std::string> ret;
        const auto raw{test::data::bls_reference_vectors};
        const std::string text{reinterpret_cast<const char*>(raw.data()), raw.size()};
        for (const std::string& line : SplitString(text, '\n')) {
            const size_t eq{line.find('=')};
            if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
            ret.emplace(line.substr(0, eq), line.substr(eq + 1));
        }
        return ret;
    }();
    return entries;
}

const std::string& Ref(const std::string& name)
{
    const auto it{Entries().find(name)};
    BOOST_REQUIRE_MESSAGE(it != Entries().end(), "missing reference vector " + name);
    return it->second;
}

std::string Ref(const std::string& base, int i) { return Ref(strprintf("%s.%d", base, i)); }

bool RefFlag(const std::string& name) { return Ref(name) == "1"; }
bool RefFlag(const std::string& base, int i) { return Ref(base, i) == "1"; }

uint256 RefHash(int i)
{
    uint256 h;
    for (int j = 0; j < 32; j++) h.begin()[j] = static_cast<uint8_t>(0xA5 ^ (i * 31 + j * 13));
    return h;
}

//! IETF KeyGen over the seed the reference generator used for index i.
CBLSSecretKey RefKey(int i)
{
    std::array<uint8_t, 32> seed;
    for (int j = 0; j < 32; j++) seed[j] = static_cast<uint8_t>(0x11 * (i + 1) + j * 7);
    blst_scalar scalar;
    blst_keygen_v3(&scalar, seed.data(), seed.size(), nullptr, 0);
    std::array<uint8_t, 32> bytes;
    blst_bendian_from_scalar(bytes.data(), &scalar);
    const CBLSSecretKey sk{bytes};
    BOOST_REQUIRE(sk.IsValid());
    return sk;
}

template <typename T>
T FromHex(const std::string& hex, BLSScheme scheme)
{
    T obj;
    BOOST_CHECK_MESSAGE(obj.SetHexStr(hex, scheme), hex);
    return obj;
}

CBLSSecretKey SecretFromHex(const std::string& hex)
{
    CBLSSecretKey sk;
    BOOST_CHECK_MESSAGE(sk.SetHexStr(hex), hex);
    return sk;
}

enum class Decoded {
    VALID,   //!< reads as a usable object
    INVALID, //!< reads, but as the null object
    THROWS,  //!< fails the deserialization of whatever contains it
};

std::ostream& operator<<(std::ostream& os, Decoded decoded)
{
    return os << (decoded == Decoded::VALID ? "VALID" : decoded == Decoded::INVALID ? "INVALID" : "THROWS");
}

template <typename T>
Decoded Unserialize(Span<const uint8_t> bytes, typename T::SchemeArg scheme = {})
{
    CDataStream stream{bytes, SER_NETWORK, PROTOCOL_VERSION};
    T obj;
    try {
        obj.Unserialize(stream, scheme);
    } catch (const std::ios_base::failure&) {
        return Decoded::THROWS;
    }
    return obj.IsValid() ? Decoded::VALID : Decoded::INVALID;
}

struct DecodeProbe {
    std::string name; //!< of the reference entry
    BLSScheme scheme;
    std::vector<uint8_t> bytes;
    Decoded from_stream;
};
} // namespace

BOOST_AUTO_TEST_SUITE(bls_reference_tests)

BOOST_AUTO_TEST_CASE(keys_and_signatures)
{
    for (int i = 0; i < 8; i++) {
        const CBLSSecretKey sk{RefKey(i)};
        BOOST_CHECK_EQUAL(sk.ToString(), Ref("A.sk", i));
        BOOST_CHECK(SecretFromHex(Ref("A.sk", i)) == sk);

        const CBLSPublicKey pk{sk.GetPublicKey()};
        BOOST_CHECK_EQUAL(pk.ToString(BASIC), Ref("A.pk.basic", i));
        BOOST_CHECK_EQUAL(pk.ToString(LEGACY), Ref("A.pk.legacy", i));
        BOOST_CHECK(FromHex<CBLSPublicKey>(Ref("A.pk.basic", i), BASIC) == pk);
        BOOST_CHECK(FromHex<CBLSPublicKey>(Ref("A.pk.legacy", i), LEGACY) == pk);

        const uint256 hash{RefHash(i)};
        const CBLSSignature sig{SignBLS(sk, hash, LEGACY)};
        BOOST_CHECK_EQUAL(sig.ToString(LEGACY), Ref("B.lsig.legacy", i));
        BOOST_CHECK_EQUAL(sig.ToString(BASIC), Ref("B.lsig.basic", i));
        BOOST_CHECK(FromHex<CBLSSignature>(Ref("B.lsig.legacy", i), LEGACY) == sig);
        BOOST_CHECK(FromHex<CBLSSignature>(Ref("B.lsig.basic", i), BASIC) == sig);
        BOOST_CHECK_EQUAL(sig.VerifyInsecure(pk, hash, LEGACY), RefFlag("B.lverify", i));
        BOOST_CHECK_EQUAL(sig.VerifyInsecure(pk, RefHash(i + 100), LEGACY), RefFlag("B.lverify.bad", i));

        // the two schemes hash a message to different points
        BOOST_CHECK(!sig.VerifyInsecure(pk, hash, BASIC));
        const CBLSSignature basic_sig{sk.Sign(hash)};
        BOOST_CHECK(basic_sig.VerifyInsecure(pk, hash, BASIC));
        BOOST_CHECK(!basic_sig.VerifyInsecure(pk, hash, LEGACY));
    }
}

BOOST_AUTO_TEST_CASE(aggregation)
{
    for (const int n : {1, 2, 3, 10}) {
        const uint256 hash{RefHash(n)};
        std::vector<CBLSSecretKey> sks;
        std::vector<CBLSPublicKey> pks;
        std::vector<CBLSSignature> legacy_sigs, basic_sigs;
        for (int i = 0; i < n; i++) {
            sks.push_back(RefKey(200 + n * 16 + i));
            pks.push_back(sks.back().GetPublicKey());
            legacy_sigs.push_back(SignBLS(sks.back(), hash, LEGACY));
            basic_sigs.push_back(sks.back().Sign(hash));
        }

        const auto legacy_agg{AggregateSecureBLS(legacy_sigs, pks, hash, LEGACY)};
        BOOST_CHECK_EQUAL(legacy_agg.ToString(LEGACY), Ref("C.aggsec.legacy", n));
        BOOST_CHECK_EQUAL(legacy_agg.VerifySecureAggregated(pks, hash, LEGACY), RefFlag("C.versec.legacy", n));
        BOOST_CHECK_EQUAL(legacy_agg.VerifySecureAggregated(pks, RefHash(n + 50), LEGACY), RefFlag("C.versec.legacy.bad", n));
        BOOST_CHECK(!legacy_agg.VerifySecureAggregated(pks, hash, BASIC));

        const auto basic_agg{AggregateSecureBLS(basic_sigs, pks, hash, BASIC)};
        BOOST_CHECK_EQUAL(basic_agg.ToString(BASIC), Ref("C.aggsec.basic", n));
        BOOST_CHECK_EQUAL(basic_agg.VerifySecureAggregated(pks, hash, BASIC), RefFlag("C.versec.basic", n));
        BOOST_CHECK(!basic_agg.VerifySecureAggregated(pks, RefHash(n + 50), BASIC));
        BOOST_CHECK(!basic_agg.VerifySecureAggregated(pks, hash, LEGACY));

        if (n > 1) {
            // the set of signers is part of what is verified, their order is not
            BOOST_CHECK(!basic_agg.VerifySecureAggregated(Span{pks}.first(n - 1), hash, BASIC));
            auto other_signer{pks};
            other_signer[0] = RefKey(0).GetPublicKey();
            BOOST_CHECK(!basic_agg.VerifySecureAggregated(other_signer, hash, BASIC));
            auto reversed{pks};
            std::reverse(reversed.begin(), reversed.end());
            BOOST_CHECK(basic_agg.VerifySecureAggregated(reversed, hash, BASIC));
            BOOST_CHECK(legacy_agg.VerifySecureAggregated(reversed, hash, LEGACY));
        }

        BOOST_CHECK_EQUAL(CBLSSignature::AggregateInsecure(basic_sigs).ToString(BASIC), Ref("C.agg.G2", n));
        BOOST_CHECK_EQUAL(CBLSPublicKey::AggregateInsecure(pks).ToString(BASIC), Ref("C.agg.G1", n));
        BOOST_CHECK_EQUAL(CBLSSecretKey::AggregateInsecure(sks).ToString(), Ref("C.agg.sk", n));
    }
}

BOOST_AUTO_TEST_CASE(aggregation_of_distinct_messages)
{
    std::vector<CBLSPublicKey> pks;
    std::vector<CBLSSignature> legacy_sigs, basic_sigs;
    std::vector<uint256> hashes;
    for (int i = 0; i < 4; i++) {
        const CBLSSecretKey sk{RefKey(300 + i)};
        pks.push_back(sk.GetPublicKey());
        hashes.push_back(RefHash(300 + i));
        legacy_sigs.push_back(SignBLS(sk, hashes.back(), LEGACY));
        basic_sigs.push_back(sk.Sign(hashes.back()));
    }
    const auto legacy_agg{CBLSSignature::AggregateInsecure(legacy_sigs)};
    const auto basic_agg{CBLSSignature::AggregateInsecure(basic_sigs)};
    BOOST_CHECK_EQUAL(legacy_agg.ToString(LEGACY), Ref("D.lagg"));
    BOOST_CHECK(basic_agg.VerifyInsecureAggregated(pks, hashes));
    BOOST_CHECK(!legacy_agg.VerifyInsecureAggregated(pks, hashes));

    auto wrong_hashes{hashes};
    wrong_hashes[0] = RefHash(304);
    BOOST_CHECK(!basic_agg.VerifyInsecureAggregated(pks, wrong_hashes));

    // Two signers of one message: a valid aggregate that is refused, as any with a repeated message is.
    std::vector<uint256> same_hash(2, hashes[0]);
    std::vector<CBLSPublicKey> two_pks{pks[0], pks[1]};
    std::vector<CBLSSignature> sigs{RefKey(300).Sign(hashes[0]), RefKey(301).Sign(hashes[0])};
    BOOST_CHECK(!CBLSSignature::AggregateInsecure(sigs).VerifyInsecureAggregated(two_pks, same_hash));
}

BOOST_AUTO_TEST_CASE(threshold)
{
    constexpr int threshold{3}, size{5};
    const uint256 hash{RefHash(77)};

    std::vector<CBLSSecretKey> coefficients;
    std::vector<CBLSPublicKey> vvec;
    for (int i = 0; i < threshold; i++) {
        coefficients.push_back(RefKey(400 + i));
        vvec.push_back(coefficients.back().GetPublicKey());
    }
    BOOST_CHECK_EQUAL(SignBLS(coefficients[0], hash, LEGACY).ToString(LEGACY), Ref("E.mastersig"));

    std::vector<CBLSId> ids;
    std::vector<CBLSSignature> sig_shares;
    for (int i = 0; i < size; i++) {
        ids.emplace_back(RefHash(500 + i));
        CBLSSecretKey sk_share;
        BOOST_REQUIRE(sk_share.SecretKeyShare(coefficients, ids.back()));
        BOOST_CHECK_EQUAL(sk_share.ToString(), Ref("E.skshare", i));
        CBLSPublicKey pk_share;
        BOOST_REQUIRE(pk_share.PublicKeyShare(vvec, ids.back()));
        BOOST_CHECK_EQUAL(pk_share.ToString(LEGACY), Ref("E.pkshare", i));
        BOOST_CHECK_EQUAL(sk_share.GetPublicKey() == pk_share, RefFlag("E.sharematch", i));
        sig_shares.push_back(SignBLS(sk_share, hash, LEGACY));
        BOOST_CHECK_EQUAL(sig_shares.back().ToString(LEGACY), Ref("E.sigshare.sign", i));
    }

    CBLSSignature recovered;
    BOOST_REQUIRE(recovered.Recover(Span{sig_shares}.first(threshold), Span{ids}.first(threshold)));
    BOOST_CHECK_EQUAL(recovered.ToString(LEGACY), Ref("E.sigrecover"));
    BOOST_CHECK_EQUAL(recovered.VerifyInsecure(vvec[0], hash, LEGACY), RefFlag("E.sigrecover.verify"));

    CBLSSignature other_subset;
    BOOST_REQUIRE(other_subset.Recover(Span{sig_shares}.subspan(1, threshold), Span{ids}.subspan(1, threshold)));
    BOOST_CHECK_EQUAL(other_subset == recovered, RefFlag("E.sigrecover.same"));

    // more shares than needed recover the same signature, fewer a different one
    CBLSSignature all_shares, too_few;
    BOOST_REQUIRE(all_shares.Recover(sig_shares, ids));
    BOOST_CHECK(all_shares == recovered);
    BOOST_REQUIRE(too_few.Recover(Span{sig_shares}.first(threshold - 1), Span{ids}.first(threshold - 1)));
    BOOST_CHECK(too_few != recovered);

    // shares that cannot be combined: a repeated id, a null id, mismatched or too small counts
    CBLSSignature failed;
    std::vector<CBLSId> bad_ids{ids[0], ids[1], ids[0]};
    BOOST_CHECK(!failed.Recover(Span{sig_shares}.first(threshold), bad_ids));
    bad_ids[2] = CBLSId{uint256::ZERO};
    BOOST_CHECK(!failed.Recover(Span{sig_shares}.first(threshold), bad_ids));
    BOOST_CHECK(!failed.Recover(Span{sig_shares}.first(threshold), Span{ids}.first(threshold - 1)));
    BOOST_CHECK(!failed.Recover(Span{sig_shares}.first(1), Span{ids}.first(1)));
    std::vector<CBLSSignature> bad_shares{sig_shares[0], CBLSSignature{}, sig_shares[2]};
    BOOST_CHECK(!failed.Recover(bad_shares, Span{ids}.first(threshold)));
    BOOST_CHECK(!failed.IsValid());

    CBLSSecretKey sk_share;
    CBLSPublicKey pk_share;
    BOOST_CHECK(!sk_share.SecretKeyShare(Span{coefficients}.first(1), ids[0]));
    BOOST_CHECK(!pk_share.PublicKeyShare(Span{vvec}.first(1), ids[0]));
    BOOST_CHECK(!sk_share.SecretKeyShare(coefficients, CBLSId{}));
    BOOST_CHECK(!pk_share.PublicKeyShare(vvec, CBLSId{}));
    std::vector<CBLSSecretKey> bad_coefficients{coefficients[0], CBLSSecretKey{}, coefficients[2]};
    std::vector<CBLSPublicKey> bad_vvec{vvec[0], CBLSPublicKey{}, vvec[2]};
    BOOST_CHECK(!sk_share.SecretKeyShare(bad_coefficients, ids[0]));
    BOOST_CHECK(!pk_share.PublicKeyShare(bad_vvec, ids[0]));
    BOOST_CHECK(!sk_share.IsValid() && !pk_share.IsValid());

    // An id is a number modulo the group order: the order is the id zero, and ids that differ by it are one id.
    const std::vector<uint8_t> order{ParseHex("73eda753299d7d483339d80809a1d80553bda402fffe5bfeffffffff00000001")};
    std::vector<uint8_t> five(32, 0), order_plus_five{order};
    five[31] = 5;
    order_plus_five[31] += 5;
    const CBLSId id_order{uint256{order}}, id_five{uint256{five}}, id_order_plus_five{uint256{order_plus_five}};
    CBLSSecretKey sk_five, sk_order_plus_five;
    CBLSPublicKey pk_five, pk_order_plus_five;
    BOOST_REQUIRE(sk_five.SecretKeyShare(coefficients, id_five) && pk_five.PublicKeyShare(vvec, id_five));
    BOOST_REQUIRE(sk_order_plus_five.SecretKeyShare(coefficients, id_order_plus_five));
    BOOST_REQUIRE(pk_order_plus_five.PublicKeyShare(vvec, id_order_plus_five));
    BOOST_CHECK(sk_five == sk_order_plus_five && pk_five == pk_order_plus_five);
    BOOST_CHECK(sk_five.GetPublicKey() == pk_five);
    bad_ids = {ids[0], ids[1], id_order};
    BOOST_CHECK(!failed.Recover(Span{sig_shares}.first(threshold), bad_ids));
    bad_ids = {ids[0], id_five, id_order_plus_five};
    BOOST_CHECK(!failed.Recover(Span{sig_shares}.first(threshold), bad_ids));
    // the share of the id zero is the secret itself
    BOOST_REQUIRE(sk_share.SecretKeyShare(coefficients, id_order));
    BOOST_CHECK(sk_share == coefficients[0]);

    // A result cannot be written over one of the values it is computed from.
    BOOST_CHECK(!coefficients[0].SecretKeyShare(coefficients, ids[0]));
    BOOST_CHECK(!vvec[0].PublicKeyShare(vvec, ids[0]));
    BOOST_CHECK(!sig_shares[0].Recover(Span{sig_shares}.first(threshold), Span{ids}.first(threshold)));
}

BOOST_AUTO_TEST_CASE(secret_key_range)
{
    BOOST_CHECK_EQUAL(RefKey(700).ToString(), Ref("G.inrange.mod0"));

    CBLSSecretKey sk;
    BOOST_CHECK_EQUAL(Ref("G.big.mod0"), "EXC");
    BOOST_CHECK(!sk.SetHexStr(std::string(64, 'f')));
    BOOST_CHECK(!sk.SetHexStr(std::string(64, '0')));

    // the group order itself is accepted as it is, anything above it is not
    const std::string order{"73eda753299d7d483339d80809a1d80553bda402fffe5bfeffffffff00000001"};
    BOOST_CHECK_EQUAL(SecretFromHex(order).ToString(), Ref("G.order.mod0"));
    BOOST_CHECK(!sk.SetHexStr("73eda753299d7d483339d80809a1d80553bda402fffe5bfeffffffff00000002"));
    BOOST_CHECK(!sk.SetHexStr("e7db4ea6533afa906673b0101343b00aa77b4805fffcb7fdfffffffe00000002"));
    // It is the same key as zero, and a sum that includes it is reduced.
    CBLSSecretKey sum{SecretFromHex(order)};
    sum.AggregateInsecure(SecretFromHex(order));
    BOOST_CHECK_EQUAL(sum.ToString(), std::string(64, '0'));
    std::vector<CBLSSecretKey> orders(2, SecretFromHex(order));
    BOOST_CHECK_EQUAL(CBLSSecretKey::AggregateInsecure(orders).ToString(), std::string(64, '0'));

    const std::string order_minus_one{"73eda753299d7d483339d80809a1d80553bda402fffe5bfeffffffff00000000"};
    BOOST_CHECK_EQUAL(SecretFromHex(order_minus_one).ToString(), Ref("G.orderm1.mod0"));

    const CBLSSecretKey a{RefKey(702)}, b{RefKey(703)};
    CBLSPublicKey shared_a, shared_b;
    BOOST_REQUIRE(shared_a.DHKeyExchange(a, b.GetPublicKey()));
    BOOST_REQUIRE(shared_b.DHKeyExchange(b, a.GetPublicKey()));
    BOOST_CHECK_EQUAL(shared_a.ToString(BASIC), Ref("G.dh"));
    BOOST_CHECK_EQUAL(shared_a == shared_b, RefFlag("G.dh.sym"));
}

BOOST_AUTO_TEST_CASE(decode_edges)
{
    const CBLSSecretKey sk{RefKey(800)};
    const CBLSPublicKey pk{sk.GetPublicKey()};
    const uint256 hash{RefHash(800)};
    const CBLSSignature sig{SignBLS(sk, hash, LEGACY)};

    const auto bytes = [](size_t size, uint8_t fill, std::initializer_list<std::pair<size_t, uint8_t>> set = {}) {
        std::vector<uint8_t> ret(size, fill);
        for (const auto& [pos, value] : set) ret[pos] = value;
        return ret;
    };
    const auto flipped = [](const auto& array, size_t pos, uint8_t mask, bool toggle) {
        std::vector<uint8_t> ret{array.begin(), array.end()};
        ret[pos] = toggle ? ret[pos] ^ mask : ret[pos] | mask;
        return ret;
    };
    const auto of = [](const auto& array) { return std::vector<uint8_t>{array.begin(), array.end()}; };

    const std::vector<DecodeProbe> probes{
        // canonical infinity
        {"H.g1.inf.basic", BASIC, bytes(48, 0, {{0, 0xc0}}), Decoded::THROWS},
        {"H.g1.inf.legacy", LEGACY, bytes(48, 0, {{0, 0xc0}}), Decoded::THROWS},
        {"H.g2.inf.basic", BASIC, bytes(96, 0, {{0, 0xc0}}), Decoded::THROWS},
        {"H.g2.inf.legacy", LEGACY, bytes(96, 0, {{0, 0xc0}}), Decoded::THROWS},
        // infinity flag with a stray bit
        {"H.g1.badinf.basic", BASIC, bytes(48, 0, {{0, 0xc0}, {47, 1}}), Decoded::THROWS},
        {"H.g1.badinf.legacy", LEGACY, bytes(48, 0, {{0, 0xc0}, {47, 1}}), Decoded::THROWS},
        {"H.g2.badinf.basic", BASIC, bytes(96, 0, {{0, 0xc0}, {95, 1}}), Decoded::THROWS},
        {"H.g2.badinf.legacy", LEGACY, bytes(96, 0, {{0, 0xc0}, {95, 1}}), Decoded::THROWS},
        {"H.g1.zeros.basic", BASIC, bytes(48, 0), Decoded::INVALID},
        {"H.g1.zeros.legacy", LEGACY, bytes(48, 0), Decoded::INVALID},
        {"H.g2.zeros.basic", BASIC, bytes(96, 0), Decoded::INVALID},
        {"H.g2.zeros.legacy", LEGACY, bytes(96, 0), Decoded::INVALID},
        // a coordinate not below the field modulus
        {"H.g1.ff.basic", BASIC, bytes(48, 0xff), Decoded::THROWS},
        {"H.g1.ff.legacy", LEGACY, bytes(48, 0xff), Decoded::THROWS},
        {"H.g2.ff.basic", BASIC, bytes(96, 0xff), Decoded::THROWS},
        {"H.g2.ff.legacy", LEGACY, bytes(96, 0xff), Decoded::THROWS},
        {"H.g1.fflow.legacy", LEGACY, bytes(48, 0xff, {{0, 0x1f}}), Decoded::THROWS},
        // the encoding of one scheme read under the other
        {"H.g1.legacyAsBasic", BASIC, of(pk.ToBytes(LEGACY)), Decoded::INVALID},
        {"H.g1.basicAsLegacy", LEGACY, of(pk.ToBytes(BASIC)), Decoded::VALID},
        {"H.g2.legacyAsBasic", BASIC, of(sig.ToBytes(LEGACY)), Decoded::INVALID},
        {"H.g2.basicAsLegacy", LEGACY, of(sig.ToBytes(BASIC)), Decoded::VALID},
        // the sign bit flipped: the negated point
        {"H.g1.negated.legacy", LEGACY, flipped(pk.ToBytes(LEGACY), 0, 0x80, true), Decoded::VALID},
        {"H.g1.negated.basic", BASIC, flipped(pk.ToBytes(BASIC), 0, 0x20, true), Decoded::VALID},
        // on the curve but outside the prime order subgroup
        {"H.g1.nonsub.legacy", LEGACY, bytes(48, 0, {{47, 4}}), Decoded::VALID},
        {"H.g1.nonsub.basic", BASIC, bytes(48, 0, {{0, 0x80}, {47, 4}}), Decoded::INVALID},
        {"H.g2.nonsub.legacy", LEGACY, bytes(96, 0, {{47, 1}}), Decoded::VALID},
        {"H.g2.nonsub.basic", BASIC, bytes(96, 0, {{0, 0x80}, {95, 1}}), Decoded::INVALID},
        // not the x coordinate of any point
        {"H.g1.nocurve.legacy", LEGACY, bytes(48, 0, {{47, 1}}), Decoded::VALID},
        {"H.g1.nocurve.basic", BASIC, bytes(48, 0, {{0, 0x80}, {47, 1}}), Decoded::INVALID},
        // reserved bits of the second coordinate set
        {"H.g2.flag48.basic", BASIC, flipped(sig.ToBytes(BASIC), 48, 0xe0, false), Decoded::THROWS},
        // legacy sign bit on values that do not decode to a point
        {"H.g1.nocurve.sign.legacy", LEGACY, bytes(48, 0, {{0, 0x80}, {47, 1}}), Decoded::VALID},
        {"H.g1.overp.sign.legacy", LEGACY, bytes(48, 0xff, {{0, 0x9f}}), Decoded::THROWS},
        {"H.g2.x2.sign.legacy", LEGACY, bytes(96, 0, {{0, 0x80}, {47, 2}}), Decoded::VALID},
        {"H.g2.x3.sign.legacy", LEGACY, bytes(96, 0, {{0, 0x80}, {47, 3}}), Decoded::THROWS},
        {"H.g2.x2.legacy", LEGACY, bytes(96, 0, {{47, 2}}), Decoded::VALID},
        {"H.g2.overp.sign.legacy", LEGACY, bytes(96, 0xff, {{0, 0x9f}}), Decoded::THROWS},
    };

    for (const DecodeProbe& probe : probes) {
        const std::string& name{probe.name};
        BOOST_TEST_CONTEXT(name)
        {
            // What the library made of the bytes: nothing, or a point with this encoding.
            const bool rejected{Ref(name + (Entries().count(name) ? "" : ".ser")) == "EXC"};
            const std::string encoding{rejected ? "" : Ref(name + ".ser")};
            const bool infinity{encoding.starts_with("c0")};
            const bool zeros{std::all_of(probe.bytes.begin(), probe.bytes.end(), [](uint8_t b) { return b == 0; })};
            // An object holds neither the point at infinity nor anything read from zeros.
            const bool expect_valid{!rejected && !infinity && !zeros};

            const bool is_g1{probe.bytes.size() == BLS_CURVE_PUBKEY_SIZE};
            CBLSPublicKey probe_pk;
            CBLSSignature probe_sig;
            bool valid;
            std::string hex;
            Decoded from_stream;
            if (is_g1) {
                probe_pk.SetBytes(probe.bytes, probe.scheme);
                valid = probe_pk.IsValid();
                hex = probe_pk.ToString(probe.scheme);
                from_stream = Unserialize<CBLSPublicKey>(probe.bytes, probe.scheme);
            } else {
                probe_sig.SetBytes(probe.bytes, probe.scheme);
                valid = probe_sig.IsValid();
                hex = probe_sig.ToString(probe.scheme);
                from_stream = Unserialize<CBLSSignature>(probe.bytes, probe.scheme);
            }
            BOOST_CHECK_EQUAL(valid, expect_valid);
            if (valid) BOOST_CHECK_EQUAL(hex, encoding);
            BOOST_CHECK_EQUAL(from_stream, probe.from_stream);

            // Whatever was accepted, nothing here is a key or a signature that verifies.
            if (valid && is_g1) BOOST_CHECK(!sig.VerifyInsecure(probe_pk, hash, LEGACY));
            if (valid && !is_g1) BOOST_CHECK(!probe_sig.VerifyInsecure(pk, hash, LEGACY));
            if (valid && !is_g1) BOOST_CHECK(!probe_sig.VerifyInsecure(pk, hash, BASIC));
        }
    }

    BOOST_CHECK_EQUAL(Ref("H.len47"), "EXC");
    CBLSPublicKey short_pk{pk};
    short_pk.SetBytes(bytes(47, 1), BASIC);
    BOOST_CHECK(!short_pk.IsValid());
}

//! Shares of a legacy threshold signature and what they recover to,
//! from https://gist.github.com/xdustinface/318c2c08c36ab12a2b1963caf1f7815c
BOOST_AUTO_TEST_CASE(legacy_quorum_signature)
{
    const uint256 hash{uint256S("b6d8ee31bbd375dfd55d5fb4b02cfccc68709e64f4c5ffcd3895ceb46540311d")};
    const std::string threshold_pk_hex{"97a12b918eac73718d55b7fca60435ec0b442d7e24b45cbd80f5cf2ea2e14c4164333fffdb00d27e309abbafacaa9c34"};
    const std::string threshold_sig_hex{"031c536960c9c44efefa4a3334d2d1b808f46abe121cd14a1d0b6ee6b6dca85fd3087d86fe5b1327e6792be53f4ed4df19c3af3aac79c0bb9dc36fc2a30ba566de6a82cd3e4af2d6654cbe02bb52769a06c1644a4c63b3c3a6fd16e5e68e5c0b"};
    const std::vector<std::string> sk_shares_hex{
        "43dd263542a8d10bc9f843b46f15c86cd87e00c8f45fe5339b30c08e3233d8e3",
        "5e7247ef1a0e671b8349e7be3f50ec88f1eafde90345f34520010e4aa9f18731",
        "34bcc40dea17bb03ec085ac46a0ea9614b3ffc4bae8b0b292f3d7c54662b00a6",
        "139f967b6f4af5dfe2bebf8085b6332efe31c2dc348d02e6b4745a0e7e56a469",
        "08442e959054d87b5de3553ef8cfd9da923241664c35c6548b5e3271a86b4a18",
        "2698613947a156639b423ad1a9fbe45863d58540d8ebd08612504bf9cf4743ea",
        "1871c9270c8344028946eee64e79d09e4915dd3b717ffc1c9aa86faff88c0475",
        "68409938427df3567e8948c1fff8610b5cf94872eb959c90a714b7ff0f339e88",
        "70d3e3ad7d22bd30e4e2ca108a3fa47f4873bda28f3b000a218339b09db6f58a",
        "39ea630e894b71d9f28fefb551611824f16d4b16d29fdea8bb3dbd857a6bc317",
    };
    const std::vector<std::string> sig_shares_hex{
        "0888879c99852460912fd28c7a9138926c1e87fd6609fd2d3d307764e49feb85702fd8f9b3b836bc11f7ce151b769dc70b760879d26f8c33a29e24f69297f45ef028f0794e63ddb0610db7de1a608b6d6a2129ada62b845004a408f651fd44a5",
        "16efc39fa5aa979245a82334856a97ebf3027bc6be7d35df117267a3c9b1618e32477fe1b8f4a23bdba346bf2b2b35ad0b395227de76827fd32eb9952e0d97b7dba275040101131a7fc38ea381a3099c2b3205177866ee4ab3119593bb58d092",
        "8407afd2776ab9d3f9293f1519ace1a9ce8aaf07d0a6a9785ec7ae4ae47e42102c09cfb3c8655dba014d53933af6a0b41244df006575e85e333271c90fcad80410cab4962bf4bba1570770775b1282f142b526d521a38fbc14336f22dc44a683",
        "027061a8c2d631e40882ce6919d3e5f45c4c74ac32a3bce94e5661d06cdecf2d492dfab99e9a9dd8a29a90fe8f816be30178bf9277a3751882e49bb9f08437f5f2cd9dbc12c2fdcccaf7578838e87273fd2ba87f20cf00ca5fec56822f845769",
        "178ece91967145b1dfc02de703dbd8049b05d626f18a71303ea0c23ee3b60a52bd61cc30fea3e4a562b20c20e0439a2f108b4e6b8a646f647afb64e3b355eba382380ef2c634f3a56de066b7a764249aba1c42c49d76d65e094e890cbbf005a3",
        "193da45d31d728acc92165173253fd8689301b448c81039350ae6916a72f157b00da469a7ab6d2b5db2dac216f47073d089afdcdffce25b6aac991f4745c803f164d7426b8da7d19bf699f5e5489f715ac32e539db90610d7df47121556c1a20",
        "938ce6cc265fa15fe67314ac4773f18ff0b49c01eac626814abc2f836814068aeba8582d619a3e0c08dc4bafecda84a818b6a7abd350637e72a47356e5919e3a72be66316417c598338e4ab85f8d25535bd6c4a5fb16767ac470902e0cf19df0",
        "985039d55a92f6fb3b324b0b9f1c7ddcd5f443d6d1ce72549f043b967ded7d56dc4320dc8569a1c41c6cfb4c150d8c61095d3b325e3308a321ceb43369fe56807fba67b6b313f072ab2cdbaa872b52a9a2e75bf396f1b2007152067f756946b7",
        "922717d8c170862662d08a4c29943cc26bb05daff0f07b0b7c352651ec64ee5a1d032bad24dfb42243e9afe044ed25680694b183b657948a91533e9a73b6bf359ff907d5088503137edc8e271ac3d2003a4daf8d36f3f847cc87afc6f9007c72",
        "02bc8e3ea8409418949fb4106e00893b49983495035b47026a1747eb6f89b05d4c1b8e357e89ddfa7c9f8145b78e0c480177842f20b98b1f7ca2ed26cfb9895380e4d86aeb60c2326bc43753a0633167a7c4ae7a526ce927ade1388a6cdc11d1",
    };
    const std::vector<std::string> ids_hex{
        "4393e2a243c3db776dcdbe2535493440d29cb65006a1e6f0f8d3f1e1488cbf1a",
        "8b2d776ac75cfca1559b5616ade4eb376a6478556135276e4b3210fe170b9f59",
        "f2015bdbc0daa70c41a25d2450b96f433ac7d568126505e9997794bb357cf3af",
        "5818a68f2f34e5ff7d1d43beca5ff103739dd918efda4bac7fd4ede6c53dc6af",
        "965437bdf51278f716078477a2eae595a9d2b0aa3fe69a387b30936c13c7d68e",
        "fd14695a48a35fe6a1f9894accfa83349508e350b3f743d494074fe204b17166",
        "7e3c28e59ff54bf097b2f3ada4a30f5613227951116675127fc97c98405f2067",
        "5e427dba092e3d81d057c0277a9a465e036c8336c59a18f27d7c21bc51910904",
        "90976dbe492de3eda7623c7ad6523ed9f63f83c3200c383fccd9f22408e18e1b",
        "cd474447afd5df18a0c10c9e2d5216eace9c624119974280236a043cb4b7f8ae",
    };

    const auto threshold_pk{FromHex<CBLSPublicKey>(threshold_pk_hex, LEGACY)};
    const auto threshold_sig{FromHex<CBLSSignature>(threshold_sig_hex, LEGACY)};
    std::vector<CBLSSignature> sig_shares;
    std::vector<CBLSId> ids;
    for (size_t i = 0; i < sk_shares_hex.size(); i++) {
        ids.emplace_back(uint256S(ids_hex[i]));
        sig_shares.push_back(SignBLS(SecretFromHex(sk_shares_hex[i]), hash, LEGACY));
        BOOST_CHECK_EQUAL(sig_shares.back().ToString(LEGACY), sig_shares_hex[i]);
        BOOST_CHECK(FromHex<CBLSSignature>(sig_shares_hex[i], LEGACY) == sig_shares.back());
    }

    CBLSSignature recovered;
    BOOST_REQUIRE(recovered.Recover(sig_shares, ids));
    BOOST_CHECK(recovered == threshold_sig);
    BOOST_CHECK_EQUAL(recovered.ToString(LEGACY), threshold_sig_hex);
    BOOST_CHECK(recovered.VerifyInsecure(threshold_pk, hash, LEGACY));
    BOOST_CHECK(!recovered.VerifyInsecure(threshold_pk, hash, BASIC));
}

BOOST_AUTO_TEST_CASE(points_that_are_not_keys_or_signatures)
{
    const CBLSSecretKey sk{RefKey(0)};
    const CBLSPublicKey pk{sk.GetPublicKey()};
    const uint256 hash{RefHash(0)};

    // Basic: decoding checks the subgroup, so none of these becomes an object.
    CBLSPublicKey basic_pk;
    BOOST_CHECK(!basic_pk.SetHexStr("8d5d0fb73b9c92df4eab4216e48c3e358578b4cc30f82c268bd6fef3bd34b558628daf1afef798d4c3b0fcd8b28c8973", BASIC));
    BOOST_CHECK(!basic_pk.SetHexStr("85" + std::string(94, '0'), BASIC));
    for (int first = 0; first < 256; first++) {
        std::vector<uint8_t> g1(BLS_CURVE_PUBKEY_SIZE, 0), g2(BLS_CURVE_SIG_SIZE, 0);
        g1[0] = g2[0] = static_cast<uint8_t>(first);
        CBLSSignature basic_sig;
        basic_pk.SetBytes(g1, BASIC);
        basic_sig.SetBytes(g2, BASIC);
        BOOST_CHECK_MESSAGE(!basic_pk.IsValid() && !basic_sig.IsValid(), first);
        g2[48] = 0xff;
        basic_sig.SetBytes(g2, BASIC);
        BOOST_CHECK_MESSAGE(!basic_sig.IsValid(), first);
    }

    const auto is_rejected = [](const std::vector<uint8_t>& bytes) {
        CBLSPublicKey as_pk;
        CBLSSignature as_sig;
        if (bytes.size() == BLS_CURVE_PUBKEY_SIZE) {
            as_pk.SetBytes(bytes, BASIC);
            return !as_pk.IsValid() && Unserialize<CBLSPublicKey>(bytes, BASIC) != Decoded::VALID;
        }
        as_sig.SetBytes(bytes, BASIC);
        return !as_sig.IsValid() && Unserialize<CBLSSignature>(bytes, BASIC) != Decoded::VALID;
    };
    const auto changed = [](const auto& array, size_t pos, uint8_t clear, uint8_t set) {
        std::vector<uint8_t> ret{array.begin(), array.end()};
        ret[pos] = (ret[pos] & ~clear) | set;
        return ret;
    };
    const auto pk_bytes{pk.ToBytes(BASIC)};
    const auto sig_bytes{sk.Sign(hash).ToBytes(BASIC)};
    BOOST_REQUIRE(!is_rejected(changed(pk_bytes, 0, 0, 0)) && !is_rejected(changed(sig_bytes, 0, 0, 0)));
    // the flag of a compressed encoding is missing, or that of infinity is set on a point
    BOOST_CHECK(is_rejected(changed(pk_bytes, 0, 0x80, 0)));
    BOOST_CHECK(is_rejected(changed(sig_bytes, 0, 0x80, 0)));
    BOOST_CHECK(is_rejected(changed(pk_bytes, 0, 0, 0x40)));
    BOOST_CHECK(is_rejected(changed(sig_bytes, 0, 0, 0x40)));
    // the second coordinate of a signature has no flags, each of these bits puts it above the modulus
    for (const uint8_t bit : {0x20, 0x40, 0x80}) {
        BOOST_CHECK_MESSAGE(is_rejected(changed(sig_bytes, 48, 0, bit)), bit);
    }
    // infinity is only the flags followed by zeros
    for (const size_t size : {BLS_CURVE_PUBKEY_SIZE, BLS_CURVE_SIG_SIZE}) {
        for (const size_t pos : {size_t{1}, size_t{47}, size_t{48}, size - 1}) {
            if (pos >= size) continue;
            std::vector<uint8_t> infinity(size, 0);
            infinity[0] = 0xc0;
            infinity[pos] = 1;
            BOOST_CHECK_MESSAGE(is_rejected(infinity), strprintf("%d %d", size, pos));
        }
    }
    // a coordinate equal to the field modulus
    const std::string p_hex{"1a0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaab"};
    const std::string flagged_p_hex{"9a" + p_hex.substr(2)};
    BOOST_CHECK(is_rejected(ParseHex(flagged_p_hex)));
    BOOST_CHECK(is_rejected(ParseHex(flagged_p_hex + std::string(96, '0'))));
    BOOST_CHECK(is_rejected(ParseHex("80" + std::string(94, '0') + p_hex)));
    // Small coordinates: some are of no point of the curve, the rest of points outside of the subgroup.
    for (uint8_t x = 0; x < 64; x++) {
        for (const uint8_t flags : {0x80, 0xa0}) {
            std::vector<uint8_t> g1(BLS_CURVE_PUBKEY_SIZE, 0), g2(BLS_CURVE_SIG_SIZE, 0);
            g1[0] = g2[0] = flags;
            g1[47] = g2[47] = x;
            BOOST_CHECK_MESSAGE(is_rejected(g1) && is_rejected(g2), x);
            g2[47] = 0;
            g2[95] = x;
            BOOST_CHECK_MESSAGE(is_rejected(g2), x);
        }
    }

    // Legacy: decoding does not check the subgroup. A point outside of it becomes
    // an object, and is rejected when it is verified.
    const std::string legacy_pk_hex{"11df3a748b713460f9b21083315c0dca1742b7962ca98685be4094d302e84b0884a04c1a55beb0ed921dae1dd66c0111"};
    const std::string legacy_bad_pk_hex{"11df3a748b713460f9b21083315c0dca1742b7962ca98685be4094d302e84b0884a04c1a55beb0ed921dae1dd66c0a11"};
    const std::string legacy_sig_hex{"0888879c99852460912fd28c7a9138926c1e87fd6609fd2d3d307764e49feb85702fd8f9b3b836bc11f7ce151b769dc70b760879d26f8c33a29e24f69297f45ef028f0794e63ddb0610db7de1a608b6d6a2129ada62b845004a408f651fd44a5"};
    const std::string legacy_bad_sig_hex{"0888879c99852460912fd28c7a9138926c1e87fd6609fd2d3d307764e49feb85702fd8f9b3b836bc11f7ce151b769dc70b760879d26f8c33a29e24f69297f45ef028f0794e63ddb0610db7de1a608b6d6a2129ada62b845004a408f651fd44a6"};
    for (const auto& hex : {legacy_pk_hex, legacy_bad_pk_hex}) {
        const auto legacy_pk{FromHex<CBLSPublicKey>(hex, LEGACY)};
        BOOST_CHECK_EQUAL(legacy_pk.ToString(LEGACY), hex);
        BOOST_CHECK_EQUAL(Unserialize<CBLSPublicKey>(ParseHex(hex), LEGACY), Decoded::VALID);
    }
    for (const auto& hex : {legacy_sig_hex, legacy_bad_sig_hex}) {
        const auto legacy_sig{FromHex<CBLSSignature>(hex, LEGACY)};
        BOOST_CHECK_EQUAL(legacy_sig.ToString(LEGACY), hex);
        BOOST_CHECK_EQUAL(Unserialize<CBLSSignature>(ParseHex(hex), LEGACY), Decoded::VALID);
    }
    const CBLSSignature sig{SignBLS(sk, hash, LEGACY)};
    std::vector<CBLSPublicKey> bad_pks{FromHex<CBLSPublicKey>(legacy_bad_pk_hex, LEGACY)};
    std::vector<uint256> hashes{hash};
    BOOST_CHECK(!sig.VerifyInsecure(bad_pks[0], hash, LEGACY));
    BOOST_CHECK(!sig.VerifyInsecureAggregated(bad_pks, hashes));
    BOOST_CHECK(!sig.VerifySecureAggregated(bad_pks, hash, LEGACY));
    const auto bad_sig{FromHex<CBLSSignature>(legacy_bad_sig_hex, LEGACY)};
    std::vector<CBLSPublicKey> pks{pk};
    BOOST_CHECK(!bad_sig.VerifyInsecure(pk, hash, LEGACY));
    BOOST_CHECK(!bad_sig.VerifyInsecureAggregated(pks, hashes));
    BOOST_CHECK(!bad_sig.VerifySecureAggregated(pks, hash, LEGACY));

    // Legacy: both coordinates equal to the field modulus read as zero, which is
    // no point at all.
    const std::string modulus{"1a0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaab"};
    CBLSSignature degenerate;
    degenerate.SetBytes(ParseHex(modulus + modulus), LEGACY);
    BOOST_CHECK(degenerate.IsValid());
    const CBLSPublicKey generator{SecretFromHex(std::string(63, '0') + "1").GetPublicKey()};
    BOOST_CHECK(!degenerate.VerifyInsecure(generator, hash, LEGACY));

    // The identity is not a signature, whichever way it is reached.
    for (const BLSScheme scheme : {LEGACY, BASIC}) {
        CBLSSignature identity{SignBLS(sk, hash, scheme)};
        identity.SubInsecure(identity);
        BOOST_CHECK(!identity.VerifyInsecure(pk, hash, scheme));
        BOOST_CHECK(!identity.VerifyInsecureAggregated(pks, hashes));
        BOOST_CHECK(!identity.VerifySecureAggregated(pks, hash, scheme));
    }
}

BOOST_AUTO_TEST_CASE(aggregation_is_a_sum)
{
    const uint256 hash{RefHash(1)};
    std::vector<CBLSSecretKey> sks{RefKey(1), RefKey(2), RefKey(3)};
    std::vector<CBLSPublicKey> pks;
    std::vector<CBLSSignature> sigs;
    for (const auto& sk : sks) {
        pks.push_back(sk.GetPublicKey());
        sigs.push_back(sk.Sign(hash));
    }
    const CBLSSecretKey agg_sk{CBLSSecretKey::AggregateInsecure(sks)};
    const CBLSPublicKey agg_pk{CBLSPublicKey::AggregateInsecure(pks)};
    const CBLSSignature agg_sig{CBLSSignature::AggregateInsecure(sigs)};
    BOOST_CHECK(agg_sk.GetPublicKey() == agg_pk);
    BOOST_CHECK(agg_sk.Sign(hash) == agg_sig);
    BOOST_CHECK(agg_sig.VerifyInsecure(agg_pk, hash, BASIC));

    // in any order, all at once or one by one
    std::reverse(sks.begin(), sks.end());
    BOOST_CHECK(CBLSSecretKey::AggregateInsecure(sks) == agg_sk);
    CBLSSecretKey sk{sks[1]};
    CBLSPublicKey pk{pks[1]};
    CBLSSignature sig{sigs[1]};
    for (const size_t i : {0, 2}) {
        sk.AggregateInsecure(sks[2 - i]);
        pk.AggregateInsecure(pks[i]);
        sig.AggregateInsecure(sigs[i]);
    }
    BOOST_CHECK(sk == agg_sk && pk == agg_pk && sig == agg_sig);

    // and taken apart again
    sig.SubInsecure(sigs[0]);
    sig.SubInsecure(sigs[2]);
    BOOST_CHECK(sig == sigs[1]);

    // multiplying by a secret key is the same as that many additions
    const CBLSSecretKey three{SecretFromHex(std::string(63, '0') + "3")};
    CBLSPublicKey tripled_pk{pks[0]};
    CBLSSignature tripled_sig{sigs[0]};
    tripled_pk.MulInsecure(three);
    tripled_sig.MulInsecure(three);
    std::vector<CBLSPublicKey> same_pk(3, pks[0]);
    std::vector<CBLSSignature> same_sig(3, sigs[0]);
    BOOST_CHECK(tripled_pk == CBLSPublicKey::AggregateInsecure(same_pk));
    BOOST_CHECK(tripled_sig == CBLSSignature::AggregateInsecure(same_sig));
    BOOST_CHECK(tripled_sig.VerifyInsecure(tripled_pk, hash, BASIC));
}

BOOST_AUTO_TEST_CASE(stream_round_trip)
{
    const CBLSSecretKey sk{RefKey(5)};
    const CBLSPublicKey pk{sk.GetPublicKey()};
    const auto round_trip = [](const auto& obj, auto written_as, auto read_as) {
        CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
        obj.Serialize(stream, written_as);
        BOOST_CHECK_EQUAL(stream.size(), obj.SerSize);
        std::decay_t<decltype(obj)> read;
        read.Unserialize(stream, read_as);
        BOOST_CHECK(stream.empty());
        return read;
    };
    for (const BLSScheme scheme : {LEGACY, BASIC}) {
        const CBLSSignature sig{SignBLS(sk, RefHash(5), scheme)};
        BOOST_CHECK(round_trip(pk, scheme, scheme) == pk);
        BOOST_CHECK(round_trip(sig, scheme, scheme) == sig);
        // a null object is all zeros under either scheme
        BOOST_CHECK(!round_trip(CBLSPublicKey{}, scheme, scheme == LEGACY ? BASIC : LEGACY).IsValid());
        BOOST_CHECK_EQUAL(CBLSSignature{}.ToString(scheme), std::string(2 * BLS_CURVE_SIG_SIZE, '0'));
    }
    BOOST_CHECK(round_trip(sk, BLSNoScheme{}, BLSNoScheme{}) == sk);
    BOOST_CHECK(!round_trip(CBLSSecretKey{}, BLSNoScheme{}, BLSNoScheme{}).IsValid());
    BOOST_CHECK_EQUAL(Unserialize<CBLSSecretKey>(std::vector<uint8_t>(32, 0xff)), Decoded::THROWS);
}

BOOST_AUTO_TEST_CASE(ies_key_is_the_shared_point)
{
    // The key of the cipher is the start of the basic encoding of the Diffie-Hellman point.
    const CBLSSecretKey ephemeral{RefKey(702)}, recipient{RefKey(703)};
    const std::vector<uint8_t> key{ParseHex(Ref("G.dh").substr(0, 64))};
    const std::vector<uint8_t> plaintext(48, 0x5a);

    CBLSIESEncryptedBlob blob;
    blob.ephemeralPubKey = ephemeral.GetPublicKey();
    blob.ivSeed = RefHash(1);
    blob.data.resize(plaintext.size());
    AES256CBCEncrypt{key.data(), blob.ivSeed.begin(), /*padIn=*/false}.Encrypt(plaintext.data(), plaintext.size(), blob.data.data());

    CDataStream decrypted{SER_NETWORK, PROTOCOL_VERSION};
    BOOST_REQUIRE(blob.Decrypt(0, recipient, decrypted));
    BOOST_CHECK(std::equal(plaintext.begin(), plaintext.end(), UCharCast(decrypted.data()), UCharCast(decrypted.data() + decrypted.size())));

    CBLSIESMultiRecipientBlobs blobs;
    blobs.InitEncrypt(2);
    BOOST_REQUIRE(blobs.Encrypt(0, recipient.GetPublicKey(), plaintext));
    BOOST_REQUIRE(blobs.Encrypt(1, ephemeral.GetPublicKey(), plaintext));
    BOOST_CHECK(blobs.blobs[0] != blobs.blobs[1]);
    for (const auto& [idx, owner, other] : {std::tuple{0, recipient, ephemeral}, std::tuple{1, ephemeral, recipient}}) {
        CBLSIESMultiRecipientBlobs::Blob out;
        BOOST_REQUIRE(blobs.Decrypt(idx, owner, out));
        BOOST_CHECK(out == plaintext);
        BOOST_REQUIRE(blobs.Decrypt(idx, other, out));
        BOOST_CHECK(out != plaintext);
    }
    CBLSIESMultiRecipientBlobs::Blob out;
    BOOST_CHECK(!blobs.Decrypt(2, recipient, out));
    BOOST_CHECK(!blobs.Decrypt(0, CBLSSecretKey{}, out));
    BOOST_CHECK(!blobs.Encrypt(0, CBLSPublicKey{}, plaintext));
}

BOOST_AUTO_TEST_SUITE_END()
