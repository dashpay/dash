// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/providertx.h>

#include <evo/dmn_types.h>
#include <evo/sharedcollateral.h>
#include <util/std23.h>

#include <chainparams.h>
#include <consensus/validation.h>
#include <deploymentstatus.h>
#include <hash.h>
#include <script/standard.h>
#include <tinyformat.h>

#include <set>

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

bool IsShareListTriviallyValid(const CollateralShares& shares,
                               const std::vector<std::vector<unsigned char>>& join_sigs,
                               uint32_t early_period_blocks, CAmount early_penalty, CAmount required_collateral,
                               const CKeyID& keyIDVoting, TxValidationState& state)
{
    if (shares.size() < CProRegTx::MIN_SHARES || shares.size() > CProRegTx::MAX_SHARES) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-shares-count");
    }
    if (join_sigs.size() != shares.size()) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-shares-sig-count");
    }
    for (const auto& sig : join_sigs) {
        if (sig.size() != CPubKey::COMPACT_SIGNATURE_SIZE) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-shares-sig-size");
        }
    }
    if (early_period_blocks > CProRegTx::MAX_EARLY_PERIOD_BLOCKS) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-shares-early-period");
    }

    CAmount total_amount{0};
    CAmount min_amount{std::numeric_limits<CAmount>::max()};
    std::set<CKeyID> seen_owner_keys;
    std::set<CScript> seen_refund_scripts;
    for (const auto& share : shares) {
        // Bounding each amount by the required collateral first makes the sum overflow-safe
        if (share.amount < CCollateralShare::MIN_AMOUNT || share.amount > required_collateral) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-shares-amount");
        }
        total_amount += share.amount;
        min_amount = std::min(min_amount, share.amount);

        if (share.keyIDOwner.IsNull()) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-shares-key-null");
        }
        if (!seen_owner_keys.emplace(share.keyIDOwner).second) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-shares-dup-key");
        }
        if (!seen_refund_scripts.emplace(share.scriptRefund).second) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-shares-dup-refund");
        }

        for (const CScript* script : {&share.scriptRefund, &share.scriptReward}) {
            if (script == &share.scriptReward && script->empty()) {
                // An empty reward script means "use the refund script"
                continue;
            }
            if (sharedcollateral::IsSharedCollateralScript(*script)) {
                return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-shares-payee-template");
            }
            if (!IsValidPayoutScript(*script)) {
                return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-shares-payee");
            }
            CTxDestination dest;
            if (!ExtractDestination(*script, dest)) {
                return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-shares-payee-dest");
            }
            if (dest == CTxDestination(PKHash(keyIDVoting))) {
                return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-shares-payee-reuse");
            }
            for (const auto& other : shares) {
                if (dest == CTxDestination(PKHash(other.keyIDOwner))) {
                    return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-shares-payee-reuse");
                }
            }
        }
    }
    if (total_amount != required_collateral) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-shares-amount-sum");
    }
    if (early_penalty < 0 || early_penalty >= min_amount) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-shares-penalty");
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

static bool IsNetInfoTriviallyValid(const std::shared_ptr<NetInfoInterface>& net_info, MnType type, TxValidationState& state)
{
    if (!net_info->HasEntries(NetInfoPurpose::CORE_P2P)) {
        // Mandatory for all nodes
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-netinfo-empty");
    }
    if (type == MnType::Regular) {
        // Regular nodes shouldn't populate Platform-specific fields
        if (net_info->HasEntries(NetInfoPurpose::PLATFORM_HTTPS) || net_info->HasEntries(NetInfoPurpose::PLATFORM_P2P)) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-netinfo-bad");
        }
    }
    if (net_info->CanStorePlatform() && type == MnType::Evo) {
        // Platform fields are mandatory for EvoNodes
        if (!net_info->HasEntries(NetInfoPurpose::PLATFORM_HTTPS) || !net_info->HasEntries(NetInfoPurpose::PLATFORM_P2P)) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-netinfo-empty");
        }
    }
    return true;
}

static bool CheckNetInfo(const NetInfoInterface& net_info, TxValidationState& state)
{
    switch (net_info.Validate()) {
    case NetInfoStatus::BadAddress:
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-netinfo-addr");
    case NetInfoStatus::BadPort:
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-netinfo-port");
    case NetInfoStatus::BadType:
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-netinfo-addr-type");
    case NetInfoStatus::NotRoutable:
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-netinfo-addr-unroutable");
    case NetInfoStatus::Malformed:
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-netinfo-bad");
    case NetInfoStatus::Success:
        return true;
    case NetInfoStatus::BadInput:
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-netinfo-entry");
    case NetInfoStatus::Duplicate:
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-dup-netinfo-entry");
    case NetInfoStatus::MaxLimit:
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-netinfo-maxlimit");
    }
    assert(false);
}

bool CheckProviderNetworkFields(const std::shared_ptr<NetInfoInterface>& net_info, MnType type, uint16_t version,
                                const uint160* platform_node_id, uint16_t platform_p2p_port,
                                uint16_t platform_http_port, bool allow_empty, TxValidationState& state)
{
    if (!net_info || net_info->CanStorePlatform() != (version >= ProTxVersion::ExtAddr)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-netinfo-version");
    }
    if (net_info->IsEmpty()) {
        if (!allow_empty) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-netinfo-empty");
        }
    } else {
        if (!IsNetInfoTriviallyValid(net_info, type, state) || !CheckNetInfo(*net_info, state)) {
            return false;
        }
    }

    if (type != MnType::Evo) return true;
    if (platform_node_id && platform_node_id->IsNull()) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-platform-nodeid");
    }
    if (version >= ProTxVersion::ExtAddr) {
        if (platform_p2p_port != 0) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-platform-p2p-port");
        }
        if (platform_http_port != 0) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-platform-http-port");
        }
        return true;
    }

    if (::IsNodeOnMainnet()) {
        if (platform_p2p_port != ::MainParams().GetDefaultPlatformP2PPort()) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-platform-p2p-port");
        }
        if (platform_http_port != ::MainParams().GetDefaultPlatformHTTPPort()) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-platform-http-port");
        }
    }
    if (platform_p2p_port == ::MainParams().GetDefaultPort()) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-platform-p2p-port");
    }
    if (platform_http_port == ::MainParams().GetDefaultPort()) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-platform-http-port");
    }

    const uint16_t core_port{net_info->GetPrimary().GetPort()};
    if (platform_p2p_port == platform_http_port || platform_p2p_port == core_port || platform_http_port == core_port) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-platform-dup-ports");
    }
    return true;
}

bool CProRegTx::IsTriviallyValid(TxValidationState& state) const
{
    if (nVersion == 0 || nVersion > ProTxVersion::ExtAddr) {
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

    if (IsShared()) {
        if (nVersion < ProTxVersion::ExtAddr) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-version");
        }
        if (nType != MnType::Regular) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-shares-evo");
        }
        // The collateral must be internal; the funding inputs and outputs are covered by the consent digest
        if (!collateralOutpoint.hash.IsNull()) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-shares-external");
        }
        // The share owner keys replace the owner key; owner rewards derive from the share table
        if (!keyIDOwner.IsNull()) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-shares-owner-key");
        }
        if (!payouts.empty()) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-shares-payouts");
        }
    } else {
        if (!vchJoinSigs.empty() || nEarlyPeriodBlocks != 0 || nEarlyPenalty != 0) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-shares-empty-fields");
        }
        if (keyIDOwner.IsNull()) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-key-null");
        }
    }
    if (!pubKeyOperator.Get().IsValid() || keyIDVoting.IsNull()) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-key-null");
    }
    if (pubKeyOperator.IsLegacy() != (nVersion == ProTxVersion::LegacyBLS)) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-operator-pubkey");
    }
    if (IsShared()) {
        if (!IsShareListTriviallyValid(shares, vchJoinSigs, nEarlyPeriodBlocks, nEarlyPenalty,
                                       GetMnType(nType).collat_amount, keyIDVoting, state)) {
            return false;
        }
    } else {
        const auto owner_payouts = GetOwnerPayouts(*this);
        if (!IsPayoutListTriviallyValid(owner_payouts, keyIDOwner, keyIDVoting, state)) return false;
    }
    if (netInfo->CanStorePlatform() != (nVersion >= ProTxVersion::ExtAddr)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-netinfo-version");
    }
    if (!netInfo->IsEmpty() && !IsNetInfoTriviallyValid(netInfo, nType, state)) {
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
    const std::string payee = IsShared() ? strprintf("shares(%s), earlyPeriodBlocks=%d, earlyPenalty=%d",
                                                     ShareListToString(shares), nEarlyPeriodBlocks, nEarlyPenalty)
                                         : PayoutListToString(GetOwnerPayouts(*this));

    return strprintf("CProRegTx(nVersion=%d, nType=%d, collateralOutpoint=%s, netInfo=%s, nOperatorReward=%f, "
                     "ownerAddress=%s, pubKeyOperator=%s, votingAddress=%s, scriptPayout=%s, platformNodeID=%s%s)\n",
                     nVersion, std23::to_underlying(nType), collateralOutpoint.ToStringShort(), netInfo->ToString(),
                     static_cast<double>(nOperatorReward) / 100, EncodeDestination(PKHash(keyIDOwner)), pubKeyOperator.ToString(),
                     EncodeDestination(PKHash(keyIDVoting)), payee, platformNodeID.ToString(),
                     (nVersion >= ProTxVersion::ExtAddr
                          ? ""
                          : strprintf(", platformP2PPort=%d, platformHTTPPort=%d", platformP2PPort, platformHTTPPort)));
}

bool CProUpServTx::IsTriviallyValid(TxValidationState& state) const
{
    if (nVersion == 0 || nVersion > ProTxVersion::ExtAddr) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-version");
    }
    if (nVersion < ProTxVersion::BasicBLS && nType == MnType::Evo) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-evo-version");
    }
    if (!IsValidMnType(nType)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-type");
    }
    if (netInfo->CanStorePlatform() != (nVersion >= ProTxVersion::ExtAddr)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-netinfo-version");
    }
    if (netInfo->IsEmpty()) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-netinfo-empty");
    }
    if (!IsNetInfoTriviallyValid(netInfo, nType, state)) {
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

bool CProUpRegTx::IsTriviallyValid(TxValidationState& state) const
{
    if (nVersion == 0 || nVersion > ProTxVersion::ExtAddr) {
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

bool CProUpRevTx::IsTriviallyValid(TxValidationState& state) const
{
    if (nVersion == 0 || nVersion > ProTxVersion::ExtAddr) {
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
