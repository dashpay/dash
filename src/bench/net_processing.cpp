// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <bench/bench.h>
#include <net_processing.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>
#include <validation.h>

using namespace std::literals;

static void InventoryBatch(benchmark::Bench& bench, uint32_t count)
{
    const auto setup = MakeNoLogFileContext<TestingSetup>(ChainType::REGTEST);
    auto& chainstate = *static_cast<TestChainState*>(&setup->m_node.chainman->ActiveChainstate());
    chainstate.JumpOutOfIbd();
    auto& peerman = *setup->m_node.peerman;
    const auto& connman = *static_cast<ConnmanTestMsg*>(setup->m_node.connman.get());
    auto peer{MakeTestPeer(/*id=*/0)};
    peerman.InitializeNode(*peer, NODE_NETWORK);

    std::vector<CInv> invs;
    invs.reserve(count);
    for (uint32_t i = 1; i <= count; ++i) {
        invs.emplace_back(MSG_SPORK, ArithToUint256(arith_uint256{i}));
    }
    CDataStream inventory{SER_NETWORK, PROTOCOL_VERSION};
    inventory << invs;
    const std::atomic<bool> interrupt{false};
    SetMockTime(1'700'000'000s);
    const auto now{GetTime<std::chrono::microseconds>()};

    bench.batch(count).unit("inventory").run([&] {
        LOCK(NetEventsInterface::g_msgproc_mutex);
        auto announcements = inventory;
        peerman.ProcessMessage(*peer, NetMsgType::INV, announcements, now, interrupt);
        peerman.SendMessages(peer.get());
        connman.FlushSendBuffer(*peer);
        auto notfound = inventory;
        peerman.ProcessMessage(*peer, NetMsgType::NOTFOUND, notfound, now, interrupt);
    });

    peerman.FinalizeNode(*peer);
    chainstate.ResetIbd();
    SetMockTime(0s);
}

static void InventoryBatch100(benchmark::Bench& bench) { InventoryBatch(bench, 100); }

static void InventoryBatch50000(benchmark::Bench& bench) { InventoryBatch(bench, 50'000); }

BENCHMARK(InventoryBatch100, benchmark::PriorityLevel::HIGH);
BENCHMARK(InventoryBatch50000, benchmark::PriorityLevel::HIGH);
