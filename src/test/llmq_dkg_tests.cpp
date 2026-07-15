// Copyright (c) 2022-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/dkgsession.h>
#include <llmq/dkgsessionhandler.h>
#include <protocol.h>
#include <streams.h>
#include <util/helpers.h>
#include <util/std23.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(llmq_dkg_tests)

BOOST_AUTO_TEST_CASE(llmq_dkgerror)
{
    using namespace llmq;
    for (auto i : util::irange(std23::to_underlying(llmq::DKGError::type::_COUNT))) {
        BOOST_REQUIRE(GetSimulatedErrorRate(llmq::DKGError::type(i)) == 0.0);
        SetSimulatedDKGErrorRate(llmq::DKGError::type(i), 1.0);
        BOOST_REQUIRE(GetSimulatedErrorRate(llmq::DKGError::type(i)) == 1.0);
    }
    BOOST_REQUIRE(GetSimulatedErrorRate(llmq::DKGError::type::_COUNT) == 0.0);
    SetSimulatedDKGErrorRate(llmq::DKGError::type::_COUNT, 1.0);
    BOOST_REQUIRE(GetSimulatedErrorRate(llmq::DKGError::type::_COUNT) == 0.0);
}

BOOST_AUTO_TEST_CASE(pending_messages_local_first_uses_full_remote_allowance)
{
    using namespace llmq;

    auto make_message = [] { return std::make_shared<CDataStream>(SER_NETWORK, PROTOCOL_VERSION); };
    auto make_hash = [](uint8_t value) {
        uint256 hash;
        hash.begin()[0] = value;
        return hash;
    };

    CDKGPendingMessages pending{/*max_messages_per_node=*/2};
    pending.PushPendingMessage(/*from=*/-1, make_message(), make_hash(1));
    pending.PushPendingMessage(/*from=*/1, make_message(), make_hash(2));
    pending.PushPendingMessage(/*from=*/1, make_message(), make_hash(3));
    pending.PushPendingMessage(/*from=*/2, make_message(), make_hash(4));
    pending.PushPendingMessage(/*from=*/2, make_message(), make_hash(5));

    BOOST_CHECK_EQUAL(pending.PopPendingMessages(6).size(), 5U);
}

BOOST_AUTO_TEST_CASE(pending_messages_bounded_across_node_ids)
{
    using namespace llmq;

    auto make_message = [] { return std::make_shared<CDataStream>(SER_NETWORK, PROTOCOL_VERSION); };
    auto make_hash = [](uint8_t value) {
        uint256 hash;
        hash.begin()[0] = value;
        return hash;
    };

    CDKGPendingMessages pending{/*max_messages_per_node=*/2};
    pending.PushPendingMessage(/*from=*/1, make_message(), make_hash(1));
    pending.PushPendingMessage(/*from=*/1, make_message(), make_hash(2));

    // One peer's full quota does not consume the queue-wide remote allowance.
    pending.PushPendingMessage(/*from=*/2, make_message(), make_hash(3));
    pending.PushPendingMessage(/*from=*/2, make_message(), make_hash(4));
    BOOST_CHECK(pending.HasSeen(make_hash(3)));
    BOOST_CHECK(pending.HasSeen(make_hash(4)));

    // Fresh NodeIds cannot bypass the queue-wide remote-message cap.
    pending.PushPendingMessage(/*from=*/3, make_message(), make_hash(5));
    pending.PushPendingMessage(/*from=*/4, make_message(), make_hash(6));
    BOOST_CHECK(!pending.HasSeen(make_hash(5)));
    BOOST_CHECK(!pending.HasSeen(make_hash(6)));

    // Duplicates are rejected before charging the new NodeId's quota.
    pending.PushPendingMessage(/*from=*/3, make_message(), make_hash(1));
    pending.PushPendingMessage(/*from=*/3, make_message(), make_hash(1));

    BOOST_CHECK_EQUAL(pending.PopPendingMessages(5).size(), 4U);
    pending.PushPendingMessage(/*from=*/3, make_message(), make_hash(7));
    pending.PushPendingMessage(/*from=*/3, make_message(), make_hash(8));
    pending.PushPendingMessage(/*from=*/3, make_message(), make_hash(9));
    BOOST_CHECK(pending.HasSeen(make_hash(7)));
    BOOST_CHECK(pending.HasSeen(make_hash(8)));
    BOOST_CHECK(!pending.HasSeen(make_hash(9)));

    pending.Clear();
    pending.PushPendingMessage(/*from=*/3, make_message(), make_hash(7));
    BOOST_CHECK(pending.HasSeen(make_hash(7)));
}

BOOST_AUTO_TEST_SUITE_END()
