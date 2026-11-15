// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING.MIT or https://opensource.org/license/MIT

#include "util.hpp"
#include "secure.h"

namespace bls {
void Util::Hash256(uint8_t* output, const uint8_t* message,
                    size_t messageLen) {
    blst_sha256(output, message, messageLen);
}

void Util::md_hmac(uint8_t *mac, const uint8_t *in, size_t in_len, const uint8_t *key,
    size_t key_len) {
    constexpr size_t block_size = 64;
    constexpr size_t md_len = 32;
    // The pads and the padded key are derived from the key, so they live in
    // the same secure memory as the private keys (see SecAlloc).
    util::SecVector<uint8_t> buf(block_size + md_len + block_size + in_len + block_size);
    uint8_t *opad = buf.data();                   // block_size + md_len
    uint8_t *ipad = opad + block_size + md_len;   // block_size + in_len
    uint8_t *_key = ipad + block_size + in_len;   // block_size

    if (key_len > block_size) {
        Hash256(_key, key, key_len);
        key_len = md_len;
    } else {
        memcpy(_key, key, key_len);
    }
    memset(_key + key_len, 0, block_size - key_len);
    key = _key;

    for (size_t i = 0; i < block_size; i++) {
        opad[i] = 0x5C ^ key[i];
        ipad[i] = 0x36 ^ key[i];
    }
    memcpy(ipad + block_size, in, in_len);
    Hash256(opad + block_size, ipad, block_size + in_len);
    Hash256(mac, opad, block_size + md_len);
}

std::string Util::HexStr(const uint8_t* data, size_t len) {
    std::stringstream s;
    s << std::hex;
    for (size_t i=0; i < len; ++i)
        s << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
    return s.str();
}

std::string Util::HexStr(const std::vector<uint8_t> &data) {
    std::stringstream s;
    s << std::hex;
    for (size_t i=0; i < data.size(); ++i)
        s << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
    return s.str();
}

uint8_t Util::char2int(const char input) {
    if(input >= '0' && input <= '9')
        return input - '0';
    if(input >= 'A' && input <= 'F')
        return input - 'A' + 10;
    if(input >= 'a' && input <= 'f')
        return input - 'a' + 10;
    throw std::invalid_argument("Invalid input string");
}

std::vector<uint8_t> Util::HexToBytes(const std::string hex) {
    if (hex.size() % 2 != 0) {
        throw std::invalid_argument("Invalid input string, length must be multple of 2");
    }
    std::vector<uint8_t> ret = std::vector<uint8_t>();
    size_t start_at = 0;
    if (hex.rfind("0x", 0) == 0 || hex.rfind("0X", 0) == 0) {
        start_at = 2;
    }

    for (size_t i = start_at; i < hex.size(); i += 2) {
        ret.push_back(char2int(hex[i]) * 16 + char2int(hex[i+1]));
    }
    return ret;
}

void Util::IntToFourBytes(uint8_t* result,
                           const uint32_t input) {
    for (size_t i = 0; i < 4; i++) {
        result[3 - i] = (input >> (i * 8));
    }
}

uint32_t Util::FourBytesToInt(const uint8_t* bytes) {
    uint32_t sum = 0;
    for (size_t i = 0; i < 4; i++) {
        uint32_t addend = static_cast<uint32_t>(bytes[i]) << (8 * (3 - i));
        sum += addend;
    }
    return sum;
}

bool Util::HasOnlyZeros(const Bytes& bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](uint8_t byte){ return byte == 0x00; });
}
} // namespace bls
