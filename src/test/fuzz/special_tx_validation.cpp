// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <consensus/validation.h>
#include <evo/chainhelper.h>
#include <evo/specialtxman.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/util/mining.h>
#include <test/util/setup_common.h>
#include <util/check.h>
#include <validation.h>
#include <validationinterface.h>
#include <version.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace {
const TestingSetup* g_setup;
} // namespace

void initialize_special_tx_validation()
{
    static const auto testing_setup = MakeNoLogFileContext<const TestingSetup>(
        CBaseChainParams::REGTEST, {"-dip3params=2:2", "-testactivationheight=v20@2", "-testactivationheight=mn_rr@2"});
    g_setup = testing_setup.get();
    // Mine blocks past the activation height so DIP3/v20/mn_rr are active
    for (int i = 0; i < 3; ++i) {
        MineBlock(g_setup->m_node, CScript() << OP_TRUE);
    }
    SyncWithValidationInterfaceQueue();
}

static CMutableTransaction DeserializeCandidateTx(FuzzedDataProvider& fuzzed_data_provider)
{
    CMutableTransaction tx;
    bool deserialized{false};

    CDataStream ds(
        fuzzed_data_provider.ConsumeBytes<uint8_t>(fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 4096)),
        SER_NETWORK, INIT_PROTO_VERSION);
    try {
        int nVersion;
        ds >> nVersion;
        ds.SetVersion(nVersion);
        ds >> tx;
        deserialized = true;
    } catch (const std::ios_base::failure&) {
    }

    if (!deserialized) {
        tx.nVersion = fuzzed_data_provider.ConsumeBool() ? CTransaction::CURRENT_VERSION : CTransaction::SPECIAL_VERSION;
        tx.nType = fuzzed_data_provider.ConsumeIntegral<uint16_t>();
        tx.nLockTime = fuzzed_data_provider.ConsumeIntegral<uint32_t>();
        tx.vExtraPayload = fuzzed_data_provider.ConsumeBytes<uint8_t>(
            fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 256));
    }

    return tx;
}

FUZZ_TARGET(special_tx_validation, .init = initialize_special_tx_validation)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    const CMutableTransaction base_candidate = DeserializeCandidateTx(fuzzed_data_provider);

    static constexpr std::array<uint16_t, 9> SPECIAL_TYPES{
        TRANSACTION_PROVIDER_REGISTER,
        TRANSACTION_PROVIDER_UPDATE_SERVICE,
        TRANSACTION_PROVIDER_UPDATE_REGISTRAR,
        TRANSACTION_PROVIDER_UPDATE_REVOKE,
        TRANSACTION_COINBASE,
        TRANSACTION_QUORUM_COMMITMENT,
        TRANSACTION_MNHF_SIGNAL,
        TRANSACTION_ASSET_LOCK,
        TRANSACTION_ASSET_UNLOCK,
    };

    auto* const special_tx = g_setup->m_node.chain_helper->special_tx.get();
    Assert(special_tx != nullptr);

    LOCK(::cs_main);
    const CBlockIndex* const pindex_prev = g_setup->m_node.chainman->ActiveChain().Tip();
    if (pindex_prev == nullptr) return;
    const CCoinsViewCache& coins_view = g_setup->m_node.chainman->ActiveChainstate().CoinsTip();

    const auto iterations = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(1, SPECIAL_TYPES.size());
    for (size_t i = 0; i < iterations; ++i) {
        CMutableTransaction mut_tx{base_candidate};
        mut_tx.nVersion = CTransaction::SPECIAL_VERSION;
        mut_tx.nType = SPECIAL_TYPES[i];

        if (fuzzed_data_provider.ConsumeBool()) {
            mut_tx.vExtraPayload = fuzzed_data_provider.ConsumeBytes<uint8_t>(
                fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 512));
        }

        TxValidationState state;
        const bool accepted{
            special_tx->CheckSpecialTx(CTransaction{mut_tx}, pindex_prev, coins_view,
                                       fuzzed_data_provider.ConsumeBool(), state)};
        Assert(accepted == state.IsValid());
    }
}
