// Copyright 2018 Chia Network Inc

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//    http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cstring>
#include <new>
#include "bls.hpp"

namespace bls {
namespace {

class SecureBytes {
 public:
    explicit SecureBytes(size_t size) : m_size(size), m_data(Util::SecAlloc<uint8_t>(size)) {
        if (m_data == nullptr) {
            throw std::bad_alloc();
        }
    }

    ~SecureBytes() {
        Util::SecureWipe(m_data, m_size);
        Util::SecFree(m_data);
    }

    SecureBytes(const SecureBytes&) = delete;
    SecureBytes& operator=(const SecureBytes&) = delete;

    uint8_t* data() { return m_data; }

 private:
    const size_t m_size;
    uint8_t* const m_data;
};

class SecureRelicBn {
 public:
    SecureRelicBn() {
        bn_null(m_value);
        bn_new(m_value);
    }

    ~SecureRelicBn() {
#if ALLOC == DYNAMIC
        if (m_value != nullptr && m_value->dp != nullptr && m_value->alloc > 0) {
            Util::SecureWipe(m_value->dp, m_value->alloc * sizeof(dig_t));
        }
        bn_free(m_value);
#elif ALLOC == AUTO
        Util::SecureWipe(m_value, sizeof(m_value));
#endif
    }

    SecureRelicBn(const SecureRelicBn&) = delete;
    SecureRelicBn& operator=(const SecureRelicBn&) = delete;

    bn_st* get() { return m_value; }

 private:
    bn_t m_value;
};

} // namespace

ExtendedPrivateKey ExtendedPrivateKey::FromSeed(const Bytes& bytes) {
    // "BLS HD seed" in ascii
    const uint8_t prefix[] = {66, 76, 83, 32, 72, 68, 32, 115, 101, 101, 100};

    SecureBytes hashInput(bytes.size() + 1);
    std::memcpy(hashInput.data(), bytes.begin(), bytes.size());

    // 32 bytes for secret key, and 32 bytes for chaincode
    SecureBytes ILeft(PrivateKey::PRIVATE_KEY_SIZE);
    SecureBytes IRight(ChainCode::SIZE);

    // Hash the seed into 64 bytes, half will be sk, half will be cc
    hashInput.data()[bytes.size()] = 0;
    md_hmac(ILeft.data(), hashInput.data(), bytes.size() + 1, prefix, sizeof(prefix));

    hashInput.data()[bytes.size()] = 1;
    md_hmac(IRight.data(), hashInput.data(), bytes.size() + 1, prefix, sizeof(prefix));

    // Make sure private key is less than the curve order
    SecureRelicBn skBn;
    SecureRelicBn order;
    g1_get_ord(order.get());

    bn_read_bin(skBn.get(), ILeft.data(), PrivateKey::PRIVATE_KEY_SIZE);
    bn_mod_basic(skBn.get(), skBn.get(), order.get());
    bn_write_bin(ILeft.data(), PrivateKey::PRIVATE_KEY_SIZE, skBn.get());

    ExtendedPrivateKey esk(ExtendedPublicKey::REVISION, 0, 0, 0,
                           ChainCode::FromBytes(Bytes(IRight.data(), ChainCode::SIZE)),
                           PrivateKey::FromBytes(Bytes(ILeft.data(), PrivateKey::PRIVATE_KEY_SIZE)));

    return esk;
}

ExtendedPrivateKey ExtendedPrivateKey::FromBytes(const Bytes& bytes) {
    uint32_t version = Util::FourBytesToInt(bytes.begin());
    uint32_t depth = bytes[4];
    uint32_t parentFingerprint = Util::FourBytesToInt(bytes.begin() + 5);
    uint32_t childNumber = Util::FourBytesToInt(bytes.begin() + 9);
    const uint8_t* ccPointer = bytes.begin() + 13;
    const uint8_t* skPointer = ccPointer + ChainCode::SIZE;

    ExtendedPrivateKey esk(version, depth, parentFingerprint, childNumber,
                          ChainCode::FromBytes(Bytes(ccPointer, ChainCode::SIZE)),
                          PrivateKey::FromBytes(Bytes(skPointer, PrivateKey::PRIVATE_KEY_SIZE)));
    return esk;
}

ExtendedPrivateKey ExtendedPrivateKey::PrivateChild(uint32_t i, const bool fLegacy) const {
    if (depth >= 255) {
        throw std::logic_error("Cannot go further than 255 levels");
    }
    // Hardened keys have i >= 2^31. Non-hardened have i < 2^31
    uint32_t cmp = (1 << 31);
    bool hardened = i >= cmp;

    SecureBytes ILeft(PrivateKey::PRIVATE_KEY_SIZE);
    SecureBytes IRight(ChainCode::SIZE);

    // Chain code is used as hmac key
    SecureBytes hmacKey(ChainCode::SIZE);
    chainCode.Serialize(hmacKey.data());

    size_t inputLen = hardened ? PrivateKey::PRIVATE_KEY_SIZE + 4 + 1
                                : G1Element::SIZE + 4 + 1;
    // Hmac input includes sk or pk, int i, and byte with 0 or 1
    SecureBytes hmacInput(inputLen);

    // Fill the input with the required data
    if (hardened) {
        sk.Serialize(hmacInput.data());
        Util::IntToFourBytes(hmacInput.data() + PrivateKey::PRIVATE_KEY_SIZE, i);
    } else {
        memcpy(hmacInput.data(), sk.GetG1Element().Serialize(fLegacy).data(), G1Element::SIZE);
        Util::IntToFourBytes(hmacInput.data() + G1Element::SIZE, i);
    }
    hmacInput.data()[inputLen - 1] = 0;

    md_hmac(ILeft.data(), hmacInput.data(), inputLen,
                    hmacKey.data(), ChainCode::SIZE);

    // Change 1 byte to generate a different sequence for chaincode
    hmacInput.data()[inputLen - 1] = 1;

    md_hmac(IRight.data(), hmacInput.data(), inputLen,
                    hmacKey.data(), ChainCode::SIZE);

    PrivateKey newSk = PrivateKey::FromBytes(Bytes(ILeft.data(), PrivateKey::PRIVATE_KEY_SIZE), true);
    newSk = PrivateKey::Aggregate({sk, newSk});

    ExtendedPrivateKey esk(version, depth + 1,
                           sk.GetG1Element().GetFingerprint(), i,
                           ChainCode::FromBytes(Bytes(IRight.data(), ChainCode::SIZE)),
                           newSk);

    return esk;
}

uint32_t ExtendedPrivateKey::GetVersion() const {
    return version;
}

uint8_t ExtendedPrivateKey::GetDepth() const {
    return depth;
}

uint32_t ExtendedPrivateKey::GetParentFingerprint() const {
    return parentFingerprint;
}

uint32_t ExtendedPrivateKey::GetChildNumber() const {
    return childNumber;
}

ExtendedPublicKey ExtendedPrivateKey::PublicChild(uint32_t i) const {
    return PrivateChild(i).GetExtendedPublicKey();
}

PrivateKey ExtendedPrivateKey::GetPrivateKey() const {
    return sk;
}

G1Element ExtendedPrivateKey::GetPublicKey() const {
    return sk.GetG1Element();
}

ChainCode ExtendedPrivateKey::GetChainCode() const {
    return chainCode;
}

ExtendedPublicKey ExtendedPrivateKey::GetExtendedPublicKey(const bool fLegacy) const {
    std::vector<uint8_t> buffer(ExtendedPublicKey::SIZE);
    Util::IntToFourBytes(buffer.data(), version);
    buffer[4] = depth;
    Util::IntToFourBytes(buffer.data() + 5, parentFingerprint);
    Util::IntToFourBytes(buffer.data() + 9, childNumber);

    chainCode.Serialize(buffer.data() + 13);
    auto vecG1 = sk.GetG1Element().Serialize(fLegacy);
    buffer.insert(buffer.begin() + 13 + ChainCode::SIZE, vecG1.begin(), vecG1.end());

    return ExtendedPublicKey::FromBytes(Bytes(buffer), fLegacy);
}

// Comparator implementation.
bool operator==(ExtendedPrivateKey const &a,  ExtendedPrivateKey const &b) {
    return (a.GetPrivateKey() == b.GetPrivateKey() &&
            a.GetChainCode() == b.GetChainCode());
}

bool operator!=(ExtendedPrivateKey const&a,  ExtendedPrivateKey const&b) {
    return !(a == b);
}

void ExtendedPrivateKey::Serialize(uint8_t *buffer) const {
    Util::IntToFourBytes(buffer, version);
    buffer[4] = depth;
    Util::IntToFourBytes(buffer + 5, parentFingerprint);
    Util::IntToFourBytes(buffer + 9, childNumber);
    chainCode.Serialize(buffer + 13);
    sk.Serialize(buffer + 13 + ChainCode::SIZE);
}

std::vector<uint8_t> ExtendedPrivateKey::Serialize() const {
    std::vector<uint8_t> data(SIZE);
    Serialize(data.data());
    return data;
}

// Destructors in PrivateKey and ChainCode handle cleaning of memory
ExtendedPrivateKey::~ExtendedPrivateKey() {}
} // end namespace bls
