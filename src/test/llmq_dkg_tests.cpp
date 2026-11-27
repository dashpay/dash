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
std::shared_ptr<DataStream> MakeDKGMessage()
{
    return std::make_shared<DataStream>();
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

BOOST_AUTO_TEST_CASE(signing_history_registration_and_revival_cutoffs)
{
    const auto protx = MakeTestHash(1);
    const llmq::SigningRecords records{{protx, {{99, false}, {100, true}, {101, false}, {102, true}}}};
    const auto count = [&](int registered, int revived) {
        auto state = std::make_shared<CDeterministicMNState>();
        state->netInfo = NetInfoInterface::MakeNetInfo(state->nVersion);
        state->nRegisteredHeight = registered;
        state->nPoSeRevivedHeight = revived;
        auto mn = std::make_shared<CDeterministicMN>(0);
        mn->proTxHash = protx;
        mn->collateralOutpoint = COutPoint(protx, 0);
        state->keyIDOwner.begin()[0] = 1;
        mn->pdmnState = state;
        CDeterministicMNList mn_list;
        mn_list.AddMN(mn);
        return llmq::CountSignedCommitments(records, mn_list, protx);
    };

    BOOST_CHECK(count(99, -1) == std::make_pair(3, 2));
    BOOST_CHECK(count(100, -1) == std::make_pair(2, 1));
    BOOST_CHECK(count(100, 101) == std::make_pair(1, 1));
    BOOST_CHECK(count(101, 100) == std::make_pair(1, 1));
    BOOST_CHECK(count(102, 102) == std::make_pair(0, 0));
    BOOST_CHECK(llmq::CountSignedCommitments(records, {}, protx) == std::make_pair(0, 0));
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

BOOST_AUTO_TEST_CASE(signing_history_votes_leave_enough_members_for_the_quorum)
{
    CDeterministicMNList mn_list;
    std::vector<uint256> candidates;
    llmq::SigningRecords records;
    for (const auto i : util::irange(8)) {
        const auto protx = MakeTestHash(static_cast<uint8_t>(i + 1));
        auto state = std::make_shared<CDeterministicMNState>();
        state->netInfo = NetInfoInterface::MakeNetInfo(state->nVersion);
        state->keyIDOwner.begin()[0] = static_cast<uint8_t>(i + 1);
        auto mn = std::make_shared<CDeterministicMN>(i);
        mn->proTxHash = protx;
        mn->collateralOutpoint = COutPoint(protx, 0);
        mn->pdmnState = state;
        mn_list.AddMN(mn);
        candidates.emplace_back(protx);
        const bool non_signer{i < 2};
        records.emplace(protx, std::vector<llmq::SigningRecord>(6, {1, !non_signer}));
    }

    const auto votes = llmq::SelectSigningHistoryVotes(records, mn_list, candidates, /*min_size=*/6);
    BOOST_CHECK_EQUAL(votes.size(), 2U);
    BOOST_CHECK(votes.at(candidates[0]) == std::make_pair(6, 0));
    BOOST_CHECK(votes.at(candidates[1]) == std::make_pair(6, 0));

    BOOST_CHECK(llmq::SelectSigningHistoryVotes(records, mn_list, candidates, /*min_size=*/7).empty());
    BOOST_CHECK(llmq::SelectSigningHistoryVotes(records, mn_list, {candidates.begin(), candidates.begin() + 7}, 6).empty());
}

BOOST_AUTO_TEST_SUITE_END()
