// Copyright 2020 Chia Network Inc

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//    http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <string.h>

#include <cstring>

#include "bls.hpp"
#include "legacy.hpp"

namespace {
// The legacy codec reproduces the byte-level behavior of the relic-based
// implementation exactly, including its handling of malformed input. relic
// was built without CHECK, so its internal errors did not throw: an
// out-of-range field element left the coordinate at zero, and a failed
// square root left y holding its pre-existing raw contents (0, or a raw
// limb 1 when the sign bit had been stored). All of it is consensus-visible
// through re-serialization and is pinned by test-vectors/reference.txt.

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

inline void FpZero(blst_fp& out) { memset(&out, 0x00, sizeof(blst_fp)); }

inline void FpOneConst(blst_fp& out)
{
    uint8_t be[48] = {0};
    be[47] = 1;
    blst_fp_from_bendian(&out, be);
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
    uint8_t xbytes[G1Element::SIZE];
    memcpy(xbytes, bytes, G1Element::SIZE);
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

const size_t G1Element::SIZE;

G1Element G1Element::FromBytes(Bytes const bytes, bool fLegacy)
{
    G1Element ele = G1Element::FromBytesUnchecked(bytes, fLegacy);
    if (!fLegacy) {
        ele.CheckValid();
    }
    return ele;
}

G1Element G1Element::FromBytesUnchecked(Bytes const bytes, bool fLegacy)
{
    if (bytes.size() != SIZE) {
        throw std::invalid_argument("G1Element::FromBytes: Invalid size");
    }

    // check if the element is canonical
    const uint8_t* raw_bytes = bytes.begin();
    bool fZerosOnly =
        Util::HasOnlyZeros(Bytes(raw_bytes + 1, bytes.size() - 1));
    if ((bytes[0] & 0xc0) == 0xc0) {  // representing infinity
        // enforce that infinity must be 0xc0000..00
        if (bytes[0] != 0xc0 || !fZerosOnly) {
            throw std::invalid_argument(
                "Given G1 infinity element must be canonical");
        }
        return G1Element();  // return infinity element (point all zero)
    }
    if (fLegacy) {
        G1Element ele;
        blst_p1_affine a;
        LegacyDecodeG1(a, bytes.begin(), (bytes[0] & 0x80) != 0);
        // Build the Jacobian point by hand: blst_p1_from_affine maps the
        // all-zero affine pair to infinity, but relic kept degenerate
        // decodes (e.g. x = 0, y = 0) as ordinary z=1 points, which is
        // consensus-visible through re-serialization.
        ele.p.x = a.x;
        ele.p.y = a.y;
        FpOneConst(ele.p.z);
        return ele;
    }
    if ((bytes[0] & 0xc0) != 0x80) {
        throw std::invalid_argument(
            "Given G1 non-infinity element must start with 0b10");
    }
    if (fZerosOnly) {
        throw std::invalid_argument(
            "G1 non-infinity element can't have only zeros");
    }

    blst_p1_affine a;
    BLST_ERROR err = blst_p1_uncompress(&a, bytes.begin());
    if (err != BLST_SUCCESS)
        throw std::invalid_argument("G1Element::FromBytes: Invalid bytes");

    return G1Element::FromAffine(a);
}

G1Element G1Element::FromByteVector(const std::vector<uint8_t>& bytevec, bool fLegacy)
{
    return G1Element::FromBytes(Bytes(bytevec), fLegacy);
}

G1Element G1Element::FromNative(const blst_p1& element)
{
    G1Element ele;
    memcpy(&(ele.p), &element, sizeof(blst_p1));
    return ele;
}

G1Element G1Element::FromAffine(const blst_p1_affine& element)
{
    G1Element ele;
    blst_p1_from_affine(&(ele.p), &element);
    return ele;
}

G1Element G1Element::FromMessage(
    const std::vector<uint8_t>& message,
    const uint8_t* dst,
    int dst_len)
{
    return FromMessage(Bytes(message), dst, dst_len);
}

G1Element G1Element::FromMessage(
    Bytes const message,
    const uint8_t* dst,
    int dst_len)
{
    G1Element ans;
    const byte* aug = nullptr;
    size_t aug_len = 0;

    blst_hash_to_g1(
        &(ans.p),
        message.begin(),
        (int)message.size(),
        dst,
        dst_len,
        aug,
        aug_len);

    assert(ans.IsValid());
    return ans;
}

G1Element G1Element::Generator()
{
    G1Element ele;
    ele.p = *(blst_p1_generator());
    return ele;
}

bool G1Element::IsValid() const
{
    // Infinity was considered a valid G1Element in older Relic versions
    // on which this library was previously based.
    // For historical compatibililty this behavior is maintained.

    if (blst_p1_is_inf(&p))
        return true;

    return blst_p1_in_g1(&p);
}

void G1Element::CheckValid() const
{
    if (!IsValid())
        throw std::invalid_argument("G1 element is invalid");
}

void G1Element::ToNative(blst_p1* output) const
{
    memcpy(output, &p, sizeof(blst_p1));
}

void G1Element::ToAffine(blst_p1_affine* output) const
{
    blst_p1_to_affine(output, &p);
}

G1Element G1Element::Negate() const
{
    G1Element ans = G1Element::FromNative(p);
    blst_p1_cneg(&(ans.p), true);
    return ans;
}

GTElement G1Element::Pair(const G2Element& b) const { return (*this) & b; }

uint32_t G1Element::GetFingerprint(const bool fLegacy) const
{
    uint8_t buffer[G1Element::SIZE];
    uint8_t hash[32];
    memcpy(buffer, SerializeToArray(fLegacy).data(), G1Element::SIZE);
    Util::Hash256(hash, buffer, G1Element::SIZE);
    return Util::FourBytesToInt(hash);
}

std::vector<uint8_t> G1Element::Serialize(const bool fLegacy) const
{
    const auto arr = SerializeToArray(fLegacy);
    return std::vector<uint8_t>{arr.begin(), arr.end()};
}

std::array<uint8_t, G1Element::SIZE> G1Element::SerializeToArray(const bool fLegacy) const
{
    std::array<uint8_t, G1Element::SIZE> result{};
    if (!fLegacy) {
        blst_p1_compress(result.data(), &p);
        return result;
    }

    if (blst_p1_is_inf(&p)) {
        result[0] = 0xc0;
        return result;
    }

    blst_p1_affine a;
    blst_p1_to_affine(&a, &p);
    blst_bendian_from_fp(result.data(), &a.x);
    if (FpSignGtHalf(a.y)) {
        result[0] |= 0x80;
    }
    return result;
}

bool operator==(const G1Element& a, const G1Element& b)
{
    return blst_p1_is_equal(&(a.p), &(b.p));
}

bool operator!=(const G1Element& a, const G1Element& b) { return !(a == b); }

std::ostream& operator<<(std::ostream& os, const G1Element& ele)
{
    return os << Util::HexStr(ele.Serialize());
}

G1Element& operator+=(G1Element& a, const G1Element& b)
{
    blst_p1_add_or_double(&(a.p), &(a.p), &(b.p));
    return a;
}

G1Element operator+(const G1Element& a, const G1Element& b)
{
    G1Element ans;
    blst_p1_add_or_double(&(ans.p), &(a.p), &(b.p));
    return ans;
}

G1Element operator*(const G1Element& a, const blst_scalar& k)
{
    G1Element ans;
    byte* bte = Util::SecAlloc<byte>(32);
    blst_lendian_from_scalar(bte, &k);
    blst_p1_mult(&(ans.p), &(a.p), bte, 256);
    Util::SecFree(bte);

    return ans;
}

G1Element operator*(const blst_scalar& k, const G1Element& a) { return a * k; }

// G2Element definitions below

const size_t G2Element::SIZE;

G2Element G2Element::FromBytes(Bytes const bytes, const bool fLegacy)
{
    G2Element ele = G2Element::FromBytesUnchecked(bytes, fLegacy);
    if (!fLegacy) {
        ele.CheckValid();
    }
    return ele;
}

G2Element G2Element::FromBytesUnchecked(Bytes const bytes, const bool fLegacy)
{
    if (bytes.size() != SIZE) {
        throw std::invalid_argument("G2Element::FromBytes: Invalid size");
    }

    if (fLegacy) {
        // The relic implementation computed its zeros-only check over a
        // buffer that still contained the first byte unmasked, so for the
        // legacy G2 codec any input with the two top bits set - canonical
        // infinity included - was rejected as non-canonical.
        if ((bytes[0] & 0xc0) == 0xc0) {
            throw std::invalid_argument(
                "Given G2 infinity element must be canonical");
        }
        G2Element ele;
        blst_p2_affine a;
        LegacyDecodeG2(a, bytes.begin(), (bytes[0] & 0x80) != 0);
        // See the G1 legacy path: keep degenerate all-zero decodes as
        // ordinary z=1 points instead of mapping them to infinity.
        ele.q.x = a.x;
        ele.q.y = a.y;
        FpOneConst(ele.q.z.fp[0]);
        FpZero(ele.q.z.fp[1]);
        return ele;
    }

    if ((bytes[48] & 0xe0) != 0x00) {
        throw std::invalid_argument(
            "Given G2 element must always have 48th byte start with 0b000");
    }

    bool fZerosOnly = (bytes[0] & 0x1f) == 0 &&
                      Util::HasOnlyZeros(Bytes(bytes.begin() + 1, bytes.size() - 1));
    if ((bytes[0] & 0xc0) == 0xc0) {  // infinity
        // enforce that infinity must be 0xc0000..00
        if (bytes[0] != 0xc0 || !fZerosOnly) {
            throw std::invalid_argument(
                "Given G2 infinity element must be canonical");
        }
        return G2Element();
    }

    if ((bytes[0] & 0xc0) != 0x80) {
        throw std::invalid_argument(
            "G2 non-inf element must have 0th byte start with 0b10");
    }

    if (fZerosOnly) {
        throw std::invalid_argument(
            "G2 non-infinity element can't have only zeros");
    }

    blst_p2_affine a;
    BLST_ERROR err = blst_p2_uncompress(&a, bytes.begin());
    if (err != BLST_SUCCESS)
        throw std::invalid_argument("G2Element::FromBytes: Invalid bytes");

    return G2Element::FromAffine(a);
}

G2Element G2Element::FromByteVector(const std::vector<uint8_t>& bytevec, bool fLegacy)
{
    return G2Element::FromBytes(Bytes(bytevec), fLegacy);
}

G2Element G2Element::FromNative(const blst_p2& element)
{
    G2Element ele;
    memcpy(&(ele.q), &element, sizeof(blst_p2));
    return ele;
}

G2Element G2Element::FromAffine(const blst_p2_affine& element)
{
    G2Element ele;
    blst_p2_from_affine(&(ele.q), &element);
    return ele;
}

G2Element G2Element::FromMessage(
    const std::vector<uint8_t>& message,
    const uint8_t* dst,
    int dst_len,
    const bool fLegacy)
{
    return FromMessage(Bytes(message), dst, dst_len, fLegacy);
}

G2Element G2Element::FromMessage(
    Bytes const message,
    const uint8_t* dst,
    int dst_len,
    const bool fLegacy)
{
    G2Element ans;
    if (fLegacy) {
        ep2_map_legacy(&(ans.q), message.begin(), BLS::MESSAGE_HASH_LEN);
    } else {
        const byte* aug = nullptr;
        size_t aug_len = 0;

        blst_hash_to_g2(
            &(ans.q),
            message.begin(),
            (int)message.size(),
            dst,
            dst_len,
            aug,
            aug_len);
    }
    assert(ans.IsValid());
    return ans;
}

G2Element G2Element::Generator()
{
    G2Element ele;
    ele.q = (*blst_p2_generator());
    return ele;
}

bool G2Element::IsValid() const
{
    // Infinity was considered a valid G2Element in older Relic versions
    // on which this library was previously based.
    // For historical compatibililty this behavior is maintained.

    if (blst_p2_is_inf(&q))
        return true;

    return blst_p2_in_g2(&q);
}

void G2Element::CheckValid() const
{
    if (!IsValid())
        throw std::invalid_argument("G2 element is invalid");
}

void G2Element::ToNative(blst_p2* output) const
{
    memcpy(output, (blst_p2*)&q, sizeof(blst_p2));
}

void G2Element::ToAffine(blst_p2_affine* output) const
{
    blst_p2_to_affine(output, &q);
}

G2Element G2Element::Copy() { return *this; }

G2Element G2Element::Negate() const
{
    G2Element ans = G2Element::FromNative(q);
    blst_p2_cneg(&(ans.q), true);
    return ans;
}

GTElement G2Element::Pair(const G1Element& a) const { return a & (*this); }

std::vector<uint8_t> G2Element::Serialize(const bool fLegacy) const
{
    const auto arr = G2Element::SerializeToArray(fLegacy);
    return std::vector<uint8_t>{arr.begin(), arr.end()};
}

std::array<uint8_t, G2Element::SIZE> G2Element::SerializeToArray(const bool fLegacy) const
{
    std::array<uint8_t, G2Element::SIZE> result{};
    if (!fLegacy) {
        blst_p2_compress(result.data(), &q);
        return result;
    }

    if (blst_p2_is_inf(&q)) {
        result[0] = 0xc0;
        return result;
    }

    blst_p2_affine a;
    blst_p2_to_affine(&a, &q);
    // legacy layout: x.c0 || x.c1 (the modern IETF layout is the swap)
    blst_bendian_from_fp(result.data(), &a.x.fp[0]);
    blst_bendian_from_fp(result.data() + 48, &a.x.fp[1]);
    // legacy sign: c1 > (p-1)/2, no tie-break (relic's ep2_pck)
    if (FpSignGtHalf(a.y.fp[1])) {
        result[0] |= 0x80;
    }
    return result;
}

bool operator==(G2Element const& a, G2Element const& b)
{
    return blst_p2_is_equal(&(a.q), &(b.q));
}

bool operator!=(G2Element const& a, G2Element const& b) { return !(a == b); }

std::ostream& operator<<(std::ostream& os, const G2Element& s)
{
    return os << Util::HexStr(s.Serialize());
}

G2Element& operator+=(G2Element& a, const G2Element& b)
{
    blst_p2_add_or_double(&(a.q), &(a.q), &(b.q));
    return a;
}

G2Element operator+(const G2Element& a, const G2Element& b)
{
    G2Element ans;
    blst_p2_add_or_double(&(ans.q), &(a.q), &(b.q));
    return ans;
}

G2Element operator*(const G2Element& a, const blst_scalar& k)
{
    G2Element ans;
    byte* bte = Util::SecAlloc<byte>(32);
    blst_lendian_from_scalar(bte, &k);
    blst_p2_mult(&(ans.q), &(a.q), bte, 256);
    Util::SecFree(bte);

    return ans;
}

G2Element operator*(const blst_scalar& k, const G2Element& a) { return a * k; }

// GTElement

const size_t GTElement::SIZE;

/*
 * Currently deserliazation is not available - these are currently
 * broken and just return the zero element
 */
GTElement GTElement::FromBytes(Bytes const bytes)
{
    GTElement ele = GTElement::FromBytesUnchecked(bytes);
    //
    // this doesn't seem to be the proper check as it doesn't work as expeced
    //
    // if (!blst_fp12_in_group(&(ele.r)))
    //     throw std::invalid_argument("GTElement is invalid");
    return ele;
}

GTElement GTElement::FromBytesUnchecked(Bytes const bytes)
{
    if (bytes.size() != SIZE) {
        throw std::invalid_argument("GTElement::FromBytes: Invalid size");
    }
    GTElement ele = GTElement();
    memcpy(&(ele.r), bytes.begin(), SIZE);
    return ele;
}

GTElement GTElement::FromByteVector(const std::vector<uint8_t>& bytevec)
{
    return GTElement::FromBytes(Bytes(bytevec));
}

GTElement GTElement::FromNative(const blst_fp12* element)
{
    GTElement ele = GTElement();
    ele.r = *element;
    return ele;
}

GTElement GTElement::FromAffine(const blst_p1_affine& affine)
{
    GTElement ele = GTElement();
    blst_aggregated_in_g1(&ele.r, &affine);
    return ele;
}

GTElement GTElement::FromAffine(const blst_p2_affine& affine)
{
    GTElement ele = GTElement();
    blst_aggregated_in_g2(&ele.r, &affine);
    return ele;
}

GTElement GTElement::Unity()
{
    GTElement ele = GTElement();
    ele.FromNative(blst_fp12_one());
    return ele;
}

bool operator==(GTElement const& a, GTElement const& b)
{
    return blst_fp12_is_equal(&(a.r), &(b.r));
}

bool operator!=(GTElement const& a, GTElement const& b) { return !(a == b); }

std::ostream& operator<<(std::ostream& os, GTElement const& ele)
{
    return os << Util::HexStr(ele.Serialize());
}

GTElement operator&(const G1Element& a, const G2Element& b)
{
    blst_fp12 ans;

    blst_p1_affine aff1;
    blst_p2_affine aff2;
    a.ToAffine(&aff1);
    b.ToAffine(&aff2);

    blst_miller_loop(&ans, &aff2, &aff1);
    blst_final_exp(&ans, &ans);

    GTElement ret = GTElement::FromNative(&ans);

    return ret;
}

GTElement operator*(GTElement& a, GTElement& b)
{
    GTElement ans;
    blst_fp12_mul(&(ans.r), &(a.r), &(b.r));
    return ans;
}

void GTElement::Serialize(uint8_t* buffer) const
{
    memcpy(buffer, &r, GTElement::SIZE);
}

std::vector<uint8_t> GTElement::Serialize() const
{
    std::vector<uint8_t> data(GTElement::SIZE);
    Serialize(data.data());
    return data;
}

std::array<uint8_t, GTElement::SIZE> GTElement::SerializeToArray() const
{
    std::array<uint8_t, GTElement::SIZE> data{};
    Serialize(data.data());
    return data;
}

}  // end namespace bls
