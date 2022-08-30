// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>

#include <chainparams.h>
#include <consensus/merkle.h>
#include <index/blockfilterindex.h>
#include <interfaces/chain.h>
#include <net.h>
#include <netmessagemaker.h>
#include <pow.h>
#include <protocol.h>
#include <script/script.h>
#include <test/util/index.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

constexpr int NUM_FILTERS{1000};
constexpr int ELEMENTS_PER_FILTER{1000};

class BlockFilterIndexBenchSetup
{
private:
    std::unique_ptr<TestChain100Setup> m_testing_setup{MakeNoLogFileContext<TestChain100Setup>()};
    std::unique_ptr<BlockFilterIndex> m_filter_index;
    const CBlockIndex* m_stop_index{nullptr};

public:
    BlockFilterIndexBenchSetup()
    {
        const CScript coinbase_script{GetScriptForRawPubKey(m_testing_setup->coinbaseKey.GetPubKey())};
        Chainstate& chainstate{m_testing_setup->m_node.chainman->ActiveChainstate()};

        for (int height = 0; height < NUM_FILTERS; ++height) {
            CBlock block{m_testing_setup->CreateBlock({}, coinbase_script, chainstate)};
            CMutableTransaction coinbase{*block.vtx.front()};
            for (int element = 0; element < ELEMENTS_PER_FILTER; ++element) {
                coinbase.vout.emplace_back(0, CScript{} << CScriptNum{element} << OP_DROP << OP_TRUE);
            }
            block.vtx.front() = MakeTransactionRef(std::move(coinbase));
            block.hashMerkleRoot = BlockMerkleRoot(block);
            block.nNonce = 0;
            while (!CheckProofOfWork(block.GetHash(), block.nBits, m_testing_setup->m_node.chainman->GetConsensus())) {
                ++block.nNonce;
            }

            if (!m_testing_setup->m_node.chainman->ProcessNewBlock(std::make_shared<const CBlock>(block), true, true, nullptr)) {
                throw std::runtime_error("failed to create block filter benchmark chain");
            }
        }

        m_filter_index = std::make_unique<BlockFilterIndex>(
            interfaces::MakeChain(m_testing_setup->m_node), BlockFilterType::BASIC_FILTER,
            /*n_cache_size=*/8 << 20, /*f_memory=*/false);
        if (!m_filter_index->Start()) {
            throw std::runtime_error("failed to start block filter benchmark index");
        }
        IndexWaitSynced(*m_filter_index);

        LOCK(cs_main);
        m_stop_index = m_testing_setup->m_node.chainman->ActiveChain().Tip();
    }

    ~BlockFilterIndexBenchSetup()
    {
        m_filter_index->Interrupt();
        m_filter_index->Stop();
    }

    BlockFilterIndex& Index() const { return *m_filter_index; }
    const CBlockIndex* StopIndex() const { return m_stop_index; }
    int StartHeight(int count) const { return m_stop_index->nHeight - count + 1; }
};

std::vector<BlockFilter> LookupFilterRange(const BlockFilterIndexBenchSetup& setup, int count)
{
    std::vector<BlockFilter> filters;
    if (!setup.Index().LookupFilterRange(setup.StartHeight(count), setup.StopIndex(), filters)) {
        throw std::runtime_error("block filter range lookup failed");
    }
    return filters;
}

std::vector<CSerializedNetMsg> SerializeFilters(const std::vector<BlockFilter>& filters, const CNetMsgMaker& msg_maker)
{
    std::vector<CSerializedNetMsg> messages;
    messages.reserve(filters.size());
    for (const auto& filter : filters) {
        messages.emplace_back(msg_maker.Make(NetMsgType::CFILTER, filter));
    }
    return messages;
}

void BlockFilterIndexLookupOne(benchmark::Bench& bench)
{
    BlockFilterIndexBenchSetup setup;

    bench.unit("filter").run([&] {
        BlockFilter filter;
        if (!setup.Index().LookupFilter(setup.StopIndex(), filter)) {
            throw std::runtime_error("block filter lookup failed");
        }
        ankerl::nanobench::doNotOptimizeAway(filter);
    });
}

void BlockFilterIndexLookupRange100(benchmark::Bench& bench)
{
    BlockFilterIndexBenchSetup setup;
    constexpr int count{100};

    bench.batch(count).unit("filter").run([&] {
        auto filters{LookupFilterRange(setup, count)};
        ankerl::nanobench::doNotOptimizeAway(filters);
    });
}

void BlockFilterIndexLookupRange1000(benchmark::Bench& bench)
{
    BlockFilterIndexBenchSetup setup;

    bench.batch(NUM_FILTERS).unit("filter").run([&] {
        auto filters{LookupFilterRange(setup, NUM_FILTERS)};
        ankerl::nanobench::doNotOptimizeAway(filters);
    });
}

void BlockFilterIndexLookupRange1000Bytes(benchmark::Bench& bench)
{
    BlockFilterIndexBenchSetup setup;
    const auto expected{LookupFilterRange(setup, NUM_FILTERS)};
    size_t encoded_bytes{0};
    for (const auto& filter : expected) {
        encoded_bytes += filter.GetEncodedFilter().size();
    }

    bench.batch(encoded_bytes).unit("byte").run([&] {
        auto filters{LookupFilterRange(setup, NUM_FILTERS)};
        ankerl::nanobench::doNotOptimizeAway(filters);
    });
}

void BlockFilterSerialize1000(benchmark::Bench& bench)
{
    BlockFilterIndexBenchSetup setup;
    const auto filters{LookupFilterRange(setup, NUM_FILTERS)};
    const CNetMsgMaker msg_maker{PROTOCOL_VERSION};

    bench.batch(NUM_FILTERS).unit("filter").run([&] {
        auto messages{SerializeFilters(filters, msg_maker)};
        ankerl::nanobench::doNotOptimizeAway(messages);
    });
}

void BlockFilterLookupAndSerialize1000(benchmark::Bench& bench)
{
    BlockFilterIndexBenchSetup setup;
    const CNetMsgMaker msg_maker{PROTOCOL_VERSION};

    bench.batch(NUM_FILTERS).unit("filter").run([&] {
        const auto filters{LookupFilterRange(setup, NUM_FILTERS)};
        auto messages{SerializeFilters(filters, msg_maker)};
        ankerl::nanobench::doNotOptimizeAway(messages);
    });
}

} // namespace

BENCHMARK(BlockFilterIndexLookupOne, benchmark::PriorityLevel::LOW);
BENCHMARK(BlockFilterIndexLookupRange100, benchmark::PriorityLevel::LOW);
BENCHMARK(BlockFilterIndexLookupRange1000, benchmark::PriorityLevel::LOW);
BENCHMARK(BlockFilterIndexLookupRange1000Bytes, benchmark::PriorityLevel::LOW);
BENCHMARK(BlockFilterSerialize1000, benchmark::PriorityLevel::LOW);
BENCHMARK(BlockFilterLookupAndSerialize1000, benchmark::PriorityLevel::LOW);
