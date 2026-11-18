// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls/bls.h>

#include <dashbls/legacy.hpp>
#include <crypto/common.h>
#include <crypto/sha256.h>
#include <random.h>
#include <span.h>

#ifndef BUILD_BITCOIN_INTERNAL
#include <support/allocators/mt_pooled_secure.h>
#include <support/cleanse.h>
#endif

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

CBLSId::CBLSId(const uint256& nHash) : CBLSWrapper<CBLSIdImplicit, BLS_CURVE_ID_SIZE, CBLSId>()
{
    impl = nHash;
    fValid = true;
    cachedHash.SetNull();
}

void CBLSSecretKey::AggregateInsecure(const CBLSSecretKey& o)
{
    assert(IsValid() && o.IsValid());
    impl = bls::PrivateKey::Aggregate({impl, o.impl});
    cachedHash.SetNull();
}

CBLSSecretKey CBLSSecretKey::AggregateInsecure(Span<CBLSSecretKey> sks)
{
    if (sks.empty()) {
        return {};
    }

    std::vector<bls::PrivateKey> v;
    v.reserve(sks.size());
    for (const auto& sk : sks) {
        v.emplace_back(sk.impl);
    }

    CBLSSecretKey ret;
    ret.impl = bls::PrivateKey::Aggregate(v);
    ret.fValid = true;
    ret.cachedHash.SetNull();
    return ret;
}

#ifndef BUILD_BITCOIN_INTERNAL
void CBLSSecretKey::MakeNewKey()
{
    unsigned char buf[SerSize];
    while (true) {
        GetStrongRandBytes({buf, sizeof(buf)});
        try {
            impl = bls::PrivateKey::FromBytes(bls::Bytes(reinterpret_cast<const uint8_t*>(buf), SerSize));
            if (impl == bls::PrivateKey()) {
                continue;
            }
            break;
        } catch (...) {
        }
    }
    memory_cleanse(buf, sizeof(buf));
    fValid = true;
    cachedHash.SetNull();
}
#endif

bool CBLSSecretKey::SecretKeyShare(Span<CBLSSecretKey> msk, const CBLSId& _id)
{
    fValid = false;
    cachedHash.SetNull();

    if (!_id.IsValid()) {
        return false;
    }

    std::vector<bls::PrivateKey> mskVec;
    mskVec.reserve(msk.size());
    for (const CBLSSecretKey& sk : msk) {
        if (!sk.IsValid()) {
            return false;
        }
        mskVec.emplace_back(sk.impl);
    }

    try {
        impl = bls::Threshold::PrivateKeyShare(mskVec, bls::Bytes(_id.impl.begin(), _id.impl.size()));
    } catch (...) {
        return false;
    }

    fValid = true;
    cachedHash.SetNull();
    return true;
}

CBLSPublicKey CBLSSecretKey::GetPublicKey() const
{
    if (!IsValid()) {
        return {};
    }

    CBLSPublicKey pubKey;
    pubKey.impl = impl.GetG1Element();
    pubKey.fValid = true;
    pubKey.cachedHash.SetNull();
    return pubKey;
}

CBLSSignature CBLSSecretKey::Sign(const uint256& hash) const
{
    if (!IsValid()) {
        return {};
    }

    CBLSSignature sigRet;
    try {
        sigRet.impl = impl.SignG2(hash.begin(), hash.size(), UCharCast(BASIC_SCHEME_DST.data()), BASIC_SCHEME_DST.size());
        sigRet.fValid = true;
    } catch (...) {
        sigRet.fValid = false;
    }

    sigRet.cachedHash.SetNull();

    return sigRet;
}

void CBLSPublicKey::AggregateInsecure(const CBLSPublicKey& o)
{
    assert(IsValid() && o.IsValid());
    blst_p1_add_or_double(&impl.p, &impl.p, &o.impl.p);
    cachedHash.SetNull();
}

CBLSPublicKey CBLSPublicKey::AggregateInsecure(Span<CBLSPublicKey> pks)
{
    if (pks.empty()) {
        return {};
    }

    CBLSPublicKey ret;
    for (const auto& pk : pks) {
        blst_p1_add_or_double(&ret.impl.p, &ret.impl.p, &pk.impl.p);
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

    // Horner's method: the polynomial with these coefficients, evaluated at the id
    const blst_scalar x{ScalarFromId(_id.impl)};
    blst_p1 y{mpk.back().impl.p};
    for (size_t i = mpk.size() - 1; i > 0; --i) {
        Multiply(y, y, x);
        blst_p1_add_or_double(&y, &y, &mpk[i - 1].impl.p);
    }
    impl.p = y;

    fValid = true;
    return true;
}

void CBLSPublicKey::MulInsecure(const CBLSSecretKey& factor)
{
    assert(IsValid() && factor.IsValid());
    impl = factor.impl * impl;
    cachedHash.SetNull();
}

bool CBLSPublicKey::DHKeyExchange(const CBLSSecretKey& sk, const CBLSPublicKey& pk)
{
    fValid = false;
    cachedHash.SetNull();

    if (!sk.IsValid() || !pk.IsValid()) {
        return false;
    }
    impl = sk.impl * pk.impl;
    fValid = true;
    cachedHash.SetNull();
    return true;
}

void CBLSSignature::AggregateInsecure(const CBLSSignature& o)
{
    assert(IsValid() && o.IsValid());
    blst_p2_add_or_double(&impl.q, &impl.q, &o.impl.q);
    cachedHash.SetNull();
}

CBLSSignature CBLSSignature::AggregateInsecure(Span<CBLSSignature> sigs)
{
    if (sigs.empty()) {
        return {};
    }

    CBLSSignature ret;
    for (const auto& sig : sigs) {
        blst_p2_add_or_double(&ret.impl.q, &ret.impl.q, &sig.impl.q);
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
        pk_bytes.push_back(pk.impl.SerializeToArray());
    }
    const std::vector<blst_scalar> factors{SecureAggregationFactors(pk_bytes)};

    CBLSSignature ret;
    for (size_t i = 0; i < sigs.size(); i++) {
        blst_p2 multiple;
        Multiply(multiple, sigs[i].impl.q, factors[i]);
        blst_p2_add_or_double(&ret.impl.q, &ret.impl.q, &multiple);
    }
    ret.fValid = true;
    return ret;
}

void CBLSSignature::SubInsecure(const CBLSSignature& o)
{
    assert(IsValid() && o.IsValid());
    blst_p2 negated{o.impl.q};
    blst_p2_cneg(&negated, true);
    blst_p2_add_or_double(&impl.q, &impl.q, &negated);
    cachedHash.SetNull();
}

void CBLSSignature::MulInsecure(const CBLSSecretKey& factor)
{
    assert(IsValid() && factor.IsValid());
    impl = factor.impl * impl;
    cachedHash.SetNull();
}

bool CBLSSignature::VerifyInsecure(const CBLSPublicKey& pubKey, const uint256& hash, const BLSScheme scheme) const
{
    if (!IsValid() || !pubKey.IsValid()) {
        return false;
    }
    if (!IsKeyOrSignature(impl.q) || !IsKeyOrSignature(pubKey.impl.p)) {
        return false;
    }

    SignatureCheck check{impl.q};
    check.Add(pubKey.impl.p, hash, scheme);
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
    if (!IsKeyOrSignature(impl.q)) {
        return false;
    }

    SignatureCheck check{impl.q};
    for (size_t i = 0; i < pubKeys.size(); i++) {
        if (!pubKeys[i].IsValid() || !IsKeyOrSignature(pubKeys[i].impl.p)) {
            return false;
        }
        check.Add(pubKeys[i].impl.p, hashes[i], BLSScheme::Basic);
    }
    return check.Verify();
}

bool CBLSSignature::VerifySecureAggregated(Span<CBLSPublicKey> pks, const uint256& hash, const BLSScheme scheme) const
{
    if (pks.empty()) {
        return false;
    }
    if (!IsKeyOrSignature(impl.q)) {
        return false;
    }

    std::vector<PublicKeyBytes> pk_bytes;
    pk_bytes.reserve(pks.size());
    for (const auto& pk : pks) {
        if (!IsKeyOrSignature(pk.impl.p)) {
            return false;
        }
        pk_bytes.push_back(pk.impl.SerializeToArray(scheme == BLSScheme::Legacy));
    }
    const std::vector<blst_scalar> factors{SecureAggregationFactors(pk_bytes)};

    blst_p1 aggregated{};
    for (size_t i = 0; i < pks.size(); i++) {
        blst_p1 multiple;
        Multiply(multiple, pks[i].impl.p, factors[i]);
        blst_p1_add_or_double(&aggregated, &aggregated, &multiple);
    }
    if (blst_p1_is_inf(&aggregated)) {
        return false;
    }

    SignatureCheck check{impl.q};
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
        Multiply(multiple, sigs[i].impl.q, weight);
        blst_p2_add_or_double(&recovered, &recovered, &multiple);
    }
    impl.q = recovered;

    fValid = true;
    return true;
}

#ifndef BUILD_BITCOIN_INTERNAL

static std::once_flag init_flag;
static mt_pooled_secure_allocator<uint8_t>* secure_allocator_instance;
static void create_secure_allocator()
{
    // make sure LockedPoolManager is initialized first (ensures destruction order)
    LockedPoolManager::Instance();

    // static variable in function scope ensures it's initialized when first accessed
    // and destroyed before LockedPoolManager
    static mt_pooled_secure_allocator<uint8_t> a(sizeof(blst_p2) + sizeof(size_t));
    secure_allocator_instance = &a;
}

static mt_pooled_secure_allocator<uint8_t>& get_secure_allocator()
{
    std::call_once(init_flag, create_secure_allocator);
    return *secure_allocator_instance;
}

static void* secure_allocate(size_t n)
{
    uint8_t* ptr = get_secure_allocator().allocate(n + sizeof(size_t));
    *reinterpret_cast<size_t*>(ptr) = n;
    return ptr + sizeof(size_t);
}

static void secure_free(void* p)
{
    if (p == nullptr) {
        return;
    }

    uint8_t* ptr = reinterpret_cast<uint8_t*>(p) - sizeof(size_t);
    size_t n = *reinterpret_cast<size_t*>(ptr);
    return get_secure_allocator().deallocate(ptr, n + sizeof(size_t));
}
#endif

bool BLSInit()
{
#ifndef BUILD_BITCOIN_INTERNAL
    bls::BLS::SetSecureAllocator(secure_allocate, secure_free);
#endif
    return true;
}
