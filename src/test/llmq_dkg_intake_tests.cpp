// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls/bls.h>
#include <llmq/commitment.h>
#include <llmq/dkgmessages.h>
#include <llmq/params.h>
#include <serialize.h>
#include <streams.h>
#include <test/util/llmq_tests.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <ios>
#include <vector>

using namespace llmq;
using namespace llmq::testutils;

BOOST_FIXTURE_TEST_SUITE(llmq_dkg_intake_tests, BasicTestingSetup)

// A short read throws ios_base::failure too, so asserting the exception type alone would pass even
// with the limits removed. These match the limit's own message instead.
static auto BitsetLimitExceeded()
{
    return [](const std::ios_base::failure& e) {
        return std::string_view{e.what()}.find("Bitset length limit exceeded") != std::string_view::npos;
    };
}

static auto VectorLimitExceeded()
{
    return [](const std::ios_base::failure& e) {
        return std::string_view{e.what()}.find("Vector length limit exceeded") != std::string_view::npos;
    };
}

// A bitset's length is declared on the wire as a CompactSize, so five bytes can claim MAX_SIZE
// (33.5M) bits. Before the length was bounded, ReadFixedBitSet resized the bit vector and allocated
// a packed byte buffer for the full declared count -- roughly 8 MiB from a ~70-byte payload -- and
// only then threw on the short read. The limit must be applied before any allocation.
BOOST_AUTO_TEST_CASE(bitset_length_bounded_before_allocation)
{
    CDataStream s(SER_NETWORK, PROTOCOL_VERSION);
    WriteCompactSize(s, MAX_SIZE);

    std::vector<bool> bits;
    BOOST_CHECK_EXCEPTION(LimitedBitSetFormatter<Consensus::MAX_LLMQ_SIZE>().Unser(s, bits),
                          std::ios_base::failure, BitsetLimitExceeded());
    BOOST_CHECK(bits.empty());
}

// One bit past the limit is rejected; exactly the limit is accepted. A quorum can never exceed
// MAX_LLMQ_SIZE members (chainparams rejects such a params set at startup), so the boundary is the
// largest bitset any honest LLMQ message can carry.
BOOST_AUTO_TEST_CASE(bitset_limit_boundary)
{
    const auto roundtrip = [](size_t bit_count) {
        CDataStream s(SER_NETWORK, PROTOCOL_VERSION);
        LimitedBitSetFormatter<Consensus::MAX_LLMQ_SIZE>().Ser(s, std::vector<bool>(bit_count, true));
        std::vector<bool> out;
        LimitedBitSetFormatter<Consensus::MAX_LLMQ_SIZE>().Unser(s, out);
        return out;
    };

    const std::vector<bool> at_limit = roundtrip(Consensus::MAX_LLMQ_SIZE);
    BOOST_CHECK_EQUAL(at_limit.size(), size_t(Consensus::MAX_LLMQ_SIZE));
    BOOST_CHECK(std::all_of(at_limit.begin(), at_limit.end(), [](bool b) { return b; }));

    BOOST_CHECK_THROW(roundtrip(Consensus::MAX_LLMQ_SIZE + 1), std::ios_base::failure);
}

// The wire format must be unchanged by the bound: the limit is a deserialization-side check only,
// so a bitset still round-trips bit-for-bit, including the partial trailing byte.
BOOST_AUTO_TEST_CASE(bitset_roundtrip_preserves_bits)
{
    std::vector<bool> src(77);
    src[0] = src[13] = src[76] = true;

    CDataStream s(SER_NETWORK, PROTOCOL_VERSION);
    LimitedBitSetFormatter<Consensus::MAX_LLMQ_SIZE>().Ser(s, src);
    // CompactSize(77) + ceil(77/8) packed bytes.
    BOOST_CHECK_EQUAL(s.size(), size_t{1} + 10);

    std::vector<bool> dst;
    LimitedBitSetFormatter<Consensus::MAX_LLMQ_SIZE>().Unser(s, dst);
    BOOST_CHECK(src == dst);
}

// The same bound has to hold through the real message serializers, which is where an attacker
// actually reaches it: QCOMPLAINT and QPCOMMITMENT carry member bitsets, and CFinalCommitment
// carries signers/validMembers reachable from any peer via QFCOMMITMENT.
BOOST_AUTO_TEST_CASE(oversized_bitset_rejected_through_message_serializers)
{
    const auto oversized_prefix = [](Consensus::LLMQType type) {
        CDataStream s(SER_NETWORK, PROTOCOL_VERSION);
        s << type << uint256::ONE << uint256::TWO;
        WriteCompactSize(s, MAX_SIZE); // first bitset, with no payload behind it
        return s;
    };

    const Consensus::LLMQType type = Consensus::LLMQType::LLMQ_400_85;

    CDataStream complaint = oversized_prefix(type);
    CDKGComplaint qc;
    BOOST_CHECK_EXCEPTION(complaint >> qc, std::ios_base::failure, BitsetLimitExceeded());

    CDataStream commitment = oversized_prefix(type);
    CDKGPrematureCommitment qpc;
    BOOST_CHECK_EXCEPTION(commitment >> qpc, std::ios_base::failure, BitsetLimitExceeded());

    CDataStream final_commitment(SER_NETWORK, PROTOCOL_VERSION);
    final_commitment << uint16_t{CFinalCommitment::LEGACY_BLS_NON_INDEXED_QUORUM_VERSION} << type << uint256::ONE;
    WriteCompactSize(final_commitment, MAX_SIZE); // signers
    CFinalCommitment fqc;
    BOOST_CHECK_EXCEPTION(final_commitment >> fqc, std::ios_base::failure, BitsetLimitExceeded());
}

// Vector-valued DKG fields are bounded the same way, so a declared element count cannot outrun the
// bytes behind it before the worker gets a chance to reject the message.
BOOST_AUTO_TEST_CASE(oversized_vectors_rejected_through_message_serializers)
{
    const Consensus::LLMQType type = Consensus::LLMQType::LLMQ_400_85;

    CDataStream justification(SER_NETWORK, PROTOCOL_VERSION);
    justification << type << uint256::ONE << uint256::TWO;
    WriteCompactSize(justification, Consensus::MAX_LLMQ_SIZE + 1);
    CDKGJustification qj;
    BOOST_CHECK_EXCEPTION(justification >> qj, std::ios_base::failure, VectorLimitExceeded());

    CDataStream contribution(SER_NETWORK, PROTOCOL_VERSION);
    contribution << type << uint256::ONE << uint256::TWO;
    WriteCompactSize(contribution, Consensus::MAX_LLMQ_SIZE + 1); // vvec
    CDKGContribution qc;
    BOOST_CHECK_EXCEPTION(contribution >> qc, std::ios_base::failure, VectorLimitExceeded());
}

// Every DKG message a real quorum can produce must still round-trip, for the smallest and the
// largest quorum type -- a bound that rejected honest traffic would partition the node.
BOOST_AUTO_TEST_CASE(well_formed_messages_roundtrip)
{
    for (const auto type : {Consensus::LLMQType::LLMQ_TEST, Consensus::LLMQType::LLMQ_400_85}) {
        const Consensus::LLMQParams& params = GetLLMQParams(type);

        CDKGComplaint complaint(params);
        complaint.llmqType = params.type;
        complaint.quorumHash = uint256::ONE;
        complaint.proTxHash = uint256::TWO;
        for (int i = 0; i < params.size; i += 2) {
            complaint.badMembers[i] = true;
        }
        complaint.complainForMembers[0] = true;
        complaint.sig = CreateRandomBLSSignature();

        CDataStream s(SER_NETWORK, PROTOCOL_VERSION);
        s << complaint;
        CDKGComplaint decoded;
        s >> decoded;
        BOOST_CHECK(decoded.badMembers == complaint.badMembers);
        BOOST_CHECK(decoded.complainForMembers == complaint.complainForMembers);

        CDKGPrematureCommitment commitment(params);
        commitment.llmqType = params.type;
        commitment.quorumHash = uint256::ONE;
        commitment.proTxHash = uint256::TWO;
        for (int i = 0; i < params.size; ++i) {
            commitment.validMembers[i] = true;
        }
        commitment.quorumPublicKey = CreateRandomBLSPublicKey();
        commitment.quorumVvecHash = uint256::ONE;
        commitment.quorumSig = CreateRandomBLSSignature();
        commitment.sig = CreateRandomBLSSignature();

        CDataStream s2(SER_NETWORK, PROTOCOL_VERSION);
        s2 << commitment;
        CDKGPrematureCommitment decoded2;
        s2 >> decoded2;
        BOOST_CHECK(decoded2.validMembers == commitment.validMembers);
        BOOST_CHECK_EQUAL(decoded2.CountValidMembers(), params.size);

        // Worst legitimate justification: one contribution per quorum member, at the bound.
        CDKGJustification justification;
        justification.llmqType = params.type;
        justification.quorumHash = uint256::ONE;
        justification.proTxHash = uint256::TWO;
        for (int i = 0; i < params.size; ++i) {
            CBLSSecretKey sk;
            sk.MakeNewKey();
            justification.contributions.push_back({static_cast<uint32_t>(i), sk});
        }
        justification.sig = CreateRandomBLSSignature();

        CDataStream s3(SER_NETWORK, PROTOCOL_VERSION);
        s3 << justification;
        CDKGJustification decoded3;
        s3 >> decoded3;
        BOOST_CHECK_EQUAL(decoded3.contributions.size(), size_t(params.size));

        // A member with nothing to justify sends an empty vector; the bound must admit that too.
        justification.contributions.clear();
        CDataStream s4(SER_NETWORK, PROTOCOL_VERSION);
        s4 << justification;
        CDKGJustification decoded4;
        s4 >> decoded4;
        BOOST_CHECK(decoded4.contributions.empty());

        // CDKGContribution hand-rolls Serialize/Unserialize rather than using SERIALIZE_METHODS, so
        // its vvec bound is wired up separately and needs its own round-trip. threshold is the
        // largest vvec an honest member sends (340 for llmq_400_85).
        CDKGContribution contribution;
        contribution.llmqType = params.type;
        contribution.quorumHash = uint256::ONE;
        contribution.proTxHash = uint256::TWO;
        auto vvec = std::make_shared<std::vector<CBLSPublicKey>>();
        for (int i = 0; i < params.threshold; ++i) {
            vvec->push_back(CreateRandomBLSPublicKey());
        }
        contribution.vvec = std::move(vvec);
        auto contributions = std::make_shared<CBLSIESMultiRecipientObjects<CBLSSecretKey>>();
        contributions->ephemeralPubKey = CreateRandomBLSPublicKey();
        contributions->ivSeed = uint256::ONE;
        contributions->blobs.assign(params.size, std::vector<unsigned char>(48, 0xab));
        contribution.contributions = std::move(contributions);
        contribution.sig = CreateRandomBLSSignature();

        CDataStream s5(SER_NETWORK, PROTOCOL_VERSION);
        s5 << contribution;
        CDKGContribution decoded5;
        s5 >> decoded5;
        BOOST_CHECK_EQUAL(decoded5.vvec->size(), size_t(params.threshold));
        BOOST_CHECK_EQUAL(decoded5.contributions->blobs.size(), size_t(params.size));
        BOOST_CHECK(decoded5.vvec->front() == contribution.vvec->front());
    }
}

BOOST_AUTO_TEST_SUITE_END()
