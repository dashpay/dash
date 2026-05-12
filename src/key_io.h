// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_KEY_IO_H
#define BITCOIN_KEY_IO_H

#include <key.h>
#include <pubkey.h>
#include <script/standard.h>

#include <string>

class CChainParams;

CKey DecodeSecret(const std::string& str);
std::string EncodeSecret(const CKey& key);

CExtKey DecodeExtKey(const std::string& str);
std::string EncodeExtKey(const CExtKey& extkey);
CExtPubKey DecodeExtPubKey(const std::string& str);
std::string EncodeExtPubKey(const CExtPubKey& extpubkey);

std::string EncodeDestination(const CTxDestination& dest);
CTxDestination DecodeDestination(const std::string& str);
CTxDestination DecodeDestination(const std::string& str, std::string& error_msg);
bool IsValidDestinationString(const std::string& str);
bool IsValidDestinationString(const std::string& str, const CChainParams& params);

/**
 * DIP-18 Dash Platform addresses (bech32m).
 *
 * Platform addresses decode to a 20-byte HASH160 prefixed by a type byte:
 *   0xb0 -> Platform P2PKH (addresses of the form dash1k... / tdash1k...)
 *   0x80 -> Platform P2SH  (addresses of the form dash1s... / tdash1s...)
 *
 * Unlike base58 Dash addresses, Platform destinations have no on-chain
 * scriptPubKey: they are only valid as credit output recipients of an
 * asset-lock special transaction (see DIP-27 and src/evo/assetlocktx.h).
 */
struct PlatformP2PKHDestination : public BaseHash<uint160>
{
    PlatformP2PKHDestination() = default;
    explicit PlatformP2PKHDestination(const uint160& hash) : BaseHash(hash) {}
};

struct PlatformP2SHDestination : public BaseHash<uint160>
{
    PlatformP2SHDestination() = default;
    explicit PlatformP2SHDestination(const uint160& hash) : BaseHash(hash) {}
};

using PlatformDestination = std::variant<CNoDestination, PlatformP2PKHDestination, PlatformP2SHDestination>;

bool IsValidPlatformDestination(const PlatformDestination& dest);
std::string EncodePlatformDestination(const PlatformDestination& dest);
PlatformDestination DecodePlatformDestination(const std::string& str);
PlatformDestination DecodePlatformDestination(const std::string& str, std::string& error_str);
PlatformDestination DecodePlatformDestination(const std::string& str, const CChainParams& params, std::string& error_str);

#endif // BITCOIN_KEY_IO_H
