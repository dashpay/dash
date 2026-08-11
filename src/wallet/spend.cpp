// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coinjoin/common.h>
#include <coinjoin/options.h>
#include <consensus/consensus.h>
#include <consensus/validation.h>
#include <evo/dmn_types.h>
#include <interfaces/chain.h>
#include <policy/policy.h>
#include <script/signingprovider.h>
#include <util/check.h>
#include <util/fees.h>
#include <util/moneystr.h>
#include <util/trace.h>
#include <util/translation.h>
#include <wallet/coincontrol.h>
#include <wallet/fees.h>
#include <wallet/receive.h>
#include <wallet/spend.h>
#include <wallet/transaction.h>
#include <wallet/wallet.h>

#include <cmath>

using interfaces::FoundBlock;

namespace wallet {
static constexpr size_t OUTPUT_GROUP_MAX_ENTRIES{100};

int CalculateMaximumSignedInputSize(const CTxOut& txout, const COutPoint outpoint, const SigningProvider* provider, const CCoinControl* coin_control)
{
    CMutableTransaction txn;
    txn.vin.push_back(CTxIn(outpoint));
    if (!provider || !DummySignInput(*provider, txn.vin[0], txout, coin_control)) {
        return -1;
    }
    return ::GetSerializeSize(txn.vin[0], PROTOCOL_VERSION);
}

int CalculateMaximumSignedInputSize(const CTxOut& txout, const CWallet* wallet, const CCoinControl* coin_control)
{
    const std::unique_ptr<SigningProvider> provider = wallet->GetSolvingProvider(txout.scriptPubKey);
    return CalculateMaximumSignedInputSize(txout, COutPoint(), provider.get(), coin_control);
}

// txouts needs to be in the order of tx.vin
int64_t CalculateMaximumSignedTxSize(const CTransaction &tx, const CWallet *wallet, const std::vector<CTxOut>& txouts, const CCoinControl* coin_control)
{
    CMutableTransaction txNew(tx);
    if (!wallet->DummySignTx(txNew, txouts, coin_control)) {
        return -1;
    }
    return ::GetSerializeSize(txNew, PROTOCOL_VERSION);
}

int64_t CalculateMaximumSignedTxSize(const CTransaction &tx, const CWallet *wallet, const CCoinControl* coin_control)
{
    std::vector<CTxOut> txouts;
    // Look up the inputs. The inputs are either in the wallet, or in coin_control.
    for (const CTxIn& input : tx.vin) {
        const auto mi = wallet->mapWallet.find(input.prevout.hash);
        // Can not estimate size without knowing the input details
        if (mi != wallet->mapWallet.end()) {
            assert(input.prevout.n < mi->second.tx->vout.size());
            txouts.emplace_back(mi->second.tx->vout.at(input.prevout.n));
        } else if (coin_control) {
            const auto txout{coin_control->GetExternalOutput(input.prevout)};
            if (!txout) {
                return -1;
            }
            txouts.emplace_back(*txout);
        } else {
            return -1;
        }
    }
    return CalculateMaximumSignedTxSize(tx, wallet, txouts, coin_control);
}

uint64_t CoinsResult::size() const
{
    return legacy.size() + other.size();
}

std::vector<COutput> CoinsResult::all() const
{
    std::vector<COutput> all;
    all.reserve(this->size());
    all.insert(all.end(), legacy.begin(), legacy.end());
    all.insert(all.end(), other.begin(), other.end());
    return all;
}

void CoinsResult::clear()
{
    legacy.clear();
    other.clear();
}

// Fetch and validate the coin control selected inputs.
// Coins could be internal (from the wallet) or external.
util::Result<PreSelectedInputs> FetchSelectedInputs(const CWallet& wallet, const CCoinControl& coin_control,
                                            const CoinSelectionParams& coin_selection_params)
{
    PreSelectedInputs result;
    for (const COutPoint& outpoint : coin_control.ListSelected()) {
        int input_bytes = -1;
        CTxOut txout;
        if (auto ptr_wtx = wallet.GetWalletTx(outpoint.hash)) {
            // Clearly invalid input, fail
            if (ptr_wtx->tx->vout.size() <= outpoint.n) {
                return util::Error{strprintf(_("Invalid pre-selected input %s"), outpoint.ToString())};
            }
            txout = ptr_wtx->tx->vout.at(outpoint.n);
            input_bytes = CalculateMaximumSignedInputSize(txout, &wallet, &coin_control);
        } else {
            // The input is external. We did not find the tx in mapWallet.
            const auto out{coin_control.GetExternalOutput(outpoint)};
            if (!out) {
                return util::Error{strprintf(_("Not found pre-selected input %s"), outpoint.ToString())};
            }
            txout = *out;
        }

        if (input_bytes == -1) {
            input_bytes = CalculateMaximumSignedInputSize(txout, outpoint, &coin_control.m_external_provider, &coin_control);
        }

        if (coin_control.nCoinType == CoinType::ONLY_FULLY_MIXED) {
            // Make sure to include mixed preset inputs only,
            // even if some non-mixed inputs were manually selected via CoinControl
            if (!wallet.IsFullyMixed(outpoint)) continue;
        }

        // If available, override calculated size with coin control specified size
        if (coin_control.HasInputWeight(outpoint)) {
            input_bytes = GetVirtualTransactionSize(coin_control.GetInputWeight(outpoint), 0, 0);
        }

        if (input_bytes == -1) {
            return util::Error{strprintf(_("Not solvable pre-selected input %s"), outpoint.ToString())}; // Not solvable, can't estimate size for fee
        }

        /* Set some defaults for depth, spendable, solvable, safe, time, and from_me as these don't matter for preset inputs since no selection is being done. */
        COutput output(outpoint, txout, /*depth=*/ 0, input_bytes, /*spendable=*/ true, /*solvable=*/ true, /*safe=*/ true, /*time=*/ 0, /*from_me=*/ false, coin_selection_params.m_effective_feerate);
        result.Insert(output, coin_selection_params.m_subtract_fee_outputs);
    }
    return result;
}

CoinsResult AvailableCoins(const CWallet& wallet,
                           const CCoinControl* coinControl,
                           std::optional<CFeeRate> feerate,
                           const CAmount& nMinimumAmount,
                           const CAmount& nMaximumAmount,
                           const CAmount& nMinimumSumAmount,
                           const uint64_t nMaximumCount,
                           bool only_spendable)
{
    AssertLockHeld(wallet.cs_wallet);

    CoinType nCoinType = coinControl ? coinControl->nCoinType : CoinType::ALL_COINS;

    CoinsResult result;
    // Either the WALLET_FLAG_AVOID_REUSE flag is not set (in which case we always allow), or we default to avoiding, and only in the case where
    // a coin control object is provided, and has the avoid address reuse flag set to false, do we allow already used addresses
    bool allow_used_addresses = !wallet.IsWalletFlagSet(WALLET_FLAG_AVOID_REUSE) || (coinControl && !coinControl->m_avoid_address_reuse);
    const int min_depth = {coinControl ? coinControl->m_min_depth : DEFAULT_MIN_DEPTH};
    const int max_depth = {coinControl ? coinControl->m_max_depth : DEFAULT_MAX_DEPTH};
    const bool only_safe = {coinControl ? !coinControl->m_include_unsafe_inputs : true};

    std::set<uint256> trusted_parents;
    for (const auto* pwtx : wallet.GetSpendableTXs()) {
        const uint256& wtxid = pwtx->GetHash();
        const CWalletTx& wtx = *pwtx;

        if (wallet.IsTxImmatureCoinBase(wtx))
            continue;

        int nDepth = wallet.GetTxDepthInMainChain(wtx);

        // We should not consider coins which aren't at least in our mempool
        // It's possible for these to be conflicted via ancestors which we may never be able to detect
        if (nDepth == 0 && !wtx.InMempool())
            continue;

        bool safeTx = CachedTxIsTrusted(wallet, wtx, trusted_parents);

        if (only_safe && !safeTx) {
            continue;
        }

        if (nDepth < min_depth || nDepth > max_depth) {
            continue;
        }

        bool tx_from_me = CachedTxIsFromMe(wallet, wtx, ISMINE_ALL);

        for (unsigned int i = 0; i < wtx.tx->vout.size(); i++) {
            const CTxOut& output = wtx.tx->vout[i];
            const COutPoint outpoint(wtxid, i);

            bool found = false;
            switch (nCoinType) {
                case CoinType::ONLY_FULLY_MIXED: {
                    found = CoinJoin::IsDenominatedAmount(output.nValue) &&
                            wallet.IsFullyMixed(outpoint);
                    break;
                }
                case CoinType::ONLY_READY_TO_MIX: {
                    found = CoinJoin::IsDenominatedAmount(output.nValue) &&
                            !wallet.IsFullyMixed(outpoint);
                    break;
                }
                case CoinType::ONLY_NONDENOMINATED: {
                    // NOTE: do not use collateral amounts
                    found = !CoinJoin::IsCollateralAmount(output.nValue) &&
                            !CoinJoin::IsDenominatedAmount(output.nValue);
                    break;
                }
                case CoinType::ONLY_MASTERNODE_COLLATERAL: {
                    found = dmn_types::IsCollateralAmount(output.nValue);
                    break;
                }
                case CoinType::ONLY_COINJOIN_COLLATERAL: {
                    found = CoinJoin::IsCollateralAmount(output.nValue);
                    break;
                }
                case CoinType::ALL_COINS: {
                    found = true;
                    break;
                }
            } // no default case, so the compiler can warn about missing cases
            if (!found) continue;

            if (output.nValue < nMinimumAmount || output.nValue > nMaximumAmount)
                continue;

            // Skip manually selected coins (the caller can fetch them directly)
            if (coinControl && coinControl->HasSelected() && coinControl->IsSelected(outpoint))
                continue;

            if (wallet.IsLockedCoin(outpoint) && nCoinType != CoinType::ONLY_MASTERNODE_COLLATERAL)
                continue;

            if (wallet.IsSpent(outpoint))
                continue;

            isminetype mine = wallet.IsMine(output);

            if (mine == ISMINE_NO) {
                continue;
            }

            if (!allow_used_addresses && wallet.IsSpentKey(output.scriptPubKey)) {
                continue;
            }

            std::unique_ptr<SigningProvider> provider = wallet.GetSolvingProvider(output.scriptPubKey);

            int input_bytes = CalculateMaximumSignedInputSize(output, COutPoint(), provider.get(), coinControl);
            // Because CalculateMaximumSignedInputSize just uses ProduceSignature and makes a dummy signature,
            // it is safe to assume that this input is solvable if input_bytes is greater -1.
            bool solvable = input_bytes > -1;
            bool spendable = ((mine & ISMINE_SPENDABLE) != ISMINE_NO) || (((mine & ISMINE_WATCH_ONLY) != ISMINE_NO) && (coinControl && coinControl->fAllowWatchOnly && solvable));

            // Filter by spendable outputs only
            if (!spendable && only_spendable) continue;

            // When parsing a scriptPubKey, Solver returns the parsed pubkeys or hashes (depending on the script)
            // We don't need those here, so we are leaving them in return_values_unused
            std::vector<std::vector<uint8_t>> return_values_unused;
            TxoutType type;

            // If the Output is P2SH and spendable, we want to know if it is
            // a P2SH (legacy). We can determine this from the redeemScript.
            // If the Output is not spendable, it will be classified as a P2SH (legacy),
            // since we have no way of knowing otherwise without the redeemScript
            if (output.scriptPubKey.IsPayToScriptHash() && solvable) {
                CScript redeemScript;
                CTxDestination destination;
                if (!ExtractDestination(output.scriptPubKey, destination))
                    continue;
                const CScriptID& hash = CScriptID(std::get<ScriptHash>(destination));
                if (!provider->GetCScript(hash, redeemScript))
                    continue;
                type = Solver(redeemScript, return_values_unused);
            } else {
                type = Solver(output.scriptPubKey, return_values_unused);
            }

            COutput coin(outpoint, output, nDepth, input_bytes, spendable, solvable, safeTx, wtx.GetTxTime(), tx_from_me, feerate);
            switch (type) {
            case TxoutType::SCRIPTHASH:
            case TxoutType::PUBKEYHASH:
                result.legacy.push_back(coin);
                break;
            default:
                result.other.push_back(coin);
            };

            // Cache total amount as we go
            result.total_amount += output.nValue;
            if (coin.HasEffectiveValue()) {
                result.total_effective_amount = result.total_effective_amount.has_value() ?
                        *result.total_effective_amount + coin.GetEffectiveValue() : coin.GetEffectiveValue();
            }
            // Checks the sum amount of all UTXO's.
            if (nMinimumSumAmount != MAX_MONEY) {
                if (result.total_amount >= nMinimumSumAmount) {
                    return result;
                }
            }

            // Checks the maximum number of UTXO's.
            if (nMaximumCount > 0 && result.size() >= nMaximumCount) {
                return result;
            }
        }
    }

    return result;
}

CoinsResult AvailableCoinsListUnspent(const CWallet& wallet, const CCoinControl* coinControl, const CAmount& nMinimumAmount, const CAmount& nMaximumAmount, const CAmount& nMinimumSumAmount, const uint64_t nMaximumCount)
{
    return AvailableCoins(wallet, coinControl, /*feerate=*/ std::nullopt, nMinimumAmount, nMaximumAmount, nMinimumSumAmount, nMaximumCount, /*only_spendable=*/false);
}

const CTxOut& FindNonChangeParentOutput(const CWallet& wallet, const CTransaction& tx, int output)
{
    AssertLockHeld(wallet.cs_wallet);
    const CTransaction* ptx = &tx;
    int n = output;
    while (OutputIsChange(wallet, ptx->vout[n]) && ptx->vin.size() > 0) {
        const COutPoint& prevout = ptx->vin[0].prevout;
        auto it = wallet.mapWallet.find(prevout.hash);
        if (it == wallet.mapWallet.end() || it->second.tx->vout.size() <= prevout.n ||
            !wallet.IsMine(it->second.tx->vout[prevout.n])) {
            break;
        }
        ptx = it->second.tx.get();
        n = prevout.n;
    }
    return ptx->vout[n];
}

const CTxOut& FindNonChangeParentOutput(const CWallet& wallet, const COutPoint& outpoint)
{
    AssertLockHeld(wallet.cs_wallet);
    return FindNonChangeParentOutput(wallet, *wallet.GetWalletTx(outpoint.hash)->tx, outpoint.n);
}

std::map<CTxDestination, std::vector<COutput>> ListCoins(const CWallet& wallet)
{
    AssertLockHeld(wallet.cs_wallet);

    std::map<CTxDestination, std::vector<COutput>> result;

    for (COutput& coin : AvailableCoinsListUnspent(wallet).all()) {
        CTxDestination address;
        if ((coin.spendable || (wallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS) && coin.solvable)) &&
            ExtractDestination(FindNonChangeParentOutput(wallet, coin.outpoint).scriptPubKey, address)) {
            result[address].emplace_back(std::move(coin));
        }
    }

    // Include watch-only for LegacyScriptPubKeyMan wallets without private keys
    const bool include_watch_only = wallet.GetLegacyScriptPubKeyMan() && wallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    const isminetype is_mine_filter = include_watch_only ? ISMINE_WATCH_ONLY : ISMINE_SPENDABLE;
    for (const COutPoint& output : wallet.setLockedCoins) {
        auto it = wallet.mapWallet.find(output.hash);
        if (it != wallet.mapWallet.end()) {
            const auto& wtx = it->second;
            int depth = wallet.GetTxDepthInMainChain(wtx);
            if (depth >= 0 && output.n < wtx.tx->vout.size() &&
                wallet.IsMine(wtx.tx->vout[output.n]) == is_mine_filter
            ) {
                CTxDestination address;
                if (ExtractDestination(FindNonChangeParentOutput(wallet, *wtx.tx, output.n).scriptPubKey, address)) {
                    const auto out = wtx.tx->vout.at(output.n);
                    result[address].emplace_back(
                            COutPoint(wtx.GetHash(), output.n), out, depth, CalculateMaximumSignedInputSize(out, &wallet, /*coin_control=*/nullptr), /*spendable=*/ true, /*solvable=*/ true, /*safe=*/ false, wtx.GetTxTime(), CachedTxIsFromMe(wallet, wtx, ISMINE_ALL));
                }
            }
        }
    }

    return result;
}

static bool isGroupISLocked(const OutputGroup& group, interfaces::Chain& chain)
{
    return std::all_of(group.m_outputs.begin(), group.m_outputs.end(), [&chain](const auto& output) {
        return chain.isInstantSendLockedTx(output->outpoint.hash);
    });
}

// Dash: Group outputs once, computing both the positive-only and mixed groups in a single pass
// (upstream's OutputGroupTypeMap keys this by OutputType as well, but Dash only ever deals with
// a single output type, so a plain Groups suffices here).
Groups GroupOutputs(const CWallet& wallet, const std::vector<COutput>& outputs, const CoinSelectionParams& coin_sel_params, const CoinEligibilityFilter& filter)
{
    FilteredOutputGroups filtered_groups = GroupOutputs(wallet, outputs, coin_sel_params, std::vector<SelectionFilter>{{filter}});
    auto it = filtered_groups.find(filter);
    return it != filtered_groups.end() ? it->second : Groups{};
}

FilteredOutputGroups GroupOutputs(const CWallet& wallet, const std::vector<COutput>& outputs, const CoinSelectionParams& coin_sel_params, const std::vector<SelectionFilter>& filters)
{
    FilteredOutputGroups filtered_groups;

    if (!coin_sel_params.m_avoid_partial_spends) {
        // Allowing partial spends means no grouping. Each COutput gets its own OutputGroup.
        for (const COutput& output : outputs) {
            // Skip outputs we cannot spend
            if (!output.spendable) continue;

            size_t ancestors, descendants;
            wallet.chain().getTransactionAncestry(output.outpoint.hash, ancestors, descendants);

            // Make an OutputGroup containing just this output
            OutputGroup group{coin_sel_params};
            group.Insert(std::make_shared<COutput>(output), ancestors, descendants);

            bool isISLocked = isGroupISLocked(group, wallet.chain());
            // Each filter maps to a different set of groups
            for (const auto& sel_filter : filters) {
                const auto& filter = sel_filter.filter;
                // Check the OutputGroup's eligibility. Only add the eligible ones.
                if (!group.EligibleForSpending(filter, isISLocked)) continue;

                Groups& groups_out = filtered_groups[filter];
                groups_out.mixed_group.push_back(group);
                if (group.GetSelectionAmount() > 0) groups_out.positive_group.push_back(group);
            }
        }
        return filtered_groups;
    }

    // We want to combine COutputs that have the same scriptPubKey into single OutputGroups
    // except when there are more than OUTPUT_GROUP_MAX_ENTRIES COutputs grouped in an OutputGroup.
    // To do this, we maintain a map where the key is the scriptPubKey and the value is a vector of OutputGroups.
    // For each COutput, we check if the scriptPubKey is in the map, and if it is, the COutput is added
    // to the last OutputGroup in the vector for the scriptPubKey. When the last OutputGroup has
    // OUTPUT_GROUP_MAX_ENTRIES COutputs, a new OutputGroup is added to the end of the vector.
    // Two separate maps are kept: one including every output ("mixed"), and one that skips
    // negative-effective-value outputs at insertion time ("positive only"), since group-level
    // filtering after the fact is not equivalent to per-output filtering during grouping.
    typedef std::map<CScript, std::vector<OutputGroup>> ScriptPubKeyToOutgroup;
    const auto& group_outputs = [](
            const COutput& output, size_t ancestors, size_t descendants,
            ScriptPubKeyToOutgroup& groups_map, const CoinSelectionParams& coin_sel_params,
            bool positive_only) {
        std::vector<OutputGroup>& groups = groups_map[output.txout.scriptPubKey];

        if (groups.size() == 0) {
            // No OutputGroups for this scriptPubKey yet, add one
            groups.emplace_back(coin_sel_params);
        }

        // Get the last OutputGroup in the vector so that we can add the COutput to it
        // A pointer is used here so that group can be reassigned later if it is full.
        OutputGroup* group = &groups.back();

        // Check if this OutputGroup is full. We limit to OUTPUT_GROUP_MAX_ENTRIES when using -avoidpartialspends
        // to avoid surprising users with very high fees.
        if (group->m_outputs.size() >= OUTPUT_GROUP_MAX_ENTRIES) {
            // The last output group is full, add a new group to the vector and use that group for the insertion
            groups.emplace_back(coin_sel_params);
            group = &groups.back();
        }

        // Filter for positive only before adding the output to group
        if (!positive_only || output.GetEffectiveValue() > 0) {
            group->Insert(std::make_shared<COutput>(output), ancestors, descendants);
        }
    };

    ScriptPubKeyToOutgroup spk_to_groups_map;
    ScriptPubKeyToOutgroup spk_to_positive_groups_map;
    for (const auto& output : outputs) {
        // Skip outputs we cannot spend
        if (!output.spendable) continue;

        size_t ancestors, descendants;
        wallet.chain().getTransactionAncestry(output.outpoint.hash, ancestors, descendants);

        group_outputs(output, ancestors, descendants, spk_to_groups_map, coin_sel_params, /*positive_only=*/ false);
        group_outputs(output, ancestors, descendants, spk_to_positive_groups_map, coin_sel_params, /*positive_only=*/ true);
    }

    // Now we go through the entire maps and pull out the OutputGroups
    const auto& push_output_groups = [&](const ScriptPubKeyToOutgroup& groups_map, bool positive_only) {
        for (const auto& spk_and_groups_pair : groups_map) {
            const std::vector<OutputGroup>& groups_per_spk = spk_and_groups_pair.second;

            // Go through the vector backwards. This allows for the first item we deal with being the partial group.
            for (auto group_it = groups_per_spk.rbegin(); group_it != groups_per_spk.rend(); group_it++) {
                const OutputGroup& group = *group_it;
                bool is_partial_group = (group_it == groups_per_spk.rbegin() && groups_per_spk.size() > 1);

                // Check the OutputGroup's eligibility. Only add the eligible ones.
                if (positive_only && group.GetSelectionAmount() <= 0) continue;
                if (group.m_outputs.empty()) continue;
                bool isISLocked = isGroupISLocked(group, wallet.chain());

                // Each filter maps to a different set of groups
                for (const auto& sel_filter : filters) {
                    const auto& filter = sel_filter.filter;

                    // Don't include partial groups if there are full groups too and we don't want partial groups
                    if (is_partial_group && !filter.m_include_partial_groups) continue;

                    if (!group.EligibleForSpending(filter, isISLocked)) continue;

                    Groups& groups_out = filtered_groups[filter];
                    if (positive_only) {
                        groups_out.positive_group.push_back(group);
                    } else {
                        groups_out.mixed_group.push_back(group);
                    }
                }
            }
        }
    };

    push_output_groups(spk_to_groups_map, /*positive_only=*/ false);
    push_output_groups(spk_to_positive_groups_map, /*positive_only=*/ true);

    return filtered_groups;
}

// Returns true if the result contains an error and the message is not empty
static bool HasErrorMsg(const util::Result<SelectionResult>& res) { return !util::ErrorString(res).empty(); }

util::Result<SelectionResult> AttemptSelection(const CWallet& wallet, const CAmount& nTargetValue, Groups& groups, Groups& mixed_groups,
                                                const CoinSelectionParams& coin_selection_params, bool allow_mixed_output_types, CoinType nCoinType)
{
    // Run coin selection on each OutputType and compute the Waste Metric
    std::vector<SelectionResult> results;
    {
        // Groups (both positive-only and mixed) for this filter, over available_coins.legacy, were
        // already computed once ahead of time for every filter AutomaticCoinSelection walks -- see
        // GroupOutputs' filter-list overload. AttemptSelection itself never calls GroupOutputs.
        auto result{ChooseSelectionResult(nTargetValue, groups, coin_selection_params, nCoinType, wallet.m_default_max_tx_fee)};
        // If any specific error message appears here, then something particularly wrong happened.
        if (HasErrorMsg(result)) return result; // So let's return the specific error.
        // Append the favorable result.
        if (result) results.push_back(*result);
    }

    // If we can't fund the transaction from any individual OutputType, run coin selection
    // over all available coins, else pick the best solution from the results
    if (results.size() == 0) {
        if (allow_mixed_output_types) {
            // mixed_groups (over available_coins.all(), i.e. legacy + other) for this filter was
            // likewise already computed once ahead of time, alongside 'groups' above.
            return ChooseSelectionResult(nTargetValue, mixed_groups, coin_selection_params, nCoinType, wallet.m_default_max_tx_fee);
        }
        return util::Error();
    }
    return *std::min_element(results.begin(), results.end());
};

util::Result<SelectionResult> ChooseSelectionResult(const CAmount& nTargetValue, Groups& groups, const CoinSelectionParams& coin_selection_params,
                                                     CoinType nCoinType, CAmount max_tx_fee)
{
    // Vector of results. We will choose the best one based on waste.
    std::vector<SelectionResult> results;
    std::vector<util::Result<SelectionResult>> errors;
    auto append_error = [&] (util::Result<SelectionResult>&& result) {
        // If any specific error message appears here, then something different from a simple "no selection found" happened.
        // Let's save it, so it can be retrieved to the user if no other selection algorithm succeeded.
        if (HasErrorMsg(result)) errors.emplace_back(std::move(result));
    };

    int max_inputs_weight = MAX_STANDARD_TX_SIZE - coin_selection_params.tx_noinputs_size;

    // Note that unlike KnapsackSolver, we do not include the fee for creating a change output as BnB will not create a change output.
    groups.positive_group.clear(); // Cleared to skip BnB and SRD as they're unaware of mixed coins
    if (auto bnb_result{SelectCoinsBnB(groups.positive_group, nTargetValue, coin_selection_params.m_cost_of_change, max_inputs_weight)}) {
        results.push_back(*bnb_result);
    } else {
        append_error(std::move(bnb_result));
    }

    max_inputs_weight -= coin_selection_params.change_output_size;

    // The knapsack solver has some legacy behavior where it will spend dust outputs. We retain this behavior, so don't filter for positive only here.
    if (auto knapsack_result{KnapsackSolver(groups.mixed_group, nTargetValue, coin_selection_params.m_min_change_target,
                                            coin_selection_params.rng_fast, max_inputs_weight, nCoinType == CoinType::ONLY_FULLY_MIXED,
                                            max_tx_fee)}) {
        knapsack_result->ComputeAndSetWaste(coin_selection_params.min_viable_change, coin_selection_params.m_cost_of_change, coin_selection_params.m_change_fee);
        results.push_back(*knapsack_result);
    } else {
        append_error(std::move(knapsack_result));
    }

    if (auto srd_result{SelectCoinsSRD(groups.positive_group, nTargetValue, coin_selection_params.rng_fast, max_inputs_weight)}) {
        srd_result->ComputeAndSetWaste(coin_selection_params.min_viable_change, coin_selection_params.m_cost_of_change, coin_selection_params.m_change_fee);
        results.push_back(*srd_result);
    } else {
        append_error(std::move(srd_result));
    }

    if (results.size() == 0) {
        // No solution found, retrieve the first explicit error (if any).
        // future: add "error level" so the worst one can be picked instead.
        return errors.empty() ? util::Error() : std::move(errors.front());
    }

    // Choose the result with the least waste
    // If the waste is the same, choose the one which spends more inputs.
    return *std::min_element(results.begin(), results.end());
}

/**
 * Dash: walk the pre-selected inputs in outpoint order and stop as soon as their selection
 * amount covers nTargetValue. Used when coin control requests that only as many of the
 * manually selected inputs as are actually needed be spent, rather than all of them
 * (see CCoinControl::fRequireAllInputs). Note this does not minimise the input count or the
 * selected value; it only avoids sweeping every selected coin.
 */
static PreSelectedInputs TrimPreSelectedInputs(const PreSelectedInputs& pre_set_inputs, const CAmount& nTargetValue, bool subtract_fee_outputs)
{
    PreSelectedInputs trimmed;
    for (const auto& output : pre_set_inputs.coins) {
        // Insert before testing, so a non-empty input set always contributes at least one coin
        // even when nTargetValue is 0. An empty selection would trip the non-empty assert in
        // GetSelectionWaste().
        trimmed.Insert(*output, subtract_fee_outputs);
        if (trimmed.total_amount >= nTargetValue) break;
    }
    return trimmed;
}

util::Result<SelectionResult> SelectCoins(const CWallet& wallet, CoinsResult& available_coins, const PreSelectedInputs& pre_set_inputs,
                                          const CAmount& nTargetValue, const CCoinControl& coin_control,
                                          const CoinSelectionParams& coin_selection_params)
{
    // Note: this function should never be used for "always free" tx types like dstx

    // Deduct preset inputs amount from the search target
    CAmount selection_target = nTargetValue - pre_set_inputs.total_amount;

    // Return if automatic coin selection is disabled, and we don't cover the selection target
    if (!coin_control.m_allow_other_inputs && selection_target > 0) {
        return util::Error{_("The preselected coins total amount does not cover the transaction target. "
                             "Please allow other inputs to be automatically selected or include more coins manually")};
    }

    // Return if we can cover the target only with the preset inputs
    if (selection_target <= 0) {
        SelectionResult result(nTargetValue, SelectionAlgorithm::MANUAL);
        if (coin_control.fRequireAllInputs) {
            result.AddInputs(pre_set_inputs.coins, coin_selection_params.m_subtract_fee_outputs);
        } else {
            // Dash: spend only as many of the manually selected inputs as are needed to cover the target
            const PreSelectedInputs trimmed{TrimPreSelectedInputs(pre_set_inputs, nTargetValue, coin_selection_params.m_subtract_fee_outputs)};
            result.AddInputs(trimmed.coins, coin_selection_params.m_subtract_fee_outputs);
        }
        result.ComputeAndSetWaste(coin_selection_params.min_viable_change, coin_selection_params.m_cost_of_change, coin_selection_params.m_change_fee);
        return result;
    }

    // Return early if we cannot cover the target with the wallet's UTXO.
    // We use the total effective value if we are not subtracting fee from outputs and 'available_coins' contains the data.
    CAmount available_coins_total_amount = coin_selection_params.m_subtract_fee_outputs ? available_coins.GetTotalAmount() :
            (available_coins.GetEffectiveTotalAmount().has_value() ? *available_coins.GetEffectiveTotalAmount() : 0);
    if (selection_target > available_coins_total_amount) {
        return util::Error(); // Insufficient funds
    }

    // Start wallet Coin Selection procedure
    auto op_selection_result = AutomaticCoinSelection(wallet, available_coins, selection_target, coin_control, coin_selection_params);
    if (!op_selection_result) return op_selection_result;

    // If needed, add preset inputs to the automatic coin selection result
    if (!pre_set_inputs.coins.empty()) {
        SelectionResult preselected(pre_set_inputs.total_amount, SelectionAlgorithm::MANUAL);
        preselected.AddInputs(pre_set_inputs.coins, coin_selection_params.m_subtract_fee_outputs);
        op_selection_result->Merge(preselected);
        op_selection_result->ComputeAndSetWaste(coin_selection_params.min_viable_change,
                                                coin_selection_params.m_cost_of_change,
                                                coin_selection_params.m_change_fee);
    }
    return op_selection_result;
}

util::Result<SelectionResult> AutomaticCoinSelection(const CWallet& wallet, CoinsResult& available_coins, const CAmount& value_to_select, const CCoinControl& coin_control, const CoinSelectionParams& coin_selection_params)
{
    CoinType nCoinType = coin_control.nCoinType;

    unsigned int limit_ancestor_count = 0;
    unsigned int limit_descendant_count = 0;
    wallet.chain().getPackageLimits(limit_ancestor_count, limit_descendant_count);
    const size_t max_ancestors = (size_t)std::max<int64_t>(1, limit_ancestor_count);
    const size_t max_descendants = (size_t)std::max<int64_t>(1, limit_descendant_count);
    const bool fRejectLongChains = gArgs.GetBoolArg("-walletrejectlongchains", DEFAULT_WALLET_REJECT_LONG_CHAINS);

    // form groups from remaining coins; note that preset coins will not
    // automatically have their associated (same address) coins included
    if (coin_control.m_avoid_partial_spends && available_coins.size() > OUTPUT_GROUP_MAX_ENTRIES) {
        // Cases where we have 101+ outputs all pointing to the same destination may result in
        // privacy leaks as they will potentially be deterministically sorted. We solve that by
        // explicitly shuffling the outputs before processing
        Shuffle(available_coins.legacy.begin(), available_coins.legacy.end(), coin_selection_params.rng_fast);
        Shuffle(available_coins.other.begin(), available_coins.other.end(), coin_selection_params.rng_fast);
    }

    // Coin Selection attempts to select inputs from a pool of eligible UTXOs to fund the
    // transaction at a target feerate. If an attempt fails, more attempts may be made using a more
    // permissive CoinEligibilityFilter.
    util::Result<SelectionResult> res = [&] {
        // Place coins eligibility filters on a scope increasing order.
        std::vector<SelectionFilter> ordered_filters{
                // If possible, fund the transaction with confirmed UTXOs only. Prefer at least six
                // confirmations on outputs received from other wallets and only spend confirmed change.
                {CoinEligibilityFilter(1, 6, 0), /*allow_mixed_output_types=*/false},
                {CoinEligibilityFilter(1, 1, 0)},
        };
        // Fall back to using zero confirmation change (but with as few ancestors in the mempool as
        // possible) if we cannot fund the transaction otherwise.
        if (wallet.m_spend_zero_conf_change) {
            ordered_filters.push_back({CoinEligibilityFilter(0, 1, 2)});
            ordered_filters.push_back({CoinEligibilityFilter(0, 1, std::min((size_t)4, max_ancestors/3), std::min((size_t)4, max_descendants/3))});
            ordered_filters.push_back({CoinEligibilityFilter(0, 1, max_ancestors/2, max_descendants/2)});
            // If partial groups are allowed, relax the requirement of spending OutputGroups (groups
            // of UTXOs sent to the same address, which are obviously controlled by a single wallet)
            // in their entirety.
            ordered_filters.push_back({CoinEligibilityFilter(0, 1, max_ancestors-1, max_descendants-1, /*include_partial=*/true)});
            // Try with unsafe inputs if they are allowed. This may spend unconfirmed outputs
            // received from other wallets.
            if (coin_control.m_include_unsafe_inputs) {
                ordered_filters.push_back({CoinEligibilityFilter(/*conf_mine=*/0, /*conf_theirs*/0, max_ancestors-1, max_descendants-1, /*include_partial=*/true)});
            }
            // Try with unlimited ancestors/descendants. The transaction will still need to meet
            // mempool ancestor/descendant policy to be accepted to mempool and broadcasted, but
            // OutputGroups use heuristics that may overestimate ancestor/descendant counts.
            if (!fRejectLongChains) {
                ordered_filters.push_back({CoinEligibilityFilter(0, 1, std::numeric_limits<uint64_t>::max(),
                                                                   std::numeric_limits<uint64_t>::max(),
                                                                   /*include_partial=*/true)});
            }
        }

        // Group outputs for every filter above in a single pass, instead of re-running the
        // (potentially expensive) grouping process once per filter inside AttemptSelection.
        // Two caches are built: 'filtered_groups' over available_coins.legacy (the privacy-preserving,
        // single-OutputType pass) and 'filtered_mixed_groups' over available_coins.all() (legacy + other,
        // used by AttemptSelection's mixed-output-types fallback). AttemptSelection itself performs no
        // GroupOutputs calls at all; both of its inputs are already-computed Groups from these caches.
        FilteredOutputGroups filtered_groups = GroupOutputs(wallet, available_coins.legacy, coin_selection_params, ordered_filters);
        FilteredOutputGroups filtered_mixed_groups = GroupOutputs(wallet, available_coins.all(), coin_selection_params, ordered_filters);

        // Walk-through the filters until the solution gets found.
        // If no solution is found, return the first detailed error (if any).
        // future: add "error level" so the worst one can be picked instead.
        // Sentinel for filters with no cached groups in one (or both) of the passes below; never
        // populated, only ever read as an empty Groups by ChooseSelectionResult.
        Groups empty_groups;
        std::vector<util::Result<SelectionResult>> res_detailed_errors;
        for (const auto& select_filter : ordered_filters) {
            // A missing cache entry means no coin was eligible for this filter in that particular
            // pass; still try (the other pass, or a less permissive filter's cache, may still find
            // a solution), just with an empty Groups for the pass that has nothing cached.
            auto it = filtered_groups.find(select_filter.filter);
            Groups& groups = (it != filtered_groups.end()) ? it->second : empty_groups;
            auto mit = filtered_mixed_groups.find(select_filter.filter);
            Groups& mixed_groups = (mit != filtered_mixed_groups.end()) ? mit->second : empty_groups;

            if (auto res{AttemptSelection(wallet, value_to_select, groups, mixed_groups,
                                          coin_selection_params, select_filter.allow_mixed_output_types, nCoinType)}) {
                return res; // result found
            } else {
                // If any specific error message appears here, then something particularly wrong might have happened.
                // Save the error and continue the selection process. So if no solutions gets found, we can return
                // the detailed error to the upper layers.
                if (HasErrorMsg(res)) res_detailed_errors.emplace_back(res);
            }
        }
        // Coin Selection failed.
        return res_detailed_errors.empty() ? util::Result<SelectionResult>(util::Error()) : res_detailed_errors.front();
    }();

    return res;
}

static bool IsCurrentForAntiFeeSniping(interfaces::Chain& chain, const uint256& block_hash)
{
    if (chain.isInitialBlockDownload()) {
        return false;
    }
    constexpr int64_t MAX_ANTI_FEE_SNIPING_TIP_AGE = 8 * 60 * 60; // in seconds
    int64_t block_time;
    CHECK_NONFATAL(chain.findBlock(block_hash, FoundBlock().time(block_time)));
    if (block_time < (GetTime() - MAX_ANTI_FEE_SNIPING_TIP_AGE)) {
        return false;
    }
    return true;
}

/**
 * Set a height-based locktime for new transactions (uses the height of the
 * current chain tip unless we are not synced with the current chain
 */
static void DiscourageFeeSniping(CMutableTransaction& tx, FastRandomContext& rng_fast,
                                 interfaces::Chain& chain, const uint256& block_hash, int block_height)
{
    // All inputs must be added by now
    assert(!tx.vin.empty());
    // Discourage fee sniping.
    //
    // For a large miner the value of the transactions in the best block and
    // the mempool can exceed the cost of deliberately attempting to mine two
    // blocks to orphan the current best block. By setting nLockTime such that
    // only the next block can include the transaction, we discourage this
    // practice as the height restricted and limited blocksize gives miners
    // considering fee sniping fewer options for pulling off this attack.
    //
    // A simple way to think about this is from the wallet's point of view we
    // always want the blockchain to move forward. By setting nLockTime this
    // way we're basically making the statement that we only want this
    // transaction to appear in the next block; we don't want to potentially
    // encourage reorgs by allowing transactions to appear at lower heights
    // than the next block in forks of the best chain.
    //
    // Of course, the subsidy is high enough, and transaction volume low
    // enough, that fee sniping isn't a problem yet, but by implementing a fix
    // now we ensure code won't be written that makes assumptions about
    // nLockTime that preclude a fix later.
    if (IsCurrentForAntiFeeSniping(chain, block_hash)) {
        tx.nLockTime = block_height;

        // Secondly occasionally randomly pick a nLockTime even further back, so
        // that transactions that are delayed after signing for whatever reason,
        // e.g. high-latency mix networks and some CoinJoin implementations, have
        // better privacy.
        if (rng_fast.randrange(10) == 0) {
            tx.nLockTime = std::max(0, int(tx.nLockTime) - int(rng_fast.randrange(100)));
        }
    } else {
        // If our chain is lagging behind, we can't discourage fee sniping nor help
        // the privacy of high-latency transactions. To avoid leaking a potentially
        // unique "nLockTime fingerprint", set nLockTime to a constant.
        tx.nLockTime = 0;
    }
    // Sanity check all values
    assert(tx.nLockTime < LOCKTIME_THRESHOLD); // Type must be block height
    assert(tx.nLockTime <= uint64_t(block_height));
    for (const auto& in : tx.vin) {
        // Can not be FINAL for locktime to work
        assert(in.nSequence != CTxIn::SEQUENCE_FINAL);
        // May be MAX NONFINAL to disable BIP68
        if (in.nSequence == CTxIn::MAX_SEQUENCE_NONFINAL) continue;
        // The wallet does not support any other sequence-use right now.
        assert(false);
    }
}

static util::Result<CreatedTransactionResult> CreateTransactionInternal(
        CWallet& wallet,
        const std::vector<CRecipient>& vecSend,
        int change_pos,
        const CCoinControl& coin_control,
        bool sign,
        int nExtraPayloadSize) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    AssertLockHeld(wallet.cs_wallet);

    // out variables, to be packed into returned result structure
    CAmount nFeeRet;
    int nChangePosInOut = change_pos;

    FastRandomContext rng_fast;
    CMutableTransaction txNew; // The resulting transaction that we make

    CoinSelectionParams coin_selection_params{rng_fast}; // Parameters for coin selection, init with dummy
    coin_selection_params.m_avoid_partial_spends = coin_control.m_avoid_partial_spends;

    // Set the long term feerate estimate to the wallet's consolidate feerate
    coin_selection_params.m_long_term_feerate = wallet.m_consolidate_feerate;

    CAmount recipients_sum = 0;
    ReserveDestination reservedest(&wallet);
    const bool sort_bip69{nChangePosInOut == -1};
    unsigned int outputs_to_subtract_fee_from = 0; // The number of outputs which we are subtracting the fee from
    for (const auto& recipient : vecSend) {
        recipients_sum += recipient.nAmount;

        if (recipient.fSubtractFeeFromAmount) {
            outputs_to_subtract_fee_from++;
            coin_selection_params.m_subtract_fee_outputs = true;
        }
    }

    // Create change script that will be used if we need change
    CScript scriptChange;
    bilingual_str error; // possible error str

    // coin control: send change to custom address
    if (!std::get_if<CNoDestination>(&coin_control.destChange)) {
        scriptChange = GetScriptForDestination(coin_control.destChange);
    } else { // no coin control: send change to newly generated address
        // Note: We use a new key here to keep it from being obvious which side is the change.
        //  The drawback is that by not reusing a previous key, the change may be lost if a
        //  backup is restored, if the backup doesn't have the new private key for the change.
        //  If we reused the old key, it would be possible to add code to look for and
        //  rediscover unknown transactions that were written with keys of ours to recover
        //  post-backup change.

        // Reserve a new key pair from key pool. If it fails, provide a dummy
        // destination in case we don't need change.
        CTxDestination dest;
        auto op_dest = reservedest.GetReservedDestination(true);
        if (!op_dest) {
            error = _("Transaction needs a change address, but we can't generate it.") + Untranslated(" ") + util::ErrorString(op_dest);
        } else {
            dest = *op_dest;
            scriptChange = GetScriptForDestination(dest);
        }
        // A valid destination implies a change script (and
        // vice-versa). An empty change script will abort later, if the
        // change keypool ran out, but change is required.
        CHECK_NONFATAL(IsValidDestination(dest) != scriptChange.empty());
    }
    CTxOut change_prototype_txout(0, scriptChange);
    coin_selection_params.change_output_size = GetSerializeSize(change_prototype_txout);

    // Get size of spending the change output
    int change_spend_size = CalculateMaximumSignedInputSize(change_prototype_txout, &wallet);
    // If the wallet doesn't know how to sign change output, assume p2sh-p2pkh
    // as lower-bound to allow BnB to do it's thing
    if (change_spend_size == -1) {
        coin_selection_params.change_spend_size = DUMMY_NESTED_P2PKH_INPUT_SIZE;
    } else {
        coin_selection_params.change_spend_size = (size_t)change_spend_size;
    }

    // Set discard feerate
    coin_selection_params.m_discard_feerate = coin_control.m_discard_feerate ? *coin_control.m_discard_feerate : GetDiscardRate(wallet);

    // Get the fee rate to use effective values in coin selection
    FeeCalculation feeCalc;
    coin_selection_params.m_effective_feerate = GetMinimumFeeRate(wallet, coin_control, &feeCalc);
    // Do not, ever, assume that it's fine to change the fee rate if the user has explicitly
    // provided one
    if (coin_control.m_feerate && coin_selection_params.m_effective_feerate > *coin_control.m_feerate) {
        return util::Error{strprintf(_("Fee rate (%s) is lower than the minimum fee rate setting (%s)"), coin_control.m_feerate->ToString(FeeEstimateMode::DUFF_B), coin_selection_params.m_effective_feerate.ToString(FeeEstimateMode::DUFF_B))};
    }
    if (feeCalc.reason == FeeReason::FALLBACK && !wallet.m_allow_fallback_fee) {
        // eventually allow a fallback fee
        return util::Error{strprintf(_("Fee estimation failed. Fallbackfee is disabled. Wait a few blocks or enable %s."), "-fallbackfee")};
    }

    // Calculate the cost of change
    // Cost of change is the cost of creating the change output + cost of spending the change output in the future.
    // For creating the change output now, we use the effective feerate.
    // For spending the change output in the future, we use the discard feerate for now.
    // So cost of change = (change output size * effective feerate) + (size of spending change output * discard feerate)
    coin_selection_params.m_change_fee = coin_selection_params.m_effective_feerate.GetFee(coin_selection_params.change_output_size);
    coin_selection_params.m_cost_of_change = coin_selection_params.m_discard_feerate.GetFee(coin_selection_params.change_spend_size) + coin_selection_params.m_change_fee;

    coin_selection_params.m_min_change_target = GenerateChangeTarget(std::floor(recipients_sum / vecSend.size()), coin_selection_params.m_change_fee, rng_fast);

    // The smallest change amount should be:
    // 1. at least equal to dust threshold
    // 2. at least 1 sat greater than fees to spend it at m_discard_feerate
    const auto dust = GetDustThreshold(change_prototype_txout, coin_selection_params.m_discard_feerate);
    const auto change_spend_fee = coin_selection_params.m_discard_feerate.GetFee(coin_selection_params.change_spend_size);
    coin_selection_params.min_viable_change = std::max(change_spend_fee + 1, dust);

    // vouts to the payees
    if (!coin_selection_params.m_subtract_fee_outputs) {
        coin_selection_params.tx_noinputs_size = 9; // Static vsize overhead + outputs vsize. 4 nVersion, 4 nLocktime, 1 input count
        coin_selection_params.tx_noinputs_size += GetSizeOfCompactSize(vecSend.size()); // bytes for output count
        if (nExtraPayloadSize != 0) {
            // Special txes carry an extra payload which is not part of txNew, but is accounted for
            // in the final size below. Coin Selection now derives the change amount from this
            // target, so the payload has to be included here or we would underpay the fee.
            coin_selection_params.tx_noinputs_size += GetSizeOfCompactSize(nExtraPayloadSize) + nExtraPayloadSize;
        }
    }
    for (const auto& recipient : vecSend)
    {
        CTxOut txout(recipient.nAmount, recipient.scriptPubKey);

        // Include the fee cost for outputs.
        if (!coin_selection_params.m_subtract_fee_outputs) {
            coin_selection_params.tx_noinputs_size += ::GetSerializeSize(txout, PROTOCOL_VERSION);
        }

        if (IsDust(txout, wallet.chain().relayDustFee())) {
            return util::Error{_("Transaction amount too small")};
        }
        txNew.vout.push_back(txout);
    }

    // Include the fees for things that aren't inputs, excluding the change output
    const CAmount not_input_fees = coin_selection_params.m_effective_feerate.GetFee(coin_selection_params.tx_noinputs_size);
    CAmount selection_target = recipients_sum + not_input_fees;

    // This can only happen if feerate is 0, and requested destinations are value of 0 (e.g. OP_RETURN)
    // and no pre-selected inputs. This will result in 0-input transaction, which is consensus-invalid anyways
    if (selection_target == 0 && !coin_control.HasSelected()) {
        return util::Error{_("Transaction requires one destination of non-0 value, a non-0 feerate, or a pre-selected input")};
    }

    // Fetch manually selected coins
    PreSelectedInputs preset_inputs;
    if (coin_control.HasSelected()) {
        auto res_fetch_inputs = FetchSelectedInputs(wallet, coin_control, coin_selection_params);
        if (!res_fetch_inputs) return util::Error{util::ErrorString(res_fetch_inputs)};
        preset_inputs = *res_fetch_inputs;
    }

    // Fetch wallet available coins if "other inputs" are
    // allowed (coins automatically selected by the wallet)
    CoinsResult available_coins;
    if (coin_control.m_allow_other_inputs) {
        available_coins = AvailableCoins(wallet,
                                         &coin_control,
                                         coin_selection_params.m_effective_feerate,
                                         1,            /*nMinimumAmount*/
                                         MAX_MONEY,    /*nMaximumAmount*/
                                         MAX_MONEY,    /*nMinimumSumAmount*/
                                         0);           /*nMaximumCount*/
    }

    // Choose coins to use
    auto select_coins_res = SelectCoins(wallet, available_coins, preset_inputs, /*nTargetValue=*/selection_target, coin_control, coin_selection_params);
    if (!select_coins_res) {
        if (coin_control.nCoinType == CoinType::ONLY_NONDENOMINATED) {
            return util::Error{_("Unable to locate enough non-denominated funds for this transaction.")};
        } else if (coin_control.nCoinType == CoinType::ONLY_FULLY_MIXED) {
            return util::Error{_("Unable to locate enough mixed funds for this transaction.") +
                               Untranslated(" ") + strprintf(_("%s uses exact denominated amounts to send funds, you might simply need to mix some more coins."), gCoinJoinName)};
        }
        // 'SelectCoins' either returns a specific error message or, if empty, means a general "Insufficient funds".
        const bilingual_str& err = util::ErrorString(select_coins_res);
        return util::Error{err.empty() ? _("Insufficient funds.") : err};
    }
    const SelectionResult& result = *select_coins_res;
    TRACE5(coin_selection, selected_coins, wallet.GetName().c_str(), GetAlgorithmName(result.GetAlgo()).c_str(), result.GetTarget(), result.GetWaste(), result.GetSelectedValue());

    // Dash: fully mixed CoinJoin sends spend exact denominated amounts and must never create a
    // change output; any excess is dropped to fees instead.
    const CAmount change_amount = coin_control.nCoinType == CoinType::ONLY_FULLY_MIXED
                                      ? 0
                                      : result.GetChange(coin_selection_params.min_viable_change, coin_selection_params.m_change_fee);
    CTxOut newTxOut(change_amount, scriptChange);
    if (change_amount > 0) {
        if (nChangePosInOut == -1) {
            // Insert change txn at random position:
            nChangePosInOut = rng_fast.randrange(txNew.vout.size() + 1);
        } else if ((unsigned int)nChangePosInOut > txNew.vout.size()) {
            return util::Error{_("Transaction change output index out of range")};
        }
        txNew.vout.insert(txNew.vout.begin() + nChangePosInOut, newTxOut);
    } else {
        nChangePosInOut = -1;
    }

    // We're making a copy of vecSend because it's const, sortedVecSend should be used
    // in place of vecSend in all subsequent usage.
    std::vector<CRecipient> sortedVecSend{vecSend};
    if (sort_bip69) {
        std::sort(txNew.vout.begin(), txNew.vout.end(), CompareOutputBIP69());
        // The output reduction loop uses vecSend to map to txNew.vout, we need to
        // shuffle them both to ensure this mapping remains consistent
        std::sort(sortedVecSend.begin(), sortedVecSend.end(),
                    [](const CRecipient& a, const CRecipient& b) {
                        return a.nAmount < b.nAmount || (a.nAmount == b.nAmount && a.scriptPubKey < b.scriptPubKey);
                    });

        // If there was a change output added before, we must update its position now
        if (nChangePosInOut != -1) {
            const auto it = std::find(txNew.vout.begin(), txNew.vout.end(), newTxOut);
            assert(it != txNew.vout.end());
            nChangePosInOut = std::distance(txNew.vout.begin(), it);
        }
    };

    // The sequence number is set to non-maxint so that DiscourageFeeSniping
    // works.
    const uint32_t nSequence{CTxIn::SEQUENCE_FINAL - 1};
    for (const auto& coin : result.GetInputSet()) {
        txNew.vin.emplace_back(coin->outpoint, CScript(), nSequence);
    }
    DiscourageFeeSniping(txNew, rng_fast, wallet.chain(), wallet.GetLastBlockHash(), wallet.GetLastBlockHeight());

    // Fill in final vin and shuffle/sort it
    if (sort_bip69) { std::sort(txNew.vin.begin(), txNew.vin.end(), CompareInputBIP69()); }
    else { Shuffle(txNew.vin.begin(), txNew.vin.end(), coin_selection_params.rng_fast); }

    // Calculate the transaction fee
    int nBytes = CalculateMaximumSignedTxSize(CTransaction(txNew), &wallet, &coin_control);
    if (nBytes == -1) {
        return util::Error{_("Missing solving data for estimating transaction size")};
    }

    if (nExtraPayloadSize != 0) {
        // account for extra payload in fee calculation
        nBytes += GetSizeOfCompactSize(nExtraPayloadSize) + nExtraPayloadSize;
    }

    CAmount fee_needed = coin_selection_params.m_effective_feerate.GetFee(nBytes);
    nFeeRet = result.GetSelectedValue() - recipients_sum - change_amount;

    // Dash: coin selection sizes its target (and, through SelectionResult::m_target, the
    // change amount) using coin_selection_params.tx_noinputs_size, which assumes the vin-count
    // CompactSize prefix is always 1 byte because the final input count isn't known yet. Once
    // the tx crosses a CompactSize size class (253+ inputs), the true prefix is wider than
    // assumed and fee_needed (computed from the accurately-measured final nBytes above) can
    // exceed nFeeRet by a few duffs. Recover the shortfall from the change output, which is
    // otherwise unspoken for, instead of failing the whole transaction.
    if (!coin_selection_params.m_subtract_fee_outputs && fee_needed > nFeeRet && nChangePosInOut != -1) {
        const CAmount shortfall = fee_needed - nFeeRet;
        CTxOut& change = txNew.vout.at(nChangePosInOut);
        if (change.nValue - shortfall >= coin_selection_params.min_viable_change) {
            change.nValue -= shortfall;
            nFeeRet += shortfall;
        } else {
            // The change output can't absorb the shortfall without becoming uneconomical.
            // Drop it entirely, let its whole value go to the fee, and resize since removing
            // an output changes the transaction's serialized size.
            nFeeRet += change.nValue;
            txNew.vout.erase(txNew.vout.begin() + nChangePosInOut);
            nChangePosInOut = -1;
            nBytes = CalculateMaximumSignedTxSize(CTransaction(txNew), &wallet, &coin_control);
            if (nBytes == -1) {
                return util::Error{_("Missing solving data for estimating transaction size")};
            }
            if (nExtraPayloadSize != 0) {
                nBytes += GetSizeOfCompactSize(nExtraPayloadSize) + nExtraPayloadSize;
            }
            fee_needed = coin_selection_params.m_effective_feerate.GetFee(nBytes);
        }
    }

    // The only time that fee_needed should be less than the amount available for fees is when
    // we are subtracting the fee from the outputs. If this occurs at any other time, it is a bug.
    if (!coin_selection_params.m_subtract_fee_outputs && fee_needed > nFeeRet) {
        return util::Error{Untranslated(STR_INTERNAL_BUG("Fee needed > fee paid"))};
    }

    // If there is a change output and we overpay the fees then increase the change to match the fee needed
    if (nChangePosInOut != -1 && fee_needed < nFeeRet) {
        auto& change = txNew.vout.at(nChangePosInOut);
        change.nValue += nFeeRet - fee_needed;
        nFeeRet = fee_needed;
    }

    // Reduce output values for subtractFeeFromAmount
    if (coin_selection_params.m_subtract_fee_outputs) {
        CAmount to_reduce = fee_needed - nFeeRet;
        int i = 0;
        bool fFirst = true;
        for (const auto& recipient : sortedVecSend)
        {
            if (i == nChangePosInOut) {
                ++i;
            }
            CTxOut& txout = txNew.vout[i];

            if (recipient.fSubtractFeeFromAmount)
            {
                txout.nValue -= to_reduce / outputs_to_subtract_fee_from; // Subtract fee equally from each selected recipient

                if (fFirst) // first receiver pays the remainder not divisible by output count
                {
                    fFirst = false;
                    txout.nValue -= to_reduce % outputs_to_subtract_fee_from;
                }
                // Error if this output is reduced to be below dust
                if (IsDust(txout, wallet.chain().relayDustFee())) {
                    if (txout.nValue < 0) {
                        return util::Error{_("The transaction amount is too small to pay the fee")};
                    } else {
                        return util::Error{_("The transaction amount is too small to send after the fee has been deducted")};
                    }
                }
            }
            ++i;
        }
        nFeeRet = fee_needed;
    }

    // Give up if change keypool ran out and change is required
    if (scriptChange.empty() && nChangePosInOut != -1) {
        return util::Error{error};
    }

    if (sign && !wallet.SignTransaction(txNew)) {
        return util::Error{_("Signing transaction failed")};
    }

    // Return the constructed transaction data.
    CTransactionRef tx = MakeTransactionRef(std::move(txNew));

    // Limit size
    if ((sign && ::GetSerializeSize(*tx, PROTOCOL_VERSION) > MAX_STANDARD_TX_SIZE) ||
        (!sign && static_cast<size_t>(nBytes) > MAX_STANDARD_TX_SIZE)) {
        return util::Error{_("Transaction too large")};
    }

    if (fee_needed > nFeeRet) {
        return util::Error{_("Fee needed > fee paid")};
    }

    if (nFeeRet > wallet.m_default_max_tx_fee) {
        return util::Error{TransactionErrorString(TransactionError::MAX_FEE_EXCEEDED)};
    }

    if (gArgs.GetBoolArg("-walletrejectlongchains", DEFAULT_WALLET_REJECT_LONG_CHAINS)) {
        // Lastly, ensure this tx will pass the mempool's chain limits
        if (!wallet.chain().checkChainLimits(tx)) {
            return util::Error{_("Transaction has too long of a mempool chain")};
        }
    }

    // Before we return success, we assume any change key will be used to prevent
    // accidental reuse.
    reservedest.KeepDestination();

    wallet.WalletLogPrintf("Fee Calculation: Fee:%d Bytes:%u Tgt:%d (requested %d) Reason:\"%s\" Decay %.5f: Estimation: (%g - %g) %.2f%% %.1f/(%.1f %d mem %.1f out) Fail: (%g - %g) %.2f%% %.1f/(%.1f %d mem %.1f out)\n",
              nFeeRet, nBytes, feeCalc.returnedTarget, feeCalc.desiredTarget, StringForFeeReason(feeCalc.reason), feeCalc.est.decay,
              feeCalc.est.pass.start, feeCalc.est.pass.end,
              (feeCalc.est.pass.totalConfirmed + feeCalc.est.pass.inMempool + feeCalc.est.pass.leftMempool) > 0.0 ? 100 * feeCalc.est.pass.withinTarget / (feeCalc.est.pass.totalConfirmed + feeCalc.est.pass.inMempool + feeCalc.est.pass.leftMempool) : 0.0,
              feeCalc.est.pass.withinTarget, feeCalc.est.pass.totalConfirmed, feeCalc.est.pass.inMempool, feeCalc.est.pass.leftMempool,
              feeCalc.est.fail.start, feeCalc.est.fail.end,
              (feeCalc.est.fail.totalConfirmed + feeCalc.est.fail.inMempool + feeCalc.est.fail.leftMempool) > 0.0 ? 100 * feeCalc.est.fail.withinTarget / (feeCalc.est.fail.totalConfirmed + feeCalc.est.fail.inMempool + feeCalc.est.fail.leftMempool) : 0.0,
              feeCalc.est.fail.withinTarget, feeCalc.est.fail.totalConfirmed, feeCalc.est.fail.inMempool, feeCalc.est.fail.leftMempool);
    return CreatedTransactionResult(tx, nFeeRet, nChangePosInOut, feeCalc);
}

util::Result<CreatedTransactionResult> CreateTransaction(
        CWallet& wallet,
        const std::vector<CRecipient>& vecSend,
        int change_pos,
        const CCoinControl& coin_control,
        bool sign,
        int nExtraPayloadSize)
{
    if (vecSend.empty()) {
        return util::Error{_("Transaction must have at least one recipient")};
    }

    if (std::any_of(vecSend.cbegin(), vecSend.cend(), [](const auto& recipient){ return recipient.nAmount < 0; })) {
        return util::Error{_("Transaction amounts must not be negative")};
    }

    LOCK(wallet.cs_wallet);

    auto res = CreateTransactionInternal(wallet, vecSend, change_pos, coin_control, sign, nExtraPayloadSize);
    TRACE4(coin_selection, normal_create_tx_internal, wallet.GetName().c_str(), bool(res),
           res ? res->fee : 0, res ? res->change_pos : 0);
    if (!res) return res;
    const auto& txr_ungrouped = *res;
    // try with avoidpartialspends unless it's enabled already
    if (txr_ungrouped.fee > 0 /* 0 means non-functional fee rate estimation */ && wallet.m_max_aps_fee > -1 && !coin_control.m_avoid_partial_spends) {
        TRACE1(coin_selection, attempting_aps_create_tx, wallet.GetName().c_str());
        CCoinControl tmp_cc = coin_control;
        tmp_cc.m_avoid_partial_spends = true;

        // Reuse the change destination from the first creation attempt to avoid skipping BIP44 indexes
        const int ungrouped_change_pos = txr_ungrouped.change_pos;
        if (ungrouped_change_pos != -1) {
            ExtractDestination(txr_ungrouped.tx->vout[ungrouped_change_pos].scriptPubKey, tmp_cc.destChange);
        }

        auto txr_grouped = CreateTransactionInternal(wallet, vecSend, change_pos, tmp_cc, sign, nExtraPayloadSize);
        // if fee of this alternative one is within the range of the max fee, we use this one
        const bool use_aps{txr_grouped.has_value() ? (txr_grouped->fee <= txr_ungrouped.fee + wallet.m_max_aps_fee) : false};
        TRACE5(coin_selection, aps_create_tx_internal, wallet.GetName().c_str(), use_aps, txr_grouped.has_value(),
               txr_grouped.has_value() ? txr_grouped->fee : 0, txr_grouped.has_value() ? txr_grouped->change_pos : 0);
        if (txr_grouped) {
            wallet.WalletLogPrintf("Fee non-grouped = %lld, grouped = %lld, using %s\n",
                txr_ungrouped.fee, txr_grouped->fee, use_aps ? "grouped" : "non-grouped");
            if (use_aps) return txr_grouped;
        }
    }
    return res;
}

bool FundTransaction(CWallet& wallet, CMutableTransaction& tx, CAmount& nFeeRet, int& nChangePosInOut, bilingual_str& error, bool lockUnspents, const std::set<int>& setSubtractFeeFromOutputs, CCoinControl coinControl)
{
    std::vector<CRecipient> vecSend;

    // If no specific change position was requested, apply BIP69
    if (nChangePosInOut == -1) {
        std::sort(tx.vin.begin(), tx.vin.end(), CompareInputBIP69());
        std::sort(tx.vout.begin(), tx.vout.end(), CompareOutputBIP69());
    }

    // Turn the txout set into a CRecipient vector.
    for (size_t idx = 0; idx < tx.vout.size(); idx++) {
        const CTxOut& txOut = tx.vout[idx];
        CRecipient recipient = {txOut.scriptPubKey, txOut.nValue, setSubtractFeeFromOutputs.count(idx) == 1};
        vecSend.push_back(recipient);
    }

    // Acquire the locks to prevent races to the new locked unspents between the
    // CreateTransaction call and LockCoin calls (when lockUnspents is true).
    LOCK(wallet.cs_wallet);

    // Fetch specified UTXOs from the UTXO set to get the scriptPubKeys and values of the outputs being selected
    // and to match with the given solving_data. Only used for non-wallet outputs.
    std::map<COutPoint, Coin> coins;
    for (const CTxIn& txin : tx.vin) {
        coins[txin.prevout]; // Create empty map entry keyed by prevout.
    }
    wallet.chain().findCoins(coins);

    for (const CTxIn& txin : tx.vin) {
        const auto& outPoint = txin.prevout;
        if (wallet.IsMine(outPoint)) {
            // The input was found in the wallet, so select as internal
            coinControl.Select(outPoint);
        } else if (coins[outPoint].out.IsNull()) {
            error = _("Unable to find UTXO for external input");
            return false;
        } else {
            // The input was not in the wallet, but is in the UTXO set, so select as external
            coinControl.SelectExternal(outPoint, coins[outPoint].out);
        }
    }

    auto res = CreateTransaction(wallet, vecSend, nChangePosInOut, coinControl, /*sign=*/false, tx.vExtraPayload.size());
    if (!res) {
        error = util::ErrorString(res);
        return false;
    }
    const auto& txr = *res;
    CTransactionRef tx_new = txr.tx;
    nFeeRet = txr.fee;
    nChangePosInOut = txr.change_pos;

    if (nChangePosInOut != -1) {
        tx.vout.insert(tx.vout.begin() + nChangePosInOut, tx_new->vout[nChangePosInOut]);
    }

    // Copy output sizes from new transaction; they may have had the fee
    // subtracted from them.
    for (unsigned int idx = 0; idx < tx.vout.size(); idx++) {
        tx.vout[idx].nValue = tx_new->vout[idx].nValue;
    }

    // Add new txins while keeping original txin scriptSig/order.
    for (const CTxIn& txin : tx_new->vin) {
        if (!coinControl.IsSelected(txin.prevout)) {
            tx.vin.push_back(txin);

        }
        if (lockUnspents) {
            wallet.LockCoin(txin.prevout);
        }

    }

    return true;
}

bool GenBudgetSystemCollateralTx(CWallet& wallet, CTransactionRef& tx, uint256 hash, CAmount amount, const COutPoint& outpoint)
{
    const CScript scriptChange{CScript() << OP_RETURN << ToByteVector(hash)};
    const std::vector<CRecipient> vecSend{{scriptChange, amount, false}};

    CCoinControl coinControl;
    if (!outpoint.IsNull()) {
        // Fund the collateral from the given outpoint only.
        coinControl.m_allow_other_inputs = false;
        coinControl.Select(outpoint);
    }

    auto res{CreateTransaction(wallet, vecSend, RANDOM_CHANGE_POSITION, coinControl)};
    if (!res) {
        wallet.WalletLogPrintf("%s -- Error: %s\n", __func__, util::ErrorString(res).original);
        return false;
    }
    tx = res->tx;
    return true;
}
} // namespace wallet
