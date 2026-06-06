// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/providertx.h>

#include <evo/dmn_types.h>
#include <util/std23.h>

#include <set>

#include <chainparams.h>
#include <consensus/validation.h>
#include <deploymentstatus.h>
#include <hash.h>
#include <script/standard.h>
#include <tinyformat.h>
#include <validation.h>

namespace ProTxVersion {
template <typename T>
[[nodiscard]] uint16_t GetMaxFromDeployment(gsl::not_null<const CBlockIndex*> pindexPrev,
                                            const ChainstateManager& chainman, std::optional<bool> is_basic_override)
{
    constexpr bool is_extaddr_eligible{std::is_same_v<std::decay_t<T>, CProRegTx> || std::is_same_v<std::decay_t<T>, CProUpServTx>};
    // DIP0026 multi-party payouts apply to the owner-side payout, which is only carried by
    // ProRegTx and ProUpRegTx. ProUpServTx (operator payout) and ProUpRevTx are not eligible.
    constexpr bool is_multipayout_eligible{std::is_same_v<std::decay_t<T>, CProRegTx> || std::is_same_v<std::decay_t<T>, CProUpRegTx>};

    const bool is_basic{is_basic_override ? *is_basic_override
                                          : DeploymentActiveAfter(pindexPrev, chainman.GetConsensus(), Consensus::DEPLOYMENT_V19)};
    const bool is_extaddr{is_extaddr_eligible && DeploymentActiveAfter(pindexPrev, chainman, Consensus::DEPLOYMENT_V24)};
    bool is_multipayout{is_multipayout_eligible && DeploymentActiveAfter(pindexPrev, chainman, Consensus::DEPLOYMENT_V25)};

    // A v4 CProRegTx (MultiPayout > ExtAddr) implies extended-address netInfo, so multi-payout
    // must never outrun the extended-address fork for an extaddr-eligible type. We enforce this
    // in code rather than relying on chainparams ordering of V24/V25. CProUpRegTx carries no
    // netInfo and is not extaddr-eligible, so it may reach v4 on DEPLOYMENT_V25 alone.
    if (is_extaddr_eligible && is_multipayout && !is_extaddr) {
        is_multipayout = false;
    }
    return ProTxVersion::GetMax(is_basic, is_extaddr, is_multipayout);
}
template uint16_t GetMaxFromDeployment<CProRegTx>(gsl::not_null<const CBlockIndex*> pindexPrev,
                                                  const ChainstateManager& chainman,
                                                  std::optional<bool> is_basic_override);
template uint16_t GetMaxFromDeployment<CProUpServTx>(gsl::not_null<const CBlockIndex*> pindexPrev,
                                                     const ChainstateManager& chainman,
                                                     std::optional<bool> is_basic_override);
template uint16_t GetMaxFromDeployment<CProUpRegTx>(gsl::not_null<const CBlockIndex*> pindexPrev,
                                                    const ChainstateManager& chainman,
                                                    std::optional<bool> is_basic_override);
template uint16_t GetMaxFromDeployment<CProUpRevTx>(gsl::not_null<const CBlockIndex*> pindexPrev,
                                                    const ChainstateManager& chainman,
                                                    std::optional<bool> is_basic_override);
} // namespace ProTxVersion

std::string PayoutShare::ToString() const
{
    CTxDestination dest;
    const std::string payee{ExtractDestination(scriptPayout, dest) ? EncodeDestination(dest)
                                                                   : HexStr(scriptPayout)};
    return strprintf("%s:%d", payee, payoutShareReward);
}

template <typename ProTx>
bool IsNetInfoTriviallyValid(const ProTx& proTx, TxValidationState& state)
{
    if (!proTx.netInfo->HasEntries(NetInfoPurpose::CORE_P2P)) {
        // Mandatory for all nodes
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-netinfo-empty");
    }
    if (proTx.nType == MnType::Regular) {
        // Regular nodes shouldn't populate Platform-specific fields
        if (proTx.netInfo->HasEntries(NetInfoPurpose::PLATFORM_HTTPS) ||
            proTx.netInfo->HasEntries(NetInfoPurpose::PLATFORM_P2P)) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-netinfo-bad");
        }
    }
    if (proTx.netInfo->CanStorePlatform() && proTx.nType == MnType::Evo) {
        // Platform fields are mandatory for EvoNodes
        if (!proTx.netInfo->HasEntries(NetInfoPurpose::PLATFORM_HTTPS) ||
            !proTx.netInfo->HasEntries(NetInfoPurpose::PLATFORM_P2P)) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-netinfo-empty");
        }
    }
    return true;
}

// DIP0026: validate the owner-side payout for a ProRegTx/ProUpRegTx. For nVersion < MultiPayout
// this is the single scriptPayout (must be p2pkh or p2sh). For nVersion >= MultiPayout it is the
// payoutShares set, which must be non-empty and at most MAX_PAYOUT_SHARES, each share a p2pkh/p2sh
// payee with a nonzero reward not exceeding TOTAL_BASIS_POINTS, with no duplicate scripts, summing
// to exactly TOTAL_BASIS_POINTS. (Uniqueness and the nonzero-reward rule are stricter than the
// three DIP0026 conditions; see the implementation notes / companion dips PR.)
bool CheckPayoutShares(uint16_t nVersion, const CScript& scriptPayout,
                       const std::vector<PayoutShare>& payoutShares, TxValidationState& state)
{
    if (nVersion < ProTxVersion::MultiPayout) {
        // Pre-v4 carries a single scriptPayout and no shares; reject any cross-version mix.
        if (!payoutShares.empty()) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payout-shares-unexpected");
        }
        if (!scriptPayout.IsPayToPublicKeyHash() && !scriptPayout.IsPayToScriptHash()) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payee");
        }
        return true;
    }
    // v4 carries the shares and no single scriptPayout.
    if (!scriptPayout.empty()) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payout-script-unexpected");
    }
    if (payoutShares.empty() || payoutShares.size() > PayoutShare::MAX_PAYOUT_SHARES) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payout-shares-count");
    }
    int64_t total{0};
    std::set<CScript> seen;
    for (const auto& share : payoutShares) {
        if (!share.scriptPayout.IsPayToPublicKeyHash() && !share.scriptPayout.IsPayToScriptHash()) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payee");
        }
        if (share.payoutShareReward == 0 || share.payoutShareReward > PayoutShare::TOTAL_BASIS_POINTS) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payout-share-reward");
        }
        if (!seen.insert(share.scriptPayout).second) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payout-share-duplicate");
        }
        total += share.payoutShareReward;
    }
    if (total != PayoutShare::TOTAL_BASIS_POINTS) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payout-shares-sum");
    }
    return true;
}

bool CProRegTx::IsTriviallyValid(gsl::not_null<const CBlockIndex*> pindexPrev, const ChainstateManager& chainman,
                                 TxValidationState& state) const
{
    if (nVersion == 0 || nVersion > ProTxVersion::GetMaxFromDeployment<decltype(*this)>(pindexPrev, chainman)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-version");
    }
    if (nVersion < ProTxVersion::BasicBLS && nType == MnType::Evo) {
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
    if (pubKeyOperator.IsLegacy() != (nVersion == ProTxVersion::LegacyBLS)) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-operator-pubkey");
    }
    if (!CheckPayoutShares(nVersion, scriptPayout, payoutShares, state)) {
        // pass the state returned by the helper above
        return false;
    }
    if (netInfo->CanStorePlatform() != (nVersion >= ProTxVersion::ExtAddr)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-netinfo-version");
    }
    if (!netInfo->IsEmpty() && !IsNetInfoTriviallyValid(*this, state)) {
        // pass the state returned by the function above
        return false;
    }
    for (const auto& entry : netInfo->GetEntries()) {
        if (!entry.IsTriviallyValid()) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-netinfo-bad");
        }
    }

    // don't allow reuse of a payout key for the owner/voting keys (don't allow people to put the
    // payee key onto an online server). For v4 this applies to every payout share.
    for (const auto& share : GetPayoutShares()) {
        CTxDestination payoutDest;
        if (!ExtractDestination(share.scriptPayout, payoutDest)) {
            // should not happen as we checked script types before
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payee-dest");
        }
        if (payoutDest == CTxDestination(PKHash(keyIDOwner)) || payoutDest == CTxDestination(PKHash(keyIDVoting))) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payee-reuse");
        }
    }

    if (nOperatorReward > 10000) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-operator-reward");
    }

    return true;
}

std::string CProRegTx::MakeSignString() const
{
    std::string s;

    // We only include the important stuff in the string form...

    std::string strPayout;
    if (nVersion >= ProTxVersion::MultiPayout) {
        // DIP0026: payoutSharesStr = address(share0)|reward0|...|address(shareN)|rewardN
        for (size_t i = 0; i < payoutShares.size(); ++i) {
            if (i > 0) strPayout += "|";
            CTxDestination dest;
            const std::string addr{ExtractDestination(payoutShares[i].scriptPayout, dest)
                                       ? EncodeDestination(dest)
                                       : HexStr(payoutShares[i].scriptPayout)};
            strPayout += addr + "|" + strprintf("%d", payoutShares[i].payoutShareReward);
        }
    } else {
        CTxDestination destPayout;
        strPayout = ExtractDestination(scriptPayout, destPayout) ? EncodeDestination(destPayout)
                                                                 : HexStr(scriptPayout);
    }

    s += strPayout + "|";
    s += strprintf("%d", nOperatorReward) + "|";
    s += EncodeDestination(PKHash(keyIDOwner)) + "|";
    s += EncodeDestination(PKHash(keyIDVoting)) + "|";

    // ... and also the full hash of the payload as a protection against malleability and replays
    s += ::SerializeHash(*this).ToString();

    return s;
}

std::string CProRegTx::ToString() const
{
    CTxDestination dest;
    std::string payee = "unknown";
    if (ExtractDestination(scriptPayout, dest)) {
        payee = EncodeDestination(dest);
    }

    return strprintf("CProRegTx(nVersion=%d, nType=%d, collateralOutpoint=%s, netInfo=%s, nOperatorReward=%f, "
                     "ownerAddress=%s, pubKeyOperator=%s, votingAddress=%s, scriptPayout=%s, platformNodeID=%s%s)\n",
                     nVersion, std23::to_underlying(nType), collateralOutpoint.ToStringShort(), netInfo->ToString(),
                     (double)nOperatorReward / 100, EncodeDestination(PKHash(keyIDOwner)), pubKeyOperator.ToString(),
                     EncodeDestination(PKHash(keyIDVoting)), payee, platformNodeID.ToString(),
                     (nVersion >= ProTxVersion::ExtAddr
                          ? ""
                          : strprintf(", platformP2PPort=%d, platformHTTPPort=%d", platformP2PPort, platformHTTPPort)));
}

bool CProUpServTx::IsTriviallyValid(gsl::not_null<const CBlockIndex*> pindexPrev, const ChainstateManager& chainman,
                                    TxValidationState& state) const
{
    if (nVersion == 0 || nVersion > ProTxVersion::GetMaxFromDeployment<decltype(*this)>(pindexPrev, chainman)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-version");
    }
    if (nVersion < ProTxVersion::BasicBLS && nType == MnType::Evo) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-evo-version");
    }
    if (netInfo->CanStorePlatform() != (nVersion >= ProTxVersion::ExtAddr)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-netinfo-version");
    }
    if (netInfo->IsEmpty()) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-netinfo-empty");
    }
    if (!IsNetInfoTriviallyValid(*this, state)) {
        // pass the state returned by the function above
        return false;
    }
    for (const auto& entry : netInfo->GetEntries()) {
        if (!entry.IsTriviallyValid()) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-netinfo-bad");
        }
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

    return strprintf("CProUpServTx(nVersion=%d, nType=%d, proTxHash=%s, netInfo=%s, operatorPayoutAddress=%s, "
                     "platformNodeID=%s%s)\n",
                     nVersion, std23::to_underlying(nType), proTxHash.ToString(), netInfo->ToString(), payee,
                     platformNodeID.ToString(),
                     (nVersion >= ProTxVersion::ExtAddr
                          ? ""
                          : strprintf(", platformP2PPort=%d, platformHTTPPort=%d", platformP2PPort, platformHTTPPort)));
}

bool CProUpRegTx::IsTriviallyValid(gsl::not_null<const CBlockIndex*> pindexPrev, const ChainstateManager& chainman,
                                   TxValidationState& state) const
{
    if (nVersion == 0 || nVersion > ProTxVersion::GetMaxFromDeployment<decltype(*this)>(pindexPrev, chainman)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-version");
    }
    if (nMode != 0) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-mode");
    }

    if (!pubKeyOperator.Get().IsValid() || keyIDVoting.IsNull()) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-key-null");
    }
    if (pubKeyOperator.IsLegacy() != (nVersion == ProTxVersion::LegacyBLS)) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-operator-pubkey");
    }
    if (!CheckPayoutShares(nVersion, scriptPayout, payoutShares, state)) {
        // pass the state returned by the helper above
        return false;
    }
    return true;
}

std::string CProUpRegTx::ToString() const
{
    CTxDestination dest;
    std::string payee = "unknown";
    if (ExtractDestination(scriptPayout, dest)) {
        payee = EncodeDestination(dest);
    }

    return strprintf("CProUpRegTx(nVersion=%d, proTxHash=%s, pubKeyOperator=%s, votingAddress=%s, payoutAddress=%s)",
        nVersion, proTxHash.ToString(), pubKeyOperator.ToString(), EncodeDestination(PKHash(keyIDVoting)), payee);
}

bool CProUpRevTx::IsTriviallyValid(gsl::not_null<const CBlockIndex*> pindexPrev, const ChainstateManager& chainman,
                                   TxValidationState& state) const
{
    if (nVersion == 0 || nVersion > ProTxVersion::GetMaxFromDeployment<decltype(*this)>(pindexPrev, chainman)) {
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
