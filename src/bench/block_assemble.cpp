// Copyright (c) 2011-2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <chainparams.h>
#include <consensus/consensus.h>
#include <consensus/validation.h>
#include <script/standard.h>
#include <test/util.h>
#include <txmempool.h>
#include <validation.h>


#include <vector>

static void AssembleBlock(benchmark::Bench& bench)
{
    const CScript redeemScript = CScript() << OP_DROP << OP_TRUE;
    const CScript SCRIPT_PUB =
        CScript() << OP_HASH160 << ToByteVector(CScriptID(redeemScript))
                  << OP_EQUAL;

    const CScript scriptSig = CScript() << std::vector<uint8_t>(100, 0xff)
                                        << ToByteVector(redeemScript);

    // Collect some loose transactions that spend the coinbases of our mined blocks
    constexpr size_t NUM_BLOCKS{200};
    std::vector<CTransactionRef> txs;
    txs.resize(NUM_BLOCKS - Params().GetConsensus().nCoinbaseMaturity + 1);
    for (size_t b{0}; b < NUM_BLOCKS; ++b) {
        CMutableTransaction tx;
        tx.vin.push_back(MineBlock(SCRIPT_PUB));
        tx.vin.back().scriptSig = scriptSig;
        tx.vout.emplace_back(1337, SCRIPT_PUB);
        if (NUM_BLOCKS - b >= Params().GetConsensus().nCoinbaseMaturity)
            txs.at(b) = MakeTransactionRef(tx);
    }
    {
        LOCK(::cs_main); // Required for ::AcceptToMemoryPool.

        for (const auto& txr : txs) {
            CValidationState state;
            bool ret{::AcceptToMemoryPool(::mempool, state, txr, nullptr /* pfMissingInputs */, false /* bypass_limits */, /* nAbsurdFee */ 0)};
            assert(ret);
        }
    }

    bench.minEpochIterations(700).run([&] {
        PrepareBlock(SCRIPT_PUB);
    });
}

BENCHMARK(AssembleBlock);
