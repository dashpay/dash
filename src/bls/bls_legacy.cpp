// Copyright (c) 2021-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls/bls_legacy.h>

#include <crypto/sha256.h>

#include <cassert>
#include <cstring>

namespace bls_legacy {
namespace {

// BLS12-381 base field prime p, big-endian.
const uint8_t P_BE[48] = {
    0x1a, 0x01, 0x11, 0xea, 0x39, 0x7f, 0xe6, 0x9a, 0x4b, 0x1b, 0xa7, 0xb6,
    0x43, 0x4b, 0xac, 0xd7, 0x64, 0x77, 0x4b, 0x84, 0xf3, 0x85, 0x12, 0xbf,
    0x67, 0x30, 0xd2, 0xa0, 0xf6, 0xb0, 0xf6, 0x24, 0x1e, 0xab, 0xff, 0xfe,
    0xb1, 0x53, 0xff, 0xff, 0xb9, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xaa, 0xab};

// (p - 1) / 2, big-endian. The legacy sign convention picks the "larger"
// root: a coordinate is flagged when it is greater than this value.
const uint8_t HALF_P_BE[48] = {
    0x0d, 0x00, 0x88, 0xf5, 0x1c, 0xbf, 0xf3, 0x4d, 0x25, 0x8d, 0xd3, 0xdb,
    0x21, 0xa5, 0xd6, 0x6b, 0xb2, 0x3b, 0xa5, 0xc2, 0x79, 0xc2, 0x89, 0x5f,
    0xb3, 0x98, 0x69, 0x50, 0x7b, 0x58, 0x7b, 0x12, 0x0f, 0x55, 0xff, 0xff,
    0x58, 0xa9, 0xff, 0xff, 0xdc, 0xff, 0x7f, 0xff, 0xff, 0xff, 0xd5, 0x55};

// (2^384)^-1 mod p, big-endian. When relic failed to find a square root
// while decompressing a point whose sign bit was set, it left the raw limb
// value 1 in y, which reads back from Montgomery form as this number.
const uint8_t SQRT_FAILURE_MARKER_BE[48] = {
    0x14, 0xfe, 0xc7, 0x01, 0xe8, 0xfb, 0x0c, 0xe9, 0xed, 0x5e, 0x64, 0x27,
    0x3c, 0x4f, 0x53, 0x8b, 0x17, 0x97, 0xab, 0x14, 0x58, 0xa8, 0x8d, 0xe9,
    0x34, 0x3e, 0xa9, 0x79, 0x14, 0x95, 0x6d, 0xc8, 0x7f, 0xe1, 0x12, 0x74,
    0xd8, 0x98, 0xfa, 0xfb, 0xf4, 0xd3, 0x82, 0x59, 0x38, 0x0b, 0x48, 0x20};

// sqrt(-3) mod p, big-endian.
const uint8_t SQRT_M3_BE[48] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xbe, 0x32, 0xce, 0x5f,
    0xbe, 0xed, 0x9c, 0xa3, 0x74, 0xd3, 0x8c, 0x0e, 0xd4, 0x1e, 0xef, 0xd5,
    0xbb, 0x67, 0x52, 0x77, 0xcd, 0xf1, 0x2d, 0x11, 0xbc, 0x2f, 0xb0, 0x26,
    0xc4, 0x14, 0x00, 0x04, 0x5c, 0x03, 0xff, 0xff, 0xff, 0xfd, 0xff, 0xfd};

// (sqrt(-3) - 1) / 2 mod p, big-endian.
const uint8_t SQRT_M3_MINUS_1_HALF_BE[48] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5f, 0x19, 0x67, 0x2f,
    0xdf, 0x76, 0xce, 0x51, 0xba, 0x69, 0xc6, 0x07, 0x6a, 0x0f, 0x77, 0xea,
    0xdd, 0xb3, 0xa9, 0x3b, 0xe6, 0xf8, 0x96, 0x88, 0xde, 0x17, 0xd8, 0x13,
    0x62, 0x0a, 0x00, 0x02, 0x2e, 0x01, 0xff, 0xff, 0xff, 0xfe, 0xff, 0xfe};

// Effective cofactor h_eff of BLS12-381 G2 (RFC 9380 section 8.8.2),
// big-endian, 636 bits. Multiplying by it is equivalent to the
// endomorphism-based cofactor clearing relic performed.
const uint8_t H_EFF_BE[80] = {
    0x0b, 0xc6, 0x9f, 0x08, 0xf2, 0xee, 0x75, 0xb3, 0x58, 0x4c, 0x6a, 0x0e,
    0xa9, 0x1b, 0x35, 0x28, 0x88, 0xe2, 0xa8, 0xe9, 0x14, 0x5a, 0xd7, 0x68,
    0x99, 0x86, 0xff, 0x03, 0x15, 0x08, 0xff, 0xe1, 0x32, 0x9c, 0x2f, 0x17,
    0x87, 0x31, 0xdb, 0x95, 0x6d, 0x82, 0xbf, 0x01, 0x5d, 0x12, 0x12, 0xb0,
    0x2e, 0xc0, 0xec, 0x69, 0xd7, 0x47, 0x7c, 0x1a, 0xe9, 0x54, 0xcb, 0xc0,
    0x66, 0x89, 0xf6, 0xa3, 0x59, 0x89, 0x4c, 0x0a, 0xde, 0xbb, 0xf6, 0xb4,
    0xe8, 0x02, 0x00, 0x05, 0xaa, 0xa9, 0x55, 0x51};
constexpr size_t H_EFF_BITS{636};

void FpFromSmall(blst_fp& out, uint8_t value)
{
    uint8_t be[48]{};
    be[47] = value;
    blst_fp_from_bendian(&out, be);
}

void FpZero(blst_fp& out)
{
    memset(&out, 0, sizeof(out));
}

bool FpIsZero(const blst_fp& a)
{
    uint8_t be[48];
    blst_bendian_from_fp(be, &a);
    for (uint8_t b : be) {
        if (b != 0) return false;
    }
    return true;
}

// The legacy sign test: canonical representation greater than (p-1)/2.
bool FpIsLargeRoot(const blst_fp& a)
{
    uint8_t be[48];
    blst_bendian_from_fp(be, &a);
    return memcmp(be, HALF_P_BE, sizeof(be)) > 0;
}

// The decompression-side sign test of relic's ep2_upk: c1 > (p-1)/2, with c0
// deciding when c1 is zero. (The serialization side uses c1 alone.)
bool Fp2IsLargeRootForDecode(const blst_fp2& y)
{
    return FpIsZero(y.fp[1]) ? FpIsLargeRoot(y.fp[0]) : FpIsLargeRoot(y.fp[1]);
}

// Reads a big-endian coordinate; a value of p or more reads as zero, which is
// what relic's fp_read_bin left behind when its range check failed.
void ReadCoordinate(blst_fp& out, const uint8_t* be)
{
    if (memcmp(be, P_BE, 48) >= 0) {
        FpZero(out);
    } else {
        blst_fp_from_bendian(&out, be);
    }
}

// x^3 + 4
void CurveRhsG1(blst_fp& rhs, const blst_fp& x)
{
    blst_fp four;
    FpFromSmall(four, 4);
    blst_fp_sqr(&rhs, &x);
    blst_fp_mul(&rhs, &rhs, &x);
    blst_fp_add(&rhs, &rhs, &four);
}

// x^3 + (4 + 4i)
void CurveRhsG2(blst_fp2& rhs, const blst_fp2& x)
{
    blst_fp2 b;
    FpFromSmall(b.fp[0], 4);
    FpFromSmall(b.fp[1], 4);
    blst_fp2_sqr(&rhs, &x);
    blst_fp2_mul(&rhs, &rhs, &x);
    blst_fp2_add(&rhs, &rhs, &b);
}

// Shallue-van de Woestijne encoding of a field element into a G2 point
// (section 3 of Fouque-Tibouchi, "Indifferentiable Hashing to
// Barreto-Naehrig Curves"). Returns false when t is zero, which maps to the
// point at infinity.
bool SwEncodeG2(blst_p2_affine& out, const blst_fp2& t)
{
    if (FpIsZero(t.fp[0]) && FpIsZero(t.fp[1])) {
        return false;
    }

    // The "parity" of t decides the sign of y: whether t.c1 is the larger of
    // the two roots {t.c1, -t.c1}.
    blst_fp2 negT;
    blst_fp2_cneg(&negT, &t, true);
    uint8_t be1[48], be2[48];
    blst_bendian_from_fp(be1, &t.fp[1]);
    blst_bendian_from_fp(be2, &negT.fp[1]);
    const bool parity = memcmp(be1, be2, sizeof(be1)) > 0;

    blst_fp one;
    FpFromSmall(one, 1);
    blst_fp2 b;
    FpFromSmall(b.fp[0], 4);
    FpFromSmall(b.fp[1], 4);

    // w = t^2 + b + 1
    blst_fp2 w;
    blst_fp2_sqr(&w, &t);
    blst_fp2_add(&w, &w, &b);
    blst_fp_add(&w.fp[0], &w.fp[0], &one);

    if (FpIsZero(w.fp[0]) && FpIsZero(w.fp[1])) {
        out = *blst_p2_affine_generator();
        if (parity) {
            blst_fp2_cneg(&out.y, &out.y, true);
        }
        return true;
    }

    blst_fp2 sqrtM3, sqrtM3Minus1Half;
    blst_fp_from_bendian(&sqrtM3.fp[0], SQRT_M3_BE);
    FpZero(sqrtM3.fp[1]);
    blst_fp_from_bendian(&sqrtM3Minus1Half.fp[0], SQRT_M3_MINUS_1_HALF_BE);
    FpZero(sqrtM3Minus1Half.fp[1]);

    // w = sqrt(-3) * t / (t^2 + b + 1)
    blst_fp2_inverse(&w, &w);
    blst_fp2_mul(&w, &w, &sqrtM3);
    blst_fp2_mul(&w, &w, &t);

    // x1 = (sqrt(-3) - 1) / 2 - t * w
    blst_fp2 x1;
    blst_fp2_cneg(&x1, &w, true);
    blst_fp2_mul(&x1, &x1, &t);
    blst_fp2_add(&x1, &x1, &sqrtM3Minus1Half);

    // x2 = -1 - x1
    blst_fp2 x2;
    blst_fp2_cneg(&x2, &x1, true);
    blst_fp_sub(&x2.fp[0], &x2.fp[0], &one);

    // x3 = 1 + 1 / w^2
    blst_fp2 x3;
    blst_fp2_sqr(&x3, &w);
    blst_fp2_inverse(&x3, &x3);
    blst_fp_add(&x3.fp[0], &x3.fp[0], &one);

    // Pick the first candidate that lies on the curve, branch-free as in the paper.
    blst_fp2 rhs, y;
    CurveRhsG2(rhs, x1);
    const int chi1 = blst_fp2_sqrt(&y, &rhs) ? 1 : -1;
    CurveRhsG2(rhs, x2);
    const int chi2 = blst_fp2_sqrt(&y, &rhs) ? 1 : -1;
    const int index = ((((chi1 - 1) * chi2) % 3) + 3) % 3;

    out.x = index == 0 ? x1 : index == 1 ? x2 : x3;
    CurveRhsG2(rhs, out.x);
    const bool onCurve = blst_fp2_sqrt(&out.y, &rhs);
    assert(onCurve); // guaranteed by the construction
    (void)onCurve;

    // Align the sign of y with the parity of t.
    blst_fp2 negY;
    blst_fp2_cneg(&negY, &out.y, true);
    blst_bendian_from_fp(be1, &out.y.fp[1]);
    blst_bendian_from_fp(be2, &negY.fp[1]);
    if ((memcmp(be1, be2, sizeof(be1)) > 0) != parity) {
        out.y = negY;
    }
    return true;
}

// Reduces a 64-byte big-endian number modulo p as hi * 2^256 + lo, where both
// halves are below 2^256 < p (the way relic converted a 512-bit draw).
void FpFromWideBigEndian(blst_fp& out, const uint8_t* be64)
{
    uint8_t padded[48]{};
    blst_fp hi, lo, shift;

    memcpy(padded + 16, be64, 32);
    blst_fp_from_bendian(&hi, padded);
    memcpy(padded + 16, be64 + 32, 32);
    blst_fp_from_bendian(&lo, padded);

    memset(padded, 0, sizeof(padded));
    padded[15] = 0x01; // 2^256
    blst_fp_from_bendian(&shift, padded);

    blst_fp_mul(&out, &hi, &shift);
    blst_fp_add(&out, &out, &lo);
}

// Two SHA-256 draws of "hash || 'G2_<i>_c<j>' || counter" for counter 0 and 1,
// concatenated into a 64-byte value.
void DrawFieldElement(blst_fp& out, const uint256& hash, char i, char j)
{
    uint8_t input[32 + 8];
    memcpy(input, hash.begin(), 32);
    input[32] = 'G';
    input[33] = '2';
    input[34] = '_';
    input[35] = static_cast<uint8_t>(i);
    input[36] = '_';
    input[37] = 'c';
    input[38] = static_cast<uint8_t>(j);

    uint8_t draw[64];
    input[39] = 0;
    CSHA256().Write(input, sizeof(input)).Finalize(draw);
    input[39] = 1;
    CSHA256().Write(input, sizeof(input)).Finalize(draw + 32);

    FpFromWideBigEndian(out, draw);
}

} // namespace

bool DecodeG1(blst_p1& out, Span<const uint8_t> in)
{
    assert(in.size() == 48);
    const uint8_t prefix = in[0];
    if ((prefix & 0xc0) == 0xc0) {
        // only the canonical infinity encoding is accepted
        if (prefix != 0xc0) return false;
        for (size_t i = 1; i < in.size(); i++) {
            if (in[i] != 0) return false;
        }
        memset(&out, 0, sizeof(out));
        return true;
    }
    const bool signBit = (prefix & 0x80) != 0;

    uint8_t xbytes[48];
    memcpy(xbytes, in.data(), sizeof(xbytes));
    // The G1 codec cleared all three flag bits before reading x (the G2 codec
    // only clears the sign bit, and only when it is set).
    xbytes[0] &= 0x1f;

    blst_p1_affine a;
    ReadCoordinate(a.x, xbytes);

    blst_fp rhs;
    CurveRhsG1(rhs, a.x);
    if (blst_fp_sqrt(&a.y, &rhs)) {
        if (FpIsLargeRoot(a.y) != signBit) {
            blst_fp_cneg(&a.y, &a.y, true);
        }
    } else if (signBit) {
        blst_fp_from_bendian(&a.y, SQRT_FAILURE_MARKER_BE);
    } else {
        FpZero(a.y);
    }

    // Set z = 1 by hand: blst_p1_from_affine maps an all-zero pair to
    // infinity, whereas relic kept such degenerate decodes as ordinary points.
    out.x = a.x;
    out.y = a.y;
    FpFromSmall(out.z, 1);
    return true;
}

bool DecodeG2(blst_p2& out, Span<const uint8_t> in)
{
    assert(in.size() == 96);
    if ((in[0] & 0xc0) == 0xc0) {
        return false;
    }
    const bool signBit = (in[0] & 0x80) != 0;

    uint8_t c0[48], c1[48];
    memcpy(c0, in.data(), sizeof(c0));
    memcpy(c1, in.data() + 48, sizeof(c1));
    if (signBit) {
        c0[0] &= 0x7f;
    }

    blst_p2_affine a;
    ReadCoordinate(a.x.fp[0], c0);
    ReadCoordinate(a.x.fp[1], c1);

    blst_fp2 rhs;
    CurveRhsG2(rhs, a.x);
    if (blst_fp2_sqrt(&a.y, &rhs)) {
        if (Fp2IsLargeRootForDecode(a.y) != signBit) {
            blst_fp2_cneg(&a.y, &a.y, true);
        }
    } else {
        if (signBit) {
            blst_fp_from_bendian(&a.y.fp[0], SQRT_FAILURE_MARKER_BE);
        } else {
            FpZero(a.y.fp[0]);
        }
        FpZero(a.y.fp[1]);
    }

    out.x = a.x;
    out.y = a.y;
    FpFromSmall(out.z.fp[0], 1);
    FpZero(out.z.fp[1]);
    return true;
}

void EncodeG1(Span<uint8_t> out, const blst_p1& p)
{
    assert(out.size() == 48);
    if (blst_p1_is_inf(&p)) {
        memset(out.data(), 0, out.size());
        out[0] = 0xc0;
        return;
    }
    blst_p1_affine a;
    blst_p1_to_affine(&a, &p);
    blst_bendian_from_fp(out.data(), &a.x);
    if (FpIsLargeRoot(a.y)) {
        out[0] |= 0x80;
    }
}

void EncodeG2(Span<uint8_t> out, const blst_p2& p)
{
    assert(out.size() == 96);
    if (blst_p2_is_inf(&p)) {
        memset(out.data(), 0, out.size());
        out[0] = 0xc0;
        return;
    }
    blst_p2_affine a;
    blst_p2_to_affine(&a, &p);
    blst_bendian_from_fp(out.data(), &a.x.fp[0]);
    blst_bendian_from_fp(out.data() + 48, &a.x.fp[1]);
    // sign taken from c1 alone, no fallback to c0
    if (FpIsLargeRoot(a.y.fp[1])) {
        out[0] |= 0x80;
    }
}

void HashToG2(blst_p2& out, const uint256& hash)
{
    blst_fp2 t0, t1;
    DrawFieldElement(t0.fp[0], hash, '0', '0');
    DrawFieldElement(t0.fp[1], hash, '0', '1');
    DrawFieldElement(t1.fp[0], hash, '1', '0');
    DrawFieldElement(t1.fp[1], hash, '1', '1');

    blst_p2 sum;
    memset(&sum, 0, sizeof(sum)); // infinity
    blst_p2_affine enc;
    if (SwEncodeG2(enc, t0)) {
        blst_p2_from_affine(&sum, &enc);
    }
    if (SwEncodeG2(enc, t1)) {
        blst_p2_add_or_double_affine(&sum, &sum, &enc);
    }

    uint8_t hEffLittleEndian[sizeof(H_EFF_BE)];
    for (size_t i = 0; i < sizeof(H_EFF_BE); i++) {
        hEffLittleEndian[i] = H_EFF_BE[sizeof(H_EFF_BE) - 1 - i];
    }
    blst_p2_mult(&out, &sum, hEffLittleEndian, H_EFF_BITS);
}

} // namespace bls_legacy
