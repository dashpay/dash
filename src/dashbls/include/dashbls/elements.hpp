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

#ifndef SRC_BLSELEMENTS_HPP_
#define SRC_BLSELEMENTS_HPP_

extern "C" {
#include "blst.h"
}
#include <utility>

#include "util.hpp"

namespace bls {
class G1Element;
class G2Element;
class GTElement;

class G1Element {
public:
    static const size_t SIZE = 48;

    G1Element() { memset(&p, 0x00, sizeof(blst_p1)); }

    static G1Element FromBytes(Bytes bytes, bool legacy_encoding = false);
    static G1Element FromBytesUnchecked(Bytes bytes, bool legacy_encoding = false);
    static G1Element FromByteVector(const std::vector<uint8_t> &bytevec, bool legacy_encoding = false);
    static G1Element FromNative(const blst_p1 &element);
    static G1Element FromAffine(const blst_p1_affine &element);
    static G1Element FromMessage(
        const std::vector<uint8_t> &message,
        const uint8_t *dst,
        int dst_len);
    static G1Element FromMessage(
        Bytes message,
        const uint8_t *dst,
        int dst_len);
    static G1Element Generator();

    bool IsValid() const;
    void CheckValid() const;
    void ToNative(blst_p1 *output) const;
    void ToAffine(blst_p1_affine *output) const;
    G1Element Negate() const;
    GTElement Pair(const G2Element &b) const;
    uint32_t GetFingerprint(bool fLegacy = false) const;
    std::vector<uint8_t> Serialize(bool fLegacy = false) const;
    std::array<uint8_t, SIZE> SerializeToArray(bool fLegacy = false) const;
    G1Element Copy();

    friend bool operator==(const G1Element &a, const G1Element &b);
    friend bool operator!=(const G1Element &a, const G1Element &b);
    friend std::ostream &operator<<(std::ostream &os, const G1Element &s);
    friend G1Element &operator+=(G1Element &a, const G1Element &b);
    friend G1Element operator+(const G1Element &a, const G1Element &b);
    friend G1Element operator*(const G1Element &a, const blst_scalar &k);
    friend G1Element operator*(const blst_scalar &k, const G1Element &a);
    friend GTElement operator&(const G1Element &a, const G2Element &b);

private:
    blst_p1 p;
};

class G2Element {
public:
    static const size_t SIZE = 96;

    G2Element() { memset(&q, 0x00, sizeof(blst_p2)); }

    static G2Element FromBytes(Bytes bytes, bool legacy_encoding = false);
    static G2Element FromBytesUnchecked(Bytes bytes, bool legacy_encoding = false);
    static G2Element FromByteVector(const std::vector<uint8_t> &bytevec, bool legacy_encoding = false);
    static G2Element FromNative(const blst_p2 &element);
    static G2Element FromAffine(const blst_p2_affine &element);
    static G2Element FromMessage(
        const std::vector<uint8_t> &message,
        const uint8_t *dst,
        int dst_len, bool legacy_encoding = false);
    static G2Element FromMessage(
        Bytes message,
        const uint8_t *dst,
        int dst_len, bool legacy_encoding = false);
    static G2Element Generator();

    bool IsValid() const;
    void CheckValid() const;
    void ToNative(blst_p2 *output) const;
    void ToAffine(blst_p2_affine *output) const;
    G2Element Negate() const;
    GTElement Pair(const G1Element &a) const;
    std::vector<uint8_t> Serialize(bool fLegacy = false) const;
    std::array<uint8_t, G2Element::SIZE> SerializeToArray(bool fLegacy = false) const;
    G2Element Copy();

    friend bool operator==(G2Element const &a, G2Element const &b);
    friend bool operator!=(G2Element const &a, G2Element const &b);
    friend std::ostream &operator<<(std::ostream &os, const G2Element &s);
    friend G2Element &operator+=(G2Element &a, const G2Element &b);
    friend G2Element operator+(const G2Element &a, const G2Element &b);
    friend G2Element operator*(const G2Element &a, const blst_scalar &k);
    friend G2Element operator*(const blst_scalar &k, const G2Element &a);

private:
    blst_p2 q;
};

class GTElement {
public:
    static const size_t SIZE = sizeof(blst_fp12);

    static GTElement FromBytes(Bytes bytes);
    static GTElement FromBytesUnchecked(Bytes bytes);
    static GTElement FromByteVector(const std::vector<uint8_t> &bytevec);
    static GTElement FromNative(const blst_fp12 *element);
    static GTElement FromAffine(const blst_p1_affine &element);
    static GTElement FromAffine(const blst_p2_affine &element);
    static GTElement Unity();  // unity

    void Serialize(uint8_t *buffer) const;
    std::vector<uint8_t> Serialize() const;
    std::array<uint8_t, SIZE> SerializeToArray() const;

    friend bool operator==(GTElement const &a, GTElement const &b);
    friend bool operator!=(GTElement const &a, GTElement const &b);
    friend std::ostream &operator<<(std::ostream &os, const GTElement &s);
    friend GTElement operator*(GTElement &a, GTElement &b);

private:
    blst_fp12 r;
    GTElement() {}
};

}  // end namespace bls

#endif  // SRC_BLSELEMENTS_HPP_
