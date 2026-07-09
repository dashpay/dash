// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/providertx.h>

#include <key_io.h>
#include <script/standard.h>
#include <tinyformat.h>
#include <univalue.h>
#include <util/strencodings.h>

// Owner payout list helpers that operate purely on the serialized representation. They live in
// libbitcoin_common (rather than libbitcoin_node alongside the rest of providertx.cpp) so that
// common-layer consumers like the bloom filter and the JSON writers can use them without pulling
// in node-only dependencies.

MasternodePayoutShares LegacyPayoutAsList(const CScript& script_payout)
{
    return {{script_payout, MasternodePayoutShare::MAX_REWARD}};
}

std::string PayoutListToString(const MasternodePayoutShares& payouts)
{
    std::string ret;
    for (const auto& payout : payouts) {
        CTxDestination dest;
        const std::string payout_str = ExtractDestination(payout.scriptPayout, dest) ? EncodeDestination(dest) : HexStr(payout.scriptPayout);
        if (!ret.empty()) ret += ",";
        ret += strprintf("%s:%d", payout_str, payout.reward);
    }
    return ret;
}

UniValue PayoutListToJson(const MasternodePayoutShares& payouts)
{
    UniValue ret(UniValue::VARR);
    for (const auto& payout : payouts) {
        UniValue obj(UniValue::VOBJ);
        // Payout scripts are required to be P2PKH or P2SH (see IsPayoutListTriviallyValid), so a
        // destination can always be extracted and the address is always present.
        CTxDestination dest;
        ExtractDestination(payout.scriptPayout, dest);
        obj.pushKV("address", EncodeDestination(dest));
        obj.pushKV("script", HexStr(payout.scriptPayout));
        obj.pushKV("reward", payout.reward);
        ret.push_back(obj);
    }
    return ret;
}

std::string ShareListToString(const CollateralShares& shares)
{
    std::string ret;
    for (const auto& share : shares) {
        CTxDestination dest;
        const std::string refund_str = ExtractDestination(share.scriptRefund, dest) ? EncodeDestination(dest)
                                                                                    : HexStr(share.scriptRefund);
        if (!ret.empty()) ret += ",";
        ret += strprintf("%s:%d", refund_str, share.amount);
    }
    return ret;
}

UniValue ShareListToJson(const CollateralShares& shares)
{
    UniValue ret(UniValue::VARR);
    for (const auto& share : shares) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("amount", share.amount);
        // Refund and reward scripts are required to be P2PKH or P2SH (see IsShareListTriviallyValid),
        // so a destination can always be extracted and the addresses are always present.
        CTxDestination refund_dest;
        ExtractDestination(share.scriptRefund, refund_dest);
        obj.pushKV("refundAddress", EncodeDestination(refund_dest));
        obj.pushKV("refundScript", HexStr(share.scriptRefund));
        CTxDestination reward_dest;
        ExtractDestination(share.RewardScript(), reward_dest);
        obj.pushKV("rewardAddress", EncodeDestination(reward_dest));
        obj.pushKV("rewardScript", HexStr(share.RewardScript()));
        obj.pushKV("ownerAddress", EncodeDestination(PKHash(share.keyIDOwner)));
        ret.push_back(obj);
    }
    return ret;
}

