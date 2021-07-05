// Public Resource ℗ 2021 Satoshi Nakamoto
// Public Resource ℗ 2021 𝖡𝗂𝗍𝖼𝗈𝗂𝗇 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌
// Public Resource ℗ 2020 𝖠𝗆𝖾𝗋𝗈 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌
// THIS REPOSITORY IS LICENSED UNDER THE AMERO PUBLIC RESOURCE LICENSE.

#include <wallet/fees.h>

#include <policy/policy.h>
#include <txmempool.h>
#include <util.h>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <wallet/wallet.h>


CAmount GetRequiredFee(unsigned int nTxBytes)
{
    return std::max(CWallet::minTxFee.GetFee(nTxBytes), ::minRelayTxFee.GetFee(nTxBytes));
}

CAmount GetMinimumFee(unsigned int nTxBytes, const CCoinControl& coin_control, const CTxMemPool& pool, const CBlockPolicyEstimator& estimator, FeeCalculation *feeCalc)
{
    CAmount fee_needed = GetMinimumFeeRate(coin_control, pool, estimator, feeCalc).GetFee(nTxBytes);
    // Always obey the maximum
    if (fee_needed > maxTxFee) {
        fee_needed = maxTxFee;
        if (feeCalc) feeCalc->reason = FeeReason::MAXTXFEE;
    }
    return fee_needed;
}

CFeeRate GetRequiredFeeRate()
{
    return std::max(CWallet::minTxFee, ::minRelayTxFee);
}

CFeeRate GetMinimumFeeRate(const CCoinControl& coin_control, const CTxMemPool& pool, const CBlockPolicyEstimator& estimator, FeeCalculation *feeCalc)
{
    /* User control of how to calculate fee uses the following parameter precedence:
       1. coin_control.m_feerate
       2. coin_control.m_confirm_target
       3. payTxFee (user-set global variable)
       4. nTxConfirmTarget (user-set global variable)
       The first parameter that is set is used.
    */
    CFeeRate feerate_needed ;
    if (coin_control.m_feerate) { // 1.
        feerate_needed = *(coin_control.m_feerate);
        if (feeCalc) feeCalc->reason = FeeReason::PAYTXFEE;
        // Allow to override automatic min/max check over coin control instance
        if (coin_control.fOverrideFeeRate) return feerate_needed;
    }
    else if (!coin_control.m_confirm_target && ::payTxFee != CFeeRate(0)) { // 3. TODO: remove magic value of 0 for global payTxFee
        feerate_needed = ::payTxFee;
        if (feeCalc) feeCalc->reason = FeeReason::PAYTXFEE;
    }
    else { // 2. or 4.
        // We will use smart fee estimation
        unsigned int target = coin_control.m_confirm_target ? *coin_control.m_confirm_target : ::nTxConfirmTarget;
        // By default estimates are economical
        bool conservative_estimate = true;
        // Allow to override the default fee estimate mode over the CoinControl instance
        if (coin_control.m_fee_mode == FeeEstimateMode::CONSERVATIVE) conservative_estimate = true;
        else if (coin_control.m_fee_mode == FeeEstimateMode::ECONOMICAL) conservative_estimate = false;

        feerate_needed = estimator.estimateSmartFee(target, feeCalc, conservative_estimate);
        if (feerate_needed == CFeeRate(0)) {
            // if we don't have enough data for estimateSmartFee, then use fallbackFee
            feerate_needed = CWallet::fallbackFee;
            if (feeCalc) feeCalc->reason = FeeReason::FALLBACK;
        }
        // Obey mempool min fee when using smart fee estimation
        CFeeRate min_mempool_feerate = pool.GetMinFee(gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000);
        if (feerate_needed < min_mempool_feerate) {
            feerate_needed = min_mempool_feerate;
            if (feeCalc) feeCalc->reason = FeeReason::MEMPOOL_MIN;
        }
    }

    // prevent user from paying a fee below minRelayTxFee or minTxFee
    CFeeRate required_feerate = GetRequiredFeeRate();
    if (required_feerate > feerate_needed) {
        feerate_needed = required_feerate;
        if (feeCalc) feeCalc->reason = FeeReason::REQUIRED;
    }
    return feerate_needed;
}

CFeeRate GetDiscardRate(const CBlockPolicyEstimator& estimator)
{
    unsigned int highest_target = estimator.HighestTargetTracked(FeeEstimateHorizon::LONG_HALFLIFE);
    CFeeRate discard_rate = estimator.estimateSmartFee(highest_target, nullptr /* FeeCalculation */, false /* conservative */);
    // Don't let discard_rate be greater than longest possible fee estimate if we get a valid fee estimate
    discard_rate = (discard_rate == CFeeRate(0)) ? CWallet::m_discard_rate : std::min(discard_rate, CWallet::m_discard_rate);
    // Discard rate must be at least dustRelayFee
    discard_rate = std::max(discard_rate, ::dustRelayFee);
    return discard_rate;
}
