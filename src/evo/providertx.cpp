// Copyright (c) 2018-2022 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/dmn_types.h>
#include <evo/providertx.h>

#include <chainparams.h>
#include <consensus/validation.h>
#include <hash.h>
#include <script/standard.h>
#include <tinyformat.h>
#include <util/underlying.h>

template <typename ProRegTx>
static bool TriviallyVerifyProRegPayees(const ProRegTx& proRegTx, TxValidationState& state)
{
    const std::vector<PayoutShare>& payoutShares = proRegTx.payoutShares;
    uint16_t totalPayoutReward{0};
    if (payoutShares.size() > 32 || payoutShares.empty()) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payee-size");
    }
    if (payoutShares.size() > 1 && proRegTx.nVersion < ProRegTx::MULTI_PAYOUT_VERSION) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payee-mismatch");
    }
    for (const auto& payoutShare : payoutShares) {
        CScript scriptPayout = payoutShare.scriptPayout;
        if (!scriptPayout.IsPayToPublicKeyHash() && !scriptPayout.IsPayToScriptHash()) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payee");
        }

        totalPayoutReward += payoutShare.payoutShareReward;
        if (payoutShare.payoutShareReward > 10000) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payee-reward");
        }
    }
    if (totalPayoutReward != 10000) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payee-reward-sum");
    }
    return true;
}

bool CProRegTx::IsTriviallyValid(bool is_basic_scheme_active, bool is_multi_payout_active, TxValidationState& state) const
{
    if (nVersion == 0 || nVersion > GetVersion(is_basic_scheme_active, is_multi_payout_active)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-version");
    }
    if (nVersion != BASIC_BLS_VERSION && nType == MnType::Evo) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-evo-version");
    }
    if (!IsValidMnType(nType)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-type");
    }
    if (nMode != 0) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-mode");
    }

    if (keyIDOwner.IsNull() || !pubKeyOperator.Get().IsValid() || keyIDVoting.IsNull()) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-key-null");
    }
    if (pubKeyOperator.IsLegacy() != (nVersion == LEGACY_BLS_VERSION)) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-operator-pubkey");
    }
    if (nOperatorReward > 10000) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-operator-reward");
    }
    if (!TriviallyVerifyProRegPayees<CProRegTx>(*this, state)) {
        // pass the state returned by the function above
        return false;
    }
    for (const auto& payoutShare : payoutShares) {
        CTxDestination payoutDest;
        if (!ExtractDestination(payoutShare.scriptPayout, payoutDest)) {
            // should not happen as we checked script types before
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payee-dest");
        }
        // don't allow reuse of payout key for other keys (don't allow people to put the payee key onto an online server)
        if (payoutDest == CTxDestination(PKHash(keyIDOwner)) || payoutDest == CTxDestination(PKHash(keyIDVoting))) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payee-reuse");
        }
    }
    return true;
}

std::string CProRegTx::MakeSignString() const
{
    std::string s;

    // We only include the important stuff in the string form...

    CTxDestination destPayout;
    std::string strPayout;
    for (const auto& payoutShare : payoutShares) {
        CScript scriptPayout = payoutShare.scriptPayout;
        if (ExtractDestination(scriptPayout, destPayout)) {
            strPayout = EncodeDestination(destPayout);
        } else {
            strPayout = HexStr(scriptPayout);
        }
        if (nVersion < MULTI_PAYOUT_VERSION) {
            s += strPayout + "|";
        } else {
            s += (strPayout + "|" + strprintf("%d", payoutShare.payoutShareReward) + "|");
        }
    }
    s += strprintf("%d", nOperatorReward) + "|";
    s += EncodeDestination(PKHash(keyIDOwner)) + "|";
    s += EncodeDestination(PKHash(keyIDVoting)) + "|";

    // ... and also the full hash of the payload as a protection against malleability and replays
    s += ::SerializeHash(*this).ToString();

    return s;
}

std::string CProRegTx::ToString() const
{
    std::string payoutSharesStr;
    for (const auto& payoutShare : payoutShares) {
        if (!payoutSharesStr.empty()) payoutSharesStr += ", ";
        payoutSharesStr += payoutShare.ToString();
    }

    return strprintf("CProRegTx(nVersion=%d, nType=%d, collateralOutpoint=%s, addr=%s, nOperatorReward=%f, ownerAddress=%s, pubKeyOperator=%s, votingAddress=%s, payoutShares=%s, platformNodeID=%s, platformP2PPort=%d, platformHTTPPort=%d)",
                     nVersion, ToUnderlying(nType), collateralOutpoint.ToStringShort(), addr.ToString(), (double)nOperatorReward / 100, EncodeDestination(PKHash(keyIDOwner)), pubKeyOperator.ToString(), EncodeDestination(PKHash(keyIDVoting)), payoutSharesStr, platformNodeID.ToString(), platformP2PPort, platformHTTPPort);
}

bool CProUpServTx::IsTriviallyValid(bool is_basic_scheme_active, bool is_multi_payout_active, TxValidationState& state) const
{
    if (nVersion == 0 || nVersion > GetVersion(is_basic_scheme_active)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-version");
    }
    if (nVersion != BASIC_BLS_VERSION && nType == MnType::Evo) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-evo-version");
    }

    return true;
}

std::string CProUpServTx::ToString() const
{
    CTxDestination dest;
    std::string payee = "unknown";
    if (ExtractDestination(scriptOperatorPayout, dest)) {
        payee = EncodeDestination(dest);
    }

    return strprintf("CProUpServTx(nVersion=%d, nType=%d, proTxHash=%s, addr=%s, operatorPayoutAddress=%s, platformNodeID=%s, platformP2PPort=%d, platformHTTPPort=%d)",
                     nVersion, ToUnderlying(nType), proTxHash.ToString(), addr.ToString(), payee, platformNodeID.ToString(), platformP2PPort, platformHTTPPort);
}

bool CProUpRegTx::IsTriviallyValid(bool is_basic_scheme_active, bool is_multi_payout_active, TxValidationState& state) const
{
    if (nVersion == 0 || nVersion > GetVersion(is_basic_scheme_active, is_multi_payout_active)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-version");
    }
    if (nMode != 0) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-mode");
    }

    if (!pubKeyOperator.Get().IsValid() || keyIDVoting.IsNull()) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-key-null");
    }
    if (pubKeyOperator.IsLegacy() != (nVersion == LEGACY_BLS_VERSION)) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-operator-pubkey");
    }
    return TriviallyVerifyProRegPayees<CProUpRegTx>(*this, state);
}

std::string CProUpRegTx::ToString() const
{
    std::string payoutSharesStr;
    for (const auto& payoutShare : payoutShares) {
        if (!payoutSharesStr.empty()) payoutSharesStr += ", ";
        payoutSharesStr += payoutShare.ToString();
    }

    return strprintf("CProUpRegTx(nVersion=%d, proTxHash=%s, pubKeyOperator=%s, votingAddress=%s, payoutShares=%s)",
        nVersion, proTxHash.ToString(), pubKeyOperator.ToString(), EncodeDestination(PKHash(keyIDVoting)), payoutSharesStr);
}

bool CProUpRevTx::IsTriviallyValid(bool is_basic_scheme_active, bool is_multi_payout_active, TxValidationState& state) const
{
    if (nVersion == 0 || nVersion > GetVersion(is_basic_scheme_active)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-version");
    }

    // nReason < CProUpRevTx::REASON_NOT_SPECIFIED is always `false` since
    // nReason is unsigned and CProUpRevTx::REASON_NOT_SPECIFIED == 0
    if (nReason > CProUpRevTx::REASON_LAST) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-reason");
    }
    return true;
}

std::string CProUpRevTx::ToString() const
{
    return strprintf("CProUpRevTx(nVersion=%d, proTxHash=%s, nReason=%d)",
        nVersion, proTxHash.ToString(), nReason);
}
