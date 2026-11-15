// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING.MIT or https://opensource.org/license/MIT

#include "hdkeys.hpp"
#include "secure.h"

namespace bls {
PrivateKey HDKeys::KeyGen(const std::vector<uint8_t>& seed)
{
    return KeyGen(Bytes(seed));
}

PrivateKey HDKeys::KeyGen(const Bytes& seed)
{
    // KeyGen
    // 1. PRK = HKDF-Extract("BLS-SIG-KEYGEN-SALT-", IKM || I2OSP(0, 1))
    // 2. OKM = HKDF-Expand(PRK, keyInfo || I2OSP(L, 2), L)
    // 3. SK = OS2IP(OKM) mod r
    // 4. return SK

    const uint8_t info[1] = {0};
    const size_t infoLen = 0;

    // Required by the ietf spec to be at least 32 bytes
    if (seed.size() < 32) {
        throw std::invalid_argument("Seed size must be at least 32 bytes");
    }

    util::SecPtr<blst_scalar> skBn = util::SecMake<blst_scalar>();
    blst_keygen_v3(skBn.get(), seed.begin(), seed.size(), info, infoLen);
    util::SecVector<uint8_t> skBytes(32);
    blst_bendian_from_scalar(skBytes.data(), skBn.get());

    return PrivateKey::FromBytes(Bytes(skBytes.data(), 32));
}

void HDKeys::IKMToLamportSk(
    uint8_t* outputLamportSk,
    const uint8_t* ikm,
    size_t ikmLen,
    const uint8_t* salt,
    size_t saltLen)
{
    // Expands the ikm to 255*HASH_LEN bytes for the lamport sk
    const uint8_t info[1] = {0};
    HKDF256::ExtractExpand(
        outputLamportSk,
        HASH_LEN * 255,
        ikm,
        ikmLen,
        salt,
        saltLen,
        info,
        0);
}

void HDKeys::ParentSkToLamportPK(
    uint8_t* outputLamportPk,
    const PrivateKey& parentSk,
    uint32_t index)
{
    util::SecVector<uint8_t> salt(4);
    util::SecVector<uint8_t> ikm(HASH_LEN);
    util::SecVector<uint8_t> notIkm(HASH_LEN);
    util::SecVector<uint8_t> lamport0(HASH_LEN * 255);
    util::SecVector<uint8_t> lamport1(HASH_LEN * 255);

    Util::IntToFourBytes(salt.data(), index);
    parentSk.Serialize(ikm.data());

    for (size_t i = 0; i < HASH_LEN; i++) {  // Flips the bits
        notIkm[i] = ikm[i] ^ 0xff;
    }

    HDKeys::IKMToLamportSk(
        lamport0.data(), ikm.data(), HASH_LEN, salt.data(), 4);
    HDKeys::IKMToLamportSk(
        lamport1.data(), notIkm.data(), HASH_LEN, salt.data(), 4);

    util::SecVector<uint8_t> lamportPk(HASH_LEN * 255 * 2);

    for (size_t i = 0; i < 255; i++) {
        Util::Hash256(
            lamportPk.data() + i * HASH_LEN,
            lamport0.data() + i * HASH_LEN,
            HASH_LEN);
    }

    for (size_t i = 0; i < 255; i++) {
        Util::Hash256(
            lamportPk.data() + 255 * HASH_LEN + i * HASH_LEN,
            lamport1.data() + i * HASH_LEN,
            HASH_LEN);
    }
    Util::Hash256(outputLamportPk, lamportPk.data(), HASH_LEN * 255 * 2);
}

PrivateKey HDKeys::DeriveChildSk(const PrivateKey& parentSk, uint32_t index)
{
    util::SecVector<uint8_t> lamportPk(HASH_LEN);
    HDKeys::ParentSkToLamportPK(lamportPk.data(), parentSk, index);
    return HDKeys::KeyGen(Bytes(lamportPk.data(), lamportPk.size()));
}

PrivateKey HDKeys::DeriveChildSkUnhardened(
    const PrivateKey& parentSk,
    uint32_t index)
{
    util::SecVector<uint8_t> buf(G1Element::SIZE + 4);
    util::SecVector<uint8_t> digest(HASH_LEN);
    memcpy(
        buf.data(),
        parentSk.GetG1Element().Serialize().data(),
        G1Element::SIZE);
    Util::IntToFourBytes(buf.data() + G1Element::SIZE, index);
    Util::Hash256(digest.data(), buf.data(), G1Element::SIZE + 4);

    return PrivateKey::Aggregate(
        {parentSk,
         PrivateKey::FromBytes(Bytes(digest.data(), HASH_LEN), true)});
}

G1Element HDKeys::DeriveChildG1Unhardened(
    const G1Element& pk,
    uint32_t index)
{
    util::SecVector<uint8_t> buf(G1Element::SIZE + 4);
    util::SecVector<uint8_t> digest(HASH_LEN);
    memcpy(buf.data(), pk.Serialize().data(), G1Element::SIZE);

    Util::IntToFourBytes(buf.data() + G1Element::SIZE, index);
    Util::Hash256(digest.data(), buf.data(), G1Element::SIZE + 4);

    blst_scalar nonce;
    // The digest is big-endian; upstream chia loaded it as little-endian,
    // which only worked because operator* reversed the bytes again.
    blst_scalar_from_bendian(&nonce, digest.data());

    G1Element gen = G1Element::Generator();

    return pk + (gen * nonce);
}

G2Element HDKeys::DeriveChildG2Unhardened(
    const G2Element& pk,
    uint32_t index)
{
    util::SecVector<uint8_t> buf(G2Element::SIZE + 4);
    util::SecVector<uint8_t> digest(HASH_LEN);
    memcpy(buf.data(), pk.Serialize().data(), G2Element::SIZE);
    Util::IntToFourBytes(buf.data() + G2Element::SIZE, index);
    Util::Hash256(digest.data(), buf.data(), G2Element::SIZE + 4);

    blst_scalar nonce;
    // The digest is big-endian; upstream chia loaded it as little-endian,
    // which only worked because operator* reversed the bytes again.
    blst_scalar_from_bendian(&nonce, digest.data());

    G2Element gen = G2Element::Generator();
    return pk + gen * nonce;
}
} // namespace bls
