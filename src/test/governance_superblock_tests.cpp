// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <consensus/amount.h>
#include <governance/superblock.h>
#include <key.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <script/standard.h>
#include <uint256.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <vector>

struct SuperblockRegtestSetup : public BasicTestingSetup {
    SuperblockRegtestSetup() : BasicTestingSetup(CBaseChainParams::REGTEST) {}
};

BOOST_FIXTURE_TEST_SUITE(governance_superblock_tests, SuperblockRegtestSetup)

// Regression test for: CSuperblock::IsValid was matching expected payments
// against coinbase outputs using a forward scan that re-started at the
// previously matched index, which allowed two adjacent expected payments
// with identical scriptPubKey and amount to both match the same coinbase
// output. Each expected payment must consume a distinct output.
BOOST_AUTO_TEST_CASE(isvalid_duplicate_payments_require_distinct_outputs)
{
    const auto& consensus = Params().GetConsensus();
    const int nBlockHeight = consensus.nSuperblockStartBlock + consensus.nSuperblockCycle;
    BOOST_REQUIRE(CSuperblock::IsValidBlockHeight(nBlockHeight));

    CKey key;
    key.MakeNewKey(/*fCompressed=*/true);
    const CTxDestination dest{PKHash(key.GetPubKey())};
    const CScript scriptPayee = GetScriptForDestination(dest);
    const CAmount nPayAmount = 1 * COIN;

    // Two identical expected payments (same script, same amount).
    std::vector<CGovernancePayment> payments;
    payments.emplace_back(dest, nPayAmount, /*proposalHash=*/uint256());
    payments.emplace_back(dest, nPayAmount, /*proposalHash=*/uint256::ONE);
    BOOST_REQUIRE(payments[0].IsValid());
    BOOST_REQUIRE(payments[1].IsValid());

    CSuperblock sb(nBlockHeight, payments);
    BOOST_REQUIRE_EQUAL(sb.CountPayments(), 2);

    const CScript scriptMinerOrMN = CScript() << OP_RETURN;
    const CAmount blockReward = 500 * COIN;
    CChain dummy_chain;

    // Case 1 (regression, V24): coinbase carries only ONE output matching the
    // duplicate expected payment. With the buggy forward scan that restarted
    // at the previously matched index, both expected payments would match the
    // single matching vout and IsValid would (incorrectly) return true.
    // From V24 on, the second expected payment must find a distinct output
    // and validation must fail.
    {
        CMutableTransaction txNew;
        txNew.vout.emplace_back(blockReward - nPayAmount, scriptMinerOrMN);
        txNew.vout.emplace_back(nPayAmount, scriptPayee); // single matching output
        BOOST_CHECK(!sb.IsValid(dummy_chain, CTransaction(txNew), nBlockHeight, blockReward, /*is_v24=*/true));
    }

    // Case 2 (V24): coinbase carries TWO outputs matching the duplicate expected
    // payments. The fix must still accept this legitimate case.
    {
        CMutableTransaction txNew;
        txNew.vout.emplace_back(blockReward - 2 * nPayAmount, scriptMinerOrMN);
        txNew.vout.emplace_back(nPayAmount, scriptPayee);
        txNew.vout.emplace_back(nPayAmount, scriptPayee);
        BOOST_CHECK(sb.IsValid(dummy_chain, CTransaction(txNew), nBlockHeight, blockReward, /*is_v24=*/true));
    }

    // Case 3 (pre-V24): the stricter distinct-output rule is gated behind V24.
    // Before activation the legacy scan is preserved, so the single-output
    // coinbase from Case 1 must still be accepted to avoid changing consensus
    // for already-validated history.
    {
        CMutableTransaction txNew;
        txNew.vout.emplace_back(blockReward - nPayAmount, scriptMinerOrMN);
        txNew.vout.emplace_back(nPayAmount, scriptPayee); // single matching output
        BOOST_CHECK(sb.IsValid(dummy_chain, CTransaction(txNew), nBlockHeight, blockReward, /*is_v24=*/false));
    }
}

BOOST_AUTO_TEST_SUITE_END()
