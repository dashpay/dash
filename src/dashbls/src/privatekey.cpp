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

#include <algorithm>

#include "bls.hpp"
#include "legacy.hpp"

namespace bls {

const size_t PrivateKey::PRIVATE_KEY_SIZE;

PrivateKey PrivateKey::FromSeedBIP32(const Bytes& seed) {
    // "BLS private key seed" in ascii
    const uint8_t hmacKey[] = {66, 76, 83, 32, 112, 114, 105, 118, 97, 116, 101,
                               32, 107, 101, 121, 32, 115, 101, 101, 100};

    auto* hash = Util::SecAlloc<uint8_t>(
        PrivateKey::PRIVATE_KEY_SIZE);

    // Hash the seed into sk
    Util::md_hmac(hash, seed.begin(), (int)seed.size(), hmacKey, sizeof(hmacKey));

    // Make sure private key is less than the curve order
    PrivateKey k;
    blst_scalar_from_be_bytes(k.keydata, hash, PrivateKey::PRIVATE_KEY_SIZE);

    Util::SecFree(hash);
    return k;
}

// BLS12-381 group order r, big-endian.
// Taken from depends/blst/src/consts.c: BLS12_381_r = { 0xffffffff00000001, 0x53bda402fffe5bfe, 0x3339d80809a1d805, 0x73eda753299d7d48 }
//     (little-endian limbs of the same number, with the comment z^4 - z^2 + 1, group order).

static const uint8_t ORDER_BE[32] = {
    0x73, 0xed, 0xa7, 0x53, 0x29, 0x9d, 0x7d, 0x48, 0x33, 0x39, 0xd8, 0x08,
    0x09, 0xa1, 0xd8, 0x05, 0x53, 0xbd, 0xa4, 0x02, 0xff, 0xfe, 0x5b, 0xfe,
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01};

// Construct a private key from a bytearray.
PrivateKey PrivateKey::FromBytes(const Bytes &bytes, bool modOrder)
{
    if (bytes.size() != PRIVATE_KEY_SIZE) {
        throw std::invalid_argument("PrivateKey::FromBytes: Invalid size");
    }

    PrivateKey k;
    if (modOrder)
        // this allows any bytes to be input and does proper mod order
        blst_scalar_from_be_bytes(k.keydata, bytes.begin(), bytes.size());
    else {
        // Values strictly greater than the group order are rejected; the
        // order itself is accepted, matching the relic-based
        // implementation's bn_cmp(keydata, order) > 0 check.
        if (memcmp(bytes.begin(), ORDER_BE, PRIVATE_KEY_SIZE) > 0) {
            throw std::invalid_argument(
                "PrivateKey byte data must be less than the group order");
        }
        blst_scalar_from_bendian(k.keydata, bytes.begin());
    }
    return k;
}

// Construct a private key from a bytearray.
PrivateKey PrivateKey::FromByteVector(
    const std::vector<uint8_t> bytes,
    bool modOrder)
{
    return PrivateKey::FromBytes(Bytes(bytes), modOrder);
}

PrivateKey::PrivateKey() { AllocateKeyData(); };

// Construct a private key from another private key.
PrivateKey::PrivateKey(const PrivateKey &privateKey)
{
    privateKey.CheckKeyData();
    AllocateKeyData();
    memcpy(keydata, privateKey.keydata, sizeof(blst_scalar));
}

PrivateKey::PrivateKey(PrivateKey &&k)
    : keydata(std::exchange(k.keydata, nullptr))
{
    k.InvalidateCaches();
}

PrivateKey::~PrivateKey() { DeallocateKeyData(); }

void PrivateKey::DeallocateKeyData()
{
    if (keydata != nullptr) {
        Util::SecFree(keydata);
        keydata = nullptr;
    }
    InvalidateCaches();
}

void PrivateKey::InvalidateCaches()
{
    fG1CacheValid = false;
    fG2CacheValid = false;
}

PrivateKey &PrivateKey::operator=(const PrivateKey &other)
{
    CheckKeyData();
    other.CheckKeyData();
    InvalidateCaches();
    memcpy(keydata, other.keydata, sizeof(blst_scalar));
    return *this;
}

PrivateKey &PrivateKey::operator=(PrivateKey &&other)
{
    DeallocateKeyData();
    keydata = std::exchange(other.keydata, nullptr);
    other.InvalidateCaches();
    return *this;
}

const G1Element &PrivateKey::GetG1Element() const
{
    if (!fG1CacheValid) {
        CheckKeyData();
        blst_p1 *p = Util::SecAlloc<blst_p1>(1);
        blst_sk_to_pk_in_g1(p, keydata);

        g1Cache = G1Element::FromNative(*p);
        Util::SecFree(p);
        fG1CacheValid = true;
    }
    return g1Cache;
}

const G2Element &PrivateKey::GetG2Element() const
{
    if (!fG2CacheValid) {
        CheckKeyData();
        blst_p2 *q = Util::SecAlloc<blst_p2>(1);
        blst_sk_to_pk_in_g2(q, keydata);

        g2Cache = G2Element::FromNative(*q);
        Util::SecFree(q);
        fG2CacheValid = true;
    }
    return g2Cache;
}

bool PrivateKey::HasKeyData() const
{
    return (keydata != nullptr);
}

G1Element operator*(const G1Element &a, const PrivateKey &k)
{
    k.CheckKeyData();

    blst_p1 *ans = Util::SecAlloc<blst_p1>(1);
    a.ToNative(ans);
    byte *bte = Util::SecAlloc<byte>(32);
    blst_lendian_from_scalar(bte, k.keydata);
    blst_p1_mult(ans, ans, bte, 256);
    G1Element ret = G1Element::FromNative(*ans);
    Util::SecFree(ans);
    Util::SecFree(bte);
    return ret;
}

G1Element operator*(const PrivateKey &k, const G1Element &a) { return a * k; }

G2Element operator*(const G2Element &a, const PrivateKey &k)
{
    k.CheckKeyData();
    blst_p2 *ans = Util::SecAlloc<blst_p2>(1);
    a.ToNative(ans);
    byte *bte = Util::SecAlloc<byte>(32);
    blst_lendian_from_scalar(bte, k.keydata);
    blst_p2_mult(ans, ans, bte, 256);
    G2Element ret = G2Element::FromNative(*ans);
    Util::SecFree(ans);
    Util::SecFree(bte);
    return ret;
}

G2Element operator*(const PrivateKey &k, const G2Element &a) { return a * k; }

PrivateKey operator*(const PrivateKey& k, const blst_scalar& a)
{
    k.CheckKeyData();
    PrivateKey ret;
    blst_sk_mul_n_check(ret.keydata, k.keydata, &a);
    return ret;
}

PrivateKey operator*(const blst_scalar& a, const PrivateKey& k) { return k * a; }

G2Element PrivateKey::GetG2Power(const G2Element &element) const
{
    CheckKeyData();
    blst_p2 *q = Util::SecAlloc<blst_p2>(1);
    element.ToNative(q);
    byte *bte = Util::SecAlloc<byte>(32);
    blst_lendian_from_scalar(bte, keydata);
    blst_p2_mult(q, q, bte, 255);
    const G2Element ret = G2Element::FromNative(*q);
    Util::SecFree(q);
    Util::SecFree(bte);
    return ret;
}

PrivateKey PrivateKey::Aggregate(std::vector<PrivateKey> const &privateKeys)
{
    if (privateKeys.empty()) {
        throw std::length_error("Number of private keys must be at least 1");
    }

    PrivateKey ret;
    assert(ret.IsZero());
    for (size_t i = 0; i < privateKeys.size(); i++) {
        privateKeys[i].CheckKeyData();
        blst_sk_add_n_check(ret.keydata, ret.keydata, privateKeys[i].keydata);
    }
    return ret;
}

bool PrivateKey::IsZero() const
{
    CheckKeyData();
    blst_scalar zro;
    memset(&zro, 0x00, sizeof(blst_scalar));

    return memcmp(keydata, &zro, sizeof(blst_scalar)) == 0;
}

bool operator==(const PrivateKey &a, const PrivateKey &b)
{
    a.CheckKeyData();
    b.CheckKeyData();
    return memcmp(a.keydata, b.keydata, sizeof(blst_scalar)) == 0;
}

bool operator!=(const PrivateKey &a, const PrivateKey &b) { return !(a == b); }

void PrivateKey::Serialize(uint8_t *buffer) const
{
    if (buffer == nullptr) {
        throw std::runtime_error("PrivateKey::Serialize buffer invalid");
    }
    CheckKeyData();
    // blst_bendian_from_scalar converts through a temporary limb vector and
    // securely wipes it, which costs more than the serialization itself;
    // the scalar's native bytes are little-endian, so copy and reverse.
    blst_lendian_from_scalar(buffer, keydata);
    std::reverse(buffer, buffer + PRIVATE_KEY_SIZE);
}

std::vector<uint8_t> PrivateKey::Serialize() const
{
    std::vector<uint8_t> data(PRIVATE_KEY_SIZE);
    Serialize(data.data());
    return data;
}

std::array<uint8_t, PrivateKey::PRIVATE_KEY_SIZE> PrivateKey::SerializeToArray() const
{
    std::array<uint8_t, PRIVATE_KEY_SIZE> data{};
    Serialize(data.data());
    return data;
}

G2Element PrivateKey::SignG2(
    const uint8_t *msg,
    size_t len,
    const uint8_t *dst,
    size_t dst_len,
    const bool fLegacy) const
{
    CheckKeyData();

    blst_p2 *pt = Util::SecAlloc<blst_p2>(1);
    if (fLegacy) {
        // The relic implementation always mapped exactly
        // BLS::MESSAGE_HASH_LEN bytes regardless of len.
        ep2_map_legacy(pt, msg, BLS::MESSAGE_HASH_LEN);
    } else {
        blst_hash_to_g2(pt, msg, len, dst, dst_len, nullptr, 0);
    }
    blst_sign_pk_in_g1(pt, pt, keydata);
    G2Element ret = G2Element::FromNative(*pt);
    Util::SecFree(pt);
    return ret;
}

void PrivateKey::AllocateKeyData()
{
    assert(!keydata);
    keydata = Util::SecAlloc<blst_scalar>(1);
    memset(keydata, 0x00, sizeof(blst_scalar));
}

void PrivateKey::CheckKeyData() const
{
    if (keydata == nullptr) {
        throw std::runtime_error(
            "PrivateKey::CheckKeyData keydata not initialized");
    }
}

}  // end namespace bls
