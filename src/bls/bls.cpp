// Copyright (c) 2018-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls/bls.h>

#include <bls/bls_legacy.h>
#include <crypto/sha256.h>
#include <random.h>
#include <support/cleanse.h>

#ifndef BUILD_BITCOIN_INTERNAL
#include <support/allocators/mt_pooled_secure.h>
#endif

#include <mimalloc.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <mutex>
#include <new>
#include <set>

namespace bls {
    std::atomic<bool> bls_legacy_scheme = std::atomic<bool>(true);
}

namespace {

// --- secure memory for secret scalars ---------------------------------------
//
// Secret scalars live in memory that is wiped when released. Until BLSInit()
// runs, and always in the consensus and kernel libraries, that memory comes
// from mimalloc in secure mode; BLSInit() switches to the locked pool shared
// with the rest of the node. The choice is latched at the first allocation
// so that no block is ever released through the wrong allocator.

enum class SecureBackend : uint8_t { UNSET, MIMALLOC, LOCKED_POOL };
std::atomic<SecureBackend> g_secureBackend{SecureBackend::UNSET};

#ifndef BUILD_BITCOIN_INTERNAL
std::once_flag g_poolInitFlag;
mt_pooled_secure_allocator<uint8_t>* g_poolInstance{nullptr};

void CreateLockedPool()
{
    // make sure LockedPoolManager is initialized first (ensures destruction order)
    LockedPoolManager::Instance();

    // static variable in function scope ensures it's initialized when first accessed
    // and destroyed before LockedPoolManager
    static mt_pooled_secure_allocator<uint8_t> pool(sizeof(blst_scalar));
    g_poolInstance = &pool;
}

mt_pooled_secure_allocator<uint8_t>& LockedPool()
{
    std::call_once(g_poolInitFlag, CreateLockedPool);
    return *g_poolInstance;
}
#endif

SecureBackend ActiveSecureBackend()
{
    SecureBackend backend = g_secureBackend.load();
    if (backend == SecureBackend::UNSET) {
        g_secureBackend.compare_exchange_strong(backend, SecureBackend::MIMALLOC);
        backend = g_secureBackend.load();
    }
    return backend;
}

void* SecureAlloc(size_t n)
{
#ifndef BUILD_BITCOIN_INTERNAL
    if (ActiveSecureBackend() == SecureBackend::LOCKED_POOL) {
        return LockedPool().allocate(n);
    }
#endif
    ActiveSecureBackend();
    return mi_malloc(n);
}

void SecureFree(void* p, size_t n)
{
    memory_cleanse(p, n);
#ifndef BUILD_BITCOIN_INTERNAL
    if (ActiveSecureBackend() == SecureBackend::LOCKED_POOL) {
        LockedPool().deallocate(static_cast<uint8_t*>(p), n);
        return;
    }
#endif
    mi_free(p);
}

// --- constants ---------------------------------------------------------------

// BLS12-381 group order r, big-endian
const uint8_t ORDER_BE[32] = {
    0x73, 0xed, 0xa7, 0x53, 0x29, 0x9d, 0x7d, 0x48, 0x33, 0x39, 0xd8, 0x08,
    0x09, 0xa1, 0xd8, 0x05, 0x53, 0xbd, 0xa4, 0x02, 0xff, 0xfe, 0x5b, 0xfe,
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01};

// Ciphersuite of the basic scheme
const char BASIC_SCHEME_DST[] = "BLS_SIG_BLS12381G2_XMD:SHA-256_SSWU_RO_NUL_";

// --- scalars -----------------------------------------------------------------

//! Stack storage that is wiped on scope exit
template <typename T>
struct Wiped {
    T v{};
    ~Wiped() { memory_cleanse(&v, sizeof(v)); }
};

bool ScalarIsZero(const blst_scalar& s)
{
    static const blst_scalar zero{};
    return memcmp(&s, &zero, sizeof(s)) == 0;
}

//! A threshold id as a scalar, reduced modulo the group order
void ScalarFromId(blst_scalar& out, const uint256& id)
{
    blst_scalar_from_be_bytes(&out, id.begin(), id.size());
}

void P1Mult(blst_p1& out, const blst_p1& p, const blst_scalar& k)
{
    Wiped<std::array<uint8_t, 32>> le;
    blst_lendian_from_scalar(le.v.data(), &k);
    blst_p1_mult(&out, &p, le.v.data(), 256);
}

void P2Mult(blst_p2& out, const blst_p2& p, const blst_scalar& k)
{
    Wiped<std::array<uint8_t, 32>> le;
    blst_lendian_from_scalar(le.v.data(), &k);
    blst_p2_mult(&out, &p, le.v.data(), 256);
}

// --- points ------------------------------------------------------------------

//! Validity in the basic scheme: a subgroup element or the point at infinity
bool InG1OrInf(const blst_p1& p) { return blst_p1_is_inf(&p) || blst_p1_in_g1(&p); }
bool InG2OrInf(const blst_p2& p) { return blst_p2_is_inf(&p) || blst_p2_in_g2(&p); }

void HashToG2(blst_p2& out, const uint256& hash, bool legacy)
{
    if (legacy) {
        bls_legacy::HashToG2(out, hash);
    } else {
        blst_hash_to_g2(&out, hash.begin(), hash.size(),
                        reinterpret_cast<const uint8_t*>(BASIC_SCHEME_DST), sizeof(BASIC_SCHEME_DST) - 1,
                        nullptr, 0);
    }
}

// Decoding of the basic (IETF) compressed encodings. Only the canonical
// infinity encoding is accepted; a non-infinity encoding must have the
// compression bit set, may not encode a zero x coordinate, and must decode
// to a subgroup element.
bool DecodeBasicG1(blst_p1& out, Span<const uint8_t> in)
{
    const bool tailZero = std::all_of(in.begin() + 1, in.end(), [](uint8_t c) { return c == 0; });
    if ((in[0] & 0xc0) == 0xc0) {
        if (in[0] != 0xc0 || !tailZero) return false;
        memset(&out, 0, sizeof(out));
        return true;
    }
    if ((in[0] & 0xc0) != 0x80 || tailZero) return false;
    blst_p1_affine a;
    if (blst_p1_uncompress(&a, in.data()) != BLST_SUCCESS) return false;
    blst_p1_from_affine(&out, &a);
    return InG1OrInf(out);
}

bool DecodeBasicG2(blst_p2& out, Span<const uint8_t> in)
{
    if ((in[48] & 0xe0) != 0) return false;
    const bool zerosOnly = (in[0] & 0x1f) == 0 && std::all_of(in.begin() + 1, in.end(), [](uint8_t c) { return c == 0; });
    if ((in[0] & 0xc0) == 0xc0) {
        if (in[0] != 0xc0 || !zerosOnly) return false;
        memset(&out, 0, sizeof(out));
        return true;
    }
    if ((in[0] & 0xc0) != 0x80 || zerosOnly) return false;
    blst_p2_affine a;
    if (blst_p2_uncompress(&a, in.data()) != BLST_SUCCESS) return false;
    blst_p2_from_affine(&out, &a);
    return InG2OrInf(out);
}

// --- pairing product ---------------------------------------------------------

// Checks e(-G, sig) * prod e(pk_i, h_i) == 1. A pair with the point at
// infinity on either side contributes the identity (relic's semantics, which
// is why blst_core_verify is not used).
bool VerifyPairing(const blst_p2& sig, const std::vector<const blst_p1*>& pks, const std::vector<blst_p2>& hashes)
{
    assert(pks.size() == hashes.size());
    std::vector<blst_p1_affine> g1(pks.size() + 1);
    std::vector<blst_p2_affine> g2(pks.size() + 1);

    blst_p1 negGenerator = *blst_p1_generator();
    blst_p1_cneg(&negGenerator, true);
    blst_p1_to_affine(&g1[0], &negGenerator);
    blst_p2_to_affine(&g2[0], &sig);
    for (size_t i = 0; i < pks.size(); i++) {
        blst_p1_to_affine(&g1[i + 1], pks[i]);
        blst_p2_to_affine(&g2[i + 1], &hashes[i]);
    }

    blst_fp12 acc = *blst_fp12_one();
    for (size_t i = 0; i < g1.size(); i++) {
        if (blst_p1_affine_is_inf(&g1[i]) || blst_p2_affine_is_inf(&g2[i])) {
            continue;
        }
        blst_fp12 term;
        blst_miller_loop(&term, &g2[i], &g1[i]);
        blst_fp12_mul(&acc, &acc, &term);
    }
    blst_final_exp(&acc, &acc);
    return blst_fp12_is_one(&acc);
}

// --- secure aggregation ------------------------------------------------------

// Coefficients of the secure aggregation: the public keys are ordered by
// their encoding under the given scheme, and coefficient i is
// SHA256(i || SHA256(encodings)) reduced modulo the group order.
struct SecureAggregationOrder {
    std::vector<size_t> index;        // input index of each sorted position
    std::vector<blst_scalar> coeffs;  // coefficient of each sorted position
};

SecureAggregationOrder SecureAggregationCoefficients(const std::vector<std::array<uint8_t, BLS_CURVE_PUBKEY_SIZE>>& encodings)
{
    const size_t n = encodings.size();
    SecureAggregationOrder ret;
    ret.index.resize(n);
    for (size_t i = 0; i < n; i++) ret.index[i] = i;
    std::sort(ret.index.begin(), ret.index.end(), [&](size_t a, size_t b) {
        return memcmp(encodings[a].data(), encodings[b].data(), BLS_CURVE_PUBKEY_SIZE) < 0;
    });

    CSHA256 all;
    for (size_t i : ret.index) {
        all.Write(encodings[i].data(), BLS_CURVE_PUBKEY_SIZE);
    }
    uint8_t pkHash[CSHA256::OUTPUT_SIZE];
    all.Finalize(pkHash);

    ret.coeffs.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t prefix[4] = {uint8_t(i >> 24), uint8_t(i >> 16), uint8_t(i >> 8), uint8_t(i)};
        uint8_t digest[CSHA256::OUTPUT_SIZE];
        CSHA256().Write(prefix, sizeof(prefix)).Write(pkHash, sizeof(pkHash)).Finalize(digest);
        blst_scalar_from_be_bytes(&ret.coeffs[i], digest, sizeof(digest));
    }
    return ret;
}

// --- polynomials over the scalar field ---------------------------------------

template <typename T>
struct PolyOps;

template <>
struct PolyOps<blst_scalar> {
    static void Mul(blst_scalar& out, const blst_scalar& a, const blst_scalar& x) { blst_sk_mul_n_check(&out, &a, &x); }
    static void Add(blst_scalar& out, const blst_scalar& a, const blst_scalar& b) { blst_sk_add_n_check(&out, &a, &b); }
};

template <>
struct PolyOps<blst_p1> {
    static void Mul(blst_p1& out, const blst_p1& a, const blst_scalar& x) { P1Mult(out, a, x); }
    static void Add(blst_p1& out, const blst_p1& a, const blst_p1& b) { blst_p1_add_or_double(&out, &a, &b); }
};

template <>
struct PolyOps<blst_p2> {
    static void Mul(blst_p2& out, const blst_p2& a, const blst_scalar& x) { P2Mult(out, a, x); }
    static void Add(blst_p2& out, const blst_p2& a, const blst_p2& b) { blst_p2_add_or_double(&out, &a, &b); }
};

//! Horner evaluation of the polynomial with the given coefficients at the
//! point id. Requires at least two coefficients.
template <typename T, typename GetCoefficient>
bool PolyEvaluate(T& out, size_t n, GetCoefficient coefficient, const uint256& id)
{
    if (n < 2) return false;
    Wiped<blst_scalar> x;
    ScalarFromId(x.v, id);
    Wiped<T> y, term;
    y.v = coefficient(n - 1);
    for (size_t i = n - 1; i-- > 0;) {
        PolyOps<T>::Mul(term.v, y.v, x.v);
        PolyOps<T>::Add(y.v, term.v, coefficient(i));
    }
    out = y.v;
    return true;
}

//! Lagrange interpolation at zero of the polynomial through the given shares.
//! Fails for fewer than two shares, a zero id or duplicate ids.
template <typename T, typename GetShare>
bool LagrangeInterpolate(T& out, size_t k, GetShare share, const std::vector<uint256>& ids)
{
    if (k < 2 || ids.size() != k) return false;
    std::vector<blst_scalar> x(k);
    for (size_t i = 0; i < k; i++) {
        ScalarFromId(x[i], ids[i]);
    }

    // delta_i = prod_j x_j / (x_i * prod_{j != i} (x_j - x_i))
    blst_scalar numerator = x[0];
    for (size_t i = 1; i < k; i++) {
        blst_sk_mul_n_check(&numerator, &numerator, &x[i]);
    }
    if (ScalarIsZero(numerator)) return false;

    T acc{};
    for (size_t i = 0; i < k; i++) {
        blst_scalar denominator = x[i];
        for (size_t j = 0; j < k; j++) {
            if (j == i) continue;
            blst_scalar diff;
            blst_sk_sub_n_check(&diff, &x[j], &x[i]);
            if (ScalarIsZero(diff)) return false;
            blst_sk_mul_n_check(&denominator, &denominator, &diff);
        }
        blst_scalar delta;
        blst_sk_inverse(&delta, &denominator);
        blst_sk_mul_n_check(&delta, &numerator, &delta);
        T term;
        PolyOps<T>::Mul(term, share(i), delta);
        PolyOps<T>::Add(acc, acc, term);
    }
    out = acc;
    return true;
}

} // namespace

// --- CBLSSecretScalar --------------------------------------------------------

CBLSSecretScalar::CBLSSecretScalar(const CBLSSecretScalar& o)
{
    if (o.m_data != nullptr) {
        Mutable() = *o.m_data;
    }
}

CBLSSecretScalar& CBLSSecretScalar::operator=(const CBLSSecretScalar& o)
{
    if (this != &o) {
        if (o.m_data != nullptr) {
            Mutable() = *o.m_data;
        } else if (m_data != nullptr) {
            SecureFree(m_data, sizeof(blst_scalar));
            m_data = nullptr;
        }
    }
    return *this;
}

CBLSSecretScalar::~CBLSSecretScalar()
{
    if (m_data != nullptr) {
        SecureFree(m_data, sizeof(blst_scalar));
    }
}

const blst_scalar& CBLSSecretScalar::Get() const
{
    static const blst_scalar zero{};
    return m_data != nullptr ? *m_data : zero;
}

blst_scalar& CBLSSecretScalar::Mutable()
{
    if (m_data == nullptr) {
        m_data = static_cast<blst_scalar*>(SecureAlloc(sizeof(blst_scalar)));
        if (m_data == nullptr) throw std::bad_alloc();
        memset(m_data, 0, sizeof(blst_scalar));
    }
    return *m_data;
}

bool CBLSSecretScalar::IsZero() const
{
    return m_data == nullptr || ScalarIsZero(*m_data);
}

bool CBLSSecretScalar::operator==(const CBLSSecretScalar& o) const
{
    return memcmp(&Get(), &o.Get(), sizeof(blst_scalar)) == 0;
}

// --- CBLSId ------------------------------------------------------------------

CBLSId::CBLSId(const uint256& nHash) : CBLSWrapper<uint256, BLS_CURVE_ID_SIZE, CBLSId>()
{
    impl = nHash;
    fValid = true;
    cachedHash.SetNull();
}

// --- CBLSSecretKey -----------------------------------------------------------

bool CBLSSecretKey::DecodeImpl(Span<const uint8_t> in, bool modOrder, CBLSSecretScalar& out)
{
    if (modOrder) {
        blst_scalar_from_be_bytes(&out.Mutable(), in.data(), in.size());
        return true;
    }
    // values above the group order are rejected, the order itself is not
    if (memcmp(in.data(), ORDER_BE, sizeof(ORDER_BE)) > 0) {
        return false;
    }
    blst_scalar_from_bendian(&out.Mutable(), in.data());
    return true;
}

void CBLSSecretKey::EncodeImpl(const CBLSSecretScalar& in, bool /*specificLegacyScheme*/, uint8_t* out)
{
    blst_bendian_from_scalar(out, &in.Get());
}

void CBLSSecretKey::AggregateInsecure(const CBLSSecretKey& o)
{
    assert(IsValid() && o.IsValid());
    blst_sk_add_n_check(&impl.Mutable(), &impl.Get(), &o.impl.Get());
    cachedHash.SetNull();
}

CBLSSecretKey CBLSSecretKey::AggregateInsecure(Span<CBLSSecretKey> sks)
{
    if (sks.empty()) {
        return {};
    }

    CBLSSecretKey ret;
    blst_scalar& sum = ret.impl.Mutable();
    for (const auto& sk : sks) {
        blst_sk_add_n_check(&sum, &sum, &sk.impl.Get());
    }
    ret.fValid = true;
    ret.cachedHash.SetNull();
    return ret;
}

#ifndef BUILD_BITCOIN_INTERNAL
void CBLSSecretKey::MakeNewKey()
{
    unsigned char buf[SerSize];
    do {
        GetStrongRandBytes({buf, sizeof(buf)});
        SetBytes(buf, false);
    } while (!IsValid());
    memory_cleanse(buf, sizeof(buf));
}
#endif

bool CBLSSecretKey::SecretKeyShare(Span<CBLSSecretKey> msk, const CBLSId& _id)
{
    fValid = false;
    cachedHash.SetNull();

    if (!_id.IsValid()) {
        return false;
    }
    for (const CBLSSecretKey& sk : msk) {
        if (!sk.IsValid()) {
            return false;
        }
    }

    Wiped<blst_scalar> share;
    if (!PolyEvaluate<blst_scalar>(share.v, msk.size(), [&](size_t i) -> const blst_scalar& { return msk[i].impl.Get(); }, _id.impl)) {
        return false;
    }
    impl.Mutable() = share.v;
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
    blst_sk_to_pk_in_g1(&pubKey.impl, &impl.Get());
    pubKey.fValid = true;
    pubKey.cachedHash.SetNull();
    return pubKey;
}

CBLSSignature CBLSSecretKey::Sign(const uint256& hash, const bool specificLegacyScheme) const
{
    if (!IsValid()) {
        return {};
    }

    CBLSSignature sigRet;
    blst_p2 hashPoint;
    HashToG2(hashPoint, hash, specificLegacyScheme);
    blst_sign_pk_in_g1(&sigRet.impl, &hashPoint, &impl.Get());
    sigRet.fValid = true;
    sigRet.cachedHash.SetNull();
    return sigRet;
}

// --- CBLSPublicKey -----------------------------------------------------------

bool CBLSPublicKey::DecodeImpl(Span<const uint8_t> in, bool specificLegacyScheme, blst_p1& out)
{
    return specificLegacyScheme ? bls_legacy::DecodeG1(out, in) : DecodeBasicG1(out, in);
}

void CBLSPublicKey::EncodeImpl(const blst_p1& in, bool specificLegacyScheme, uint8_t* out)
{
    if (specificLegacyScheme) {
        bls_legacy::EncodeG1({out, BLS_CURVE_PUBKEY_SIZE}, in);
    } else {
        blst_p1_compress(out, &in);
    }
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
    ret.cachedHash.SetNull();
    return ret;
}

bool CBLSPublicKey::PublicKeyShare(Span<CBLSPublicKey> mpk, const CBLSId& _id)
{
    fValid = false;
    cachedHash.SetNull();

    if (!_id.IsValid()) {
        return false;
    }
    for (const CBLSPublicKey& pk : mpk) {
        if (!pk.IsValid()) {
            return false;
        }
    }

    if (!PolyEvaluate<blst_p1>(impl, mpk.size(), [&](size_t i) -> const blst_p1& { return mpk[i].impl; }, _id.impl)) {
        return false;
    }
    fValid = true;
    cachedHash.SetNull();
    return true;
}

bool CBLSPublicKey::DHKeyExchange(const CBLSSecretKey& sk, const CBLSPublicKey& pk)
{
    fValid = false;
    cachedHash.SetNull();

    if (!sk.IsValid() || !pk.IsValid()) {
        return false;
    }
    P1Mult(impl, pk.impl, sk.impl.Get());
    fValid = true;
    cachedHash.SetNull();
    return true;
}

// --- CBLSSignature -----------------------------------------------------------

bool CBLSSignature::DecodeImpl(Span<const uint8_t> in, bool specificLegacyScheme, blst_p2& out)
{
    return specificLegacyScheme ? bls_legacy::DecodeG2(out, in) : DecodeBasicG2(out, in);
}

void CBLSSignature::EncodeImpl(const blst_p2& in, bool specificLegacyScheme, uint8_t* out)
{
    if (specificLegacyScheme) {
        bls_legacy::EncodeG2({out, BLS_CURVE_SIG_SIZE}, in);
    } else {
        blst_p2_compress(out, &in);
    }
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
    ret.cachedHash.SetNull();
    return ret;
}

CBLSSignature CBLSSignature::AggregateSecure(Span<CBLSSignature> sigs,
                                             Span<CBLSPublicKey> pks,
                                             const uint256& hash)
{
    if (sigs.size() != pks.size() || sigs.empty()) {
        return {};
    }
    const bool legacy = bls::bls_legacy_scheme.load();

    std::vector<std::array<uint8_t, BLS_CURVE_PUBKEY_SIZE>> encodings(pks.size());
    for (size_t i = 0; i < pks.size(); i++) {
        CBLSPublicKey::EncodeImpl(pks[i].impl, legacy, encodings[i].data());
    }
    const SecureAggregationOrder order = SecureAggregationCoefficients(encodings);

    CBLSSignature ret;
    for (size_t i = 0; i < order.index.size(); i++) {
        blst_p2 term;
        P2Mult(term, sigs[order.index[i]].impl, order.coeffs[i]);
        blst_p2_add_or_double(&ret.impl, &ret.impl, &term);
    }
    ret.fValid = true;
    ret.cachedHash.SetNull();
    return ret;
}

void CBLSSignature::SubInsecure(const CBLSSignature& o)
{
    assert(IsValid() && o.IsValid());
    blst_p2 neg = o.impl;
    blst_p2_cneg(&neg, true);
    blst_p2_add_or_double(&impl, &impl, &neg);
    cachedHash.SetNull();
}

bool CBLSSignature::VerifyInsecure(const CBLSPublicKey& pubKey, const uint256& hash, const bool specificLegacyScheme) const
{
    if (!IsValid() || !pubKey.IsValid()) {
        return false;
    }
    // The basic scheme requires subgroup elements; the legacy scheme verifies
    // whatever was decoded.
    if (!specificLegacyScheme && (!InG1OrInf(pubKey.impl) || !InG2OrInf(impl))) {
        return false;
    }
    std::vector<blst_p2> hashes(1);
    HashToG2(hashes[0], hash, specificLegacyScheme);
    return VerifyPairing(impl, {&pubKey.impl}, hashes);
}

bool CBLSSignature::VerifyInsecure(const CBLSPublicKey& pubKey, const uint256& hash) const
{
    return VerifyInsecure(pubKey, hash, bls::bls_legacy_scheme.load());
}

bool CBLSSignature::VerifyInsecureAggregated(Span<CBLSPublicKey> pubKeys, Span<uint256> hashes) const
{
    if (!IsValid()) {
        return false;
    }
    assert(!pubKeys.empty() && !hashes.empty() && pubKeys.size() == hashes.size());
    const bool legacy = bls::bls_legacy_scheme.load();

    std::vector<const blst_p1*> pks;
    pks.reserve(pubKeys.size());
    for (const auto& pk : pubKeys) {
        if (!pk.IsValid()) {
            return false;
        }
        pks.push_back(&pk.impl);
    }

    if (!legacy) {
        // the basic scheme needs distinct messages and subgroup elements
        const std::set<uint256> distinct(hashes.begin(), hashes.end());
        if (distinct.size() != hashes.size()) {
            return false;
        }
        if (!InG2OrInf(impl)) {
            return false;
        }
        for (const blst_p1* pk : pks) {
            if (!InG1OrInf(*pk)) {
                return false;
            }
        }
    }

    std::vector<blst_p2> hashPoints(hashes.size());
    for (size_t i = 0; i < hashes.size(); i++) {
        HashToG2(hashPoints[i], hashes[i], legacy);
    }
    return VerifyPairing(impl, pks, hashPoints);
}

bool CBLSSignature::VerifySecureAggregated(Span<CBLSPublicKey> pks, const uint256& hash) const
{
    if (pks.empty()) {
        return false;
    }
    const bool legacy = bls::bls_legacy_scheme.load();

    std::vector<std::array<uint8_t, BLS_CURVE_PUBKEY_SIZE>> encodings(pks.size());
    for (size_t i = 0; i < pks.size(); i++) {
        CBLSPublicKey::EncodeImpl(pks[i].impl, legacy, encodings[i].data());
    }
    const SecureAggregationOrder order = SecureAggregationCoefficients(encodings);

    blst_p1 aggregate{};
    for (size_t i = 0; i < order.index.size(); i++) {
        const blst_p1& pk = pks[order.index[i]].impl;
        // the basic scheme only accepts subgroup elements
        if (!legacy && !InG1OrInf(pk)) {
            return false;
        }
        blst_p1 term;
        P1Mult(term, pk, order.coeffs[i]);
        blst_p1_add_or_double(&aggregate, &aggregate, &term);
    }
    if (!legacy && !InG2OrInf(impl)) {
        return false;
    }

    std::vector<blst_p2> hashes(1);
    HashToG2(hashes[0], hash, legacy);
    return VerifyPairing(impl, {&aggregate}, hashes);
}

bool CBLSSignature::Recover(Span<CBLSSignature> sigs, Span<CBLSId> ids)
{
    fValid = false;
    cachedHash.SetNull();

    if (sigs.empty() || ids.empty() || sigs.size() != ids.size()) {
        return false;
    }

    std::vector<uint256> idValues;
    idValues.reserve(ids.size());
    for (size_t i = 0; i < sigs.size(); i++) {
        if (!sigs[i].IsValid() || !ids[i].IsValid()) {
            return false;
        }
        idValues.push_back(ids[i].impl);
    }

    if (!LagrangeInterpolate<blst_p2>(impl, sigs.size(), [&](size_t i) -> const blst_p2& { return sigs[i].impl; }, idValues)) {
        return false;
    }
    fValid = true;
    cachedHash.SetNull();
    return true;
}

// --- initialization ----------------------------------------------------------

bool BLSInit()
{
#ifndef BUILD_BITCOIN_INTERNAL
    SecureBackend expected = SecureBackend::UNSET;
    g_secureBackend.compare_exchange_strong(expected, SecureBackend::LOCKED_POOL);
#endif
    return true;
}
