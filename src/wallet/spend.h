// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_SPEND_H
#define BITCOIN_WALLET_SPEND_H

#include <policy/fees.h> // for FeeCalculation
#include <util/result.h>
#include <wallet/coincontrol.h>
#include <wallet/coinselection.h>
#include <wallet/transaction.h>
#include <wallet/wallet.h>

#include <optional>

namespace wallet {
//! Special value for setting a random position for change output
constexpr int RANDOM_CHANGE_POSITION{-1};

/** Get the marginal bytes if spending the specified output from this transaction.
 * Use CoinControl to determine whether to expect signature grinding when calculating the size of the input spend. */
int CalculateMaximumSignedInputSize(const CTxOut& txout, const CWallet* pwallet, const CCoinControl* coin_control = nullptr);
int CalculateMaximumSignedInputSize(const CTxOut& txout, const COutPoint outpoint, const SigningProvider* pwallet, const CCoinControl* coin_control = nullptr);

/** Calculate the size of the transaction using CoinControl to determine
 * whether to expect signature grinding when calculating the size of the input spend. */
int64_t CalculateMaximumSignedTxSize(const CTransaction &tx, const CWallet *wallet, const CCoinControl* coin_control = nullptr) EXCLUSIVE_LOCKS_REQUIRED(wallet->cs_wallet);
int64_t CalculateMaximumSignedTxSize(const CTransaction &tx, const CWallet *wallet, const std::vector<CTxOut>& txouts, const CCoinControl* coin_control = nullptr);

/**
 * COutputs available for spending, stored by OutputType.
 * This struct is really just a wrapper around OutputType vectors with a convenient
 * method for concatenating and returning all COutputs as one vector.
 *
 * clear(), size() methods are implemented so that one can interact with
 * the CoinsResult struct as if it was a vector
 */
struct CoinsResult {
    /** Vectors for each OutputType */
    std::vector<COutput> legacy;

    /** Other is a catch-all for anything that doesn't match the known OutputTypes */
    std::vector<COutput> other;

    /** Concatenate and return all COutputs as one vector */
    std::vector<COutput> all() const;

    /** The following methods are provided so that CoinsResult can mimic a vector,
     * i.e., methods can work with individual OutputType vectors or on the entire object */
    uint64_t size() const;
    void clear();

    /** Sum of all available coins raw value */
    CAmount total_amount{0};
    /** Sum of all available coins effective value (each output value minus fees required to spend it) */
    std::optional<CAmount> total_effective_amount{0};

    CAmount GetTotalAmount() const { return total_amount; }
    std::optional<CAmount> GetEffectiveTotalAmount() const { return total_effective_amount; }
};

/**
 * Populate the CoinsResult struct with vectors of available COutputs, organized by OutputType.
 */
CoinsResult AvailableCoins(const CWallet& wallet,
                           const CCoinControl* coinControl = nullptr,
                           std::optional<CFeeRate> feerate = std::nullopt,
                           const CAmount& nMinimumAmount = 1,
                           const CAmount& nMaximumAmount = MAX_MONEY,
                           const CAmount& nMinimumSumAmount = MAX_MONEY,
                           const uint64_t nMaximumCount = 0,
                           bool only_spendable = true) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet);

/**
 * Wrapper function for AvailableCoins which skips the `feerate` parameter. Use this function
 * to list all available coins (e.g. listunspent RPC) while not intending to fund a transaction.
 */
CoinsResult AvailableCoinsListUnspent(const CWallet& wallet, const CCoinControl* coinControl = nullptr, const CAmount& nMinimumAmount = 1, const CAmount& nMaximumAmount = MAX_MONEY, const CAmount& nMinimumSumAmount = MAX_MONEY, const uint64_t nMaximumCount = 0) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet);

/**
 * Find non-change parent output.
 */
const CTxOut& FindNonChangeParentOutput(const CWallet& wallet, const CTransaction& tx, int output) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet);
const CTxOut& FindNonChangeParentOutput(const CWallet& wallet, const COutPoint& outpoint) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet);

/**
 * Return list of available coins and locked coins grouped by non-change output address.
 */
std::map<CTxDestination, std::vector<COutput>> ListCoins(const CWallet& wallet) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet);

// An eligibility filter to run coin selection with, and whether OutputTypes may be mixed
// while doing so.
struct SelectionFilter {
    CoinEligibilityFilter filter;
    bool allow_mixed_output_types{true};
};

/**
 * Group outputs, computing both the positive-only and mixed groups in a single pass.
 */
Groups GroupOutputs(const CWallet& wallet, const std::vector<COutput>& outputs, const CoinSelectionParams& coin_sel_params, const CoinEligibilityFilter& filter);

/**
 * Group outputs for every filter in `filters` in a single pass over `outputs`, so callers that
 * need groups for several CoinEligibilityFilters (e.g. AutomaticCoinSelection's ordered_filters
 * walk) don't have to re-run the grouping process once per filter.
 */
FilteredOutputGroups GroupOutputs(const CWallet& wallet, const std::vector<COutput>& outputs, const CoinSelectionParams& coin_sel_params, const std::vector<SelectionFilter>& filters);

/**
 * Attempt to find a valid input set that preserves privacy by not mixing OutputTypes.
 * `ChooseSelectionResult()` will be called on each OutputType individually and the best
 * the solution (according to the waste metric) will be chosen. If a valid input cannot be found from any
 * single OutputType, fallback to running `ChooseSelectionResult()` over all available coins.
 *
 * Performs no grouping itself: both `groups` and `mixed_groups` must already have been computed by
 * the caller (e.g. via GroupOutputs' filter-list overload), once, ahead of every filter it walks.
 *
 * param@[in]  wallet                    The wallet which provides solving data for the coins
 * param@[in]  nTargetValue              The target value
 * param@[in]  groups                    The groups (positive-only and mixed) already computed for this filter over available_coins.legacy
 * param@[in]  mixed_groups              The groups already computed for this filter over available_coins.all() (legacy + other), used only if allow_mixed_output_types
 * param@[in]  coin_selection_params     Parameters for the coin selection
 * param@[in]  allow_mixed_output_types  Relax restriction that SelectionResults must be of the same OutputType
 * returns                               If successful, a SelectionResult containing the input set
 *                                       If failed, returns (1) an empty error message if the target was not reached (general "Insufficient funds")
 *                                                  or (2) an specific error message if there was something particularly wrong (e.g. a selection
 *                                                  result that surpassed the tx max weight size).
 */
util::Result<SelectionResult> AttemptSelection(const CWallet& wallet, const CAmount& nTargetValue, Groups& groups, Groups& mixed_groups,
                                                const CoinSelectionParams& coin_selection_params, bool allow_mixed_output_types, CoinType nCoinType = CoinType::ALL_COINS);

/**
 * Attempt to find a valid input set that meets the provided eligibility filter and target.
 * Multiple coin selection algorithms will be run and the input set that produces the least waste
 * (according to the waste metric) will be chosen.
 *
 * param@[in]  nTargetValue              The target value
 * param@[in]  groups                    The struct containing the outputs grouped by script and divided by (1) positive only outputs and (2) all outputs (positive + negative).
 * param@[in]  coin_selection_params     Parameters for the coin selection
 * returns                               If successful, a SelectionResult containing the input set
 *                                       If failed, returns (1) an empty error message if the target was not reached (general "Insufficient funds")
 *                                                  or (2) an specific error message if there was something particularly wrong (e.g. a selection
 *                                                  result that surpassed the tx max weight size).
 */
util::Result<SelectionResult> ChooseSelectionResult(const CAmount& nTargetValue, Groups& groups, const CoinSelectionParams& coin_selection_params,
                                                     CoinType nCoinType = CoinType::ALL_COINS, CAmount max_tx_fee = 0);

// User manually selected inputs that must be part of the transaction
struct PreSelectedInputs
{
    std::set<std::shared_ptr<COutput>> coins;
    // If subtract fee from outputs is disabled, the 'total_amount'
    // will be the sum of each output effective value
    // instead of the sum of the outputs amount
    CAmount total_amount{0};

    void Insert(const COutput& output, bool subtract_fee_outputs)
    {
        if (subtract_fee_outputs) {
            total_amount += output.txout.nValue;
        } else {
            total_amount += output.GetEffectiveValue();
        }
        coins.insert(std::make_shared<COutput>(output));
    }
};

/**
 * Fetch and validate coin control selected inputs.
 * Coins could be internal (from the wallet) or external.
*/
util::Result<PreSelectedInputs> FetchSelectedInputs(const CWallet& wallet, const CCoinControl& coin_control,
                                                    const CoinSelectionParams& coin_selection_params) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet);

/**
 * Select a set of coins such that nTargetValue is met; never select unconfirmed coins if they are not ours
 * param@[in]   wallet                 The wallet which provides data necessary to spend the selected coins
 * param@[in]   available_coins        The struct of coins, organized by OutputType, available for selection prior to filtering
 * param@[in]   nTargetValue           The target value
 * param@[in]   coin_selection_params  Parameters for this coin selection such as feerates, whether to avoid partial spends,
 *                                     and whether to subtract the fee from the outputs.
 * returns                             If successful, a SelectionResult containing the selected coins
 *                                     If failed, returns (1) an empty error message if the target was not reached (general "Insufficient funds")
 *                                                or (2) an specific error message if there was something particularly wrong (e.g. a selection
 *                                                result that surpassed the tx max weight size).
 */
util::Result<SelectionResult> AutomaticCoinSelection(const CWallet& wallet, CoinsResult& available_coins, const CAmount& nTargetValue, const CCoinControl& coin_control,
                 const CoinSelectionParams& coin_selection_params) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet);

/**
 * Select all coins from coin_control, and if coin_control 'm_allow_other_inputs=true', call 'AutomaticCoinSelection' to
 * select a set of coins such that nTargetValue - pre_set_inputs.total_amount is met.
 */
util::Result<SelectionResult> SelectCoins(const CWallet& wallet, CoinsResult& available_coins, const PreSelectedInputs& pre_set_inputs,
                                          const CAmount& nTargetValue, const CCoinControl& coin_control,
                                          const CoinSelectionParams& coin_selection_params) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet);

struct CreatedTransactionResult
{
    CTransactionRef tx;
    CAmount fee;
    FeeCalculation fee_calc;
    int change_pos;

    CreatedTransactionResult(CTransactionRef _tx, CAmount _fee, int _change_pos, const FeeCalculation& _fee_calc)
        : tx(_tx), fee(_fee), fee_calc(_fee_calc), change_pos(_change_pos) {}
};

/**
 * Create a new transaction paying the recipients with a set of coins
 * selected by SelectCoins(); Also create the change output, when needed
 * @note passing change_pos as -1 will result in setting a random position
 */
util::Result<CreatedTransactionResult> CreateTransaction(CWallet& wallet, const std::vector<CRecipient>& vecSend, int change_pos, const CCoinControl& coin_control, bool sign = true, int nExtraPayloadSize = 0);

/**
 * Insert additional inputs into the transaction by
 * calling CreateTransaction();
 */
bool FundTransaction(CWallet& wallet, CMutableTransaction& tx, CAmount& nFeeRet, int& nChangePosInOut, bilingual_str& error, bool lockUnspents, const std::set<int>& setSubtractFeeFromOutputs, CCoinControl);

bool GenBudgetSystemCollateralTx(CWallet& wallet, CTransactionRef& tx, uint256 hash, CAmount amount, const COutPoint& outpoint = COutPoint());
} // namespace wallet

#endif // BITCOIN_WALLET_SPEND_H
