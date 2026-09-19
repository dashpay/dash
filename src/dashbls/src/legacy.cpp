// Copyright (c) 2021 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "legacy.hpp"
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
 * Multiplies a point by the cofactor in a Barreto-Lynn-Soctt.
 * Based on the rust implementation of pairings, zkcrypto/pairing.
 * The algorithm is Shallue–van de Woestijne encoding from
 * Section 3 of "Indifferentiable Hashing to Barreto–Naehrig Curves"
 * from Fouque-Tibouchi: <https://www.di.ens.fr/~fouque/pub/latincrypt12.pdf>
 * @param[out] r			- the result.
 * @param[in] p				- the point to multiply.
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
} // anon namespace

namespace bls {
void ep2_map_legacy(blst_p2* p, const uint8_t* msg, int len)
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
