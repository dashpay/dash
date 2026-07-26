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

namespace {
std::shared_ptr<CDataStream> MakeDKGMessage()
{
    return std::make_shared<CDataStream>(SER_NETWORK, PROTOCOL_VERSION);
}

uint256 MakeTestHash(uint8_t value)
{
    uint256 hash;
    hash.begin()[0] = value;
    return hash;
}
} // namespace

BOOST_AUTO_TEST_CASE(pending_messages_local_first_uses_full_remote_allowance)
{
    using namespace llmq;

    const uint256 protx_a = MakeTestHash(0xa1);
    const uint256 protx_b = MakeTestHash(0xb1);

    // maxMessagesPerSender=2 -> remote cap 4, plus one reserved local slot.
    CDKGPendingMessages pending{/*max_messages_per_sender=*/2};
    pending.PushPendingMessage(/*from=*/-1, /*sender_protx=*/uint256(), MakeDKGMessage(), MakeTestHash(1));
    pending.PushPendingMessage(/*from=*/1, protx_a, MakeDKGMessage(), MakeTestHash(2));
    pending.PushPendingMessage(/*from=*/1, protx_a, MakeDKGMessage(), MakeTestHash(3));
    pending.PushPendingMessage(/*from=*/2, protx_b, MakeDKGMessage(), MakeTestHash(4));
    pending.PushPendingMessage(/*from=*/2, protx_b, MakeDKGMessage(), MakeTestHash(5));

    BOOST_CHECK_EQUAL(pending.PopPendingMessages(6).size(), 5U);
}

BOOST_AUTO_TEST_CASE(pending_messages_quota_survives_reconnect)
{
    using namespace llmq;

    const uint256 protx_a = MakeTestHash(0xa1);

    CDKGPendingMessages pending{/*max_messages_per_sender=*/2};
    pending.PushPendingMessage(/*from=*/1, protx_a, MakeDKGMessage(), MakeTestHash(1));
    pending.PushPendingMessage(/*from=*/1, protx_a, MakeDKGMessage(), MakeTestHash(2));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(1)));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(2)));

    // Reconnecting mints a fresh NodeId but keeps the proTxHash, so the quota is
    // already spent and the queue-wide cap is never reached.
    pending.PushPendingMessage(/*from=*/2, protx_a, MakeDKGMessage(), MakeTestHash(3));
    pending.PushPendingMessage(/*from=*/3, protx_a, MakeDKGMessage(), MakeTestHash(4));
    BOOST_CHECK(!pending.HasSeen(MakeTestHash(3)));
    BOOST_CHECK(!pending.HasSeen(MakeTestHash(4)));

    // Draining frees queue slots but does not refund the per-sender quota.
    BOOST_CHECK_EQUAL(pending.PopPendingMessages(5).size(), 2U);
    pending.PushPendingMessage(/*from=*/4, protx_a, MakeDKGMessage(), MakeTestHash(5));
    BOOST_CHECK(!pending.HasSeen(MakeTestHash(5)));

    // A new round resets everything.
    pending.Clear();
    pending.PushPendingMessage(/*from=*/4, protx_a, MakeDKGMessage(), MakeTestHash(5));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(5)));
}

BOOST_AUTO_TEST_CASE(pending_messages_bounded_across_senders)
{
    using namespace llmq;

    const uint256 protx_a = MakeTestHash(0xa1);
    const uint256 protx_b = MakeTestHash(0xb1);
    const uint256 protx_c = MakeTestHash(0xc1);

    CDKGPendingMessages pending{/*max_messages_per_sender=*/2};
    pending.PushPendingMessage(/*from=*/1, protx_a, MakeDKGMessage(), MakeTestHash(1));
    pending.PushPendingMessage(/*from=*/1, protx_a, MakeDKGMessage(), MakeTestHash(2));

    // One sender's full quota does not consume the queue-wide remote allowance.
    pending.PushPendingMessage(/*from=*/2, protx_b, MakeDKGMessage(), MakeTestHash(3));
    pending.PushPendingMessage(/*from=*/2, protx_b, MakeDKGMessage(), MakeTestHash(4));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(3)));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(4)));

    // Distinct senders cannot push past the queue-wide remote-message cap.
    pending.PushPendingMessage(/*from=*/3, protx_c, MakeDKGMessage(), MakeTestHash(5));
    BOOST_CHECK(!pending.HasSeen(MakeTestHash(5)));

    // Duplicates are rejected before charging the sender's quota.
    pending.PushPendingMessage(/*from=*/3, protx_c, MakeDKGMessage(), MakeTestHash(1));
    pending.PushPendingMessage(/*from=*/3, protx_c, MakeDKGMessage(), MakeTestHash(1));

    BOOST_CHECK_EQUAL(pending.PopPendingMessages(5).size(), 4U);
    pending.PushPendingMessage(/*from=*/3, protx_c, MakeDKGMessage(), MakeTestHash(6));
    pending.PushPendingMessage(/*from=*/3, protx_c, MakeDKGMessage(), MakeTestHash(7));
    pending.PushPendingMessage(/*from=*/3, protx_c, MakeDKGMessage(), MakeTestHash(8));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(6)));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(7)));
    BOOST_CHECK(!pending.HasSeen(MakeTestHash(8)));
}

BOOST_AUTO_TEST_SUITE_END()
