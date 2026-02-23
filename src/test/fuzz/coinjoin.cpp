// Copyright (c) 2026-present The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coinjoin/coinjoin.h>
#include <coinjoin/common.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>

#include <cassert>
#include <cstdint>
#include <vector>

// Fuzz the CoinJoin denomination helper functions with arbitrary amounts
FUZZ_TARGET(coinjoin_denominations)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());

    // Test AmountToDenomination / DenominationToAmount roundtrip
    const CAmount amount = fuzzed_data_provider.ConsumeIntegral<CAmount>();
    const int denom = CoinJoin::AmountToDenomination(amount);
    if (denom > 0) {
        // Valid denomination — roundtrip must be consistent
        assert(CoinJoin::DenominationToAmount(denom) == amount);
        assert(CoinJoin::IsDenominatedAmount(amount));
        assert(CoinJoin::IsValidDenomination(denom));
    }

    // Test DenominationToAmount with fuzzed denom values
    const int fuzzed_denom = fuzzed_data_provider.ConsumeIntegral<int>();
    const CAmount denom_amount = CoinJoin::DenominationToAmount(fuzzed_denom);
    if (denom_amount > 0) {
        assert(CoinJoin::IsDenominatedAmount(denom_amount));
        assert(CoinJoin::AmountToDenomination(denom_amount) == fuzzed_denom);
    }

    // Test collateral amount checks
    const CAmount collateral_amount = fuzzed_data_provider.ConsumeIntegral<CAmount>();
    (void)CoinJoin::IsCollateralAmount(collateral_amount);

    // Test priority calculation
    (void)CoinJoin::CalculateAmountPriority(amount);
    (void)CoinJoin::CalculateAmountPriority(collateral_amount);
}

// Fuzz CCoinJoinQueue — deserialization + method exercising
FUZZ_TARGET(coinjoin_queue)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());

    CCoinJoinQueue queue;
    {
        CDataStream ds(fuzzed_data_provider.ConsumeRemainingBytes<uint8_t>(), SER_NETWORK, PROTOCOL_VERSION);
        try {
            ds >> queue;
        } catch (const std::ios_base::failure&) {
            return;
        }
    }

    // Exercise methods on successfully deserialized queue — must not crash
    (void)queue.GetHash();
    (void)queue.GetSignatureHash();
    (void)queue.ToString();

    // Test time bounds with various times
    (void)queue.IsTimeOutOfBounds();
    (void)queue.IsTimeOutOfBounds(0);
    (void)queue.IsTimeOutOfBounds(queue.nTime);
    (void)queue.IsTimeOutOfBounds(queue.nTime + COINJOIN_QUEUE_TIMEOUT);
    (void)queue.IsTimeOutOfBounds(queue.nTime - COINJOIN_QUEUE_TIMEOUT);
    (void)queue.IsTimeOutOfBounds(std::numeric_limits<int64_t>::max());
    (void)queue.IsTimeOutOfBounds(std::numeric_limits<int64_t>::min());
}

// Fuzz CCoinJoinBroadcastTx — deserialization + IsValidStructure
FUZZ_TARGET(coinjoin_broadcasttx)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());

    CCoinJoinBroadcastTx dstx;
    {
        CDataStream ds(fuzzed_data_provider.ConsumeRemainingBytes<uint8_t>(), SER_NETWORK, PROTOCOL_VERSION);
        try {
            ds >> dstx;
        } catch (const std::ios_base::failure&) {
            return;
        }
    }

    // Exercise methods — must not crash
    (void)dstx.IsValidStructure();
    (void)dstx.GetSignatureHash();
    (void)static_cast<bool>(dstx);
}

// Fuzz CCoinJoinEntry::AddScriptSig with fuzzed inputs
FUZZ_TARGET(coinjoin_entry_addscriptsig)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());

    // Build a CCoinJoinEntry with fuzzed DSIn entries
    CCoinJoinEntry entry;
    const size_t num_inputs = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 20);
    for (size_t i = 0; i < num_inputs; ++i) {
        CTxDSIn dsin;
        // Fuzz the outpoint
        uint256 hash;
        auto hash_bytes = fuzzed_data_provider.ConsumeBytes<uint8_t>(32);
        if (hash_bytes.size() == 32) {
            memcpy(hash.begin(), hash_bytes.data(), 32);
        }
        dsin.prevout = COutPoint(hash, fuzzed_data_provider.ConsumeIntegral<uint32_t>());
        dsin.nSequence = fuzzed_data_provider.ConsumeIntegral<uint32_t>();
        dsin.fHasSig = fuzzed_data_provider.ConsumeBool();
        entry.vecTxDSIn.push_back(dsin);
    }

    // Now try to AddScriptSig with fuzzed CTxIn
    LIMITED_WHILE(fuzzed_data_provider.remaining_bytes() > 0, 50) {
        CTxIn txin;
        uint256 hash;
        auto hash_bytes = fuzzed_data_provider.ConsumeBytes<uint8_t>(32);
        if (hash_bytes.size() == 32) {
            memcpy(hash.begin(), hash_bytes.data(), 32);
        }
        txin.prevout = COutPoint(hash, fuzzed_data_provider.ConsumeIntegral<uint32_t>());
        txin.nSequence = fuzzed_data_provider.ConsumeIntegral<uint32_t>();
        auto script_bytes = fuzzed_data_provider.ConsumeBytes<uint8_t>(
            fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 100));
        txin.scriptSig = CScript(script_bytes.begin(), script_bytes.end());

        (void)entry.AddScriptSig(txin);
    }
}

// Fuzz CCoinJoinStatusUpdate — deserialization + GetMessageByID
FUZZ_TARGET(coinjoin_status_update)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());

    // Test with arbitrary PoolMessage values first (doesn't consume much data)
    const auto raw_msg = fuzzed_data_provider.ConsumeIntegral<int32_t>();
    (void)CoinJoin::GetMessageByID(static_cast<PoolMessage>(raw_msg));

    CCoinJoinStatusUpdate status;
    {
        CDataStream ds(fuzzed_data_provider.ConsumeRemainingBytes<uint8_t>(), SER_NETWORK, PROTOCOL_VERSION);
        try {
            ds >> status;
        } catch (const std::ios_base::failure&) {
            return;
        }
    }

    // Exercise GetMessageByID with deserialized message ID — must not crash
    (void)CoinJoin::GetMessageByID(status.nMessageID);
}
