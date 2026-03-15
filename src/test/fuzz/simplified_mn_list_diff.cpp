// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls/bls.h>
#include <evo/deterministicmns.h>
#include <evo/netinfo.h>
#include <evo/simplifiedmns.h>
#include <evo/smldiff.h>
#include <llmq/commitment.h>
#include <script/script.h>
#include <script/standard.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/fuzz/util_dash.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <version.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

const TestingSetup* g_setup;

std::vector<std::byte> SerializeMerkleTree(const CPartialMerkleTree& tree)
{
    CDataStream ds(SER_NETWORK, PROTOCOL_VERSION);
    ds << tree;
    return {ds.begin(), ds.end()};
}

bool MutableTxEqual(const CMutableTransaction& lhs, const CMutableTransaction& rhs)
{
    return lhs.vin == rhs.vin &&
           lhs.vout == rhs.vout &&
           lhs.nVersion == rhs.nVersion &&
           lhs.nType == rhs.nType &&
           lhs.nLockTime == rhs.nLockTime &&
           lhs.vExtraPayload == rhs.vExtraPayload;
}

} // namespace

void initialize_simplified_mn_list_diff()
{
    static const auto testing_setup = MakeNoLogFileContext<const TestingSetup>(CBaseChainParams::REGTEST);
    g_setup = testing_setup.get();
}

FUZZ_TARGET(simplified_mn_list_diff, .init = initialize_simplified_mn_list_diff)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());

    const int source_height = fuzzed_data_provider.ConsumeIntegralInRange<int>(0, 10000);
    const uint256 source_hash = ConsumeUInt256(fuzzed_data_provider);
    CDeterministicMNList list_from(source_hash, source_height, 0);

    uint64_t next_internal_id = 1;
    uint64_t next_unique_tag = 1;
    const size_t initial_mn_count = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 8);
    for (size_t i = 0; i < initial_mn_count; ++i) {
        const MnType mn_type = fuzzed_data_provider.ConsumeBool() ? MnType::Evo : MnType::Regular;
        list_from.AddMN(MakeMasternode(next_internal_id++, next_unique_tag++, source_height, mn_type), /*fBumpTotalCount=*/true);
    }

    CDeterministicMNList list_to(list_from);
    const size_t operation_count = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 24);
    for (size_t i = 0; i < operation_count; ++i) {
        const uint8_t op = fuzzed_data_provider.ConsumeIntegralInRange<uint8_t>(0, 2);
        if (op == 0) {
            const MnType mn_type = fuzzed_data_provider.ConsumeBool() ? MnType::Evo : MnType::Regular;
            list_to.AddMN(MakeMasternode(next_internal_id++, next_unique_tag++, source_height, mn_type), /*fBumpTotalCount=*/true);
            continue;
        }

        const auto hashes = GetProTxHashes(list_to);
        if (hashes.empty()) continue;
        const size_t index = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, hashes.size() - 1);

        if (op == 1) {
            list_to.RemoveMN(hashes[index]);
            continue;
        }

        const auto old_mn = list_to.GetMN(hashes[index]);
        if (!old_mn) continue;
        auto new_state = std::make_shared<CDeterministicMNState>(*old_mn->pdmnState);
        switch (fuzzed_data_provider.ConsumeIntegralInRange<uint8_t>(0, 9)) {
        case 0:
            new_state->confirmedHash = HashFromTag(next_unique_tag++ ^ 0x33333333ULL);
            break;
        case 1: {
            CBLSSecretKey sk;
            sk.MakeNewKey();
            new_state->nVersion = ProTxVersion::BasicBLS;
            new_state->pubKeyOperator.Set(sk.GetPublicKey(), /*specificLegacyScheme=*/false);
            break;
        }
        case 2:
            new_state->keyIDVoting = CKeyID(Uint160FromTag(next_unique_tag++ ^ 0x06060606ULL));
            break;
        case 3: {
            auto net_info = NetInfoInterface::MakeNetInfo(new_state->nVersion);
            if (net_info && net_info->AddEntry(NetInfoPurpose::CORE_P2P, AddressFromTag(next_unique_tag++)) == NetInfoStatus::Success) {
                new_state->netInfo = std::move(net_info);
            }
            break;
        }
        case 4:
            new_state->scriptPayout = CScript() << OP_DUP << OP_HASH160 << ToByteVector(Uint160FromTag(next_unique_tag++)) << OP_EQUALVERIFY << OP_CHECKSIG;
            break;
        case 5:
            new_state->scriptOperatorPayout = CScript() << OP_DUP << OP_HASH160 << ToByteVector(Uint160FromTag(next_unique_tag++ ^ 0x08080808ULL)) << OP_EQUALVERIFY << OP_CHECKSIG;
            break;
        case 6:
            new_state->BanIfNotBanned(fuzzed_data_provider.ConsumeIntegralInRange<int>(0, 100000));
            break;
        case 7:
            new_state->platformNodeID = Uint160FromTag(next_unique_tag++ ^ 0x12121212ULL);
            break;
        case 8:
            new_state->platformHTTPPort = fuzzed_data_provider.ConsumeIntegral<uint16_t>();
            break;
        case 9:
            new_state->nRegisteredHeight = fuzzed_data_provider.ConsumeIntegralInRange<int>(0, 10000);
            break;
        }
        list_to.UpdateMN(*old_mn, new_state);
    }

    list_to.SetBlockHash(ConsumeUInt256(fuzzed_data_provider));
    list_to.SetHeight(fuzzed_data_provider.ConsumeIntegralInRange<int>(0, 100000));

    CSimplifiedMNListDiff diff;
    diff.baseBlockHash = list_from.GetBlockHash();
    diff.blockHash = list_to.GetBlockHash();
    diff.cbTx = CMutableTransaction{};
    diff.cbTxMerkleTree = CPartialMerkleTree{};

    list_to.ForEachMN(/*onlyValid=*/false, [&](const auto& to_mn) {
        const auto from_mn = list_from.GetMN(to_mn.proTxHash);
        if (!from_mn || to_mn.to_sml_entry() != from_mn->to_sml_entry()) {
            diff.mnList.emplace_back(to_mn.to_sml_entry());
        }
    });
    list_from.ForEachMN(/*onlyValid=*/false, [&](const auto& from_mn) {
        if (!list_to.GetMN(from_mn.proTxHash)) {
            diff.deletedMNs.emplace_back(from_mn.proTxHash);
        }
    });

    CDataStream ds(SER_NETWORK, PROTOCOL_VERSION);
    ds << diff;
    CSimplifiedMNListDiff roundtrip;
    ds >> roundtrip;

    if (roundtrip.baseBlockHash != diff.baseBlockHash || roundtrip.blockHash != diff.blockHash ||
        !MutableTxEqual(roundtrip.cbTx, diff.cbTx) ||
        SerializeMerkleTree(roundtrip.cbTxMerkleTree) != SerializeMerkleTree(diff.cbTxMerkleTree) ||
        roundtrip.deletedMNs != diff.deletedMNs || roundtrip.mnList.size() != diff.mnList.size() ||
        roundtrip.nVersion != diff.nVersion || roundtrip.deletedQuorums != diff.deletedQuorums ||
        roundtrip.newQuorums.size() != diff.newQuorums.size() || roundtrip.quorumsCLSigs != diff.quorumsCLSigs) {
        throw std::runtime_error("simplified_mn_list_diff: serialized fields mismatch");
    }
    for (size_t i = 0; i < diff.mnList.size(); ++i) {
        if (roundtrip.mnList[i] != diff.mnList[i]) {
            throw std::runtime_error("simplified_mn_list_diff: mnList mismatch");
        }
    }

    CDataStream ds_random(fuzzed_data_provider.ConsumeRemainingBytes<uint8_t>(), SER_NETWORK, PROTOCOL_VERSION);
    try {
        CSimplifiedMNListDiff random_diff;
        ds_random >> random_diff;
    } catch (const std::exception&) {
    }
}
