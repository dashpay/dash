// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_FUZZ_UTIL_DASH_H
#define BITCOIN_TEST_FUZZ_UTIL_DASH_H

#include <bls/bls.h>
#include <evo/deterministicmns.h>
#include <evo/netinfo.h>
#include <pubkey.h>
#include <script/script.h>
#include <script/standard.h>
#include <tinyformat.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

inline uint256 HashFromTag(uint64_t tag)
{
    uint256 hash;
    std::array<uint8_t, 8> seed{};
    WriteLE64(seed.data(), tag);
    for (size_t i = 0; i < hash.size(); ++i) {
        hash.begin()[i] = static_cast<uint8_t>(seed[i % seed.size()] + static_cast<uint8_t>(i * 13));
    }
    if (hash.IsNull()) {
        hash.begin()[0] = 1;
    }
    return hash;
}

inline uint160 Uint160FromTag(uint64_t tag)
{
    uint160 value;
    std::array<uint8_t, 8> seed{};
    WriteLE64(seed.data(), tag);
    for (size_t i = 0; i < value.size(); ++i) {
        value.begin()[i] = static_cast<uint8_t>(seed[i % seed.size()] + static_cast<uint8_t>(i * 7 + 1));
    }
    if (value.IsNull()) {
        value.begin()[0] = 1;
    }
    return value;
}

inline std::string AddressFromTag(uint64_t tag)
{
    const uint32_t a = 1 + (tag % 223);
    const uint32_t b = 1 + ((tag / 223) % 254);
    const uint32_t c = 1 + ((tag / (223 * 254)) % 254);
    const uint32_t d = 1 + ((tag / (223 * 254 * 254)) % 254);
    const uint32_t port = 1000 + (tag % 50000);
    return strprintf("%u.%u.%u.%u:%u", a, b, c, d, port);
}

inline CDeterministicMNCPtr MakeMasternode(const uint64_t internal_id, const uint64_t unique_tag, const int height, const MnType mn_type = MnType::Regular)
{
    auto state = std::make_shared<CDeterministicMNState>();
    state->nVersion = mn_type == MnType::Evo ? ProTxVersion::BasicBLS : ProTxVersion::LegacyBLS;
    state->nRegisteredHeight = height;
    state->nLastPaidHeight = height > 0 ? height - 1 : 0;
    state->nConsecutivePayments = static_cast<int>(unique_tag % 4);
    state->nPoSePenalty = static_cast<int>(unique_tag % 8);
    state->nPoSeRevivedHeight = -1;
    state->nRevocationReason = CProUpRevTx::REASON_NOT_SPECIFIED;
    state->confirmedHash = HashFromTag(unique_tag ^ 0x01010101ULL);
    state->confirmedHashWithProRegTxHash = HashFromTag(unique_tag ^ 0x02020202ULL);
    state->keyIDOwner = CKeyID(Uint160FromTag(unique_tag ^ 0x03030303ULL));
    state->keyIDVoting = CKeyID(Uint160FromTag(unique_tag ^ 0x04040404ULL));
    state->netInfo = NetInfoInterface::MakeNetInfo(state->nVersion);
    if (!state->netInfo ||
        state->netInfo->AddEntry(NetInfoPurpose::CORE_P2P, AddressFromTag(unique_tag)) != NetInfoStatus::Success) {
        throw std::runtime_error("failed to create deterministic masternode netInfo");
    }

    if (mn_type == MnType::Evo) {
        state->platformNodeID = Uint160FromTag(unique_tag ^ 0xABABABABULL);
        state->platformP2PPort = static_cast<uint16_t>(10000 + (unique_tag % 50000));
        state->platformHTTPPort = static_cast<uint16_t>(11000 + (unique_tag % 50000));
    }

    auto dmn = std::make_shared<CDeterministicMN>(internal_id, mn_type);
    dmn->proTxHash = HashFromTag(unique_tag ^ 0x11111111ULL);
    dmn->collateralOutpoint = COutPoint(HashFromTag(unique_tag ^ 0x22222222ULL), static_cast<uint32_t>(unique_tag % 8));
    dmn->nOperatorReward = static_cast<uint16_t>(unique_tag % 10000);
    dmn->pdmnState = state;
    return dmn;
}

inline std::vector<uint256> GetProTxHashes(const CDeterministicMNList& mn_list)
{
    std::vector<uint256> hashes;
    mn_list.ForEachMN(/*onlyValid=*/false, [&](const CDeterministicMN& dmn) { hashes.push_back(dmn.proTxHash); });
    return hashes;
}

#endif // BITCOIN_TEST_FUZZ_UTIL_DASH_H
