// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/validation.h>
#include <evo/assetlocktx.h>
#include <evo/specialtx.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <version.h>

#include <cassert>
#include <cstdint>
#include <exception>
#include <limits>
#include <vector>

namespace {
CScript ConsumeRandomScript(FuzzedDataProvider& fuzzed_data_provider, size_t max_size = 64)
{
    const std::vector<uint8_t> raw_script = fuzzed_data_provider.ConsumeBytes<uint8_t>(
        fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, max_size));
    return CScript(raw_script.begin(), raw_script.end());
}

CScript ConsumeP2PKHScript(FuzzedDataProvider& fuzzed_data_provider)
{
    const std::vector<uint8_t> key_hash = fuzzed_data_provider.ConsumeBytes<uint8_t>(20);
    return CScript() << OP_DUP << OP_HASH160 << key_hash << OP_EQUALVERIFY << OP_CHECKSIG;
}

CAmount ConsumeAmount(FuzzedDataProvider& fuzzed_data_provider)
{
    return fuzzed_data_provider.ConsumeIntegralInRange<CAmount>(-1, static_cast<CAmount>(MAX_MONEY) + 1);
}

std::vector<uint8_t> BuildAssetLockPayload(const uint8_t version, const std::vector<CTxOut>& credit_outputs)
{
    CDataStream ds(SER_NETWORK, PROTOCOL_VERSION);
    ds << version;
    ds << credit_outputs;
    return {UCharCast(ds.data()), UCharCast(ds.data() + ds.size())};
}

CBLSSignature ConsumeBLSSignature(FuzzedDataProvider& fuzzed_data_provider)
{
    CBLSSignature sig;
    auto bytes = fuzzed_data_provider.ConsumeBytes<uint8_t>(CBLSSignature::SerSize);
    bytes.resize(CBLSSignature::SerSize);
    sig.SetBytes(bytes, fuzzed_data_provider.ConsumeBool());
    return sig;
}

void initialize_asset_lock_unlock() { SelectParams(CBaseChainParams::REGTEST); }
} // namespace

FUZZ_TARGET(asset_lock_tx, .init = initialize_asset_lock_unlock)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());

    CMutableTransaction tx;
    tx.nVersion = CTransaction::SPECIAL_VERSION;
    tx.nType = TRANSACTION_ASSET_LOCK;

    std::vector<CTxOut> credit_outputs;
    const size_t num_credit_outputs = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(1, 4);
    CAmount credit_outputs_total{0};
    for (size_t i = 0; i < num_credit_outputs; ++i) {
        const CAmount amount = fuzzed_data_provider.ConsumeIntegralInRange<CAmount>(1, 10 * COIN);
        assert(MoneyRange(amount));
        assert(MoneyRange(credit_outputs_total + amount));
        credit_outputs_total += amount;
        credit_outputs.emplace_back(amount, ConsumeP2PKHScript(fuzzed_data_provider));
    }

    SetTxPayload(tx, CAssetLockPayload(credit_outputs));

    tx.vout.emplace_back(credit_outputs_total, CScript() << OP_RETURN << std::vector<uint8_t>{});

    const size_t num_regular_outputs = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 4);
    for (size_t i = 0; i < num_regular_outputs; ++i) {
        tx.vout.emplace_back(ConsumeAmount(fuzzed_data_provider), ConsumeRandomScript(fuzzed_data_provider));
    }

    const uint8_t scenario = fuzzed_data_provider.ConsumeIntegralInRange<uint8_t>(0, 11);
    switch (scenario) {
    case 0: // bad-assetlocktx-type
        tx.nType = fuzzed_data_provider.ConsumeBool() ? TRANSACTION_ASSET_UNLOCK : TRANSACTION_NORMAL;
        break;
    case 1: // bad-assetlocktx-non-empty-return
        tx.vout[0].scriptPubKey = CScript() << OP_RETURN
                                            << fuzzed_data_provider.ConsumeBytes<uint8_t>(
                                                   fuzzed_data_provider.ConsumeIntegralInRange<size_t>(1, 16));
        break;
    case 2: // bad-assetlocktx-opreturn-outofrange
        tx.vout[0].nValue = fuzzed_data_provider.ConsumeBool() ? 0 : static_cast<CAmount>(MAX_MONEY) + 1;
        break;
    case 3: // bad-assetlocktx-multiple-return
        tx.vout.emplace_back(1, CScript() << OP_RETURN << std::vector<uint8_t>{});
        break;
    case 4: // bad-assetlocktx-no-return
        tx.vout[0].scriptPubKey = ConsumeRandomScript(fuzzed_data_provider);
        if (!tx.vout[0].scriptPubKey.empty() && tx.vout[0].scriptPubKey[0] == OP_RETURN) {
            tx.vout[0].scriptPubKey = CScript() << OP_1;
        }
        break;
    case 5: // bad-assetlocktx-payload
        tx.vExtraPayload = fuzzed_data_provider.ConsumeBytes<uint8_t>(
            fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 8));
        break;
    case 6: // bad-assetlocktx-version
        tx.vExtraPayload = BuildAssetLockPayload(fuzzed_data_provider.ConsumeBool() ? uint8_t{0}
                                                                                    : std::numeric_limits<uint8_t>::max(),
                                                 credit_outputs);
        break;
    case 7: // bad-assetlocktx-emptycreditoutputs
        tx.vExtraPayload = BuildAssetLockPayload(CAssetLockPayload::CURRENT_VERSION, {});
        break;
    case 8: { // bad-assetlocktx-credit-outofrange
        auto bad_credit_outputs = credit_outputs;
        bad_credit_outputs[0].nValue = fuzzed_data_provider.ConsumeBool() ? 0 : static_cast<CAmount>(MAX_MONEY) + 1;
        tx.vExtraPayload = BuildAssetLockPayload(CAssetLockPayload::CURRENT_VERSION, bad_credit_outputs);
        break;
    }
    case 9: { // bad-assetlocktx-pubKeyHash
        auto bad_credit_outputs = credit_outputs;
        bad_credit_outputs[0].scriptPubKey = ConsumeRandomScript(fuzzed_data_provider);
        if (bad_credit_outputs[0].scriptPubKey.IsPayToPublicKeyHash()) {
            bad_credit_outputs[0].scriptPubKey = CScript() << OP_1;
        }
        tx.vExtraPayload = BuildAssetLockPayload(CAssetLockPayload::CURRENT_VERSION, bad_credit_outputs);
        break;
    }
    case 10: { // bad-assetlocktx-creditamount
        auto bad_credit_outputs = credit_outputs;
        bad_credit_outputs[0].nValue += fuzzed_data_provider.ConsumeBool() ? 1 : -1;
        tx.vExtraPayload = BuildAssetLockPayload(CAssetLockPayload::CURRENT_VERSION, bad_credit_outputs);
        break;
    }
    case 11: // Valid path
        break;
    }

    TxValidationState state;
    const bool result = CheckAssetLockTx(CTransaction(tx), state);
    assert(result == state.IsValid());
}

FUZZ_TARGET(asset_lock_tx_raw, .init = initialize_asset_lock_unlock)
{
    CDataStream ds(buffer, SER_NETWORK, INIT_PROTO_VERSION);
    try {
        const CTransaction tx(deserialize, ds);
        TxValidationState state;
        const bool result = CheckAssetLockTx(tx, state);
        assert(result == state.IsValid());
    } catch (const std::exception&) {
    }
}

FUZZ_TARGET(asset_unlock_fee, .init = initialize_asset_lock_unlock)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());

    CMutableTransaction tx;
    tx.nVersion = CTransaction::SPECIAL_VERSION;
    tx.nType = TRANSACTION_ASSET_UNLOCK;

    const uint8_t payload_version = fuzzed_data_provider.ConsumeIntegral<uint8_t>();
    const uint64_t index = fuzzed_data_provider.ConsumeIntegral<uint64_t>();
    const uint32_t fee = fuzzed_data_provider.ConsumeIntegral<uint32_t>();
    const uint32_t requested_height = fuzzed_data_provider.ConsumeIntegral<uint32_t>();
    const uint256 quorum_hash = ConsumeUInt256(fuzzed_data_provider);
    const CBLSSignature quorum_sig = ConsumeBLSSignature(fuzzed_data_provider);
    SetTxPayload(tx, CAssetUnlockPayload(payload_version, index, fee, requested_height, quorum_hash, quorum_sig));

    if (fuzzed_data_provider.ConsumeBool()) {
        // Trigger payload deserialization failures with short/truncated random bytes.
        tx.vExtraPayload = fuzzed_data_provider.ConsumeBytes<uint8_t>(
            fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 16));
    } else if (fuzzed_data_provider.ConsumeBool()) {
        // Trigger bad-txns-assetunlock-fee-outofrange.
        SetTxPayload(tx, CAssetUnlockPayload(payload_version, index, 0, requested_height, quorum_hash, quorum_sig));
    }

    CAmount txfee{0};
    TxValidationState state;
    const bool result = GetAssetUnlockFee(CTransaction(tx), txfee, state);
    assert(result == state.IsValid());
}
