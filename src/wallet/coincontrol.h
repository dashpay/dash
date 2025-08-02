// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_COINCONTROL_H
#define BITCOIN_WALLET_COINCONTROL_H

#include <key.h>
#include <policy/feerate.h>
#include <policy/fees.h>
#include <primitives/transaction.h>
#include <script/keyorigin.h>
#include <script/signingprovider.h>
#include <script/standard.h>

#include <optional>
#include <algorithm>
#include <map>
#include <set>

namespace wallet {
enum class CoinType : uint8_t
{
    ALL_COINS,
    ONLY_FULLY_MIXED,
    ONLY_READY_TO_MIX,
    ONLY_NONDENOMINATED,
    ONLY_MASTERNODE_COLLATERAL, // find masternode outputs including locked ones (use with caution)
    ONLY_COINJOIN_COLLATERAL,
    // Attributes
    MIN_COIN_TYPE = ALL_COINS,
    MAX_COIN_TYPE = ONLY_COINJOIN_COLLATERAL,
};

//! Default for -avoidpartialspends
static constexpr bool DEFAULT_AVOIDPARTIALSPENDS = false;

const int DEFAULT_MIN_DEPTH = 0;
const int DEFAULT_MAX_DEPTH = 9999999;

/** Coin Control Features. */
class CCoinControl
{
public:
    //! Custom change destination, if not set an address is generated
    CTxDestination destChange = CNoDestination();
    //! If false, only safe inputs will be used
    bool m_include_unsafe_inputs = false;
    //! If true, the selection process can add extra unselected inputs from the wallet
    //! while requires all selected inputs be used
    bool m_allow_other_inputs = false;
    //! If false, only include as many inputs as necessary to fulfill a coin selection request. Only usable together with m_allow_other_inputs
    bool fRequireAllInputs = true;
    //! Includes watch only addresses which are solvable
    bool fAllowWatchOnly = false;
    //! Override automatic min/max checks on fee, m_feerate must be set if true
    bool fOverrideFeeRate = false;
    //! Override the wallet's m_pay_tx_fee if set
    std::optional<CFeeRate> m_feerate;
    //! Override the discard feerate estimation with m_discard_feerate in CreateTransaction if set
    std::optional<CFeeRate> m_discard_feerate;
    //! Override the default confirmation target if set
    std::optional<unsigned int> m_confirm_target;
    //! Avoid partial use of funds sent to a given address
    bool m_avoid_partial_spends = DEFAULT_AVOIDPARTIALSPENDS;
    //! Forbids inclusion of dirty (previously used) addresses
    bool m_avoid_address_reuse = false;
    //! Fee estimation mode to control arguments to estimateSmartFee
    FeeEstimateMode m_fee_mode = FeeEstimateMode::UNSET;
    //! Minimum chain depth value for coin availability
    int m_min_depth = DEFAULT_MIN_DEPTH;
    //! Maximum chain depth value for coin availability
    int m_max_depth = DEFAULT_MAX_DEPTH;
    //! SigningProvider that has pubkeys and scripts to do spend size estimation for external inputs
    FlatSigningProvider m_external_provider;
    //! Controls which types of coins are allowed to be used (default: ALL_COINS)
    CoinType nCoinType = CoinType::ALL_COINS;

    CCoinControl(CoinType coin_type = CoinType::ALL_COINS);

    bool HasSelected() const
    {
        return (setSelected.size() > 0);
    }

    bool IsSelected(const COutPoint& output) const
    {
        return (setSelected.count(output) > 0);
    }

    bool IsExternalSelected(const COutPoint& output) const
    {
        return (m_external_txouts.count(output) > 0);
    }

    bool GetExternalOutput(const COutPoint& outpoint, CTxOut& txout) const
    {
        const auto ext_it = m_external_txouts.find(outpoint);
        if (ext_it == m_external_txouts.end()) {
            return false;
        }
        txout = ext_it->second;
        return true;
    }

    void Select(const COutPoint& output)
    {
        setSelected.insert(output);
    }

    void SelectExternal(const COutPoint& outpoint, const CTxOut& txout)
    {
        setSelected.insert(outpoint);
        m_external_txouts.emplace(outpoint, txout);
    }

    void UnSelect(const COutPoint& output)
    {
        setSelected.erase(output);
    }

    void UnSelectAll()
    {
        setSelected.clear();
    }

    void ListSelected(std::vector<COutPoint>& vOutpoints) const
    {
        vOutpoints.assign(setSelected.begin(), setSelected.end());
    }

    // Dash-specific helpers
    void UseCoinJoin(bool fUseCoinJoin)
    {
        nCoinType = fUseCoinJoin ? CoinType::ONLY_FULLY_MIXED : CoinType::ALL_COINS;
    }

    bool IsUsingCoinJoin() const
    {
        return nCoinType == CoinType::ONLY_FULLY_MIXED;
    }

private:
    std::set<COutPoint> setSelected;
    std::map<COutPoint, CTxOut> m_external_txouts;
};
} // namespace wallet

#endif // BITCOIN_WALLET_COINCONTROL_H
