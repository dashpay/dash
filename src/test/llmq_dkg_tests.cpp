// Copyright (c) 2022-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <active/dkgsession.h>
#include <evo/deterministicmns.h>
#include <llmq/dkgsession.h>
#include <llmq/dkgsessionhandler.h>
#include <protocol.h>
#include <streams.h>
#include <util/helpers.h>
#include <util/std23.h>

#include <algorithm>
#include <string>
#include <vector>

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

BOOST_AUTO_TEST_CASE(pending_messages_own_messages_share_quota_path)
{
    using namespace llmq;

    const uint256 own_protx = MakeTestHash(0xee);

    // Own messages (from=-1) are enqueued under this node's proTxHash and
    // charged like any other sender's.
    CDKGPendingMessages pending{/*_maxMessagesPerProTx=*/2};
    pending.PushPendingMessage(/*from=*/-1, own_protx, MakeDKGMessage(), MakeTestHash(1));
    pending.PushPendingMessage(/*from=*/-1, own_protx, MakeDKGMessage(), MakeTestHash(2));
    pending.PushPendingMessage(/*from=*/-1, own_protx, MakeDKGMessage(), MakeTestHash(3));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(2)));
    BOOST_CHECK(!pending.HasSeen(MakeTestHash(3)));

    BOOST_CHECK_EQUAL(pending.PopPendingMessages(3).size(), 2U);
}

BOOST_AUTO_TEST_CASE(pending_messages_own_message_is_processed_first)
{
    using namespace llmq;

    CDKGPendingMessages pending{/*_maxMessagesPerProTx=*/2};
    pending.PushPendingMessage(/*from=*/1, MakeTestHash(0xa1), MakeDKGMessage(), MakeTestHash(1));
    pending.PushPendingMessage(/*from=*/2, MakeTestHash(0xa2), MakeDKGMessage(), MakeTestHash(2));
    pending.PushPendingMessage(/*from=*/-1, MakeTestHash(0xee), MakeDKGMessage(), MakeTestHash(3));

    const auto msgs = pending.PopPendingMessages(3);
    BOOST_REQUIRE_EQUAL(msgs.size(), 3U);
    auto it = msgs.begin();
    BOOST_CHECK_EQUAL((it++)->first, -1);
    BOOST_CHECK_EQUAL((it++)->first, 1);
    BOOST_CHECK_EQUAL((it++)->first, 2);
}

BOOST_AUTO_TEST_CASE(pending_messages_quota_survives_reconnect)
{
    using namespace llmq;

    const uint256 protx_a = MakeTestHash(0xa1);

    CDKGPendingMessages pending{/*_maxMessagesPerProTx=*/2};
    pending.PushPendingMessage(/*from=*/1, protx_a, MakeDKGMessage(), MakeTestHash(1));
    pending.PushPendingMessage(/*from=*/1, protx_a, MakeDKGMessage(), MakeTestHash(2));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(1)));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(2)));

    // Reconnecting mints a fresh NodeId but keeps the proTxHash, so the quota
    // is already spent.
    pending.PushPendingMessage(/*from=*/2, protx_a, MakeDKGMessage(), MakeTestHash(3));
    pending.PushPendingMessage(/*from=*/3, protx_a, MakeDKGMessage(), MakeTestHash(4));
    BOOST_CHECK(!pending.HasSeen(MakeTestHash(3)));
    BOOST_CHECK(!pending.HasSeen(MakeTestHash(4)));

    // Draining frees queue slots but does not refund the per-proTx quota.
    BOOST_CHECK_EQUAL(pending.PopPendingMessages(5).size(), 2U);
    pending.PushPendingMessage(/*from=*/4, protx_a, MakeDKGMessage(), MakeTestHash(5));
    BOOST_CHECK(!pending.HasSeen(MakeTestHash(5)));

    // A new round resets everything.
    pending.Clear();
    pending.PushPendingMessage(/*from=*/4, protx_a, MakeDKGMessage(), MakeTestHash(5));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(5)));
}

BOOST_AUTO_TEST_CASE(pending_messages_quota_is_per_protx)
{
    using namespace llmq;

    const uint256 protx_a = MakeTestHash(0xa1);
    const uint256 protx_b = MakeTestHash(0xb1);

    CDKGPendingMessages pending{/*_maxMessagesPerProTx=*/2};
    pending.PushPendingMessage(/*from=*/1, protx_a, MakeDKGMessage(), MakeTestHash(1));
    pending.PushPendingMessage(/*from=*/1, protx_a, MakeDKGMessage(), MakeTestHash(2));

    // Duplicates are rejected before charging the quota.
    pending.PushPendingMessage(/*from=*/2, protx_b, MakeDKGMessage(), MakeTestHash(1));
    pending.PushPendingMessage(/*from=*/2, protx_b, MakeDKGMessage(), MakeTestHash(2));

    // One proTx's spent quota has no effect on another's.
    pending.PushPendingMessage(/*from=*/2, protx_b, MakeDKGMessage(), MakeTestHash(3));
    pending.PushPendingMessage(/*from=*/2, protx_b, MakeDKGMessage(), MakeTestHash(4));
    pending.PushPendingMessage(/*from=*/2, protx_b, MakeDKGMessage(), MakeTestHash(5));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(3)));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(4)));
    BOOST_CHECK(!pending.HasSeen(MakeTestHash(5)));

    // A quota-dropped hash is not marked seen, so a sender with remaining
    // budget can still deliver it.
    const uint256 protx_c = MakeTestHash(0xc1);
    pending.PushPendingMessage(/*from=*/3, protx_c, MakeDKGMessage(), MakeTestHash(5));
    BOOST_CHECK(pending.HasSeen(MakeTestHash(5)));

    BOOST_CHECK_EQUAL(pending.PopPendingMessages(6).size(), 5U);
}

CDeterministicMNList MakeSingleMNList(const uint256& protx, int registered, int revived)
{
    auto state = std::make_shared<CDeterministicMNState>();
    state->netInfo = NetInfoInterface::MakeNetInfo(state->nVersion);
    state->nRegisteredHeight = registered;
    state->nPoSeRevivedHeight = revived;
    state->keyIDOwner.begin()[0] = 1;
    auto mn = std::make_shared<CDeterministicMN>(0);
    mn->proTxHash = protx;
    mn->collateralOutpoint = COutPoint(protx, 0);
    mn->pdmnState = state;
    CDeterministicMNList mn_list;
    mn_list.AddMN(mn);
    return mn_list;
}

BOOST_AUTO_TEST_CASE(signing_history_registration_and_revival_cutoffs)
{
    const auto protx = MakeTestHash(1);
    const llmq::SigningRecords records{{protx, {{99, false}, {100, true}, {101, false}, {102, true}}}};
    const auto count = [&](int registered, int revived) {
        const auto mn_list = MakeSingleMNList(protx, registered, revived);
        const auto counted = llmq::CountSignedCommitments(records, mn_list, protx);
        // The operator-facing summary counts the same records as the vote
        const auto summary = llmq::SummarizeSigningHistory(records, mn_list, protx, /*expiring_until_height=*/-1);
        BOOST_CHECK_EQUAL(summary.cutoff_height, std::max(registered, revived));
        BOOST_CHECK(std::make_pair(summary.valid, summary.signed_count) == counted);
        return counted;
    };

    BOOST_CHECK(count(99, -1) == std::make_pair(3, 2));
    BOOST_CHECK(count(100, -1) == std::make_pair(2, 1));
    BOOST_CHECK(count(100, 101) == std::make_pair(1, 1));
    BOOST_CHECK(count(101, 100) == std::make_pair(1, 1));
    BOOST_CHECK(count(102, 102) == std::make_pair(0, 0));
    BOOST_CHECK(llmq::CountSignedCommitments(records, {}, protx) == std::make_pair(0, 0));
    BOOST_CHECK_EQUAL(llmq::SummarizeSigningHistory(records, {}, protx, /*expiring_until_height=*/-1).cutoff_height, -1);
}

BOOST_AUTO_TEST_CASE(signing_history_summary_recent_unsigned_and_risk)
{
    struct TestCase {
        std::string name;
        std::vector<llmq::SigningRecord> records;
        int expiring_until_height;
        int valid;
        int signed_count;
        int recent_unsigned;
        bool voted_bad;
        bool one_miss_from_voted_bad;
        bool voted_bad_after_expiry;
        bool at_risk;
    };
    constexpr bool S{true}, U{false};
    const std::vector<TestCase> tests{
        {"no history", {}, -1, 0, 0, 0, false, false, false, false},
        // Quorums of different types share base heights: none of these is after the signed one, in either order
        {"same height, signed first", {{100, S}, {100, U}, {100, U}}, -1, 3, 1, 0, false, false, false, false},
        {"same height, signed last", {{100, U}, {100, U}, {100, S}}, -1, 3, 1, 0, false, false, false, false},
        {"records out of height order", {{30, S}, {10, U}, {40, U}, {20, U}}, -1, 4, 1, 1, false, false, false, false},
        {"never signed, two so far", {{10, U}, {20, U}}, -1, 2, 0, 2, false, false, false, true},
        {"streak reset by a signed one", {{10, U}, {20, U}, {30, S}, {40, U}}, -1, 4, 1, 1, false, false, false, false},
        {"under half signed", {{10, U}, {20, U}, {30, U}, {40, U}, {50, S}, {60, S}, {70, S}}, -1, 7, 3, 0, false, false, false, true},
        {"exactly half signed", {{10, U}, {20, U}, {30, U}, {40, S}, {50, S}, {60, S}}, -1, 6, 3, 0, false, false, false, false},
        {"voted bad is not at risk", {{10, S}, {20, U}, {30, U}, {40, U}, {50, U}, {60, U}}, -1, 6, 1, 5, true, false, false, false},
        // Below the minimum for both the vote and the ratio warning, and the last one signed
        {"one miss from voted bad", {{10, U}, {20, U}, {30, U}, {40, U}, {50, S}}, -1, 5, 1, 0, false, true, false, true},
        // Half signed with one recent miss, until the four oldest signed records leave the window
        {"expiry without lookahead",
         {{10, S}, {20, S}, {30, S}, {40, S}, {50, U}, {60, U}, {70, U}, {80, U}, {90, S}, {100, U}},
         -1, 10, 5, 1, false, false, false, false},
        {"expiry with lookahead",
         {{10, S}, {20, S}, {30, S}, {40, S}, {50, U}, {60, U}, {70, U}, {80, U}, {90, S}, {100, U}},
         40, 10, 5, 1, false, true, true, true},
        // Voted bad once the four signed records of height 576 leave, although not any more once the unsigned
        // ones after them have left too (gathering at 4584 looks ahead until 1128)
        {"expiry crosses the threshold on the way",
         {{576, S}, {576, S}, {576, S}, {576, S}, {600, U}, {624, U}, {648, U}, {672, U}, {696, U}, {720, U},
          {3000, S}, {3024, S}},
         llmq::SigningHistoryExpiringUntil(4584), 12, 6, 0, false, true, true, true},
        // Neither expiry alone (7 valid, 2 signed) nor one more miss now (11, 5) gets it voted bad, but both
        // together do (8, 2)
        {"expiry and one more miss together",
         {{576, S}, {576, S}, {576, S}, {600, U}, {624, U}, {648, U}, {672, U}, {696, U}, {3000, S}, {3024, S}},
         llmq::SigningHistoryExpiringUntil(4584), 10, 5, 0, false, true, false, true},
    };

    // A record of a quorum based at h leaves the window (gathering - window, gathering] when gathering reaches
    // h + window, so the lookahead covers the records of quorums based until gathering - window + lookahead
    BOOST_CHECK_EQUAL(llmq::SigningHistoryExpiringUntil(4584), 4584 - llmq::SIGNING_HISTORY_WINDOW + 576);

    const auto protx = MakeTestHash(1);
    const auto mn_list = MakeSingleMNList(protx, /*registered=*/0, /*revived=*/-1);
    for (const auto& test : tests) {
        BOOST_TEST_CONTEXT(test.name)
        {
            const llmq::SigningRecords records{{protx, test.records}};
            const auto summary = llmq::SummarizeSigningHistory(records, mn_list, protx, test.expiring_until_height);
            BOOST_CHECK_EQUAL(summary.valid, test.valid);
            BOOST_CHECK_EQUAL(summary.signed_count, test.signed_count);
            BOOST_CHECK_EQUAL(summary.recent_unsigned, test.recent_unsigned);
            BOOST_CHECK_EQUAL(summary.voted_bad, test.voted_bad);
            BOOST_CHECK_EQUAL(summary.one_miss_from_voted_bad, test.one_miss_from_voted_bad);
            BOOST_CHECK_EQUAL(summary.voted_bad_after_expiry, test.voted_bad_after_expiry);
            BOOST_CHECK_EQUAL(llmq::IsSigningHistoryAtRisk(summary), test.at_risk);
        }
    }
}

BOOST_AUTO_TEST_CASE(signing_history_eligibility_and_quarter_threshold)
{
    struct TestCase {
        int valid;
        int signed_count;
        bool insufficient;
    };
    for (const auto& test : {TestCase{0, 0, false},
                             {5, 0, false},
                             {6, 0, true},
                             {6, 1, true},
                             {6, 2, false},
                             {7, 2, false},
                             {8, 2, true},
                             {8, 3, false}}) {
        BOOST_TEST_CONTEXT("valid=" << test.valid << ", signed=" << test.signed_count)
        {
            BOOST_CHECK_EQUAL(llmq::HasInsufficientSigningHistory(test.valid, test.signed_count), test.insufficient);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
