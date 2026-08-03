// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <hash.h>
#include <llmq/dkgsessionhandler.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <version.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <memory>

using namespace llmq;

BOOST_FIXTURE_TEST_SUITE(llmq_dkg_pending_tests, BasicTestingSetup)

namespace {
std::shared_ptr<CDataStream> MakePayload(uint32_t salt, size_t bytes = 8)
{
    auto pm = std::make_shared<CDataStream>(SER_NETWORK, PROTOCOL_VERSION);
    *pm << salt;
    pm->resize(bytes);
    return pm;
}

uint256 MakeHash(uint32_t salt)
{
    CHashWriter hw(SER_GETHASH, 0);
    hw << salt;
    return hw.GetHash();
}

void PushUnique(CDKGPendingMessages& pending, NodeId from, uint32_t salt, size_t bytes = 8)
{
    pending.PushPendingMessage(from, MakePayload(salt, bytes), MakeHash(salt));
}
} // namespace

BOOST_AUTO_TEST_CASE(pending_messages_per_node_cap)
{
    constexpr size_t max_per_node{3};
    CDKGPendingMessages pending{max_per_node, /*max_pending_bytes=*/1024};

    for (uint32_t i = 0; i < max_per_node + 5; ++i) {
        PushUnique(pending, /*from=*/1, i);
    }

    BOOST_CHECK_EQUAL(pending.Size(), max_per_node);
    BOOST_CHECK_EQUAL(pending.SizeBytes(), max_per_node * 8);
}

// A fresh NodeId on every reconnect must not grant fresh capacity in this queue.
BOOST_AUTO_TEST_CASE(pending_messages_byte_bounded_across_node_ids)
{
    constexpr size_t byte_cap{24};
    CDKGPendingMessages pending{/*max_messages_per_node=*/100, byte_cap};

    for (NodeId node = 1; node <= 20; ++node) {
        PushUnique(pending, node, static_cast<uint32_t>(node), /*bytes=*/8);
        BOOST_CHECK_LE(pending.SizeBytes(), byte_cap);
    }

    BOOST_CHECK_EQUAL(pending.SizeBytes(), byte_cap);
    BOOST_CHECK_EQUAL(pending.Size(), 3);
}

BOOST_AUTO_TEST_CASE(pending_messages_oversized_payload_rejected)
{
    CDKGPendingMessages pending{/*max_messages_per_node=*/10, /*max_pending_bytes=*/16};

    PushUnique(pending, /*from=*/1, /*salt=*/1, /*bytes=*/17);
    pending.PushPendingMessage(/*from=*/2, std::make_shared<CDataStream>(SER_NETWORK, PROTOCOL_VERSION), MakeHash(2));
    BOOST_CHECK_EQUAL(pending.Size(), 0);
    BOOST_CHECK_EQUAL(pending.SizeBytes(), 0);
}

// Duplicate replay must be side-effect free even when the queue is full. The
// old draft evicted before checking seenMessages, allowing replay-only griefing.
BOOST_AUTO_TEST_CASE(pending_messages_duplicate_does_not_evict)
{
    CDKGPendingMessages pending{/*max_messages_per_node=*/10, /*max_pending_bytes=*/16};
    PushUnique(pending, /*from=*/1, /*salt=*/1, /*bytes=*/8);
    PushUnique(pending, /*from=*/2, /*salt=*/2, /*bytes=*/8);

    pending.PushPendingMessage(/*from=*/3, MakePayload(/*salt=*/99, /*bytes=*/8), MakeHash(/*salt=*/1));

    BOOST_CHECK_EQUAL(pending.Size(), 2);
    BOOST_CHECK_EQUAL(pending.SizeBytes(), 16);
    const auto msgs = pending.PopPendingMessages(/*maxCount=*/10);
    BOOST_REQUIRE_EQUAL(msgs.size(), 2);
    BOOST_CHECK_EQUAL(msgs.front().first, 1);
    BOOST_CHECK_EQUAL(msgs.back().first, 2);
}

BOOST_AUTO_TEST_CASE(pending_messages_remove_node_releases_bytes)
{
    CDKGPendingMessages pending{/*max_messages_per_node=*/10, /*max_pending_bytes=*/24};
    PushUnique(pending, /*from=*/1, /*salt=*/1, /*bytes=*/8);
    PushUnique(pending, /*from=*/2, /*salt=*/2, /*bytes=*/16);
    BOOST_CHECK_EQUAL(pending.SizeBytes(), 24);

    pending.RemoveNode(/*nodeId=*/2);
    BOOST_CHECK_EQUAL(pending.Size(), 1);
    BOOST_CHECK_EQUAL(pending.SizeBytes(), 8);

    PushUnique(pending, /*from=*/3, /*salt=*/3, /*bytes=*/16);
    BOOST_CHECK_EQUAL(pending.Size(), 2);
    BOOST_CHECK_EQUAL(pending.SizeBytes(), 24);
}

// Capacity pressure is charged by bytes, so a peer pinning most of the memory
// gives up its oldest payload when a new peer arrives.
BOOST_AUTO_TEST_CASE(pending_messages_evicts_greediest_peer_by_bytes)
{
    CDKGPendingMessages pending{/*max_messages_per_node=*/10, /*max_pending_bytes=*/32};
    PushUnique(pending, /*from=*/1, /*salt=*/1, /*bytes=*/8);
    PushUnique(pending, /*from=*/1, /*salt=*/2, /*bytes=*/8);
    PushUnique(pending, /*from=*/1, /*salt=*/3, /*bytes=*/8);
    PushUnique(pending, /*from=*/2, /*salt=*/4, /*bytes=*/8);

    PushUnique(pending, /*from=*/3, /*salt=*/5, /*bytes=*/4);
    BOOST_CHECK_EQUAL(pending.SizeBytes(), 28);

    const auto msgs = pending.PopPendingMessages(/*maxCount=*/10);
    BOOST_REQUIRE_EQUAL(msgs.size(), 4);
    BOOST_CHECK_EQUAL(std::count_if(msgs.begin(), msgs.end(), [](const auto& msg) { return msg.first == 1; }), 2);
    BOOST_CHECK_EQUAL(std::count_if(msgs.begin(), msgs.end(), [](const auto& msg) { return msg.first == 2; }), 1);
    BOOST_CHECK_EQUAL(std::count_if(msgs.begin(), msgs.end(), [](const auto& msg) { return msg.first == 3; }), 1);
}

BOOST_AUTO_TEST_CASE(pending_messages_own_message_survives_full_queue)
{
    CDKGPendingMessages pending{/*max_messages_per_node=*/10, /*max_pending_bytes=*/16};
    PushUnique(pending, /*from=*/1, /*salt=*/1, /*bytes=*/16);
    PushUnique(pending, /*from=*/-1, /*salt=*/2, /*bytes=*/8);

    BOOST_CHECK_EQUAL(pending.Size(), 2);
    BOOST_CHECK_EQUAL(pending.SizeBytes(), 16);
    const auto msgs = pending.PopPendingMessages(/*maxCount=*/10);
    BOOST_CHECK(std::any_of(msgs.begin(), msgs.end(), [](const auto& msg) { return msg.first < 0; }));
}

BOOST_AUTO_TEST_CASE(pending_messages_remove_node_only_drops_unprocessed)
{
    CDKGPendingMessages pending{/*max_messages_per_node=*/4, /*max_pending_bytes=*/64};

    for (uint32_t i = 0; i < 4; ++i) {
        PushUnique(pending, /*from=*/7, i);
    }
    auto processed = pending.PopPendingMessages(/*maxCount=*/2);
    BOOST_REQUIRE_EQUAL(processed.size(), 2);
    BOOST_CHECK_EQUAL(pending.SizeBytes(), 16);

    pending.RemoveNode(/*nodeId=*/7);
    BOOST_CHECK_EQUAL(pending.Size(), 0);
    BOOST_CHECK_EQUAL(pending.SizeBytes(), 0);
    BOOST_CHECK_EQUAL(processed.size(), 2);

    PushUnique(pending, /*from=*/7, /*salt=*/5);
    BOOST_CHECK_EQUAL(pending.Size(), 1);
}

BOOST_AUTO_TEST_SUITE_END()
