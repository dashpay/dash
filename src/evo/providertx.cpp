// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/providertx.h>

#include <evo/dmn_types.h>
#include <util/std23.h>

#include <chainparams.h>
#include <consensus/validation.h>
#include <deploymentstatus.h>
#include <hash.h>
#include <script/standard.h>
#include <tinyformat.h>
#include <validation.h>

#include <set>

namespace ProTxVersion {
template <typename T>
[[nodiscard]] uint16_t GetMaxFromDeployment(gsl::not_null<const CBlockIndex*> pindexPrev,
                                            const ChainstateManager& chainman, std::optional<bool> is_basic_override)
{
    constexpr bool is_extaddr_eligible{std::is_same_v<std::decay_t<T>, CProRegTx> || std::is_same_v<std::decay_t<T>, CProUpServTx> ||
        std::is_same_v<std::decay_t<T>, CProRegTx> || std::is_same_v<std::decay_t<T>, CProUpRegTx>};
    const bool is_v24_active{DeploymentActiveAfter(pindexPrev, chainman, Consensus::DEPLOYMENT_V24)};
    return ProTxVersion::GetMax(
        is_basic_override ? *is_basic_override
                          : DeploymentActiveAfter(pindexPrev, chainman.GetConsensus(), Consensus::DEPLOYMENT_V19),
        is_extaddr_eligible ? is_v24_active : false);
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

static bool IsValidPayoutScript(const CScript& script)
{
    return script.IsPayToPublicKeyHash() || script.IsPayToScriptHash();
}

bool IsPayoutListTriviallyValid(const MasternodePayoutShares& payouts, const CKeyID& keyIDOwner,
                                const CKeyID& keyIDVoting, TxValidationState& state)
{
    if (payouts.empty() || payouts.size() > 8) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payouts-count");
    }

    uint32_t total_reward{0};
    std::set<CScript> seen_scripts;
    for (const auto& payout : payouts) {
        if (payout.reward < MasternodePayoutShare::MIN_REWARD || payout.reward > MasternodePayoutShare::MAX_REWARD) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payout-reward");
        }
        total_reward += payout.reward;

        if (!IsValidPayoutScript(payout.scriptPayout)) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payee");
        }
        if (!seen_scripts.emplace(payout.scriptPayout).second) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payee-dup");
        }

        CTxDestination payout_dest;
        if (!ExtractDestination(payout.scriptPayout, payout_dest)) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payee-dest");
        }
        if ((!keyIDOwner.IsNull() && payout_dest == CTxDestination(PKHash(keyIDOwner))) ||
            payout_dest == CTxDestination(PKHash(keyIDVoting))) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payee-reuse");
        }
    }

    if (total_reward != MasternodePayoutShare::MAX_REWARD) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payout-reward-sum");
    }
    return true;
}

bool IsPayoutListKeySafe(const MasternodePayoutShares& payouts, const CTxDestination& collateral_dest,
                         const CKeyID& keyIDOwner, const CKeyID& keyIDVoting,
                         bool check_payout_collateral_reuse, TxValidationState& state)
{
    if (collateral_dest == CTxDestination(PKHash(keyIDOwner)) ||
        collateral_dest == CTxDestination(PKHash(keyIDVoting))) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-collateral-reuse");
    }

    if (check_payout_collateral_reuse) {
        for (const auto& payout : payouts) {
            CTxDestination payout_dest;
            if (ExtractDestination(payout.scriptPayout, payout_dest) && payout_dest == collateral_dest) {
                return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payee-reuse");
            }
        }
    }
    return true;
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
    const auto owner_payouts = GetOwnerPayouts(*this);
    if (!IsPayoutListTriviallyValid(owner_payouts, keyIDOwner, keyIDVoting, state)) return false;
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

    if (nOperatorReward > 10000) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-operator-reward");
    }

    return true;
}

std::string CProRegTx::MakeSignString() const
{
    std::string s;

    // We only include the important stuff in the string form...

    CTxDestination dest;
    const std::string strPayout = nVersion >= ProTxVersion::ExtAddr
        ? PayoutListToString(payouts)
        : (ExtractDestination(scriptPayout, dest) ? EncodeDestination(dest) : HexStr(scriptPayout));

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
    const std::string payee = PayoutListToString(GetOwnerPayouts(*this));

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
    if (!IsPayoutListTriviallyValid(GetOwnerPayouts(*this), CKeyID{}, keyIDVoting, state)) return false;
    return true;
}

std::string CProUpRegTx::ToString() const
{
    const std::string payee = PayoutListToString(GetOwnerPayouts(*this));

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
