// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/deterministicmns.h>
#include <governance/governance.h>
#include <governance/object.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <streams.h>
#include <uint256.h>
#include <version.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <ios>
#include <limits>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(governance_vote_wire_tests, BasicTestingSetup)

namespace {
void WriteVoteHeader(CDataStream& ss)
{
    ss << COutPoint{uint256::ONE, 0} << uint256::ONE
       << int{1} /*outcome*/ << int{1} /*signal*/ << int64_t{1'700'000'000};
}

CDataStream MakeVoteWire(size_t sig_len)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    WriteVoteHeader(ss);
    ss << std::vector<unsigned char>(sig_len, 0xAA);
    return ss;
}
} // namespace

// Reject invalid signature lengths, including a maximal CompactSize prefix.
BOOST_AUTO_TEST_CASE(rejects_invalid_sizes)
{
    for (size_t bad : {size_t{0}, size_t{64}, size_t{66}, size_t{95}, size_t{97}, size_t{128}}) {
        CDataStream ss = MakeVoteWire(bad);
        CGovernanceVote vote;
        BOOST_CHECK_THROW(ss >> vote, std::ios_base::failure);
    }

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    WriteVoteHeader(ss);
    WriteCompactSize(ss, std::numeric_limits<uint64_t>::max());
    CGovernanceVote vote;
    BOOST_CHECK_THROW(ss >> vote, std::ios_base::failure);
}

// Truncated element bytes must surface as ios_base::failure so the govobjvote
// handler scores the peer.
BOOST_AUTO_TEST_CASE(truncated_signature_throws_ios_failure)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    WriteVoteHeader(ss);
    ss << uint8_t{CGovernanceVote::BLS_SIG_SIZE};
    ss.write(MakeByteSpan(std::vector<unsigned char>(10, 0xBB)));

    CGovernanceVote vote;
    BOOST_CHECK_THROW(ss >> vote, std::ios_base::failure);
}

// 65-byte ECDSA and 96-byte BLS round-trip cleanly over the network.
BOOST_AUTO_TEST_CASE(accepts_legitimate_boundary_sizes)
{
    for (size_t sig_len : {CGovernanceVote::COMPACT_SIG_SIZE, CGovernanceVote::BLS_SIG_SIZE}) {
        CDataStream ss = MakeVoteWire(sig_len);
        const size_t wire_bytes = ss.size();

        CGovernanceVote vote;
        BOOST_REQUIRE_NO_THROW(ss >> vote);
        BOOST_CHECK_EQUAL(ss.size(), 0U);

        CDataStream out(SER_NETWORK, PROTOCOL_VERSION);
        out << vote;
        BOOST_CHECK_EQUAL(out.size(), wire_bytes);
    }
}

// SER_DISK reads stay unbounded — existing on-disk data must load unchanged.
BOOST_AUTO_TEST_CASE(ser_disk_deserialization_unaffected)
{
    CDataStream ss(SER_DISK, PROTOCOL_VERSION);
    WriteVoteHeader(ss);
    ss << std::vector<unsigned char>(128, 0xCD);

    CGovernanceVote vote;
    BOOST_REQUIRE_NO_THROW(ss >> vote);
    BOOST_CHECK_EQUAL(ss.size(), 0U);
}

// Object signatures are bounded to 96 bytes (BLS) on network and disk reads alike.
BOOST_AUTO_TEST_CASE(object_signature_size_limit)
{
    Governance::Object object;
    for (int ser_type : {SER_NETWORK, SER_DISK}) {
        for (size_t sig_len : {size_t{0}, size_t{65}, size_t{96}, size_t{97}}) {
            object.vchSig.assign(sig_len, 0xAA);
            CDataStream ss(ser_type, PROTOCOL_VERSION);
            ss << object;

            Governance::Object decoded;
            if (sig_len <= Governance::Object::MAX_SIGNATURE_SIZE) {
                BOOST_CHECK_NO_THROW(ss >> decoded);
                BOOST_CHECK(decoded.vchSig == object.vchSig);
            } else {
                BOOST_CHECK_THROW(ss >> decoded, std::ios_base::failure);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(extreme_timestamp_is_rejected_without_overflow)
{
    CGovernanceVote vote;
    vote.SetTime(std::numeric_limits<int64_t>::max());

    BOOST_CHECK(!vote.IsValid(CDeterministicMNList{}, /*useVotingKey=*/false));
}

BOOST_AUTO_TEST_CASE(chrono_cache_times_preserve_disk_encoding)
{
    CDataStream vote_stream = MakeVoteWire(CGovernanceVote::COMPACT_SIG_SIZE);
    CGovernanceVote vote;
    vote_stream >> vote;

    constexpr int64_t expiration{1'700'000'600};
    const governance::OrphanVote orphan_vote{vote, NodeSeconds{std::chrono::seconds{expiration}}};

    CDataStream chrono_encoding{SER_DISK, PROTOCOL_VERSION};
    chrono_encoding << orphan_vote;
    CDataStream integer_encoding{SER_DISK, PROTOCOL_VERSION};
    integer_encoding << vote << expiration;
    BOOST_CHECK_EQUAL_COLLECTIONS(
        chrono_encoding.begin(), chrono_encoding.end(), integer_encoding.begin(), integer_encoding.end());

    governance::OrphanVote decoded;
    chrono_encoding >> decoded;
    BOOST_CHECK(decoded.vote.GetHash() == vote.GetHash());
    BOOST_CHECK_EQUAL(decoded.expiration.time_since_epoch().count(), expiration);

    constexpr int64_t creation_time{1'700'000'000};
    const vote_instance_t vote_instance{
        VOTE_OUTCOME_YES, NodeSeconds{std::chrono::seconds{expiration}}, creation_time};
    CDataStream chrono_vote_instance{SER_DISK, PROTOCOL_VERSION};
    chrono_vote_instance << vote_instance;
    CDataStream integer_vote_instance{SER_DISK, PROTOCOL_VERSION};
    integer_vote_instance << int{VOTE_OUTCOME_YES} << expiration << creation_time;
    BOOST_CHECK_EQUAL_COLLECTIONS(
        chrono_vote_instance.begin(), chrono_vote_instance.end(), integer_vote_instance.begin(), integer_vote_instance.end());

    vote_instance_t decoded_vote_instance;
    chrono_vote_instance >> decoded_vote_instance;
    BOOST_CHECK_EQUAL(decoded_vote_instance.eOutcome, VOTE_OUTCOME_YES);
    BOOST_CHECK_EQUAL(decoded_vote_instance.last_update.time_since_epoch().count(), expiration);
    BOOST_CHECK_EQUAL(decoded_vote_instance.nCreationTime, creation_time);
}

BOOST_AUTO_TEST_SUITE_END()
