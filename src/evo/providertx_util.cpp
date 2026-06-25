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
    return {{script_payout, CMasternodePayoutShare::MAX_REWARD}};
}

MasternodePayoutShares GetOwnerPayouts(const uint16_t nVersion, const CScript& script_payout,
                                       const MasternodePayoutShares& payouts)
{
    return nVersion >= ProTxVersion::MultiPayout ? payouts : LegacyPayoutAsList(script_payout);
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
