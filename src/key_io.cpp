// Copyright (c) 2014-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <key_io.h>

#include <base58.h>
#include <bech32.h>
#include <chainparams.h>
#include <util/strencodings.h>

#include <algorithm>
#include <assert.h>
#include <string.h>

namespace {
class DestinationEncoder
{
private:
    const CChainParams& m_params;

public:
    explicit DestinationEncoder(const CChainParams& params) : m_params(params) {}

    std::string operator()(const PKHash& id) const
    {
        std::vector<unsigned char> data = m_params.Base58Prefix(CChainParams::PUBKEY_ADDRESS);
        data.insert(data.end(), id.begin(), id.end());
        return EncodeBase58Check(data);
    }

    std::string operator()(const ScriptHash& id) const
    {
        std::vector<unsigned char> data = m_params.Base58Prefix(CChainParams::SCRIPT_ADDRESS);
        data.insert(data.end(), id.begin(), id.end());
        return EncodeBase58Check(data);
    }

    std::string operator()(const CNoDestination& no) const { return {}; }
};

CTxDestination DecodeDestination(const std::string& str, const CChainParams& params, std::string& error_str)
{
    std::vector<unsigned char> data;
    uint160 hash;
    error_str = "";
    if (DecodeBase58Check(str, data, 21)) {
        // base58-encoded Dash addresses.
        // Public-key-hash-addresses have version 76 (or 140 testnet).
        // The data vector contains RIPEMD160(SHA256(pubkey)), where pubkey is the serialized public key.
        const std::vector<unsigned char>& pubkey_prefix = params.Base58Prefix(CChainParams::PUBKEY_ADDRESS);
        if (data.size() == hash.size() + pubkey_prefix.size() && std::equal(pubkey_prefix.begin(), pubkey_prefix.end(), data.begin())) {
            std::copy(data.begin() + pubkey_prefix.size(), data.end(), hash.begin());
            return PKHash(hash);
        }
        // Script-hash-addresses have version 16 (or 19 testnet).
        // The data vector contains RIPEMD160(SHA256(cscript)), where cscript is the serialized redemption script.
        const std::vector<unsigned char>& script_prefix = params.Base58Prefix(CChainParams::SCRIPT_ADDRESS);
        if (data.size() == hash.size() + script_prefix.size() && std::equal(script_prefix.begin(), script_prefix.end(), data.begin())) {
            std::copy(data.begin() + script_prefix.size(), data.end(), hash.begin());
            return ScriptHash(hash);
        }

        // Set potential error message.
        error_str = "Invalid prefix for Base58-encoded address";
    }
    // Set error message if address can't be interpreted as Base58.
    if (error_str.empty()) error_str = "Invalid address format";

    return CNoDestination();
}
} // namespace

CKey DecodeSecret(const std::string& str)
{
    CKey key;
    std::vector<unsigned char> data;
    if (DecodeBase58Check(str, data, 34)) {
        const std::vector<unsigned char>& privkey_prefix = Params().Base58Prefix(CChainParams::SECRET_KEY);
        if ((data.size() == 32 + privkey_prefix.size() || (data.size() == 33 + privkey_prefix.size() && data.back() == 1)) &&
            std::equal(privkey_prefix.begin(), privkey_prefix.end(), data.begin())) {
            bool compressed = data.size() == 33 + privkey_prefix.size();
            key.Set(data.begin() + privkey_prefix.size(), data.begin() + privkey_prefix.size() + 32, compressed);
        }
    }
    if (!data.empty()) {
        memory_cleanse(data.data(), data.size());
    }
    return key;
}

std::string EncodeSecret(const CKey& key)
{
    assert(key.IsValid());
    std::vector<unsigned char> data = Params().Base58Prefix(CChainParams::SECRET_KEY);
    data.insert(data.end(), key.begin(), key.end());
    if (key.IsCompressed()) {
        data.push_back(1);
    }
    std::string ret = EncodeBase58Check(data);
    memory_cleanse(data.data(), data.size());
    return ret;
}

CExtPubKey DecodeExtPubKey(const std::string& str)
{
    CExtPubKey key;
    std::vector<unsigned char> data;
    if (DecodeBase58Check(str, data, 78)) {
        const std::vector<unsigned char>& prefix = Params().Base58Prefix(CChainParams::EXT_PUBLIC_KEY);
        if (data.size() == BIP32_EXTKEY_SIZE + prefix.size() && std::equal(prefix.begin(), prefix.end(), data.begin())) {
            key.Decode(data.data() + prefix.size());
        }
    }
    return key;
}

std::string EncodeExtPubKey(const CExtPubKey& key)
{
    std::vector<unsigned char> data = Params().Base58Prefix(CChainParams::EXT_PUBLIC_KEY);
    size_t size = data.size();
    data.resize(size + BIP32_EXTKEY_SIZE);
    key.Encode(data.data() + size);
    std::string ret = EncodeBase58Check(data);
    return ret;
}

CExtKey DecodeExtKey(const std::string& str)
{
    CExtKey key;
    std::vector<unsigned char> data;
    if (DecodeBase58Check(str, data, 78)) {
        const std::vector<unsigned char>& prefix = Params().Base58Prefix(CChainParams::EXT_SECRET_KEY);
        if (data.size() == BIP32_EXTKEY_SIZE + prefix.size() && std::equal(prefix.begin(), prefix.end(), data.begin())) {
            key.Decode(data.data() + prefix.size());
        }
    }
    return key;
}

std::string EncodeExtKey(const CExtKey& key)
{
    std::vector<unsigned char> data = Params().Base58Prefix(CChainParams::EXT_SECRET_KEY);
    size_t size = data.size();
    data.resize(size + BIP32_EXTKEY_SIZE);
    key.Encode(data.data() + size);
    std::string ret = EncodeBase58Check(data);
    if (!data.empty()) {
        memory_cleanse(data.data(), data.size());
    }
    return ret;
}

std::string EncodeDestination(const CTxDestination& dest)
{
    return std::visit(DestinationEncoder(Params()), dest);
}

CTxDestination DecodeDestination(const std::string& str, std::string& error_msg)
{
    return DecodeDestination(str, Params(), error_msg);
}

CTxDestination DecodeDestination(const std::string& str)
{
    std::string error_msg;
    return DecodeDestination(str, error_msg);
}

bool IsValidDestinationString(const std::string& str, const CChainParams& params)
{
    std::string error_msg;
    return IsValidDestination(DecodeDestination(str, params, error_msg));
}

bool IsValidDestinationString(const std::string& str)
{
    return IsValidDestinationString(str, Params());
}

namespace {
constexpr uint8_t DIP18_TYPE_BYTE_P2PKH = 0xb0;
constexpr uint8_t DIP18_TYPE_BYTE_P2SH  = 0x80;
constexpr size_t  DIP18_PAYLOAD_SIZE    = 21; // 1 type byte + 20-byte HASH160

std::string EncodePlatformBech32m(const CChainParams& params, uint8_t type_byte, const BaseHash<uint160>& hash)
{
    std::vector<uint8_t> payload;
    payload.reserve(DIP18_PAYLOAD_SIZE);
    payload.push_back(type_byte);
    payload.insert(payload.end(), hash.begin(), hash.end());
    std::vector<uint8_t> values;
    values.reserve(((DIP18_PAYLOAD_SIZE * 8) + 4) / 5);
    ConvertBits<8, 5, true>([&](uint8_t v) { values.push_back(v); }, payload.begin(), payload.end());
    return bech32::Encode(bech32::Encoding::BECH32M, params.Bech32PlatformHRP(), values);
}

class PlatformDestinationEncoder
{
private:
    const CChainParams& m_params;

public:
    explicit PlatformDestinationEncoder(const CChainParams& params) : m_params(params) {}

    std::string operator()(const PlatformP2PKHDestination& id) const
    {
        return EncodePlatformBech32m(m_params, DIP18_TYPE_BYTE_P2PKH, id);
    }
    std::string operator()(const PlatformP2SHDestination& id) const
    {
        return EncodePlatformBech32m(m_params, DIP18_TYPE_BYTE_P2SH, id);
    }
    std::string operator()(const CNoDestination&) const { return {}; }
};
} // namespace

bool IsValidPlatformDestination(const PlatformDestination& dest)
{
    return !std::holds_alternative<CNoDestination>(dest);
}

std::string EncodePlatformDestination(const PlatformDestination& dest)
{
    return std::visit(PlatformDestinationEncoder(Params()), dest);
}

PlatformDestination DecodePlatformDestination(const std::string& str, const CChainParams& params, std::string& error_str)
{
    error_str.clear();
    const bech32::DecodeResult dec = bech32::Decode(str);
    if (dec.encoding == bech32::Encoding::INVALID) {
        error_str = "Invalid bech32m encoding";
        return CNoDestination();
    }
    if (dec.encoding != bech32::Encoding::BECH32M) {
        error_str = "DIP-18 Platform addresses require bech32m checksum";
        return CNoDestination();
    }
    if (dec.hrp != params.Bech32PlatformHRP()) {
        error_str = "Invalid Platform HRP for the selected network";
        return CNoDestination();
    }
    std::vector<uint8_t> payload;
    payload.reserve((dec.data.size() * 5) / 8);
    if (!ConvertBits<5, 8, false>([&](uint8_t b) { payload.push_back(b); }, dec.data.begin(), dec.data.end())) {
        error_str = "Invalid Platform address payload encoding";
        return CNoDestination();
    }
    if (payload.size() != DIP18_PAYLOAD_SIZE) {
        error_str = "Invalid Platform address payload length";
        return CNoDestination();
    }
    uint160 hash;
    std::copy(payload.begin() + 1, payload.end(), hash.begin());
    switch (payload[0]) {
    case DIP18_TYPE_BYTE_P2PKH:
        return PlatformP2PKHDestination(hash);
    case DIP18_TYPE_BYTE_P2SH:
        return PlatformP2SHDestination(hash);
    }
    error_str = "Unknown DIP-18 type byte";
    return CNoDestination();
}

PlatformDestination DecodePlatformDestination(const std::string& str, std::string& error_str)
{
    return DecodePlatformDestination(str, Params(), error_str);
}

PlatformDestination DecodePlatformDestination(const std::string& str)
{
    std::string error_str;
    return DecodePlatformDestination(str, error_str);
}
