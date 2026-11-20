// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <protocol.h>

#include <common/system.h>

#include <algorithm>
#include <array>
#include <string_view>

// Compile-time check that every message type fits in CMessageHeader::COMMAND_SIZE.
static_assert(std::ranges::all_of(std::array{
    NetMsgType::VERSION,
    NetMsgType::VERACK,
    NetMsgType::ADDR,
    NetMsgType::ADDRV2,
    NetMsgType::SENDADDRV2,
    NetMsgType::INV,
    NetMsgType::GETDATA,
    NetMsgType::MERKLEBLOCK,
    NetMsgType::GETBLOCKS,
    NetMsgType::GETHEADERS,
    NetMsgType::TX,
    NetMsgType::HEADERS,
    NetMsgType::BLOCK,
    NetMsgType::GETADDR,
    NetMsgType::MEMPOOL,
    NetMsgType::PING,
    NetMsgType::PONG,
    NetMsgType::NOTFOUND,
    NetMsgType::FILTERLOAD,
    NetMsgType::FILTERADD,
    NetMsgType::FILTERCLEAR,
    NetMsgType::SENDHEADERS,
    NetMsgType::SENDCMPCT,
    NetMsgType::CMPCTBLOCK,
    NetMsgType::GETBLOCKTXN,
    NetMsgType::BLOCKTXN,
    NetMsgType::GETCFILTERS,
    NetMsgType::CFILTER,
    NetMsgType::GETCFHEADERS,
    NetMsgType::CFHEADERS,
    NetMsgType::GETCFCHECKPT,
    NetMsgType::CFCHECKPT,
    NetMsgType::SENDTXRCNCL,
    NetMsgType::SPORK,
    NetMsgType::GETSPORKS,
    NetMsgType::DSACCEPT,
    NetMsgType::DSVIN,
    NetMsgType::DSFINALTX,
    NetMsgType::DSSIGNFINALTX,
    NetMsgType::DSCOMPLETE,
    NetMsgType::DSSTATUSUPDATE,
    NetMsgType::DSTX,
    NetMsgType::DSQUEUE,
    NetMsgType::SENDDSQUEUE,
    NetMsgType::SYNCSTATUSCOUNT,
    NetMsgType::MNGOVERNANCESYNC,
    NetMsgType::MNGOVERNANCEOBJECT,
    NetMsgType::MNGOVERNANCEOBJECTVOTE,
    NetMsgType::GETMNLISTDIFF,
    NetMsgType::MNLISTDIFF,
    NetMsgType::QSENDRECSIGS,
    NetMsgType::QFCOMMITMENT,
    NetMsgType::QCONTRIB,
    NetMsgType::QCOMPLAINT,
    NetMsgType::QJUSTIFICATION,
    NetMsgType::QPCOMMITMENT,
    NetMsgType::QWATCH,
    NetMsgType::QSIGSESANN,
    NetMsgType::QSIGSHARESINV,
    NetMsgType::QGETSIGSHARES,
    NetMsgType::QBSIGSHARES,
    NetMsgType::QSIGREC,
    NetMsgType::QSIGSHARE,
    NetMsgType::QGETDATA,
    NetMsgType::QDATA,
    NetMsgType::CLSIG,
    NetMsgType::ISDLOCK,
    NetMsgType::MNAUTH,
    NetMsgType::GETHEADERS2,
    NetMsgType::SENDHEADERS2,
    NetMsgType::HEADERS2,
    NetMsgType::GETQUORUMROTATIONINFO,
    NetMsgType::QUORUMROTATIONINFO,
    NetMsgType::PLATFORMBAN,
}, [](std::string_view msg_type) { return msg_type.size() <= CMessageHeader::COMMAND_SIZE; }),
              "NetMsgType message types cannot be longer than COMMAND_SIZE");

/** Message types that are not allowed by blocks-relay-only policy.
 *  We do not want most of CoinJoin, DKG or LLMQ signing messages to be relayed
 *  to/from nodes via connections that were established in this mode.
 *  Make sure to keep this list up to date whenever a new message type is added.
 *  NOTE: Unlike ALL_NET_MESSAGE_TYPES in protocol.h, this list is sorted alphabetically.
 */
const static std::string netMessageTypesViolateBlocksOnly[] = {
    NetMsgType::DSACCEPT,
    NetMsgType::DSCOMPLETE,
    NetMsgType::DSFINALTX,
    NetMsgType::DSQUEUE,
    NetMsgType::DSSIGNFINALTX,
    NetMsgType::DSSTATUSUPDATE,
    NetMsgType::DSTX,
    NetMsgType::DSVIN,
    NetMsgType::GETQUORUMROTATIONINFO,
    NetMsgType::PLATFORMBAN,
    NetMsgType::QBSIGSHARES,
    NetMsgType::QCOMPLAINT,
    NetMsgType::QCONTRIB,
    NetMsgType::QDATA,
    NetMsgType::QGETDATA,
    NetMsgType::QGETSIGSHARES,
    NetMsgType::QJUSTIFICATION,
    NetMsgType::QPCOMMITMENT,
    NetMsgType::QSENDRECSIGS,
    NetMsgType::QSIGREC,
    NetMsgType::QSIGSESANN,
    NetMsgType::QSIGSHARE,
    NetMsgType::QSIGSHARESINV,
    NetMsgType::QUORUMROTATIONINFO,
    NetMsgType::QWATCH,
    NetMsgType::TX,
};
const static std::set<std::string> netMessageTypesViolateBlocksOnlySet(std::begin(netMessageTypesViolateBlocksOnly), std::end(netMessageTypesViolateBlocksOnly));

CMessageHeader::CMessageHeader(const MessageStartChars& pchMessageStartIn, const char* pszCommand, unsigned int nMessageSizeIn)
{
    memcpy(pchMessageStart, pchMessageStartIn, MESSAGE_START_SIZE);

    // Copy the command name
    size_t i = 0;
    for (; i < COMMAND_SIZE && pszCommand[i] != 0; ++i) pchCommand[i] = pszCommand[i];
    assert(pszCommand[i] == 0); // Assert that the command name passed in is not longer than COMMAND_SIZE

    nMessageSize = nMessageSizeIn;
}

std::string CMessageHeader::GetCommand() const
{
    return std::string(pchCommand, pchCommand + strnlen(pchCommand, COMMAND_SIZE));
}

bool CMessageHeader::IsCommandValid() const
{
    // Check the command string for errors
    for (const char* p1 = pchCommand; p1 < pchCommand + COMMAND_SIZE; ++p1) {
        if (*p1 == 0) {
            // Must be all zeros after the first zero
            for (; p1 < pchCommand + COMMAND_SIZE; ++p1) {
                if (*p1 != 0) {
                    return false;
                }
            }
        } else if (*p1 < ' ' || *p1 > 0x7E) {
            return false;
        }
    }

    return true;
}

CInv::CInv()
{
    type = 0;
    hash.SetNull();
}

CInv::CInv(uint32_t typeIn, const uint256& hashIn) : type(typeIn), hash(hashIn) {}

bool operator<(const CInv& a, const CInv& b)
{
    return (a.type < b.type || (a.type == b.type && a.hash < b.hash));
}

bool CInv::IsKnownType() const
{
    return GetCommandInternal() != nullptr;
}

const char* CInv::GetCommandInternal() const
{
    switch (type)
    {
        case MSG_TX:                            return NetMsgType::TX;
        case MSG_BLOCK:                         return NetMsgType::BLOCK;
        case MSG_FILTERED_BLOCK:                return NetMsgType::MERKLEBLOCK;
        case MSG_CMPCT_BLOCK:                   return NetMsgType::CMPCTBLOCK;
        case MSG_SPORK:                         return NetMsgType::SPORK;
        case MSG_DSTX:                          return NetMsgType::DSTX;
        case MSG_GOVERNANCE_OBJECT:             return NetMsgType::MNGOVERNANCEOBJECT;
        case MSG_GOVERNANCE_OBJECT_VOTE:        return NetMsgType::MNGOVERNANCEOBJECTVOTE;
        case MSG_QUORUM_FINAL_COMMITMENT:       return NetMsgType::QFCOMMITMENT;
        case MSG_QUORUM_CONTRIB:                return NetMsgType::QCONTRIB;
        case MSG_QUORUM_COMPLAINT:              return NetMsgType::QCOMPLAINT;
        case MSG_QUORUM_JUSTIFICATION:          return NetMsgType::QJUSTIFICATION;
        case MSG_QUORUM_PREMATURE_COMMITMENT:   return NetMsgType::QPCOMMITMENT;
        case MSG_QUORUM_RECOVERED_SIG:          return NetMsgType::QSIGREC;
        case MSG_CLSIG:                         return NetMsgType::CLSIG;
        case MSG_ISDLOCK:                       return NetMsgType::ISDLOCK;
        case MSG_DSQ:                           return NetMsgType::DSQUEUE;
        case MSG_PLATFORM_BAN:                  return NetMsgType::PLATFORMBAN;
        // Version 2 asset unlocks are announced by instance hash but carried in plain `tx`
        // messages; this also puts them in the blocks-only violation list, like MSG_TX
        case MSG_ASSET_UNLOCK:                  return NetMsgType::TX;
        default:
            return nullptr;
    }
}

std::string CInv::GetCommand() const
{
    auto cmd = GetCommandInternal();
    if (cmd == nullptr) {
        throw std::out_of_range(strprintf("CInv::GetCommand(): type=%d unknown type", type));
    }
    return cmd;
}

std::string CInv::ToString() const
{
    auto cmd = GetCommandInternal();
    if (!cmd) {
        return strprintf("0x%08x %s", type, hash.ToString());
    } else {
        return strprintf("%s %s", cmd, hash.ToString());
    }
}

bool NetMessageViolatesBlocksOnly(const std::string& msg_type)
{
    return netMessageTypesViolateBlocksOnlySet.find(msg_type) != netMessageTypesViolateBlocksOnlySet.end();
}

/**
 * Convert a service flag (NODE_*) to a human readable string.
 * It supports unknown service flags which will be returned as "UNKNOWN[...]".
 * @param[in] bit the service flag is calculated as (1 << bit)
 */
static std::string serviceFlagToStr(size_t bit)
{
    const uint64_t service_flag = 1ULL << bit;
    switch ((ServiceFlags)service_flag) {
    case NODE_NONE: abort();  // impossible
    case NODE_NETWORK:         return "NETWORK";
    case NODE_BLOOM:           return "BLOOM";
    case NODE_COMPACT_FILTERS: return "COMPACT_FILTERS";
    case NODE_NETWORK_LIMITED: return "NETWORK_LIMITED";
    case NODE_HEADERS_COMPRESSED: return "HEADERS_COMPRESSED";
    case NODE_P2P_V2:          return "P2P_V2";
    // Not using default, so we get warned when a case is missing
    }

    return strprintf("UNKNOWN[2^%u]", bit);
}

std::vector<std::string> serviceFlagsToStr(uint64_t flags)
{
    std::vector<std::string> str_flags;

    for (size_t i = 0; i < sizeof(flags) * 8; ++i) {
        if (flags & (1ULL << i)) {
            str_flags.emplace_back(serviceFlagToStr(i));
        }
    }

    return str_flags;
}
