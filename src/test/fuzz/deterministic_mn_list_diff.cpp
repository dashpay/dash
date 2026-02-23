// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <evo/deterministicmns.h>
#include <evo/netinfo.h>
#include <pubkey.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <version.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

const TestingSetup* g_setup;

uint256 HashFromTag(uint64_t tag)
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

uint160 Uint160FromTag(uint64_t tag)
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

uint256 ConsumeUInt256(FuzzedDataProvider& fuzzed_data_provider)
{
    const std::vector<uint8_t> bytes = fuzzed_data_provider.ConsumeBytes<uint8_t>(uint256::size());
    if (bytes.size() != uint256::size()) return HashFromTag(0xA5A5A5A5ULL);

    uint256 value;
    std::copy(bytes.begin(), bytes.end(), value.begin());
    return value;
}

std::string AddressFromTag(uint64_t tag)
{
    const uint32_t a = 1 + (tag % 223);
    const uint32_t b = 1 + ((tag / 223) % 254);
    const uint32_t c = 1 + ((tag / (223 * 254)) % 254);
    const uint32_t d = 1 + ((tag / (223 * 254 * 254)) % 254);
    const uint32_t port = 1000 + (tag % 50000);
    return strprintf("%u.%u.%u.%u:%u", a, b, c, d, port);
}

CDeterministicMNCPtr MakeMasternode(const uint64_t internal_id, const uint64_t unique_tag, const int height)
{
    auto state = std::make_shared<CDeterministicMNState>();
    state->nVersion = ProTxVersion::LegacyBLS;
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
    if (!state->netInfo || state->netInfo->AddEntry(NetInfoPurpose::CORE_P2P, AddressFromTag(unique_tag)) != NetInfoStatus::Success) {
        throw std::runtime_error("failed to create deterministic masternode netInfo");
    }

    auto dmn = std::make_shared<CDeterministicMN>(internal_id, MnType::Regular);
    dmn->proTxHash = HashFromTag(unique_tag ^ 0x11111111ULL);
    dmn->collateralOutpoint = COutPoint(HashFromTag(unique_tag ^ 0x22222222ULL), static_cast<uint32_t>(unique_tag % 8));
    dmn->nOperatorReward = static_cast<uint16_t>(unique_tag % 10000);
    dmn->pdmnState = state;
    return dmn;
}

std::vector<uint256> GetProTxHashes(const CDeterministicMNList& mn_list)
{
    std::vector<uint256> hashes;
    mn_list.ForEachMN(/*onlyValid=*/false, [&](const CDeterministicMN& dmn) {
        hashes.push_back(dmn.proTxHash);
    });
    return hashes;
}

std::vector<uint64_t> GetInternalIds(const CDeterministicMNList& mn_list)
{
    std::vector<uint64_t> ids;
    mn_list.ForEachMN(/*onlyValid=*/false, [&](const CDeterministicMN& dmn) {
        ids.push_back(dmn.GetInternalId());
    });
    return ids;
}

bool DiffHasRequiredPointers(const CDeterministicMNListDiff& diff)
{
    for (const auto& dmn : diff.addedMNs) {
        if (!dmn || !dmn->pdmnState || !dmn->pdmnState->netInfo) {
            return false;
        }
    }
    return true;
}

struct SyntheticBlockIndex {
    CBlockIndex m_index{};
    uint256 m_hash;

    SyntheticBlockIndex(const int height, const uint256& hash) : m_hash(hash)
    {
        m_index.nHeight = height;
        m_index.phashBlock = &m_hash;
    }
};

} // namespace

void initialize_deterministic_mn_list_diff()
{
    static const auto testing_setup = MakeNoLogFileContext<const TestingSetup>(CBaseChainParams::REGTEST);
    g_setup = testing_setup.get();
}

FUZZ_TARGET(deterministic_mn_list_diff, .init = initialize_deterministic_mn_list_diff)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());

    const int source_height = fuzzed_data_provider.ConsumeIntegralInRange<int>(0, 10000);
    const uint256 source_hash = ConsumeUInt256(fuzzed_data_provider);
    CDeterministicMNList list_from(source_hash, source_height, 0);

    uint64_t next_internal_id = 1;
    uint64_t next_unique_tag = 1;
    const size_t initial_mn_count = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 8);
    for (size_t i = 0; i < initial_mn_count; ++i) {
        list_from.AddMN(MakeMasternode(next_internal_id++, next_unique_tag++, source_height), /*fBumpTotalCount=*/true);
    }

    CDeterministicMNList list_to(list_from);
    const size_t operation_count = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 24);
    for (size_t i = 0; i < operation_count; ++i) {
        const uint8_t op = fuzzed_data_provider.ConsumeIntegralInRange<uint8_t>(0, 2);
        if (op == 0) {
            list_to.AddMN(MakeMasternode(next_internal_id++, next_unique_tag++, source_height), /*fBumpTotalCount=*/true);
            continue;
        }

        const auto hashes = GetProTxHashes(list_to);
        if (hashes.empty()) continue;
        const auto index = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, hashes.size() - 1);

        if (op == 1) {
            list_to.RemoveMN(hashes[index]);
            continue;
        }

        const auto old_mn = list_to.GetMN(hashes[index]);
        if (!old_mn) continue;
        auto new_state = std::make_shared<CDeterministicMNState>(*old_mn->pdmnState);
        switch (fuzzed_data_provider.ConsumeIntegralInRange<uint8_t>(0, 3)) {
        case 0:
            new_state->nPoSePenalty = fuzzed_data_provider.ConsumeIntegralInRange<int>(0, 1000);
            break;
        case 1:
            new_state->nConsecutivePayments = fuzzed_data_provider.ConsumeIntegralInRange<int>(0, 1000);
            break;
        case 2:
            new_state->nLastPaidHeight = fuzzed_data_provider.ConsumeIntegralInRange<int>(0, 100000);
            break;
        case 3:
            new_state->confirmedHash = HashFromTag(next_unique_tag++ ^ 0x33333333ULL);
            break;
        }
        list_to.UpdateMN(*old_mn, new_state);
    }

    const int target_height = fuzzed_data_provider.ConsumeIntegralInRange<int>(0, 100000);
    const uint256 target_hash = ConsumeUInt256(fuzzed_data_provider);
    list_to.SetHeight(target_height);
    list_to.SetBlockHash(target_hash);
    const SyntheticBlockIndex target_index(target_height, target_hash);

    const CDeterministicMNListDiff diff = list_from.BuildDiff(list_to);
    CDeterministicMNList applied(list_from);
    applied.ApplyDiff(&target_index.m_index, diff);
    if (!applied.IsEqual(list_to)) {
        throw std::runtime_error("deterministic_mn_list_diff: BuildDiff/ApplyDiff invariant failed");
    }

    CDataStream ds(SER_NETWORK, PROTOCOL_VERSION);
    ds << diff;
    CDeterministicMNListDiff roundtrip_diff;
    ds >> roundtrip_diff;
    CDeterministicMNList applied_roundtrip(list_from);
    applied_roundtrip.ApplyDiff(&target_index.m_index, roundtrip_diff);
    if (!applied_roundtrip.IsEqual(list_to)) {
        throw std::runtime_error("deterministic_mn_list_diff: serialized diff invariant failed");
    }

    CDeterministicMNListDiff mutated_diff = diff;
    if (fuzzed_data_provider.ConsumeBool()) {
        mutated_diff.removedMns.insert(fuzzed_data_provider.ConsumeIntegral<uint64_t>());
    }
    if (fuzzed_data_provider.ConsumeBool()) {
        CDeterministicMNState before_state;
        before_state.nVersion = ProTxVersion::LegacyBLS;
        before_state.netInfo = NetInfoInterface::MakeNetInfo(before_state.nVersion);
        if (before_state.netInfo) {
            (void)before_state.netInfo->AddEntry(NetInfoPurpose::CORE_P2P, AddressFromTag(next_unique_tag++));
        }
        before_state.keyIDOwner = CKeyID(Uint160FromTag(next_unique_tag++ ^ 0x05050505ULL));
        CDeterministicMNState after_state(before_state);
        after_state.nPoSePenalty += 1;
        const auto ids = GetInternalIds(list_from);
        const uint64_t maybe_existing_id = !ids.empty() && fuzzed_data_provider.ConsumeBool()
            ? ids.front()
            : fuzzed_data_provider.ConsumeIntegral<uint64_t>();
        mutated_diff.updatedMNs.emplace(maybe_existing_id, CDeterministicMNStateDiff(before_state, after_state));
    }
    if (fuzzed_data_provider.ConsumeBool()) {
        mutated_diff.addedMNs.emplace_back(MakeMasternode(
            fuzzed_data_provider.ConsumeBool() && !mutated_diff.addedMNs.empty() ? mutated_diff.addedMNs.front()->GetInternalId() : next_internal_id++,
            next_unique_tag++,
            source_height));
    }

    if (DiffHasRequiredPointers(mutated_diff)) {
        try {
            CDeterministicMNList applied_mutated(list_from);
            applied_mutated.ApplyDiff(&target_index.m_index, mutated_diff);
        } catch (const std::exception&) {
        }
    }

    CDeterministicMNListDiff random_diff;
    CDataStream ds_random(fuzzed_data_provider.ConsumeRemainingBytes<uint8_t>(), SER_NETWORK, PROTOCOL_VERSION);
    try {
        ds_random >> random_diff;
        if (DiffHasRequiredPointers(random_diff)) {
            try {
                CDeterministicMNList applied_random(list_from);
                applied_random.ApplyDiff(&target_index.m_index, random_diff);
            } catch (const std::exception&) {
            }
        }
    } catch (const std::exception&) {
    }
}
