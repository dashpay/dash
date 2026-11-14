// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/amount.h>
#include <governance/superblock.h>
#include <masternode/payments.h>
#include <key.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <script/standard.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <vector>

struct SuperblockRegtestSetup : public BasicTestingSetup {
    SuperblockRegtestSetup() : BasicTestingSetup(ChainType::REGTEST) {}
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
    const SuperblockStatus superblock{.state = SuperblockStatus::State::Triggered, .payments = {{nPayAmount, scriptPayee}, {nPayAmount, scriptPayee}}};
    const CAmount nPaymentsLimit = CSuperblock::GetPaymentsLimit(nBlockHeight);

    const CScript scriptMinerOrMN = CScript() << OP_RETURN;
    const CAmount blockReward = 500 * COIN;

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
        BOOST_CHECK(!IsSuperblockValid(superblock, CTransaction(txNew), blockReward, /*is_v24=*/true, nPaymentsLimit));
    }

    // Case 2 (V24): coinbase carries TWO outputs matching the duplicate expected
    // payments. The fix must still accept this legitimate case.
    {
        CMutableTransaction txNew;
        txNew.vout.emplace_back(blockReward - 2 * nPayAmount, scriptMinerOrMN);
        txNew.vout.emplace_back(nPayAmount, scriptPayee);
        txNew.vout.emplace_back(nPayAmount, scriptPayee);
        BOOST_CHECK(IsSuperblockValid(superblock, CTransaction(txNew), blockReward, /*is_v24=*/true, nPaymentsLimit));
    }

    // Case 3 (pre-V24): the stricter distinct-output rule is gated behind V24.
    // Before activation the legacy scan is preserved, so the single-output
    // coinbase from Case 1 must still be accepted to avoid changing consensus
    // for already-validated history.
    {
        CMutableTransaction txNew;
        txNew.vout.emplace_back(blockReward - nPayAmount, scriptMinerOrMN);
        txNew.vout.emplace_back(nPayAmount, scriptPayee); // single matching output
        BOOST_CHECK(IsSuperblockValid(superblock, CTransaction(txNew), blockReward, /*is_v24=*/false, nPaymentsLimit));
    }
}

// The masternode building a trigger estimates the superblock's time from its own height and clock, within the last
// maturity window before the superblock, and accepts a proposal whose window is at most two hours off that estimate.
BOOST_AUTO_TEST_CASE(proposal_window_holds_exactly_the_chosen_superblocks)
{
    const int cycle{16616};
    const int maturity_window{1662};
    const int next_superblock{155 * cycle};
    const int64_t now{1'800'000'000};
    const int64_t fudge{2 * 60 * 60};

    for (const double block_seconds : {2.62 * 60 * 0.99, 2.62 * 60, 2.62 * 60 * 1.01}) {
        for (const int blocks_to_next : {maturity_window + 1, cycle / 2 - 1, cycle / 2 + 1, cycle - 1}) {
            const int tip{next_superblock - blocks_to_next};
            const auto estimated_by_trigger_builder = [&](int superblock, int trigger_height) {
                const auto trigger_time{static_cast<int64_t>(now + (trigger_height - tip) * block_seconds)};
                return governance::EstimateBlockTime(superblock, trigger_height, trigger_time);
            };
            for (int skipped{0}; skipped < 12; ++skipped) {
                for (int payments{1}; payments <= 12; ++payments) {
                    const int first_superblock{next_superblock + skipped * cycle};
                    const auto window{governance::EstimateProposalWindow(first_superblock, payments, cycle, tip, now)};

                    for (int n{-1}; n <= payments; ++n) {
                        const int superblock{first_superblock + n * cycle};
                        if (superblock <= tip) continue;
                        const bool chosen{n >= 0 && n < payments};
                        const int64_t slack{chosen ? 0 : fudge};
                        for (const int trigger_height : {superblock - maturity_window, superblock - 1}) {
                            const int64_t time{estimated_by_trigger_builder(superblock, trigger_height)};
                            const bool inside{window.start_epoch - slack <= time && time <= window.end_epoch + slack};
                            BOOST_CHECK_MESSAGE(inside == chosen,
                                                "blocks_to_next=" << blocks_to_next << " skipped=" << skipped
                                                                  << " payments=" << payments << " n=" << n
                                                                  << " trigger_height=" << trigger_height);
                        }
                    }
                }
            }
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
