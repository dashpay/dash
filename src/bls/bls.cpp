// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls/bls.h>

#include <bls/legacy.h>
#include <crypto/common.h>
#include <crypto/sha256.h>
#include <random.h>
#include <span.h>

#include <support/cleanse.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

namespace {
constexpr std::string_view BASIC_SCHEME_DST{"BLS_SIG_BLS12381G2_XMD:SHA-256_SSWU_RO_NUL_"};

//! The point a message is signed as. The legacy scheme has its own mapping, older than the standard one.
blst_p2 HashToG2(const uint256& hash, const BLSScheme scheme)
{
    blst_p2 point;
    if (scheme == BLSScheme::Legacy) {
        bls::ep2_map_legacy(&point, hash.begin(), hash.size());
    } else {
        blst_hash_to_g2(&point, hash.begin(), hash.size(), UCharCast(BASIC_SCHEME_DST.data()), BASIC_SCHEME_DST.size(),
                        nullptr, 0);
    }
    return point;
}

//! Whether a point can be a public key or a signature. Decoding checks the subgroup for the basic encoding
//! only, so that a point decoded from the legacy one is checked here, when it is first verified with.
bool IsKeyOrSignature(const blst_p1& point)
{
    return !blst_p1_is_inf(&point) && blst_p1_on_curve(&point) && blst_p1_in_g1(&point);
}

bool IsKeyOrSignature(const blst_p2& point)
{
    return !blst_p2_is_inf(&point) && blst_p2_on_curve(&point) && blst_p2_in_g2(&point);
}

//! The number of a secret key, which is zero for a key that was never set
const blst_scalar& Scalar(const CBLSSecretScalar& secret)
{
    static const blst_scalar zero{};
    return secret.empty() ? zero : secret.front();
}

blst_scalar& Scalar(CBLSSecretScalar& secret)
{
    secret.resize(1);
    return secret.front();
}

//! A product of pairings, computed without allocating whatever the number of pairs.
class PairingProduct
{
    //! As many pairs as blst itself runs through one Miller loop
    static constexpr size_t BATCH_SIZE{16};
    std::array<blst_p1_affine, BATCH_SIZE> m_g1;
    std::array<blst_p2_affine, BATCH_SIZE> m_g2;
    size_t m_pending{0};
    blst_fp12 m_product{*blst_fp12_one()};

    void Flush()
    {
        if (m_pending == 0) return;
        std::array<const blst_p1_affine*, BATCH_SIZE> g1;
        std::array<const blst_p2_affine*, BATCH_SIZE> g2;
        for (size_t i = 0; i < m_pending; i++) {
            g1[i] = &m_g1[i];
            g2[i] = &m_g2[i];
        }
        blst_fp12 loops;
        blst_miller_loop_n(&loops, g2.data(), g1.data(), m_pending);
        blst_fp12_mul(&m_product, &m_product, &loops);
        m_pending = 0;
    }

public:
    void Add(const blst_p1_affine& g1, const blst_p2& g2)
    {
        // A pairing with the point at infinity is the identity. It is recognised here because
        // the affine form of a point cannot tell infinity from (0, 0).
        if (blst_p2_is_inf(&g2)) return;
        m_g1[m_pending] = g1;
        blst_p2_to_affine(&m_g2[m_pending], &g2);
        if (++m_pending == BATCH_SIZE) Flush();
    }

    void Add(const blst_p1& g1, const blst_p2& g2)
    {
        if (blst_p1_is_inf(&g1)) return;
        blst_p1_affine affine;
        blst_p1_to_affine(&affine, &g1);
        Add(affine, g2);
    }

    bool IsOne()
    {
        Flush();
        blst_final_exp(&m_product, &m_product);
        return blst_fp12_is_one(&m_product);
    }
};

//! e(g1, sig) == prod e(pk_i, H(m_i)), as 1 == e(-g1, sig) * prod e(pk_i, H(m_i))
class SignatureCheck
{
    PairingProduct m_product;

public:
    explicit SignatureCheck(const blst_p2& sig) { m_product.Add(BLS12_381_NEG_G1, sig); }
    void Add(const blst_p1& pk, const uint256& hash, BLSScheme scheme) { m_product.Add(pk, HashToG2(hash, scheme)); }
    bool Verify() { return m_product.IsOne(); }
};

using PublicKeyBytes = std::array<uint8_t, BLS_CURVE_PUBKEY_SIZE>;

/**
 * The factors that make an aggregate of signatures of one message secure against a signer
 * who chooses its key after seeing the others: every signature and public key is multiplied by
 * a number that depends on all of the public keys.
 *
 * Returns one factor per public key, in the order the keys were given in. The factors depend
 * on the set of keys only, not on that order.
 */
std::vector<blst_scalar> SecureAggregationFactors(const std::vector<PublicKeyBytes>& pks)
{
    std::vector<std::pair<PublicKeyBytes, size_t>> sorted;
    sorted.reserve(pks.size());
    for (size_t i = 0; i < pks.size(); i++) {
        sorted.emplace_back(pks[i], i);
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return std::memcmp(a.first.data(), b.first.data(), a.first.size()) < 0;
    });

    CSHA256 hasher;
    for (const auto& [bytes, _] : sorted) {
        hasher.Write(bytes.data(), bytes.size());
    }
    std::array<uint8_t, 4 + CSHA256::OUTPUT_SIZE> preimage;
    hasher.Finalize(preimage.data() + 4);

    std::vector<blst_scalar> factors(pks.size());
    for (size_t i = 0; i < sorted.size(); i++) {
        WriteBE32(preimage.data(), i);
        std::array<uint8_t, CSHA256::OUTPUT_SIZE> hash;
        CSHA256().Write(preimage.data(), preimage.size()).Finalize(hash.data());
        blst_scalar_from_be_bytes(&factors[sorted[i].second], hash.data(), hash.size());
    }
    return factors;
}

//! The sum of every point multiplied by its factor, for points of the subgroup: computed all at once,
//! which takes a fraction of the time of the multiplications one by one.
blst_p1 WeightedSum(const std::vector<const blst_p1*>& points, const std::vector<blst_scalar>& factors)
{
    assert(!points.empty() && points.size() == factors.size());
    std::vector<blst_p1_affine> affine(points.size());
    blst_p1s_to_affine(affine.data(), points.data(), points.size());

    // blst reads an array that follows its first element when the next pointer is null
    const blst_p1_affine* const affine_arg[]{affine.data(), nullptr};
    const uint8_t* const factors_arg[]{factors[0].b, nullptr};
    static_assert(sizeof(blst_scalar) == 32);
    std::vector<limb_t> scratch(blst_p1s_mult_pippenger_scratch_sizeof(points.size()) / sizeof(limb_t) + 1);

    blst_p1 sum;
    blst_p1s_mult_pippenger(&sum, affine_arg, points.size(), factors_arg, 256, scratch.data());
    return sum;
}

//! The points that objects hold, as WeightedSum() takes them
template <typename T, typename Point, typename Base>
std::vector<const Point*> PointsOf(Span<T> objects, Point Base::* impl)
{
    std::vector<const Point*> points;
    points.reserve(objects.size());
    for (const T& object : objects) {
        points.push_back(&(object.*impl));
    }
    return points;
}

//! The number an id stands for as the argument of a polynomial
blst_scalar ScalarFromId(const uint256& id)
{
    blst_scalar scalar;
    blst_scalar_from_be_bytes(&scalar, id.begin(), id.size());
    return scalar;
}

bool IsZero(const blst_scalar& scalar)
{
    static const blst_scalar zero{};
    return std::memcmp(&scalar, &zero, sizeof(scalar)) == 0;
}

//! blst reads a scalar from the bytes it is kept in, so that no copy of a secret one is made.
void Multiply(blst_p1& out, const blst_p1& point, const blst_scalar& scalar)
{
    blst_p1_mult(&out, &point, scalar.b, 256);
}

void Multiply(blst_p2& out, const blst_p2& point, const blst_scalar& scalar)
{
    blst_p2_mult(&out, &point, scalar.b, 256);
}
} // namespace

bool BLSDecode(blst_p1& out, Span<const uint8_t> bytes, const BLSScheme scheme)
{
    if (scheme == BLSScheme::Legacy) return bls::DecodeLegacy(out, bytes.data());
    // blst accepts nothing but the canonical encoding of infinity or of a point of the curve
    blst_p1_affine point;
    if (blst_p1_uncompress(&point, bytes.data()) != BLST_SUCCESS || !blst_p1_affine_in_g1(&point)) return false;
    blst_p1_from_affine(&out, &point);
    return true;
}

bool BLSDecode(blst_p2& out, Span<const uint8_t> bytes, const BLSScheme scheme)
{
    if (scheme == BLSScheme::Legacy) return bls::DecodeLegacy(out, bytes.data());
    // As for G1. Each coordinate is checked to be below the modulus, so the second one has no flags.
    blst_p2_affine point;
    if (blst_p2_uncompress(&point, bytes.data()) != BLST_SUCCESS || !blst_p2_affine_in_g2(&point)) return false;
    blst_p2_from_affine(&out, &point);
    return true;
}

void BLSEncode(uint8_t* out, const blst_p1& in, const BLSScheme scheme)
{
    if (scheme == BLSScheme::Legacy) return bls::EncodeLegacy(out, in);
    blst_p1_compress(out, &in);
}

void BLSEncode(uint8_t* out, const blst_p2& in, const BLSScheme scheme)
{
    if (scheme == BLSScheme::Legacy) return bls::EncodeLegacy(out, in);
    blst_p2_compress(out, &in);
}

bool BLSDecode(CBLSSecretScalar& out, Span<const uint8_t> bytes, BLSNoScheme)
{
    // BLS12-381 group order r, big-endian.
    // Taken from src/blst/src/consts.c: BLS12_381_r = { 0xffffffff00000001, 0x53bda402fffe5bfe, 0x3339d80809a1d805, 0x73eda753299d7d48 }
    //     (little-endian limbs of the same number, with the comment z^4 - z^2 + 1, group order).
    static const uint8_t ORDER_BE[BLS_CURVE_SECKEY_SIZE] = {
        0x73, 0xed, 0xa7, 0x53, 0x29, 0x9d, 0x7d, 0x48, 0x33, 0x39, 0xd8, 0x08,
        0x09, 0xa1, 0xd8, 0x05, 0x53, 0xbd, 0xa4, 0x02, 0xff, 0xfe, 0x5b, 0xfe,
        0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01};
    // Values strictly greater than the group order are rejected; the
    // order itself is accepted, matching the relic-based
    // implementation's bn_cmp(keydata, order) > 0 check.
    if (memcmp(bytes.data(), ORDER_BE, sizeof(ORDER_BE)) > 0) return false;
    blst_scalar_from_bendian(&Scalar(out), bytes.data());
    return true;
}

void BLSEncode(uint8_t* out, const CBLSSecretScalar& in, BLSNoScheme)
{
    // blst_bendian_from_scalar converts through a temporary limb vector and
    // securely wipes it, which costs more than the serialization itself;
    // the scalar's native bytes are little-endian, so copy and reverse.
    blst_lendian_from_scalar(out, &Scalar(in));
    std::reverse(out, out + BLS_CURVE_SECKEY_SIZE);
}

CBLSId::CBLSId(const uint256& nHash) : CBLSWrapper<uint256, BLS_CURVE_ID_SIZE, CBLSId>()
{
    impl = nHash;
    fValid = true;
    cachedHash.SetNull();
}

void CBLSSecretKey::AggregateInsecure(const CBLSSecretKey& o)
{
    assert(IsValid() && o.IsValid());
    // A sum starts from zero, which is what reduces a key that is the group order itself.
    static const blst_scalar zero{};
    blst_scalar& scalar{Scalar(impl)};
    blst_sk_add_n_check(&scalar, &zero, &scalar);
    blst_sk_add_n_check(&scalar, &scalar, &Scalar(o.impl));
    cachedHash.SetNull();
}

CBLSSecretKey CBLSSecretKey::AggregateInsecure(Span<CBLSSecretKey> sks)
{
    if (sks.empty()) {
        return {};
    }

    CBLSSecretKey ret;
    blst_scalar& sum{Scalar(ret.impl)};
    for (const auto& sk : sks) {
        blst_sk_add_n_check(&sum, &sum, &Scalar(sk.impl));
    }
    ret.fValid = true;
    return ret;
}

#ifndef BUILD_BITCOIN_INTERNAL
void CBLSSecretKey::MakeNewKey()
{
    unsigned char buf[SerSize];
    do {
        GetStrongRandBytes({buf, sizeof(buf)});
        SetBytes(buf);
    } while (!IsValid());
    memory_cleanse(buf, sizeof(buf));
}
#endif

bool CBLSSecretKey::SecretKeyShare(Span<CBLSSecretKey> msk, const CBLSId& _id)
{
    fValid = false;
    cachedHash.SetNull();

    if (!_id.IsValid() || msk.size() < 2) {
        return false;
    }
    for (const CBLSSecretKey& sk : msk) {
        if (!sk.IsValid()) {
            return false;
        }
    }

    // Horner's method: the polynomial with these coefficients, evaluated at the id
    const blst_scalar x{ScalarFromId(_id.impl)};
    blst_scalar& y{Scalar(impl)};
    y = Scalar(std::as_const(msk.back().impl));
    for (size_t i = msk.size() - 1; i > 0; --i) {
        blst_sk_mul_n_check(&y, &y, &x);
        blst_sk_add_n_check(&y, &y, &Scalar(std::as_const(msk[i - 1].impl)));
    }

    fValid = true;
    return true;
}

CBLSPublicKey CBLSSecretKey::GetPublicKey() const
{
    if (!IsValid()) {
        return {};
    }

    CBLSPublicKey pubKey;
    blst_sk_to_pk_in_g1(&pubKey.impl, &Scalar(impl));
    pubKey.fValid = true;
    return pubKey;
}

CBLSSignature CBLSSecretKey::Sign(const uint256& hash) const
{
    if (!IsValid()) {
        return {};
    }

    CBLSSignature sigRet;
    const blst_p2 point{HashToG2(hash, BLSScheme::Basic)};
    blst_sign_pk_in_g1(&sigRet.impl, &point, &Scalar(impl));
    sigRet.fValid = true;
    return sigRet;
}

void CBLSPublicKey::AggregateInsecure(const CBLSPublicKey& o)
{
    assert(IsValid() && o.IsValid());
    blst_p1_add_or_double(&impl, &impl, &o.impl);
    cachedHash.SetNull();
}

CBLSPublicKey CBLSPublicKey::AggregateInsecure(Span<CBLSPublicKey> pks)
{
    if (pks.empty()) {
        return {};
    }

    CBLSPublicKey ret;
    for (const auto& pk : pks) {
        blst_p1_add_or_double(&ret.impl, &ret.impl, &pk.impl);
    }
    ret.fValid = true;
    return ret;
}

bool CBLSPublicKey::PublicKeyShare(Span<CBLSPublicKey> mpk, const CBLSId& _id)
{
    fValid = false;
    cachedHash.SetNull();

    if (!_id.IsValid() || mpk.size() < 2) {
        return false;
    }
    for (const CBLSPublicKey& pk : mpk) {
        if (!pk.IsValid()) {
            return false;
        }
    }

    // The polynomial with these coefficients, evaluated at the id: every coefficient times a power of it
    const blst_scalar x{ScalarFromId(_id.impl)};
    std::vector<blst_scalar> powers(mpk.size());
    powers[0].b[0] = 1;
    for (size_t i = 1; i < powers.size(); i++) {
        blst_sk_mul_n_check(&powers[i], &powers[i - 1], &x);
    }
    impl = WeightedSum(PointsOf(mpk, &CBLSPublicKey::impl), powers);

    fValid = true;
    return true;
}

void CBLSPublicKey::MulInsecure(const CBLSSecretKey& factor)
{
    assert(IsValid() && factor.IsValid());
    Multiply(impl, impl, Scalar(factor.impl));
    cachedHash.SetNull();
}

bool CBLSPublicKey::DHKeyExchange(const CBLSSecretKey& sk, const CBLSPublicKey& pk)
{
    fValid = false;
    cachedHash.SetNull();

    if (!sk.IsValid() || !pk.IsValid()) {
        return false;
    }
    Multiply(impl, pk.impl, Scalar(sk.impl));
    fValid = true;
    return true;
}

void CBLSSignature::AggregateInsecure(const CBLSSignature& o)
{
    assert(IsValid() && o.IsValid());
    blst_p2_add_or_double(&impl, &impl, &o.impl);
    cachedHash.SetNull();
}

CBLSSignature CBLSSignature::AggregateInsecure(Span<CBLSSignature> sigs)
{
    if (sigs.empty()) {
        return {};
    }

    CBLSSignature ret;
    for (const auto& sig : sigs) {
        blst_p2_add_or_double(&ret.impl, &ret.impl, &sig.impl);
    }
    ret.fValid = true;
    return ret;
}

CBLSSignature CBLSSignature::AggregateSecure(Span<CBLSSignature> sigs,
                                             Span<CBLSPublicKey> pks,
                                             const uint256& hash)
{
    if (sigs.size() != pks.size() || sigs.empty()) {
        return {};
    }

    std::vector<PublicKeyBytes> pk_bytes;
    pk_bytes.reserve(pks.size());
    for (const auto& pk : pks) {
        BLSEncode(pk_bytes.emplace_back().data(), pk.impl, BLSScheme::Basic);
    }
    const std::vector<blst_scalar> factors{SecureAggregationFactors(pk_bytes)};

    CBLSSignature ret;
    for (size_t i = 0; i < sigs.size(); i++) {
        blst_p2 multiple;
        Multiply(multiple, sigs[i].impl, factors[i]);
        blst_p2_add_or_double(&ret.impl, &ret.impl, &multiple);
    }
    ret.fValid = true;
    return ret;
}

void CBLSSignature::SubInsecure(const CBLSSignature& o)
{
    assert(IsValid() && o.IsValid());
    blst_p2 negated{o.impl};
    blst_p2_cneg(&negated, true);
    blst_p2_add_or_double(&impl, &impl, &negated);
    cachedHash.SetNull();
}

void CBLSSignature::MulInsecure(const CBLSSecretKey& factor)
{
    assert(IsValid() && factor.IsValid());
    Multiply(impl, impl, Scalar(factor.impl));
    cachedHash.SetNull();
}

bool CBLSSignature::VerifyInsecure(const CBLSPublicKey& pubKey, const uint256& hash, const BLSScheme scheme) const
{
    if (!IsValid() || !pubKey.IsValid()) {
        return false;
    }
    if (!IsKeyOrSignature(impl) || !IsKeyOrSignature(pubKey.impl)) {
        return false;
    }

    SignatureCheck check{impl};
    check.Add(pubKey.impl, hash, scheme);
    return check.Verify();
}

bool CBLSSignature::VerifyInsecureAggregated(Span<CBLSPublicKey> pubKeys, Span<uint256> hashes) const
{
    if (!IsValid()) {
        return false;
    }
    assert(!pubKeys.empty() && !hashes.empty() && pubKeys.size() == hashes.size());

    // The basic scheme is only secure for an aggregate over distinct messages.
    std::vector<uint256> sorted{hashes.begin(), hashes.end()};
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
        return false;
    }
    if (!IsKeyOrSignature(impl)) {
        return false;
    }

    SignatureCheck check{impl};
    for (size_t i = 0; i < pubKeys.size(); i++) {
        if (!pubKeys[i].IsValid() || !IsKeyOrSignature(pubKeys[i].impl)) {
            return false;
        }
        check.Add(pubKeys[i].impl, hashes[i], BLSScheme::Basic);
    }
    return check.Verify();
}

bool CBLSSignature::VerifySecureAggregated(Span<CBLSPublicKey> pks, const uint256& hash, const BLSScheme scheme) const
{
    if (pks.empty()) {
        return false;
    }
    if (!IsKeyOrSignature(impl)) {
        return false;
    }

    std::vector<PublicKeyBytes> pk_bytes;
    pk_bytes.reserve(pks.size());
    for (const auto& pk : pks) {
        if (!IsKeyOrSignature(pk.impl)) {
            return false;
        }
        BLSEncode(pk_bytes.emplace_back().data(), pk.impl, scheme);
    }
    const std::vector<blst_scalar> factors{SecureAggregationFactors(pk_bytes)};

    const blst_p1 aggregated{WeightedSum(PointsOf(pks, &CBLSPublicKey::impl), factors)};
    if (blst_p1_is_inf(&aggregated)) {
        return false;
    }

    SignatureCheck check{impl};
    check.Add(aggregated, hash, scheme);
    return check.Verify();
}

bool CBLSSignature::Recover(Span<CBLSSignature> sigs, Span<CBLSId> ids)
{
    fValid = false;
    cachedHash.SetNull();

    if (sigs.size() < 2 || sigs.size() != ids.size()) {
        return false;
    }

    std::vector<blst_scalar> xs;
    xs.reserve(ids.size());
    for (size_t i = 0; i < sigs.size(); i++) {
        if (!sigs[i].IsValid() || !ids[i].IsValid()) {
            return false;
        }
        xs.push_back(ScalarFromId(ids[i].impl));
    }

    // Lagrange interpolation at zero. The share of x_i is weighted by
    //   prod_{j != i} x_j / (x_j - x_i) = (prod_j x_j) / (x_i * prod_{j != i} (x_j - x_i))
    blst_scalar numerator{xs[0]};
    for (size_t i = 1; i < xs.size(); i++) {
        blst_sk_mul_n_check(&numerator, &numerator, &xs[i]);
    }
    if (IsZero(numerator)) {
        return false;
    }

    blst_p2 recovered{};
    for (size_t i = 0; i < xs.size(); i++) {
        blst_scalar denominator{xs[i]};
        for (size_t j = 0; j < xs.size(); j++) {
            if (j == i) continue;
            blst_scalar difference;
            blst_sk_sub_n_check(&difference, &xs[j], &xs[i]);
            if (IsZero(difference)) {
                // two shares of the same id
                return false;
            }
            blst_sk_mul_n_check(&denominator, &denominator, &difference);
        }
        blst_scalar weight;
        blst_sk_inverse(&weight, &denominator);
        blst_sk_mul_n_check(&weight, &numerator, &weight);

        blst_p2 multiple;
        Multiply(multiple, sigs[i].impl, weight);
        blst_p2_add_or_double(&recovered, &recovered, &multiple);
    }
    impl = recovered;

    fValid = true;
    return true;
}
