// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <bls/bls.h>
#include <random.h>

// Benchmark for the OLD approach: iterative aggregation
// This mimics the code before commit d87ea73533e6477441ad995347ea5b4fddf64f9b
static void BLS_PubKeyAggregate_Iterative(size_t count, benchmark::Bench& bench)
{
    // Generate test public keys
    std::vector<CBLSPublicKey> pubKeys;
    pubKeys.reserve(count);

    for (size_t i = 0; i < count; i++) {
        CBLSSecretKey secKey;
        secKey.MakeNewKey();
        pubKeys.push_back(secKey.GetPublicKey());
    }

    // Benchmark: OLD approach - iterative aggregation
    bench.minEpochIterations(100).run([&] {
        CBLSPublicKey aggPubKey;

        for (const auto& pubKey : pubKeys) {
            if (!aggPubKey.IsValid()) {
                aggPubKey = pubKey;
            } else {
                aggPubKey.AggregateInsecure(pubKey);
            }
        }

        // Ensure the result is used to prevent optimization
        assert(aggPubKey.IsValid());
    });
}

// Benchmark for the NEW approach: batch aggregation
// This mimics the code after commit d87ea73533e6477441ad995347ea5b4fddf64f9b
static void BLS_PubKeyAggregate_Batch(size_t count, benchmark::Bench& bench)
{
    // Generate test public keys
    std::vector<CBLSPublicKey> pubKeys;
    pubKeys.reserve(count);

    for (size_t i = 0; i < count; i++) {
        CBLSSecretKey secKey;
        secKey.MakeNewKey();
        pubKeys.push_back(secKey.GetPublicKey());
    }

    // Benchmark: NEW approach - batch aggregation
    bench.minEpochIterations(100).run([&] {
        CBLSPublicKey aggPubKey = CBLSPublicKey::AggregateInsecure(pubKeys);
        // Ensure the result is used to prevent optimization
        assert(aggPubKey.IsValid());
    });
}

// Benchmarks for different sizes

// Small aggregation (5 keys) - typical for small quorum subsets
static void BLS_PubKeyAggregate_Iterative_5(benchmark::Bench& bench) { BLS_PubKeyAggregate_Iterative(5, bench); }

static void BLS_PubKeyAggregate_Batch_5(benchmark::Bench& bench) { BLS_PubKeyAggregate_Batch(5, bench); }

// Medium aggregation (25 keys) - typical for medium quorum subsets
static void BLS_PubKeyAggregate_Iterative_25(benchmark::Bench& bench) { BLS_PubKeyAggregate_Iterative(25, bench); }

static void BLS_PubKeyAggregate_Batch_25(benchmark::Bench& bench) { BLS_PubKeyAggregate_Batch(25, bench); }

// Large aggregation (50 keys) - typical for larger quorums
static void BLS_PubKeyAggregate_Iterative_50(benchmark::Bench& bench) { BLS_PubKeyAggregate_Iterative(50, bench); }

static void BLS_PubKeyAggregate_Batch_50(benchmark::Bench& bench) { BLS_PubKeyAggregate_Batch(50, bench); }

// Very large aggregation (100 keys) - stress test
static void BLS_PubKeyAggregate_Iterative_100(benchmark::Bench& bench) { BLS_PubKeyAggregate_Iterative(100, bench); }

static void BLS_PubKeyAggregate_Batch_100(benchmark::Bench& bench) { BLS_PubKeyAggregate_Batch(100, bench); }

// Extra large aggregation (200 keys) - extreme case
static void BLS_PubKeyAggregate_Iterative_200(benchmark::Bench& bench) { BLS_PubKeyAggregate_Iterative(200, bench); }

static void BLS_PubKeyAggregate_Batch_200(benchmark::Bench& bench) { BLS_PubKeyAggregate_Batch(200, bench); }

// Register all benchmarks
BENCHMARK(BLS_PubKeyAggregate_Iterative_5)
BENCHMARK(BLS_PubKeyAggregate_Batch_5)
BENCHMARK(BLS_PubKeyAggregate_Iterative_25)
BENCHMARK(BLS_PubKeyAggregate_Batch_25)
BENCHMARK(BLS_PubKeyAggregate_Iterative_50)
BENCHMARK(BLS_PubKeyAggregate_Batch_50)
BENCHMARK(BLS_PubKeyAggregate_Iterative_100)
BENCHMARK(BLS_PubKeyAggregate_Batch_100)
BENCHMARK(BLS_PubKeyAggregate_Iterative_200)
BENCHMARK(BLS_PubKeyAggregate_Batch_200)
