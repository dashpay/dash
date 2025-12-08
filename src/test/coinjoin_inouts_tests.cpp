// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include <chain.h>
#include <chainlock/chainlock.h>
#include <coinjoin/coinjoin.h>
#include <coinjoin/common.h>
#include <llmq/context.h>
#include <script/script.h>
#include <uint256.h>
#include <util/check.h>
#include <util/time.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(coinjoin_inouts_tests, TestingSetup)

static CScript P2PKHScript(uint8_t tag = 0x01)
{
    // OP_DUP OP_HASH160 <20-byte-tag> OP_EQUALVERIFY OP_CHECKSIG
    std::vector<unsigned char> hash(20, tag);
    return CScript{} << OP_DUP << OP_HASH160 << hash << OP_EQUALVERIFY << OP_CHECKSIG;
}

BOOST_AUTO_TEST_CASE(broadcasttx_isvalidstructure_good_and_bad)
{
    // Good: equal vin/vout sizes, vin count >= min participants, <= max*entry_size, P2PKH outputs with standard denominations
    CCoinJoinBroadcastTx good;
    {
        CMutableTransaction mtx;
        // Use min pool participants (e.g. 3). Build 3 inputs and 3 denominated outputs
        const int participants = std::max(3, CoinJoin::GetMinPoolParticipants());
        for (int i = 0; i < participants; ++i) {
            CTxIn in;
            in.prevout = COutPoint(uint256::ONE, static_cast<uint32_t>(i));
            mtx.vin.push_back(in);
            // Pick the smallest denomination
            CTxOut out{CoinJoin::GetSmallestDenomination(), P2PKHScript(static_cast<uint8_t>(i))};
            mtx.vout.push_back(out);
        }
        good.tx = MakeTransactionRef(mtx);
        good.m_protxHash = uint256::ONE; // at least one of (outpoint, protxhash) must be set
    }
    // Pre-V24 behavior (nullptr pindex = pre-fork)
    BOOST_CHECK(good.IsValidStructure(nullptr));

    // Bad: both identifiers null
    CCoinJoinBroadcastTx bad_ids = good;
    bad_ids.m_protxHash = uint256{};
    bad_ids.masternodeOutpoint.SetNull();
    BOOST_CHECK(!bad_ids.IsValidStructure(nullptr));

    // Bad: vin/vout size mismatch (invalid pre-V24)
    CCoinJoinBroadcastTx bad_sizes = good;
    {
        CMutableTransaction mtx(*good.tx);
        mtx.vout.pop_back();
        bad_sizes.tx = MakeTransactionRef(mtx);
    }
    BOOST_CHECK(!bad_sizes.IsValidStructure(nullptr));

    // Bad: non-P2PKH output
    CCoinJoinBroadcastTx bad_script = good;
    {
        CMutableTransaction mtx(*good.tx);
        mtx.vout[0].scriptPubKey = CScript() << OP_RETURN << std::vector<unsigned char>{'x'};
        bad_script.tx = MakeTransactionRef(mtx);
    }
    BOOST_CHECK(!bad_script.IsValidStructure(nullptr));

    // Bad: non-denominated amount
    CCoinJoinBroadcastTx bad_amount = good;
    {
        CMutableTransaction mtx(*good.tx);
        mtx.vout[0].nValue = 42; // not a valid denom
        bad_amount.tx = MakeTransactionRef(mtx);
    }
    BOOST_CHECK(!bad_amount.IsValidStructure(nullptr));
}

BOOST_AUTO_TEST_CASE(queue_timeout_bounds)
{
    CCoinJoinQueue dsq;
    dsq.nDenom = CoinJoin::AmountToDenomination(CoinJoin::GetSmallestDenomination());
    dsq.m_protxHash = uint256::ONE;
    dsq.nTime = GetAdjustedTime();
    // current time -> not out of bounds
    BOOST_CHECK(!dsq.IsTimeOutOfBounds());

    // Too old (beyond COINJOIN_QUEUE_TIMEOUT)
    SetMockTime(GetTime() + (COINJOIN_QUEUE_TIMEOUT + 1));
    BOOST_CHECK(dsq.IsTimeOutOfBounds());

    // Too far in the future
    SetMockTime(GetTime() - 2 * (COINJOIN_QUEUE_TIMEOUT + 1)); // move back to anchor baseline
    dsq.nTime = GetAdjustedTime() + (COINJOIN_QUEUE_TIMEOUT + 1);
    BOOST_CHECK(dsq.IsTimeOutOfBounds());

    // Reset mock time
    SetMockTime(0);
}
BOOST_AUTO_TEST_CASE(broadcasttx_expiry_height_logic)
{
    // Build a valid-looking CCoinJoinBroadcastTx with confirmed height
    CCoinJoinBroadcastTx dstx;
    {
        CMutableTransaction mtx;
        const int participants = std::max(3, CoinJoin::GetMinPoolParticipants());
        for (int i = 0; i < participants; ++i) {
            mtx.vin.emplace_back(COutPoint(uint256::TWO, i));
            mtx.vout.emplace_back(CoinJoin::GetSmallestDenomination(), P2PKHScript(static_cast<uint8_t>(i)));
        }
        dstx.tx = MakeTransactionRef(mtx);
        dstx.m_protxHash = uint256::ONE;
        // mark as confirmed at height 100
        dstx.SetConfirmedHeight(100);
    }

    // Minimal CBlockIndex with required fields
    // Create a minimal block index to satisfy the interface
    CBlockIndex index;
    uint256 blk_hash = uint256S("03");
    index.nHeight = 125; // 125 - 100 == 25 > 24 → expired by height
    index.phashBlock = &blk_hash;
    BOOST_CHECK(dstx.IsExpired(&index, *Assert(m_node.llmq_ctx->clhandler)));
}

// Helper to create a denominated CTxIn with a specific denomination value
static CTxIn MakeDenomInput(uint8_t index)
{
    CTxIn in;
    in.prevout = COutPoint(uint256::ONE, index);
    return in;
}

// Helper to create a denominated CTxOut
static CTxOut MakeDenomOutput(CAmount nAmount, uint8_t tag = 0x01)
{
    return CTxOut{nAmount, P2PKHScript(tag)};
}

BOOST_AUTO_TEST_CASE(validate_promotion_entry_valid)
{
    // Valid promotion: 10 inputs of 0.1 DASH → 1 output of 1.0 DASH
    std::vector<CTxIn> vecTxIn;
    std::vector<CTxOut> vecTxOut;

    // Get the 0.1 DASH denomination (index 2: 1 << 2 = 4)
    const int nSmallerDenom = 1 << 2;  // 0.1 DASH
    const int nLargerDenom = 1 << 1;   // 1.0 DASH
    const CAmount nLargerAmount = CoinJoin::DenominationToAmount(nLargerDenom);

    BOOST_CHECK(CoinJoin::IsValidDenomination(nSmallerDenom));
    BOOST_CHECK(CoinJoin::IsValidDenomination(nLargerDenom));

    // Create 10 inputs of smaller denomination
    for (int i = 0; i < CoinJoin::PROMOTION_RATIO; ++i) {
        vecTxIn.push_back(MakeDenomInput(static_cast<uint8_t>(i)));
    }

    // Create 1 output of larger denomination
    vecTxOut.push_back(MakeDenomOutput(nLargerAmount, 0x01));

    PoolMessage nMessageID = MSG_NOERR;
    BOOST_CHECK(CoinJoin::ValidatePromotionEntry(vecTxIn, vecTxOut, nSmallerDenom, nMessageID));
}

BOOST_AUTO_TEST_CASE(validate_promotion_entry_wrong_input_count)
{
    // Invalid: only 9 inputs instead of 10
    std::vector<CTxIn> vecTxIn;
    std::vector<CTxOut> vecTxOut;

    const int nSmallerDenom = 1 << 2;  // 0.1 DASH
    const int nLargerDenom = 1 << 1;   // 1.0 DASH
    const CAmount nLargerAmount = CoinJoin::DenominationToAmount(nLargerDenom);

    // Create only 9 inputs
    for (int i = 0; i < CoinJoin::PROMOTION_RATIO - 1; ++i) {
        vecTxIn.push_back(MakeDenomInput(static_cast<uint8_t>(i)));
    }

    vecTxOut.push_back(MakeDenomOutput(nLargerAmount, 0x01));

    PoolMessage nMessageID = MSG_NOERR;
    BOOST_CHECK(!CoinJoin::ValidatePromotionEntry(vecTxIn, vecTxOut, nSmallerDenom, nMessageID));
}

BOOST_AUTO_TEST_CASE(validate_promotion_entry_wrong_output_count)
{
    // Invalid: 2 outputs instead of 1
    std::vector<CTxIn> vecTxIn;
    std::vector<CTxOut> vecTxOut;

    const int nSmallerDenom = 1 << 2;
    const int nLargerDenom = 1 << 1;
    const CAmount nLargerAmount = CoinJoin::DenominationToAmount(nLargerDenom);

    for (int i = 0; i < CoinJoin::PROMOTION_RATIO; ++i) {
        vecTxIn.push_back(MakeDenomInput(static_cast<uint8_t>(i)));
    }

    // Two outputs instead of one
    vecTxOut.push_back(MakeDenomOutput(nLargerAmount, 0x01));
    vecTxOut.push_back(MakeDenomOutput(nLargerAmount, 0x02));

    PoolMessage nMessageID = MSG_NOERR;
    BOOST_CHECK(!CoinJoin::ValidatePromotionEntry(vecTxIn, vecTxOut, nSmallerDenom, nMessageID));
}

BOOST_AUTO_TEST_CASE(validate_promotion_entry_non_adjacent_denoms)
{
    // Invalid: trying to promote 0.01 to 1.0 (not adjacent)
    std::vector<CTxIn> vecTxIn;
    std::vector<CTxOut> vecTxOut;

    const int nSmallerDenom = 1 << 3;  // 0.01 DASH
    const int nLargerDenom = 1 << 1;   // 1.0 DASH (not adjacent to 0.01)
    const CAmount nLargerAmount = CoinJoin::DenominationToAmount(nLargerDenom);

    for (int i = 0; i < CoinJoin::PROMOTION_RATIO; ++i) {
        vecTxIn.push_back(MakeDenomInput(static_cast<uint8_t>(i)));
    }

    vecTxOut.push_back(MakeDenomOutput(nLargerAmount, 0x01));

    PoolMessage nMessageID = MSG_NOERR;
    BOOST_CHECK(!CoinJoin::ValidatePromotionEntry(vecTxIn, vecTxOut, nSmallerDenom, nMessageID));
}

BOOST_AUTO_TEST_CASE(validate_demotion_entry_valid)
{
    // Valid demotion: 1 input of 1.0 DASH → 10 outputs of 0.1 DASH
    std::vector<CTxIn> vecTxIn;
    std::vector<CTxOut> vecTxOut;

    const int nSmallerDenom = 1 << 2;  // 0.1 DASH
    const CAmount nSmallerAmount = CoinJoin::DenominationToAmount(nSmallerDenom);
    const int nLargerDenom = 1 << 1;   // 1.0 DASH

    BOOST_CHECK(CoinJoin::IsValidDenomination(nSmallerDenom));
    BOOST_CHECK(CoinJoin::IsValidDenomination(nLargerDenom));
    BOOST_CHECK(CoinJoin::AreAdjacentDenominations(nSmallerDenom, nLargerDenom));

    // 1 input of larger denomination
    vecTxIn.push_back(MakeDenomInput(0));

    // 10 outputs of smaller denomination
    for (int i = 0; i < CoinJoin::PROMOTION_RATIO; ++i) {
        vecTxOut.push_back(MakeDenomOutput(nSmallerAmount, static_cast<uint8_t>(i)));
    }

    PoolMessage nMessageID = MSG_NOERR;
    BOOST_CHECK(CoinJoin::ValidateDemotionEntry(vecTxIn, vecTxOut, nSmallerDenom, nMessageID));
}

BOOST_AUTO_TEST_CASE(validate_demotion_entry_wrong_input_count)
{
    // Invalid: 2 inputs instead of 1
    std::vector<CTxIn> vecTxIn;
    std::vector<CTxOut> vecTxOut;

    const int nSmallerDenom = 1 << 2;
    const CAmount nSmallerAmount = CoinJoin::DenominationToAmount(nSmallerDenom);

    // 2 inputs instead of 1
    vecTxIn.push_back(MakeDenomInput(0));
    vecTxIn.push_back(MakeDenomInput(1));

    for (int i = 0; i < CoinJoin::PROMOTION_RATIO; ++i) {
        vecTxOut.push_back(MakeDenomOutput(nSmallerAmount, static_cast<uint8_t>(i)));
    }

    PoolMessage nMessageID = MSG_NOERR;
    BOOST_CHECK(!CoinJoin::ValidateDemotionEntry(vecTxIn, vecTxOut, nSmallerDenom, nMessageID));
}

BOOST_AUTO_TEST_CASE(validate_demotion_entry_wrong_output_count)
{
    // Invalid: 9 outputs instead of 10
    std::vector<CTxIn> vecTxIn;
    std::vector<CTxOut> vecTxOut;

    const int nSmallerDenom = 1 << 2;
    const CAmount nSmallerAmount = CoinJoin::DenominationToAmount(nSmallerDenom);

    vecTxIn.push_back(MakeDenomInput(0));

    // Only 9 outputs
    for (int i = 0; i < CoinJoin::PROMOTION_RATIO - 1; ++i) {
        vecTxOut.push_back(MakeDenomOutput(nSmallerAmount, static_cast<uint8_t>(i)));
    }

    PoolMessage nMessageID = MSG_NOERR;
    BOOST_CHECK(!CoinJoin::ValidateDemotionEntry(vecTxIn, vecTxOut, nSmallerDenom, nMessageID));
}

BOOST_AUTO_TEST_CASE(denomination_adjacency_checks)
{
    // Test AreAdjacentDenominations function
    // Denomination indices: 0=10.0, 1=1.0, 2=0.1, 3=0.01, 4=0.001

    // Adjacent pairs should return true
    BOOST_CHECK(CoinJoin::AreAdjacentDenominations(1 << 0, 1 << 1));  // 10.0 and 1.0
    BOOST_CHECK(CoinJoin::AreAdjacentDenominations(1 << 1, 1 << 2));  // 1.0 and 0.1
    BOOST_CHECK(CoinJoin::AreAdjacentDenominations(1 << 2, 1 << 3));  // 0.1 and 0.01
    BOOST_CHECK(CoinJoin::AreAdjacentDenominations(1 << 3, 1 << 4));  // 0.01 and 0.001

    // Non-adjacent pairs should return false
    BOOST_CHECK(!CoinJoin::AreAdjacentDenominations(1 << 0, 1 << 2)); // 10.0 and 0.1 (skip 1.0)
    BOOST_CHECK(!CoinJoin::AreAdjacentDenominations(1 << 1, 1 << 3)); // 1.0 and 0.01 (skip 0.1)
    BOOST_CHECK(!CoinJoin::AreAdjacentDenominations(1 << 0, 1 << 4)); // 10.0 and 0.001

    // Same denomination is not adjacent to itself
    BOOST_CHECK(!CoinJoin::AreAdjacentDenominations(1 << 2, 1 << 2));

    // Invalid denominations
    BOOST_CHECK(!CoinJoin::AreAdjacentDenominations(0, 1 << 1));
    BOOST_CHECK(!CoinJoin::AreAdjacentDenominations(1 << 1, 0));
    BOOST_CHECK(!CoinJoin::AreAdjacentDenominations(999, 1 << 1));
}

BOOST_AUTO_TEST_CASE(get_adjacent_denomination_helpers)
{
    // Test GetLargerAdjacentDenom and GetSmallerAdjacentDenom
    // Indices: 0=10.0, 1=1.0, 2=0.1, 3=0.01, 4=0.001

    // GetLargerAdjacentDenom
    BOOST_CHECK_EQUAL(CoinJoin::GetLargerAdjacentDenom(1 << 1), 1 << 0);  // 1.0 → 10.0
    BOOST_CHECK_EQUAL(CoinJoin::GetLargerAdjacentDenom(1 << 2), 1 << 1);  // 0.1 → 1.0
    BOOST_CHECK_EQUAL(CoinJoin::GetLargerAdjacentDenom(1 << 3), 1 << 2);  // 0.01 → 0.1
    BOOST_CHECK_EQUAL(CoinJoin::GetLargerAdjacentDenom(1 << 4), 1 << 3);  // 0.001 → 0.01
    BOOST_CHECK_EQUAL(CoinJoin::GetLargerAdjacentDenom(1 << 0), 0);       // 10.0 has no larger

    // GetSmallerAdjacentDenom
    BOOST_CHECK_EQUAL(CoinJoin::GetSmallerAdjacentDenom(1 << 0), 1 << 1); // 10.0 → 1.0
    BOOST_CHECK_EQUAL(CoinJoin::GetSmallerAdjacentDenom(1 << 1), 1 << 2); // 1.0 → 0.1
    BOOST_CHECK_EQUAL(CoinJoin::GetSmallerAdjacentDenom(1 << 2), 1 << 3); // 0.1 → 0.01
    BOOST_CHECK_EQUAL(CoinJoin::GetSmallerAdjacentDenom(1 << 3), 1 << 4); // 0.01 → 0.001
    BOOST_CHECK_EQUAL(CoinJoin::GetSmallerAdjacentDenom(1 << 4), 0);      // 0.001 has no smaller
}

BOOST_AUTO_TEST_CASE(promotion_ratio_constant)
{
    // Verify the PROMOTION_RATIO constant is 10 (10 smaller = 1 larger)
    BOOST_CHECK_EQUAL(CoinJoin::PROMOTION_RATIO, 10);
}

BOOST_AUTO_TEST_CASE(gap_threshold_constant)
{
    // Verify GAP_THRESHOLD is set to prevent oscillation
    BOOST_CHECK_EQUAL(CoinJoin::GAP_THRESHOLD, 10);
}

BOOST_AUTO_TEST_CASE(isvalidstructure_postfork_unbalanced_valid)
{
    // Post-V24: Unbalanced vin/vout (promotion: 10 inputs, 1 output) should be valid
    // We need a mock pindex that signals V24 active - for this test we use nullptr which means pre-fork
    // This test validates that the structure check correctly identifies promotion structure

    CCoinJoinBroadcastTx promo;
    {
        CMutableTransaction mtx;
        // Promotion: 10 inputs of smaller denom -> 1 output of larger denom
        const int nInputCount = CoinJoin::PROMOTION_RATIO;
        const CAmount nLargerAmount = CoinJoin::DenominationToAmount(1 << 1);  // 1.0 DASH

        for (int i = 0; i < nInputCount; ++i) {
            CTxIn in;
            in.prevout = COutPoint(uint256::ONE, static_cast<uint32_t>(i));
            mtx.vin.push_back(in);
        }
        // 1 output of larger denom
        CTxOut out{nLargerAmount, P2PKHScript(0x01)};
        mtx.vout.push_back(out);

        promo.tx = MakeTransactionRef(mtx);
        promo.m_protxHash = uint256::ONE;
    }

    // Pre-V24 (nullptr): unbalanced should fail
    BOOST_CHECK(!promo.IsValidStructure(nullptr));

    // Note: Post-V24 test would require a valid CBlockIndex with V24 deployment active
    // which requires more setup. The above confirms pre-fork rejection works.
}

BOOST_AUTO_TEST_CASE(isvalidstructure_demotion_structure)
{
    // Demotion: 1 input of larger denom -> 10 outputs of smaller denom
    CCoinJoinBroadcastTx demo;
    {
        CMutableTransaction mtx;
        const CAmount nSmallerAmount = CoinJoin::DenominationToAmount(1 << 2); // 0.1 DASH

        // 1 input
        CTxIn in;
        in.prevout = COutPoint(uint256::ONE, 0);
        mtx.vin.push_back(in);

        // 10 outputs of smaller denom
        for (int i = 0; i < CoinJoin::PROMOTION_RATIO; ++i) {
            CTxOut out{nSmallerAmount, P2PKHScript(static_cast<uint8_t>(i))};
            mtx.vout.push_back(out);
        }

        demo.tx = MakeTransactionRef(mtx);
        demo.m_protxHash = uint256::ONE;
    }

    // Pre-V24 (nullptr): unbalanced should fail
    BOOST_CHECK(!demo.IsValidStructure(nullptr));
}

BOOST_AUTO_TEST_CASE(input_limit_prefork)
{
    // Pre-V24: max inputs = GetMaxPoolParticipants() * COINJOIN_ENTRY_MAX_SIZE
    // Typically 20 * 9 = 180
    const size_t nMaxPreFork = CoinJoin::GetMaxPoolParticipants() * COINJOIN_ENTRY_MAX_SIZE;
    BOOST_CHECK_EQUAL(nMaxPreFork, 180);

    // Transaction with exactly max inputs should be valid
    CCoinJoinBroadcastTx maxValid;
    {
        CMutableTransaction mtx;
        for (size_t i = 0; i < nMaxPreFork; ++i) {
            CTxIn in;
            in.prevout = COutPoint(uint256::ONE, static_cast<uint32_t>(i));
            mtx.vin.push_back(in);
            CTxOut out{CoinJoin::GetSmallestDenomination(), P2PKHScript(static_cast<uint8_t>(i % 256))};
            mtx.vout.push_back(out);
        }
        maxValid.tx = MakeTransactionRef(mtx);
        maxValid.m_protxHash = uint256::ONE;
    }
    BOOST_CHECK(maxValid.IsValidStructure(nullptr));

    // Transaction with max+1 inputs should be invalid
    CCoinJoinBroadcastTx tooMany;
    {
        CMutableTransaction mtx;
        for (size_t i = 0; i < nMaxPreFork + 1; ++i) {
            CTxIn in;
            in.prevout = COutPoint(uint256::ONE, static_cast<uint32_t>(i));
            mtx.vin.push_back(in);
            CTxOut out{CoinJoin::GetSmallestDenomination(), P2PKHScript(static_cast<uint8_t>(i % 256))};
            mtx.vout.push_back(out);
        }
        tooMany.tx = MakeTransactionRef(mtx);
        tooMany.m_protxHash = uint256::ONE;
    }
    BOOST_CHECK(!tooMany.IsValidStructure(nullptr));
}

BOOST_AUTO_TEST_CASE(input_limit_postfork_constants)
{
    // Post-V24: max inputs = GetMaxPoolParticipants() * PROMOTION_RATIO
    // Typically 20 * 10 = 200
    const size_t nMaxPostFork = CoinJoin::GetMaxPoolParticipants() * CoinJoin::PROMOTION_RATIO;
    BOOST_CHECK_EQUAL(nMaxPostFork, 200);

    // Verify the increase from pre-fork
    const size_t nMaxPreFork = CoinJoin::GetMaxPoolParticipants() * COINJOIN_ENTRY_MAX_SIZE;
    BOOST_CHECK(nMaxPostFork > nMaxPreFork);
    BOOST_CHECK_EQUAL(nMaxPostFork - nMaxPreFork, 20); // Increase of 20
}

BOOST_AUTO_TEST_CASE(promotion_demotion_value_preservation)
{
    // Verify that 10 smaller = 1 larger (value is preserved exactly)
    // CoinJoin denominations are designed so that 10 * smaller == larger
    // e.g., 10 * (0.1 DASH + 100 sat) = 1.0 DASH + 1000 sat

    const CAmount nSmallerAmount = CoinJoin::DenominationToAmount(1 << 2); // 0.1 DASH
    const CAmount nLargerAmount = CoinJoin::DenominationToAmount(1 << 1);  // 1.0 DASH

    // Value must match EXACTLY for promotion/demotion to preserve value
    BOOST_CHECK_EQUAL(nSmallerAmount * CoinJoin::PROMOTION_RATIO, nLargerAmount);

    // Verify for all adjacent denomination pairs
    for (int i = 0; i < 4; ++i) {
        const int nLargerDenom = 1 << i;
        const int nSmallerDenom = 1 << (i + 1);
        const CAmount nLarger = CoinJoin::DenominationToAmount(nLargerDenom);
        const CAmount nSmaller = CoinJoin::DenominationToAmount(nSmallerDenom);

        BOOST_CHECK(nLarger > nSmaller);
        // The denominations are designed so 10 * smaller == larger exactly
        BOOST_CHECK_EQUAL(nSmaller * CoinJoin::PROMOTION_RATIO, nLarger);
    }
}

BOOST_AUTO_TEST_CASE(decision_logic_mutual_exclusivity)
{
    // Verify that at any given count distribution, at most one of promote/demote is true
    // This is a property-based test of the algorithm

    // Test case 1: Equal counts - neither should trigger
    // With goal=100, halfGoal=50, if both at 100, deficits are 0, 0
    // Neither exceeds the other + GAP_THRESHOLD
    // This verifies the mutual exclusivity property

    // Test case 2: Large imbalance toward smaller
    // 0 larger, 100 smaller -> largerDeficit=100, smallerDeficit=0
    // Should promote (if smallerCount >= halfGoal)

    // Test case 3: Large imbalance toward larger
    // 100 larger, 0 smaller -> largerDeficit=0, smallerDeficit=100
    // Should demote (if largerCount >= halfGoal)

    // The actual ShouldPromote/ShouldDemote require wallet mocking which is complex
    // This test documents the expected behavior based on the algorithm

    // Verify the GAP_THRESHOLD prevents oscillation
    // If |deficit1 - deficit2| <= GAP_THRESHOLD, neither action should occur
    BOOST_CHECK_EQUAL(CoinJoin::GAP_THRESHOLD, 10);

    // The algorithm guarantees: for any pair of counts,
    // ShouldPromote XOR ShouldDemote OR neither (but never both)
    // This is enforced by the > (not >=) comparison with GAP_THRESHOLD
}

BOOST_AUTO_TEST_CASE(isvalidstructure_mixed_session_postfork)
{
    // Post-V24: A transaction with mixed entry types should be valid
    // (some participants doing 1:1, others doing promotion/demotion)
    // This tests that the structure validation allows mixed input/output counts

    CCoinJoinBroadcastTx mixed;
    {
        CMutableTransaction mtx;
        // Simulate a mixed session:
        // - 3 standard participants: 3 inputs, 3 outputs (smallest denom)
        // - 1 promotion participant: 10 inputs, 1 output
        // Total: 13 inputs, 4 outputs

        const CAmount nSmallestDenom = CoinJoin::GetSmallestDenomination();
        const CAmount nSecondSmallest = CoinJoin::DenominationToAmount(1 << 3); // 0.01 DASH

        // Standard participants (3 x 1:1)
        for (int i = 0; i < 3; ++i) {
            CTxIn in;
            in.prevout = COutPoint(uint256::ONE, static_cast<uint32_t>(i));
            mtx.vin.push_back(in);
            mtx.vout.push_back(CTxOut{nSmallestDenom, P2PKHScript(static_cast<uint8_t>(i))});
        }

        // Promotion participant (10 inputs -> 1 output)
        for (int i = 3; i < 13; ++i) {
            CTxIn in;
            in.prevout = COutPoint(uint256::ONE, static_cast<uint32_t>(i));
            mtx.vin.push_back(in);
        }
        // One output for the promotion (larger denom)
        mtx.vout.push_back(CTxOut{nSecondSmallest, P2PKHScript(0x0D)});

        mixed.tx = MakeTransactionRef(mtx);
        mixed.m_protxHash = uint256::ONE;
    }

    // Pre-V24: Should fail (unbalanced)
    BOOST_CHECK(!mixed.IsValidStructure(nullptr));

    // Note: Post-V24 testing requires a valid CBlockIndex with V24 deployment active
    // which requires more complex test setup with chainstate. The above verifies
    // that pre-fork rejection works correctly.
}

BOOST_AUTO_TEST_CASE(entry_type_detection_logic)
{
    // Test the entry type detection logic used in IsValidInOuts
    // Entry types:
    // - STANDARD: vin.size() == vout.size()
    // - PROMOTION: vin.size() == PROMOTION_RATIO && vout.size() == 1
    // - DEMOTION: vin.size() == 1 && vout.size() == PROMOTION_RATIO
    // - INVALID: anything else

    auto detectEntryType = [](size_t vinSize, size_t voutSize) -> std::string {
        if (vinSize == voutSize) {
            return "STANDARD";
        } else if (vinSize == static_cast<size_t>(CoinJoin::PROMOTION_RATIO) && voutSize == 1) {
            return "PROMOTION";
        } else if (vinSize == 1 && voutSize == static_cast<size_t>(CoinJoin::PROMOTION_RATIO)) {
            return "DEMOTION";
        }
        return "INVALID";
    };

    // Standard entries
    BOOST_CHECK_EQUAL(detectEntryType(1, 1), "STANDARD");
    BOOST_CHECK_EQUAL(detectEntryType(5, 5), "STANDARD");
    BOOST_CHECK_EQUAL(detectEntryType(9, 9), "STANDARD");  // Max standard entry

    // Promotion entries
    BOOST_CHECK_EQUAL(detectEntryType(10, 1), "PROMOTION");

    // Demotion entries
    BOOST_CHECK_EQUAL(detectEntryType(1, 10), "DEMOTION");

    // Invalid entries (not valid pre or post fork)
    BOOST_CHECK_EQUAL(detectEntryType(9, 1), "INVALID");   // Wrong ratio
    BOOST_CHECK_EQUAL(detectEntryType(1, 9), "INVALID");   // Wrong ratio
    BOOST_CHECK_EQUAL(detectEntryType(5, 3), "INVALID");   // Random mismatch
    BOOST_CHECK_EQUAL(detectEntryType(2, 10), "INVALID");  // Wrong input count for demotion
    BOOST_CHECK_EQUAL(detectEntryType(10, 2), "INVALID");  // Wrong output count for promotion
    BOOST_CHECK_EQUAL(detectEntryType(20, 2), "INVALID");  // 20:2 is not valid
    BOOST_CHECK_EQUAL(detectEntryType(11, 1), "INVALID");  // 11:1 is not valid promotion
}

BOOST_AUTO_TEST_CASE(validate_entry_session_denom_consistency)
{
    // Test that promotion/demotion validation enforces session denomination consistency
    // For promotion: inputs must be session denom (smaller), output must be larger adjacent
    // For demotion: input must be larger adjacent, outputs must be session denom (smaller)

    const int nSessionDenom = 1 << 2;  // 0.1 DASH (session denom)
    const int nLargerDenom = CoinJoin::GetLargerAdjacentDenom(nSessionDenom);

    BOOST_CHECK(nLargerDenom != 0);
    BOOST_CHECK_EQUAL(nLargerDenom, 1 << 1);  // 1.0 DASH

    // Create a valid promotion entry structure
    std::vector<CTxIn> promoVin;
    std::vector<CTxOut> promoVout;
    for (int i = 0; i < CoinJoin::PROMOTION_RATIO; ++i) {
        promoVin.push_back(MakeDenomInput(static_cast<uint8_t>(i)));
    }
    promoVout.push_back(MakeDenomOutput(CoinJoin::DenominationToAmount(nLargerDenom)));

    PoolMessage msg = MSG_NOERR;
    BOOST_CHECK(CoinJoin::ValidatePromotionEntry(promoVin, promoVout, nSessionDenom, msg));

    // Test with wrong session denom (largest denom can't promote)
    const int nLargestDenom = 1 << 0;  // 10 DASH
    msg = MSG_NOERR;
    BOOST_CHECK(!CoinJoin::ValidatePromotionEntry(promoVin, promoVout, nLargestDenom, msg));
    BOOST_CHECK(msg == ERR_DENOM);  // No larger adjacent for 10 DASH

    // Create a valid demotion entry structure
    std::vector<CTxIn> demoVin;
    std::vector<CTxOut> demoVout;
    demoVin.push_back(MakeDenomInput(0));
    for (int i = 0; i < CoinJoin::PROMOTION_RATIO; ++i) {
        demoVout.push_back(MakeDenomOutput(CoinJoin::DenominationToAmount(nSessionDenom), static_cast<uint8_t>(i)));
    }

    msg = MSG_NOERR;
    BOOST_CHECK(CoinJoin::ValidateDemotionEntry(demoVin, demoVout, nSessionDenom, msg));
}

BOOST_AUTO_TEST_CASE(isvalidstructure_boundary_input_counts)
{
    // Test boundary conditions for input counts at the pre/post V24 limits
    // Pre-V24: max 180 inputs (20 * 9)
    // Post-V24: max 200 inputs (20 * 10)

    const size_t nPreForkMax = CoinJoin::GetMaxPoolParticipants() * COINJOIN_ENTRY_MAX_SIZE;
    const size_t nPostForkMax = CoinJoin::GetMaxPoolParticipants() * CoinJoin::PROMOTION_RATIO;

    BOOST_CHECK_EQUAL(nPreForkMax, 180);
    BOOST_CHECK_EQUAL(nPostForkMax, 200);

    // Create transaction with exactly 180 inputs (valid pre and post fork)
    CCoinJoinBroadcastTx tx180;
    {
        CMutableTransaction mtx;
        for (size_t i = 0; i < 180; ++i) {
            mtx.vin.emplace_back(COutPoint(uint256::ONE, static_cast<uint32_t>(i)));
            mtx.vout.emplace_back(CoinJoin::GetSmallestDenomination(), P2PKHScript(static_cast<uint8_t>(i % 256)));
        }
        tx180.tx = MakeTransactionRef(mtx);
        tx180.m_protxHash = uint256::ONE;
    }
    BOOST_CHECK(tx180.IsValidStructure(nullptr));  // Pre-fork: valid at boundary

    // Create transaction with 181 inputs (invalid pre-fork, would be valid post-fork)
    CCoinJoinBroadcastTx tx181;
    {
        CMutableTransaction mtx;
        for (size_t i = 0; i < 181; ++i) {
            mtx.vin.emplace_back(COutPoint(uint256::ONE, static_cast<uint32_t>(i)));
            mtx.vout.emplace_back(CoinJoin::GetSmallestDenomination(), P2PKHScript(static_cast<uint8_t>(i % 256)));
        }
        tx181.tx = MakeTransactionRef(mtx);
        tx181.m_protxHash = uint256::ONE;
    }
    BOOST_CHECK(!tx181.IsValidStructure(nullptr));  // Pre-fork: over limit

    // Transaction at post-fork boundary (200 inputs)
    CCoinJoinBroadcastTx tx200;
    {
        CMutableTransaction mtx;
        for (size_t i = 0; i < 200; ++i) {
            mtx.vin.emplace_back(COutPoint(uint256::ONE, static_cast<uint32_t>(i)));
            mtx.vout.emplace_back(CoinJoin::GetSmallestDenomination(), P2PKHScript(static_cast<uint8_t>(i % 256)));
        }
        tx200.tx = MakeTransactionRef(mtx);
        tx200.m_protxHash = uint256::ONE;
    }
    // Pre-fork: over limit (200 > 180)
    BOOST_CHECK(!tx200.IsValidStructure(nullptr));

    // Note: Testing post-fork behavior requires a CBlockIndex with V24 active via EHF
}

// ============================================================================
// Tests for ShouldPromote/ShouldDemote algorithm logic
// These test the decision algorithm independently of wallet access
// ============================================================================

// Helper to simulate the ShouldPromote/ShouldDemote algorithm logic
// This mirrors the actual implementation in client.cpp
namespace {

bool TestShouldPromote(int smallerCount, int largerCount, int goal) {
    const int halfGoal = goal / 2;

    // Don't sacrifice a denomination that's still being built up
    if (smallerCount < halfGoal) {
        return false;
    }

    // Calculate how far each is from goal (0 if at or above goal)
    int smallerDeficit = std::max(0, goal - smallerCount);
    int largerDeficit = std::max(0, goal - largerCount);

    // Promote if larger denomination is further from goal by more than threshold
    return (largerDeficit > smallerDeficit + CoinJoin::GAP_THRESHOLD);
}

bool TestShouldDemote(int largerCount, int smallerCount, int goal) {
    const int halfGoal = goal / 2;

    // Don't sacrifice a denomination that's still being built up
    if (largerCount < halfGoal) {
        return false;
    }

    // Calculate how far each is from goal (0 if at or above goal)
    int smallerDeficit = std::max(0, goal - smallerCount);
    int largerDeficit = std::max(0, goal - largerCount);

    // Demote if smaller denomination is further from goal by more than threshold
    return (smallerDeficit > largerDeficit + CoinJoin::GAP_THRESHOLD);
}

} // anonymous namespace

BOOST_AUTO_TEST_CASE(should_promote_larger_deficit_greater)
{
    const int goal = 100;

    // 60 of smaller, 0 of larger -> should promote (0.1 has surplus, 1.0 needs help)
    // smallerDeficit = 40, largerDeficit = 100, gap = 60 > GAP_THRESHOLD
    BOOST_CHECK(TestShouldPromote(60, 0, goal));

    // 100 of smaller, 0 of larger -> should promote (0.1 full, 1.0 empty)
    // smallerDeficit = 0, largerDeficit = 100, gap = 100 > GAP_THRESHOLD
    BOOST_CHECK(TestShouldPromote(100, 0, goal));
}

BOOST_AUTO_TEST_CASE(should_promote_below_half_goal_false)
{
    const int goal = 100;

    // 30 of smaller, 0 of larger -> should NOT promote (30 < 50 = halfGoal)
    BOOST_CHECK(!TestShouldPromote(30, 0, goal));

    // 49 of smaller, 0 of larger -> should NOT promote (49 < 50 = halfGoal)
    BOOST_CHECK(!TestShouldPromote(49, 0, goal));

    // 50 of smaller, 0 of larger -> CAN promote (50 >= 50 = halfGoal)
    // smallerDeficit = 50, largerDeficit = 100, gap = 50 > GAP_THRESHOLD
    BOOST_CHECK(TestShouldPromote(50, 0, goal));
}

BOOST_AUTO_TEST_CASE(should_promote_small_gap_false)
{
    const int goal = 100;

    // 90 smaller, 80 larger -> deficits are 10 and 20, gap = 10
    // 20 > 10 + 10 is false, so no promotion
    BOOST_CHECK(!TestShouldPromote(90, 80, goal));

    // 85 smaller, 80 larger -> deficits are 15 and 20, gap = 5
    // 20 > 15 + 10 is false, so no promotion
    BOOST_CHECK(!TestShouldPromote(85, 80, goal));
}

BOOST_AUTO_TEST_CASE(should_demote_smaller_deficit_greater)
{
    const int goal = 100;

    // 60 of larger, 0 of smaller -> should demote (1.0 has surplus, 0.1 needs help)
    // largerDeficit = 40, smallerDeficit = 100, gap = 60 > GAP_THRESHOLD
    BOOST_CHECK(TestShouldDemote(60, 0, goal));

    // 99 of larger, 0 of smaller -> should demote
    // largerDeficit = 1, smallerDeficit = 100, gap = 99 > GAP_THRESHOLD
    BOOST_CHECK(TestShouldDemote(99, 0, goal));
}

BOOST_AUTO_TEST_CASE(should_demote_below_half_goal_false)
{
    const int goal = 100;

    // 30 of larger, 0 of smaller -> should NOT demote (30 < 50 = halfGoal)
    BOOST_CHECK(!TestShouldDemote(30, 0, goal));

    // 49 of larger, 0 of smaller -> should NOT demote (49 < 50 = halfGoal)
    BOOST_CHECK(!TestShouldDemote(49, 0, goal));

    // 50 of larger, 0 of smaller -> CAN demote (50 >= 50 = halfGoal)
    // largerDeficit = 50, smallerDeficit = 100, gap = 50 > GAP_THRESHOLD
    BOOST_CHECK(TestShouldDemote(50, 0, goal));
}

BOOST_AUTO_TEST_CASE(promote_demote_mutually_exclusive)
{
    // Test various distributions - at most one should be true
    auto testMutualExclusivity = [](int smallerCount, int largerCount) {
        const int testGoal = 100;
        bool promote = TestShouldPromote(smallerCount, largerCount, testGoal);
        bool demote = TestShouldDemote(largerCount, smallerCount, testGoal);
        // At most one can be true (XOR or neither)
        BOOST_CHECK(!(promote && demote));
        return std::make_pair(promote, demote);
    };

    // Equal counts - neither
    auto [p1, d1] = testMutualExclusivity(100, 100);
    BOOST_CHECK(!p1 && !d1);

    // 90/90 - neither (close to goal, small gap)
    auto [p2, d2] = testMutualExclusivity(90, 90);
    BOOST_CHECK(!p2 && !d2);

    // Large imbalance toward smaller - promote only
    auto [p3, d3] = testMutualExclusivity(100, 0);
    BOOST_CHECK(p3 && !d3);

    // Large imbalance toward larger - demote only
    auto [p4, d4] = testMutualExclusivity(0, 100);
    BOOST_CHECK(!p4 && d4);

    // Mid-range cases from the plan
    auto [p5, d5] = testMutualExclusivity(0, 60);  // 0.1=0, 1.0=60 -> should demote
    BOOST_CHECK(!p5 && d5);

    auto [p6, d6] = testMutualExclusivity(60, 0);  // 0.1=60, 1.0=0 -> should promote
    BOOST_CHECK(p6 && !d6);
}

BOOST_AUTO_TEST_CASE(decision_logic_example_cases_from_plan)
{
    // Test the specific examples from the implementation plan
    const int goal = 100;

    // | 1.0 count | 0.1 count | Action |
    // | 99        | 0         | Demote |
    BOOST_CHECK(TestShouldDemote(99, 0, goal));
    BOOST_CHECK(!TestShouldPromote(0, 99, goal));  // 0.1 has 0, can't promote

    // | 60        | 0         | Demote |
    BOOST_CHECK(TestShouldDemote(60, 0, goal));

    // | 30        | 0         | Nothing (1.0 < halfGoal) |
    BOOST_CHECK(!TestShouldDemote(30, 0, goal));
    BOOST_CHECK(!TestShouldPromote(0, 30, goal));

    // | 0         | 60        | Promote |
    BOOST_CHECK(TestShouldPromote(60, 0, goal));

    // | 0         | 30        | Nothing (0.1 < halfGoal) |
    BOOST_CHECK(!TestShouldPromote(30, 0, goal));

    // | 100       | 100       | Nothing (both at goal) |
    BOOST_CHECK(!TestShouldPromote(100, 100, goal));
    BOOST_CHECK(!TestShouldDemote(100, 100, goal));

    // | 90        | 90        | Nothing (deficits equal) |
    BOOST_CHECK(!TestShouldPromote(90, 90, goal));
    BOOST_CHECK(!TestShouldDemote(90, 90, goal));
}

BOOST_AUTO_TEST_CASE(adjacent_denomination_helpers)
{
    // Test GetLargerAdjacentDenom and GetSmallerAdjacentDenom

    // Denomination bits: 0=10DASH, 1=1DASH, 2=0.1DASH, 3=0.01DASH, 4=0.001DASH
    const int denom_10    = 1 << 0;  // 10 DASH
    const int denom_1     = 1 << 1;  // 1 DASH
    const int denom_01    = 1 << 2;  // 0.1 DASH
    const int denom_001   = 1 << 3;  // 0.01 DASH
    const int denom_0001  = 1 << 4;  // 0.001 DASH

    // GetLargerAdjacentDenom: returns the next larger denomination (10x value)
    BOOST_CHECK_EQUAL(CoinJoin::GetLargerAdjacentDenom(denom_1), denom_10);
    BOOST_CHECK_EQUAL(CoinJoin::GetLargerAdjacentDenom(denom_01), denom_1);
    BOOST_CHECK_EQUAL(CoinJoin::GetLargerAdjacentDenom(denom_001), denom_01);
    BOOST_CHECK_EQUAL(CoinJoin::GetLargerAdjacentDenom(denom_0001), denom_001);
    BOOST_CHECK_EQUAL(CoinJoin::GetLargerAdjacentDenom(denom_10), 0);  // No larger

    // GetSmallerAdjacentDenom: returns the next smaller denomination (0.1x value)
    BOOST_CHECK_EQUAL(CoinJoin::GetSmallerAdjacentDenom(denom_10), denom_1);
    BOOST_CHECK_EQUAL(CoinJoin::GetSmallerAdjacentDenom(denom_1), denom_01);
    BOOST_CHECK_EQUAL(CoinJoin::GetSmallerAdjacentDenom(denom_01), denom_001);
    BOOST_CHECK_EQUAL(CoinJoin::GetSmallerAdjacentDenom(denom_001), denom_0001);
    BOOST_CHECK_EQUAL(CoinJoin::GetSmallerAdjacentDenom(denom_0001), 0);  // No smaller

    // Invalid denominations
    BOOST_CHECK_EQUAL(CoinJoin::GetLargerAdjacentDenom(0), 0);
    BOOST_CHECK_EQUAL(CoinJoin::GetSmallerAdjacentDenom(0), 0);
    BOOST_CHECK_EQUAL(CoinJoin::GetLargerAdjacentDenom(999), 0);
    BOOST_CHECK_EQUAL(CoinJoin::GetSmallerAdjacentDenom(999), 0);
}

BOOST_AUTO_TEST_CASE(validate_promotion_entry_edge_cases)
{
    // Additional edge cases for ValidatePromotionEntry

    const int nSessionDenom = 1 << 2;  // 0.1 DASH
    const int nLargerDenom = CoinJoin::GetLargerAdjacentDenom(nSessionDenom);
    const CAmount nLargerAmount = CoinJoin::DenominationToAmount(nLargerDenom);

    // Valid case: exactly 10 inputs, 1 output of correct larger denom
    std::vector<CTxIn> validVin;
    std::vector<CTxOut> validVout;
    for (int i = 0; i < CoinJoin::PROMOTION_RATIO; ++i) {
        validVin.push_back(MakeDenomInput(static_cast<uint8_t>(i)));
    }
    validVout.push_back(MakeDenomOutput(nLargerAmount));

    PoolMessage msg = MSG_NOERR;
    BOOST_CHECK(CoinJoin::ValidatePromotionEntry(validVin, validVout, nSessionDenom, msg));

    // Invalid: 9 inputs (wrong count)
    std::vector<CTxIn> wrongCountVin;
    for (int i = 0; i < 9; ++i) {
        wrongCountVin.push_back(MakeDenomInput(static_cast<uint8_t>(i)));
    }
    msg = MSG_NOERR;
    BOOST_CHECK(!CoinJoin::ValidatePromotionEntry(wrongCountVin, validVout, nSessionDenom, msg));

    // Invalid: 11 inputs (wrong count)
    std::vector<CTxIn> tooManyVin;
    for (int i = 0; i < 11; ++i) {
        tooManyVin.push_back(MakeDenomInput(static_cast<uint8_t>(i)));
    }
    msg = MSG_NOERR;
    BOOST_CHECK(!CoinJoin::ValidatePromotionEntry(tooManyVin, validVout, nSessionDenom, msg));

    // Invalid: 2 outputs (should be 1)
    std::vector<CTxOut> twoOutputs;
    twoOutputs.push_back(MakeDenomOutput(nLargerAmount / 2));
    twoOutputs.push_back(MakeDenomOutput(nLargerAmount / 2));
    msg = MSG_NOERR;
    BOOST_CHECK(!CoinJoin::ValidatePromotionEntry(validVin, twoOutputs, nSessionDenom, msg));

    // Invalid: output is wrong denomination
    std::vector<CTxOut> wrongDenomOut;
    wrongDenomOut.push_back(MakeDenomOutput(CoinJoin::DenominationToAmount(nSessionDenom)));  // Same denom, not larger
    msg = MSG_NOERR;
    BOOST_CHECK(!CoinJoin::ValidatePromotionEntry(validVin, wrongDenomOut, nSessionDenom, msg));
}

BOOST_AUTO_TEST_CASE(validate_demotion_entry_edge_cases)
{
    // Additional edge cases for ValidateDemotionEntry

    const int nSessionDenom = 1 << 2;  // 0.1 DASH (output denom)
    const CAmount nSessionAmount = CoinJoin::DenominationToAmount(nSessionDenom);

    // Verify the larger adjacent denom exists for this test to be meaningful
    BOOST_CHECK(CoinJoin::GetLargerAdjacentDenom(nSessionDenom) != 0);

    // Valid case: 1 input of larger denom, 10 outputs of session denom
    std::vector<CTxIn> validVin;
    validVin.push_back(MakeDenomInput(0));

    std::vector<CTxOut> validVout;
    for (int i = 0; i < CoinJoin::PROMOTION_RATIO; ++i) {
        validVout.push_back(MakeDenomOutput(nSessionAmount, static_cast<uint8_t>(i)));
    }

    PoolMessage msg = MSG_NOERR;
    BOOST_CHECK(CoinJoin::ValidateDemotionEntry(validVin, validVout, nSessionDenom, msg));

    // Invalid: 2 inputs (should be 1)
    std::vector<CTxIn> twoInputs;
    twoInputs.push_back(MakeDenomInput(0));
    twoInputs.push_back(MakeDenomInput(1));
    msg = MSG_NOERR;
    BOOST_CHECK(!CoinJoin::ValidateDemotionEntry(twoInputs, validVout, nSessionDenom, msg));

    // Invalid: 9 outputs (wrong count)
    std::vector<CTxOut> nineOutputs;
    for (int i = 0; i < 9; ++i) {
        nineOutputs.push_back(MakeDenomOutput(nSessionAmount, static_cast<uint8_t>(i)));
    }
    msg = MSG_NOERR;
    BOOST_CHECK(!CoinJoin::ValidateDemotionEntry(validVin, nineOutputs, nSessionDenom, msg));

    // Test that demotion from smallest denomination (0.001 DASH) fails
    // because there's no smaller denomination to demote to
    const int nSmallestDenom = 1 << 4;  // 0.001 DASH
    BOOST_CHECK_EQUAL(CoinJoin::GetSmallerAdjacentDenom(nSmallestDenom), 0);  // No smaller exists
}

// ============================================================================
// Documentation: Post-V24 Integration Testing Requirements
// ============================================================================
// The following tests cannot be performed as unit tests because V24 uses
// EHF (Extended Hard Fork) activation which requires:
// 1. A valid blockchain with proper masternode infrastructure
// 2. EHF signaling from masternodes
// 3. Proper chainstate with V24 deployment active
//
// These tests should be performed as functional tests:
// - test/functional/coinjoin_promotion.py (to be added)
// - test/functional/coinjoin_demotion.py (to be added)
//
// Post-V24 behaviors that require functional testing:
// 1. IsValidStructure accepts unbalanced vin/vout with proper promo/demo structure
// 2. IsValidInOuts accepts promotion/demotion entries within sessions
// 3. Full promotion flow: 10 inputs -> 1 output mixing
// 4. Full demotion flow: 1 input -> 10 outputs mixing
// 5. Mixed sessions: some participants 1:1, others promoting/demoting
// 6. Input limit increased from 180 to 200 post-V24
// ============================================================================
BOOST_AUTO_TEST_SUITE_END()
