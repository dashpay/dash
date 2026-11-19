// Copyright (c) 2021 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls/legacy.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace {

// sqrt(-3) mod p, big-endian, zero-padded to 48 bytes. Value taken from the
// relic implementation's B12_P381_S3 constant; validated by
// s3^2 == -3 (mod p).
const uint8_t S3_BE[48] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xbe, 0x32, 0xce, 0x5f,
    0xbe, 0xed, 0x9c, 0xa3, 0x74, 0xd3, 0x8c, 0x0e, 0xd4, 0x1e, 0xef, 0xd5,
    0xbb, 0x67, 0x52, 0x77, 0xcd, 0xf1, 0x2d, 0x11, 0xbc, 0x2f, 0xb0, 0x26,
    0xc4, 0x14, 0x00, 0x04, 0x5c, 0x03, 0xff, 0xff, 0xff, 0xfd, 0xff, 0xfd};

// (sqrt(-3) - 1) / 2 mod p (relic's B12_P381_S32).
const uint8_t S32_BE[48] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5f, 0x19, 0x67, 0x2f,
    0xdf, 0x76, 0xce, 0x51, 0xba, 0x69, 0xc6, 0x07, 0x6a, 0x0f, 0x77, 0xea,
    0xdd, 0xb3, 0xa9, 0x3b, 0xe6, 0xf8, 0x96, 0x88, 0xde, 0x17, 0xd8, 0x13,
    0x62, 0x0a, 0x00, 0x02, 0x2e, 0x01, 0xff, 0xff, 0xff, 0xfe, 0xff, 0xfe};

// Effective cofactor h_eff for BLS12-381 G2 (RFC 9380 section 8.8.2),
// big-endian, 80 bytes / 636 bits. Scalar multiplication by h_eff is
// equivalent to the psi-endomorphism-based cofactor clearing the relic
// implementation performed (verified empirically point-by-point).
const uint8_t H_EFF_BE[80] = {
    0x0b, 0xc6, 0x9f, 0x08, 0xf2, 0xee, 0x75, 0xb3, 0x58, 0x4c, 0x6a, 0x0e,
    0xa9, 0x1b, 0x35, 0x28, 0x88, 0xe2, 0xa8, 0xe9, 0x14, 0x5a, 0xd7, 0x68,
    0x99, 0x86, 0xff, 0x03, 0x15, 0x08, 0xff, 0xe1, 0x32, 0x9c, 0x2f, 0x17,
    0x87, 0x31, 0xdb, 0x95, 0x6d, 0x82, 0xbf, 0x01, 0x5d, 0x12, 0x12, 0xb0,
    0x2e, 0xc0, 0xec, 0x69, 0xd7, 0x47, 0x7c, 0x1a, 0xe9, 0x54, 0xcb, 0xc0,
    0x66, 0x89, 0xf6, 0xa3, 0x59, 0x89, 0x4c, 0x0a, 0xde, 0xbb, 0xf6, 0xb4,
    0xe8, 0x02, 0x00, 0x05, 0xaa, 0xa9, 0x55, 0x51};

void FpOne(blst_fp& out)
{
    uint8_t be[48] = {0};
    be[47] = 1;
    blst_fp_from_bendian(&out, be);
}

void FpFour(blst_fp& out)
{
    uint8_t be[48] = {0};
    be[47] = 4;
    blst_fp_from_bendian(&out, be);
}

inline void FpZero(blst_fp& out) { memset(&out, 0x00, sizeof(blst_fp)); }

bool FpIsZero(const blst_fp& a)
{
    uint8_t be[48];
    blst_bendian_from_fp(be, &a);
    for (int i = 0; i < 48; i++)
        if (be[i] != 0) return false;
    return true;
}

bool Fp2IsZero(const blst_fp2& a)
{
    return FpIsZero(a.fp[0]) && FpIsZero(a.fp[1]);
}

/**
 * Based on the rust implementation of pairings, zkcrypto/pairing.
 * The algorithm is Shallue–van de Woestijne encoding from
 * Section 3 of "Indifferentiable Hashing to Barreto–Naehrig Curves"
 * from Fouque-Tibouchi: <https://www.di.ens.fr/~fouque/pub/latincrypt12.pdf>
 * @param[out] p            - the encoded point.
 * @param[in] t             - the field element to encode.
 * Returns false when the result is the point at infinity (t == 0).
 */
bool ep2_sw_encode(blst_p2_affine& p, const blst_fp2& t)
{
    if (Fp2IsZero(t)) {
        // Maps t=0 to the point at infinity.
        return false;
    }

    // parity of t: compare the canonical bytes of t.c1 against those of
    // (-t).c1
    blst_fp2 nt = t;
    blst_fp2_cneg(&nt, &nt, true);
    uint8_t buf0[48], buf1[48];
    blst_bendian_from_fp(buf0, &t.fp[1]);
    blst_bendian_from_fp(buf1, &nt.fp[1]);
    const bool parity = memcmp(buf0, buf1, 48) > 0;

    blst_fp one, four;
    FpOne(one);
    FpFour(four);
    blst_fp2 b;
    b.fp[0] = four;
    b.fp[1] = four;

    // w = t^2 + b + 1
    blst_fp2 w;
    blst_fp2_sqr(&w, &t);
    blst_fp2_add(&w, &w, &b);
    blst_fp_add(&w.fp[0], &w.fp[0], &one);

    if (Fp2IsZero(w)) {
        p = *blst_p2_affine_generator();
        if (parity) {
            blst_fp2_cneg(&p.y, &p.y, true);
        }
        return true;
    }

    blst_fp2 s3p, s32p;
    blst_fp_from_bendian(&s3p.fp[0], S3_BE);
    FpZero(s3p.fp[1]);
    blst_fp_from_bendian(&s32p.fp[0], S32_BE);
    FpZero(s32p.fp[1]);

    // w = sqrt(-3) * t / (t^2 + b + 1)
    blst_fp2_inverse(&w, &w);
    blst_fp2_mul(&w, &w, &s3p);
    blst_fp2_mul(&w, &w, &t);

    // x1 = -wt + (sqrt(-3) - 1) / 2
    blst_fp2 x1;
    x1 = w;
    blst_fp2_cneg(&x1, &x1, true);
    blst_fp2_mul(&x1, &x1, &t);
    blst_fp2_add(&x1, &x1, &s32p);

    // x2 = -x1 - 1
    blst_fp2 x2 = x1;
    blst_fp2_cneg(&x2, &x2, true);
    blst_fp_sub(&x2.fp[0], &x2.fp[0], &one);

    // x3 = 1/w^2 + 1
    blst_fp2 x3;
    blst_fp2_sqr(&x3, &w);
    blst_fp2_inverse(&x3, &x3);
    blst_fp_add(&x3.fp[0], &x3.fp[0], &one);

    const auto rhs = [&b](blst_fp2& out, const blst_fp2& x) {
        blst_fp2_sqr(&out, &x);
        blst_fp2_mul(&out, &out, &x);
        blst_fp2_add(&out, &out, &b);
    };

    blst_fp2 r, y;
    rhs(r, x1);
    const int Xx1 = blst_fp2_sqrt(&y, &r) ? 1 : -1;
    rhs(r, x2);
    const int Xx2 = blst_fp2_sqrt(&y, &r) ? 1 : -1;

    // This formula computes which index to use, in constant time
    // without conditional branches. It's taken from the paper. 3 is
    // added, because the % operator in c can be negative.
    const int index = ((((Xx1 - 1) * Xx2) % 3) + 3) % 3;

    p.x = (index == 0) ? x1 : (index == 1) ? x2 : x3;
    rhs(r, p.x);
    const bool haveRoot = blst_fp2_sqrt(&p.y, &r);
    (void)haveRoot;
    assert(haveRoot);  // guaranteed by the SW construction

    // Normalize the sign of y against the parity of t
    blst_fp2 ny = p.y;
    blst_fp2_cneg(&ny, &ny, true);
    blst_bendian_from_fp(buf0, &p.y.fp[1]);
    blst_bendian_from_fp(buf1, &ny.fp[1]);
    if ((memcmp(buf0, buf1, 48) > 0) != parity) {
        p.y = ny;
    }
    return true;
}

// Reduce a 64-byte big-endian value mod p into an fp element, matching
// relic's bn_read_bin + fp_prime_conv: split as hi*2^256 + lo with both
// halves < 2^256 < p.
void FpFromBE64Wide(blst_fp& out, const uint8_t* be64)
{
    uint8_t padded[48] = {0};
    blst_fp hi, lo, c256;

    memcpy(padded + 16, be64, 32);
    blst_fp_from_bendian(&hi, padded);
    memcpy(padded + 16, be64 + 32, 32);
    blst_fp_from_bendian(&lo, padded);

    // 2^256 mod p == 2^256 (p is 381 bits)
    memset(padded, 0, 48);
    padded[15] = 0x01;
    blst_fp_from_bendian(&c256, padded);

    blst_fp_mul(&out, &hi, &c256);
    blst_fp_add(&out, &out, &lo);
}
// The legacy codec reproduces the byte-level behavior of the relic-based
// implementation exactly, including its handling of malformed input. relic
// was built without CHECK, so its internal errors did not throw: an
// out-of-range field element left the coordinate at zero, and a failed
// square root left y holding its pre-existing raw contents (0, or a raw
// limb 1 when the sign bit had been stored). All of it is consensus-visible
// through re-serialization and is pinned by test/data/bls_reference_vectors.txt.

// BLS12-381 base field prime p, big-endian.
static const uint8_t P_BE[48] = {
    0x1a, 0x01, 0x11, 0xea, 0x39, 0x7f, 0xe6, 0x9a, 0x4b, 0x1b, 0xa7, 0xb6,
    0x43, 0x4b, 0xac, 0xd7, 0x64, 0x77, 0x4b, 0x84, 0xf3, 0x85, 0x12, 0xbf,
    0x67, 0x30, 0xd2, 0xa0, 0xf6, 0xb0, 0xf6, 0x24, 0x1e, 0xab, 0xff, 0xfe,
    0xb1, 0x53, 0xff, 0xff, 0xb9, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xaa, 0xab};

// (p - 1) / 2, big-endian. The legacy sign convention is y > (p-1)/2
// ("larger" root), matching relic's ep_pck/ep2_pck for pairing-friendly
// curves.
static const uint8_t HALF_P_BE[48] = {
    0x0d, 0x00, 0x88, 0xf5, 0x1c, 0xbf, 0xf3, 0x4d, 0x25, 0x8d, 0xd3, 0xdb,
    0x21, 0xa5, 0xd6, 0x6b, 0xb2, 0x3b, 0xa5, 0xc2, 0x79, 0xc2, 0x89, 0x5f,
    0xb3, 0x98, 0x69, 0x50, 0x7b, 0x58, 0x7b, 0x12, 0x0f, 0x55, 0xff, 0xff,
    0x58, 0xa9, 0xff, 0xff, 0xdc, 0xff, 0x7f, 0xff, 0xff, 0xff, 0xd5, 0x55};

// (2^384)^-1 mod p, big-endian. When relic (built without CHECK) failed to
// find a square root during legacy point decompression with the sign bit
// set, the y coordinate was left holding a raw limb value of 1, which reads
// back from Montgomery form as this constant. Preserved bit-for-bit for
// compatibility with historical consensus data.
static const uint8_t MONT_R_INV_BE[48] = {
    0x14, 0xfe, 0xc7, 0x01, 0xe8, 0xfb, 0x0c, 0xe9, 0xed, 0x5e, 0x64, 0x27,
    0x3c, 0x4f, 0x53, 0x8b, 0x17, 0x97, 0xab, 0x14, 0x58, 0xa8, 0x8d, 0xe9,
    0x34, 0x3e, 0xa9, 0x79, 0x14, 0x95, 0x6d, 0xc8, 0x7f, 0xe1, 0x12, 0x74,
    0xd8, 0x98, 0xfa, 0xfb, 0xf4, 0xd3, 0x82, 0x59, 0x38, 0x0b, 0x48, 0x20};

inline void FpFromBE48(blst_fp& out, const uint8_t* be)
{
    blst_fp_from_bendian(&out, be);
}

// Compares a big-endian 48-byte value against a constant. Returns the
// memcmp sign.
inline int CmpBE48(const uint8_t* a, const uint8_t* b)
{
    return memcmp(a, b, 48);
}

// relic's ep2_upk sign rule (decompression side, with the IETF tie-break):
// sign = c1 > (p-1)/2, but when c1 == 0 the sign of c0 is used instead.
// Note the serialization side (ep2_pck) uses c1 only, with no tie-break.
inline bool Fp2SignUpk(const blst_fp2& y)
{
    uint8_t be[48];
    blst_bendian_from_fp(be, &y.fp[1]);
    bool c1Zero = true;
    for (int i = 0; i < 48; i++)
        if (be[i] != 0) { c1Zero = false; break; }
    if (!c1Zero) return CmpBE48(be, HALF_P_BE) > 0;
    blst_bendian_from_fp(be, &y.fp[0]);
    return CmpBE48(be, HALF_P_BE) > 0;
}

// The "larger root" test used by both the legacy codec (relic's
// ep_pck/ep2_pck convention) and the legacy hash-to-curve: y > (p-1)/2 on
// the canonical representation.
inline bool FpSignGtHalf(const blst_fp& a)
{
    uint8_t be[48];
    blst_bendian_from_fp(be, &a);
    return CmpBE48(be, HALF_P_BE) > 0;
}
} // anon namespace

namespace bls {
static void LegacyDecodeG1(
    blst_p1_affine& a,
    const uint8_t* bytes,
    const bool signBit)
{
    uint8_t xbytes[48];
    memcpy(xbytes, bytes, 48);
    // the relic-based G1 codec erased the top three bits of the first byte
    // unconditionally, before the sign bit was even examined (unlike G2,
    // which only clears the sign bit)
    xbytes[0] &= 0x1f;

    if (CmpBE48(xbytes, P_BE) >= 0) {
        // relic's fp_read_bin error path: coordinate left untouched (zero)
        FpZero(a.x);
    } else {
        FpFromBE48(a.x, xbytes);
    }

    // rhs = x^3 + 4
    blst_fp rhs;
    blst_fp_sqr(&rhs, &a.x);
    blst_fp_mul(&rhs, &rhs, &a.x);
    blst_fp four;
    uint8_t fourBytes[48] = {0};
    fourBytes[47] = 4;
    FpFromBE48(four, fourBytes);
    blst_fp_add(&rhs, &rhs, &four);

    if (blst_fp_sqrt(&a.y, &rhs)) {
        if (FpSignGtHalf(a.y) != signBit) {
            blst_fp_cneg(&a.y, &a.y, true);
        }
    } else {
        // relic's ep_upk failure: y keeps the raw sign-bit marker
        if (signBit) {
            FpFromBE48(a.y, MONT_R_INV_BE);
        } else {
            FpZero(a.y);
        }
    }
}

static void LegacyDecodeG2(
    blst_p2_affine& a,
    const uint8_t* bytes,
    const bool signBit)
{
    uint8_t c0[48], c1[48];
    memcpy(c0, bytes, 48);
    memcpy(c1, bytes + 48, 48);
    if (signBit) {
        c0[0] &= 0x7f;
    }

    if (CmpBE48(c0, P_BE) >= 0) {
        FpZero(a.x.fp[0]);
    } else {
        FpFromBE48(a.x.fp[0], c0);
    }
    if (CmpBE48(c1, P_BE) >= 0) {
        FpZero(a.x.fp[1]);
    } else {
        FpFromBE48(a.x.fp[1], c1);
    }

    // rhs = x^3 + (4, 4)
    blst_fp2 rhs;
    blst_fp2_sqr(&rhs, &a.x);
    blst_fp2_mul(&rhs, &rhs, &a.x);
    blst_fp four;
    uint8_t fourBytes[48] = {0};
    fourBytes[47] = 4;
    FpFromBE48(four, fourBytes);
    blst_fp2 b2;
    b2.fp[0] = four;
    b2.fp[1] = four;
    blst_fp2_add(&rhs, &rhs, &b2);

    if (blst_fp2_sqrt(&a.y, &rhs)) {
        if (Fp2SignUpk(a.y) != signBit) {
            blst_fp2_cneg(&a.y, &a.y, true);
        }
    } else {
        if (signBit) {
            FpFromBE48(a.y.fp[0], MONT_R_INV_BE);
        } else {
            FpZero(a.y.fp[0]);
        }
        FpZero(a.y.fp[1]);
    }
}

bool DecodeLegacy(blst_p1& point, const uint8_t* bytes)
{
    if ((bytes[0] & 0xc0) == 0xc0) {
        // infinity, which is nothing but the two flags
        if (bytes[0] != 0xc0 || !std::all_of(bytes + 1, bytes + 48, [](uint8_t byte) { return byte == 0; })) {
            return false;
        }
        point = {};
        return true;
    }
    blst_p1_affine a;
    LegacyDecodeG1(a, bytes, (bytes[0] & 0x80) != 0);
    // Build the Jacobian point by hand: blst_p1_from_affine maps the
    // all-zero affine pair to infinity, but relic kept degenerate
    // decodes (e.g. x = 0, y = 0) as ordinary z=1 points, which is
    // consensus-visible through re-serialization.
    point.x = a.x;
    point.y = a.y;
    FpOne(point.z);
    return true;
}

void EncodeLegacy(uint8_t* bytes, const blst_p1& point)
{
    memset(bytes, 0, 48);
    if (blst_p1_is_inf(&point)) {
        bytes[0] = 0xc0;
        return;
    }

    blst_p1_affine a;
    blst_p1_to_affine(&a, &point);
    blst_bendian_from_fp(bytes, &a.x);
    if (FpSignGtHalf(a.y)) {
        bytes[0] |= 0x80;
    }
}

bool DecodeLegacy(blst_p2& point, const uint8_t* bytes)
{
    // The relic implementation computed its zeros-only check over a
    // buffer that still contained the first byte unmasked, so for the
    // legacy G2 codec any input with the two top bits set - canonical
    // infinity included - was rejected as non-canonical.
    if ((bytes[0] & 0xc0) == 0xc0) {
        return false;
    }
    blst_p2_affine a;
    LegacyDecodeG2(a, bytes, (bytes[0] & 0x80) != 0);
    // See the G1 legacy path: keep degenerate all-zero decodes as
    // ordinary z=1 points instead of mapping them to infinity.
    point.x = a.x;
    point.y = a.y;
    FpOne(point.z.fp[0]);
    FpZero(point.z.fp[1]);
    return true;
}

void EncodeLegacy(uint8_t* bytes, const blst_p2& point)
{
    memset(bytes, 0, 96);
    if (blst_p2_is_inf(&point)) {
        bytes[0] = 0xc0;
        return;
    }

    blst_p2_affine a;
    blst_p2_to_affine(&a, &point);
    // legacy layout: x.c0 || x.c1 (the modern IETF layout is the swap)
    blst_bendian_from_fp(bytes, &a.x.fp[0]);
    blst_bendian_from_fp(bytes + 48, &a.x.fp[1]);
    // legacy sign: c1 > (p-1)/2, no tie-break (relic's ep2_pck)
    if (FpSignGtHalf(a.y.fp[1])) {
        bytes[0] |= 0x80;
    }
}

void ep2_map_legacy(blst_p2* p, const uint8_t* msg, size_t len)
{
    if (len != 32) {
        throw std::invalid_argument(
            "ep2_map_legacy requires a 32-byte message hash");
    }

    uint8_t input[32 + 8];
    memcpy(input, msg, 32);

    uint8_t t00Bytes[64], t01Bytes[64], t10Bytes[64], t11Bytes[64];

    const auto draw = [&input](uint8_t* out, char i, char j) {
        // tag: "G2_<i>_c<j>" followed by a counter byte
        input[32 + 0] = 0x47;  // G
        input[32 + 1] = 0x32;  // 2
        input[32 + 2] = 0x5f;  // _
        input[32 + 3] = (uint8_t)i;
        input[32 + 4] = 0x5f;  // _
        input[32 + 5] = 0x63;  // c
        input[32 + 6] = (uint8_t)j;
        input[32 + 7] = 0;
        blst_sha256(out, input, 32 + 8);
        input[32 + 7] = 1;
        blst_sha256(out + 32, input, 32 + 8);
    };

    draw(t00Bytes, '0', '0');
    draw(t01Bytes, '0', '1');
    draw(t10Bytes, '1', '0');
    draw(t11Bytes, '1', '1');

    blst_fp2 t0, t1;
    FpFromBE64Wide(t0.fp[0], t00Bytes);
    FpFromBE64Wide(t0.fp[1], t01Bytes);
    FpFromBE64Wide(t1.fp[0], t10Bytes);
    FpFromBE64Wide(t1.fp[1], t11Bytes);

    blst_p2_affine a0, a1;
    const bool has0 = ep2_sw_encode(a0, t0);
    const bool has1 = ep2_sw_encode(a1, t1);

    blst_p2 sum;
    memset(&sum, 0x00, sizeof(blst_p2));  // infinity
    if (has0) {
        blst_p2_from_affine(&sum, &a0);
    }
    if (has1) {
        blst_p2_add_or_double_affine(&sum, &sum, &a1);
    }

    // Clear the cofactor by multiplying with h_eff (equivalent to the
    // psi-based clearing the relic implementation used).
    uint8_t heffLE[sizeof(H_EFF_BE)];
    for (size_t i = 0; i < sizeof(H_EFF_BE); i++) {
        heffLE[i] = H_EFF_BE[sizeof(H_EFF_BE) - 1 - i];
    }
    blst_p2_mult(p, &sum, heffLE, 636);
}
} // namespace bls
