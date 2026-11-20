// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PROTOCOL_H
#define BITCOIN_PROTOCOL_H

#include <netaddress.h>
#include <serialize.h>
#include <streams.h>
#include <uint256.h>
#include <util/time.h>

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <variant>

/** Message header.
 * (4) message start.
 * (12) command.
 * (4) size.
 * (4) checksum.
 */
class CMessageHeader
{
public:
    static constexpr size_t MESSAGE_START_SIZE = 4;
    static constexpr size_t COMMAND_SIZE = 12;
    static constexpr size_t MESSAGE_SIZE_SIZE = 4;
    static constexpr size_t CHECKSUM_SIZE = 4;
    static constexpr size_t MESSAGE_SIZE_OFFSET = MESSAGE_START_SIZE + COMMAND_SIZE;
    static constexpr size_t CHECKSUM_OFFSET = MESSAGE_SIZE_OFFSET + MESSAGE_SIZE_SIZE;
    static constexpr size_t HEADER_SIZE = MESSAGE_START_SIZE + COMMAND_SIZE + MESSAGE_SIZE_SIZE + CHECKSUM_SIZE;
    typedef unsigned char MessageStartChars[MESSAGE_START_SIZE];

    explicit CMessageHeader() = default;

    /** Construct a P2P message header from message-start characters, a command and the size of the message.
     * @note Passing in a `pszCommand` longer than COMMAND_SIZE will result in a run-time assertion error.
     */
    CMessageHeader(const MessageStartChars& pchMessageStartIn, const char* pszCommand, unsigned int nMessageSizeIn);

    std::string GetCommand() const;
    bool IsCommandValid() const;

    SERIALIZE_METHODS(CMessageHeader, obj) { READWRITE(obj.pchMessageStart, obj.pchCommand, obj.nMessageSize, obj.pchChecksum); }

    char pchMessageStart[MESSAGE_START_SIZE]{};
    char pchCommand[COMMAND_SIZE]{};
    uint32_t nMessageSize{std::numeric_limits<uint32_t>::max()};
    uint8_t pchChecksum[CHECKSUM_SIZE]{};
};

/**
 * Bitcoin protocol message types. When adding new message types, don't forget
 * to update ALL_NET_MESSAGE_TYPES below.
 */
namespace NetMsgType {
/**
 * The version message provides information about the transmitting node to the
 * receiving node at the beginning of a connection.
 */
inline constexpr const char* VERSION{"version"};
/**
 * The verack message acknowledges a previously-received version message,
 * informing the connecting node that it can begin to send other messages.
 */
inline constexpr const char* VERACK{"verack"};
/**
 * The addr (IP address) message relays connection information for peers on the
 * network.
 */
inline constexpr const char* ADDR{"addr"};
/**
 * The addrv2 message relays connection information for peers on the network just
 * like the addr message, but is extended to allow gossiping of longer node
 * addresses (see BIP155).
 */
inline constexpr const char* ADDRV2{"addrv2"};
/**
 * The sendaddrv2 message signals support for receiving ADDRV2 messages (BIP155).
 * It also implies that its sender can encode as ADDRV2 and would send ADDRV2
 * instead of ADDR to a peer that has signaled ADDRV2 support by sending SENDADDRV2.
 */
inline constexpr const char* SENDADDRV2{"sendaddrv2"};
/**
 * The inv message (inventory message) transmits one or more inventories of
 * objects known to the transmitting peer.
 */
inline constexpr const char* INV{"inv"};
/**
 * The getdata message requests one or more data objects from another node.
 */
inline constexpr const char* GETDATA{"getdata"};
/**
 * The merkleblock message is a reply to a getdata message which requested a
 * block using the inventory type MSG_MERKLEBLOCK.
 * @since protocol version 70001 as described by BIP37.
 */
inline constexpr const char* MERKLEBLOCK{"merkleblock"};
/**
 * The getblocks message requests an inv message that provides block header
 * hashes starting from a particular point in the block chain.
 */
inline constexpr const char* GETBLOCKS{"getblocks"};
/**
 * The getheaders message requests a headers message that provides block
 * headers starting from a particular point in the block chain.
 * @since protocol version 31800.
 */
inline constexpr const char* GETHEADERS{"getheaders"};
/**
 * The tx message transmits a single transaction.
 */
inline constexpr const char* TX{"tx"};
/**
 * The headers message sends one or more block headers to a node which
 * previously requested certain headers with a getheaders message.
 * @since protocol version 31800.
 */
inline constexpr const char* HEADERS{"headers"};
/**
 * The block message transmits a single serialized block.
 */
inline constexpr const char* BLOCK{"block"};
/**
 * The getaddr message requests an addr message from the receiving node,
 * preferably one with lots of IP addresses of other receiving nodes.
 */
inline constexpr const char* GETADDR{"getaddr"};
/**
 * The mempool message requests the TXIDs of transactions that the receiving
 * node has verified as valid but which have not yet appeared in a block.
 * @since protocol version 60002 as described by BIP35.
 *   Only available with service bit NODE_BLOOM, see also BIP111.
 */
inline constexpr const char* MEMPOOL{"mempool"};
/**
 * The ping message is sent periodically to help confirm that the receiving
 * peer is still connected.
 */
inline constexpr const char* PING{"ping"};
/**
 * The pong message replies to a ping message, proving to the pinging node that
 * the ponging node is still alive.
 * @since protocol version 60001 as described by BIP31.
 */
inline constexpr const char* PONG{"pong"};
/**
 * The notfound message is a reply to a getdata message which requested an
 * object the receiving node does not have available for relay.
 * @since protocol version 70001.
 */
inline constexpr const char* NOTFOUND{"notfound"};
/**
 * The filterload message tells the receiving peer to filter all relayed
 * transactions and requested merkle blocks through the provided filter.
 * @since protocol version 70001 as described by BIP37.
 *   Only available with service bit NODE_BLOOM since protocol version
 *   70011 as described by BIP111.
 */
inline constexpr const char* FILTERLOAD{"filterload"};
/**
 * The filteradd message tells the receiving peer to add a single element to a
 * previously-set bloom filter, such as a new public key.
 * @since protocol version 70001 as described by BIP37.
 *   Only available with service bit NODE_BLOOM since protocol version
 *   70011 as described by BIP111.
 */
inline constexpr const char* FILTERADD{"filteradd"};
/**
 * The filterclear message tells the receiving peer to remove a previously-set
 * bloom filter.
 * @since protocol version 70001 as described by BIP37.
 *   Only available with service bit NODE_BLOOM since protocol version
 *   70011 as described by BIP111.
 */
inline constexpr const char* FILTERCLEAR{"filterclear"};
/**
 * Indicates that a node prefers to receive new block announcements via a
 * "headers" message rather than an "inv".
 * @since protocol version 70012 as described by BIP130.
 */
inline constexpr const char* SENDHEADERS{"sendheaders"};

/**
 * Contains a 1-byte bool and 8-byte LE version number.
 * Indicates that a node is willing to provide blocks via "cmpctblock" messages.
 * May indicate that a node prefers to receive new block announcements via a
 * "cmpctblock" message rather than an "inv", depending on message contents.
 * @since protocol version 70209 as described by BIP 152
 */
inline constexpr const char* SENDCMPCT{"sendcmpct"};
/**
 * Contains a CBlockHeaderAndShortTxIDs object - providing a header and
 * list of "short txids".
 * @since protocol version 70209 as described by BIP 152
 */
inline constexpr const char* CMPCTBLOCK{"cmpctblock"};
/**
 * Contains a BlockTransactionsRequest
 * Peer should respond with "blocktxn" message.
 * @since protocol version 70209 as described by BIP 152
 */
inline constexpr const char* GETBLOCKTXN{"getblocktxn"};
/**
 * Contains a BlockTransactions.
 * Sent in response to a "getblocktxn" message.
 * @since protocol version 70209 as described by BIP 152
 */
inline constexpr const char* BLOCKTXN{"blocktxn"};
/**
 * getcfilters requests compact filters for a range of blocks.
 * Only available with service bit NODE_COMPACT_FILTERS as described by
 * BIP 157 & 158.
 */
inline constexpr const char* GETCFILTERS{"getcfilters"};
/**
 * cfilter is a response to a getcfilters request containing a single compact
 * filter.
 */
inline constexpr const char* CFILTER{"cfilter"};
/**
 * getcfheaders requests a compact filter header and the filter hashes for a
 * range of blocks, which can then be used to reconstruct the filter headers
 * for those blocks.
 * Only available with service bit NODE_COMPACT_FILTERS as described by
 * BIP 157 & 158.
 */
inline constexpr const char* GETCFHEADERS{"getcfheaders"};
/**
 * cfheaders is a response to a getcfheaders request containing a filter header
 * and a vector of filter hashes for each subsequent block in the requested range.
 */
inline constexpr const char* CFHEADERS{"cfheaders"};
/**
 * getcfcheckpt requests evenly spaced compact filter headers, enabling
 * parallelized download and validation of the headers between them.
 * Only available with service bit NODE_COMPACT_FILTERS as described by
 * BIP 157 & 158.
 */
inline constexpr const char* GETCFCHECKPT{"getcfcheckpt"};
/**
 * cfcheckpt is a response to a getcfcheckpt request containing a vector of
 * evenly spaced filter headers for blocks on the requested chain.
 */
inline constexpr const char* CFCHECKPT{"cfcheckpt"};
/**
 * Contains a 4-byte version number and an 8-byte salt.
 * The salt is used to compute short txids needed for efficient
 * txreconciliation, as described by BIP 330.
 */
inline constexpr const char* SENDTXRCNCL{"sendtxrcncl"};

// Dash message types
// NOTE: do NOT declare non-implmented here, we don't want them to be exposed to the outside
// TODO: add description
inline constexpr const char* SPORK{"spork"};
inline constexpr const char* GETSPORKS{"getsporks"};
inline constexpr const char* DSACCEPT{"dsa"};
inline constexpr const char* DSVIN{"dsi"};
inline constexpr const char* DSFINALTX{"dsf"};
inline constexpr const char* DSSIGNFINALTX{"dss"};
inline constexpr const char* DSCOMPLETE{"dsc"};
inline constexpr const char* DSSTATUSUPDATE{"dssu"};
inline constexpr const char* DSTX{"dstx"};
inline constexpr const char* DSQUEUE{"dsq"};
inline constexpr const char* SENDDSQUEUE{"senddsq"};
inline constexpr const char* SYNCSTATUSCOUNT{"ssc"};
inline constexpr const char* MNGOVERNANCESYNC{"govsync"};
inline constexpr const char* MNGOVERNANCEOBJECT{"govobj"};
inline constexpr const char* MNGOVERNANCEOBJECTVOTE{"govobjvote"};
inline constexpr const char* GETMNLISTDIFF{"getmnlistd"};
inline constexpr const char* MNLISTDIFF{"mnlistdiff"};
inline constexpr const char* QSENDRECSIGS{"qsendrecsigs"};
inline constexpr const char* QFCOMMITMENT{"qfcommit"};
inline constexpr const char* QCONTRIB{"qcontrib"};
inline constexpr const char* QCOMPLAINT{"qcomplaint"};
inline constexpr const char* QJUSTIFICATION{"qjustify"};
inline constexpr const char* QPCOMMITMENT{"qpcommit"};
inline constexpr const char* QWATCH{"qwatch"};
inline constexpr const char* QSIGSESANN{"qsigsesann"};
inline constexpr const char* QSIGSHARESINV{"qsigsinv"};
inline constexpr const char* QGETSIGSHARES{"qgetsigs"};
inline constexpr const char* QBSIGSHARES{"qbsigs"};
inline constexpr const char* QSIGREC{"qsigrec"};
inline constexpr const char* QSIGSHARE{"qsigshare"};
inline constexpr const char* QGETDATA{"qgetdata"};
inline constexpr const char* QDATA{"qdata"};
inline constexpr const char* CLSIG{"clsig"};
inline constexpr const char* ISDLOCK{"isdlock"};
inline constexpr const char* MNAUTH{"mnauth"};
inline constexpr const char* GETHEADERS2{"getheaders2"};
inline constexpr const char* SENDHEADERS2{"sendheaders2"};
inline constexpr const char* HEADERS2{"headers2"};
inline constexpr const char* GETQUORUMROTATIONINFO{"getqrinfo"};
inline constexpr const char* QUORUMROTATIONINFO{"qrinfo"};
inline constexpr const char* PLATFORMBAN{"platformban"};
}; // namespace NetMsgType

/** All known message types (see above). Keep this in the same order as the list of messages above. */
inline const std::array ALL_NET_MESSAGE_TYPES{std::to_array<std::string>({
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
    // Dash message types
    // NOTE: do NOT include non-implmented here, we want them to be "Unknown command" in ProcessMessage()
    NetMsgType::SPORK,
    NetMsgType::GETSPORKS,
    NetMsgType::SENDDSQUEUE,
    NetMsgType::DSACCEPT,
    NetMsgType::DSVIN,
    NetMsgType::DSFINALTX,
    NetMsgType::DSSIGNFINALTX,
    NetMsgType::DSCOMPLETE,
    NetMsgType::DSSTATUSUPDATE,
    NetMsgType::DSTX,
    NetMsgType::DSQUEUE,
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
})};

/* Whether the message type violates blocks-relay-only policy */
bool NetMessageViolatesBlocksOnly(const std::string& msg_type);

/** nServices flags */
enum ServiceFlags : uint64_t {
    // NOTE: When adding here, be sure to update serviceFlagToStr too
    // Nothing
    NODE_NONE = 0,
    // NODE_NETWORK means that the node is capable of serving the complete block chain. It is currently
    // set by all Dash Core non pruned nodes, and is unset by SPV clients or other light clients.
    NODE_NETWORK = (1 << 0),
    // NODE_BLOOM means the node is capable and willing to handle bloom-filtered connections.
    NODE_BLOOM = (1 << 2),
    // NODE_COMPACT_FILTERS means the node will service basic block filter requests.
    // See BIP157 and BIP158 for details on how this is implemented.
    NODE_COMPACT_FILTERS = (1 << 6),
    // NODE_NETWORK_LIMITED means the same as NODE_NETWORK with the limitation of only
    // serving the last 288 blocks
    // See BIP159 for details on how this is implemented.
    NODE_NETWORK_LIMITED = (1 << 10),
    // description will be provided
    NODE_HEADERS_COMPRESSED = (1 << 11),

    // NODE_P2P_V2 means the node supports BIP324 transport
    NODE_P2P_V2 = (1 << 12),

    // Bits 24-31 are reserved for temporary experiments. Just pick a bit that
    // isn't getting used, or one not being used much, and notify the
    // bitcoin-development mailing list. Remember that service bits are just
    // unauthenticated advertisements, so your code must be robust against
    // collisions and other cases where nodes may be advertising a service they
    // do not actually support. Other service bits should be allocated via the
    // BIP process.
};

/**
 * Convert service flags (a bitmask of NODE_*) to human readable strings.
 * It supports unknown service flags which will be returned as "UNKNOWN[...]".
 * @param[in] flags multiple NODE_* bitwise-OR-ed together
 */
std::vector<std::string> serviceFlagsToStr(uint64_t flags);

/**
 * State independent service flags.
 * If the return value is changed, contrib/seeds/makeseeds.py
 * should be updated appropriately to filter for nodes with
 * desired service flags (compatible with our new flags).
 */
constexpr ServiceFlags SeedsServiceFlags() { return ServiceFlags(NODE_NETWORK); }

/**
 * Checks if a peer with the given service flags may be capable of having a
 * robust address-storage DB.
 */
static inline bool MayHaveUsefulAddressDB(ServiceFlags services)
{
    return (services & NODE_NETWORK) || (services & NODE_NETWORK_LIMITED);
}

/** A CService with information about it as peer */
class CAddress : public CService
{
    static constexpr std::chrono::seconds TIME_INIT{100000000};

    /** Historically, CAddress disk serialization stored the CLIENT_VERSION, optionally OR'ed with
     *  the ADDRV2_FORMAT flag to indicate V2 serialization. The first field has since been
     *  disentangled from client versioning, and now instead:
     *  - The low bits (masked by DISK_VERSION_IGNORE_MASK) store the fixed value DISK_VERSION_INIT,
     *    (in case any code exists that treats it as a client version) but are ignored on
     *    deserialization.
     *  - The high bits (masked by ~DISK_VERSION_IGNORE_MASK) store actual serialization information.
     *    Only 0 or DISK_VERSION_ADDRV2 (equal to the historical ADDRV2_FORMAT) are valid now, and
     *    any other value triggers a deserialization failure. Other values can be added later if
     *    needed.
     *
     *  For disk deserialization, ADDRV2_FORMAT in the stream version signals that ADDRV2
     *  deserialization is permitted, but the actual format is determined by the high bits in the
     *  stored version field. For network serialization, the stream version having ADDRV2_FORMAT or
     *  not determines the actual format used (as it has no embedded version number).
     */
    static constexpr uint32_t DISK_VERSION_INIT{220000};
    static constexpr uint32_t DISK_VERSION_IGNORE_MASK{0b00000000'00000111'11111111'11111111};
    /** The version number written in disk serialized addresses to indicate V2 serializations.
     * It must be exactly 1<<29, as that is the value that historical versions used for this
     * (they used their internal ADDRV2_FORMAT flag here). */
    static constexpr uint32_t DISK_VERSION_ADDRV2{1 << 29};
    static_assert((DISK_VERSION_INIT & ~DISK_VERSION_IGNORE_MASK) == 0, "DISK_VERSION_INIT must be covered by DISK_VERSION_IGNORE_MASK");
    static_assert((DISK_VERSION_ADDRV2 & DISK_VERSION_IGNORE_MASK) == 0, "DISK_VERSION_ADDRV2 must not be covered by DISK_VERSION_IGNORE_MASK");

public:
    CAddress() : CService{} {};
    explicit CAddress(CService ipIn, ServiceFlags nServicesIn) : CService{ipIn}, nServices{nServicesIn} {};
    CAddress(CService ipIn, ServiceFlags nServicesIn, NodeSeconds time) : CService{ipIn}, nTime{time}, nServices{nServicesIn} {};

    enum class Format {
        Disk,
        Network,
    };
    struct SerParams : CNetAddr::SerParams {
        const Format fmt;
        SER_PARAMS_OPFUNC
    };
    static constexpr SerParams V1_NETWORK{{CNetAddr::Encoding::V1}, Format::Network};
    static constexpr SerParams V2_NETWORK{{CNetAddr::Encoding::V2}, Format::Network};
    static constexpr SerParams V1_DISK{{CNetAddr::Encoding::V1}, Format::Disk};
    static constexpr SerParams V2_DISK{{CNetAddr::Encoding::V2}, Format::Disk};

    SERIALIZE_METHODS_PARAMS(CAddress, obj, SerParams, params)
    {
        bool use_v2;
        if (params.fmt == Format::Disk) {
            // In the disk serialization format, the encoding (v1 or v2) is determined by a flag version
            // that's part of the serialization itself. ADDRV2_FORMAT in the stream version only determines
            // whether V2 is chosen/permitted at all.
            uint32_t stored_format_version = DISK_VERSION_INIT;
            if (params.enc == Encoding::V2) stored_format_version |= DISK_VERSION_ADDRV2;
            READWRITE(stored_format_version);
            stored_format_version &= ~DISK_VERSION_IGNORE_MASK; // ignore low bits
            if (stored_format_version == 0) {
                use_v2 = false;
            } else if (stored_format_version == DISK_VERSION_ADDRV2 && params.enc == Encoding::V2) {
                // Only support v2 deserialization if V2 is set.
                use_v2 = true;
            } else {
                throw std::ios_base::failure("Unsupported CAddress disk format version");
            }
        } else {
            assert(params.fmt == Format::Network);
            // In the network serialization format, the encoding (v1 or v2) is determined directly by
            // the value of enc in the stream params, as no explicitly encoded version
            // exists in the stream.
            use_v2 = params.enc == Encoding::V2;
        }

        READWRITE(Using<LossyChronoFormatter<uint32_t>>(obj.nTime));
        // nServices is serialized as CompactSize in V2; as uint64_t in V1.
        if (use_v2) {
            uint64_t services_tmp;
            SER_WRITE(obj, services_tmp = obj.nServices);
            READWRITE(Using<CompactSizeFormatter<false>>(services_tmp));
            SER_READ(obj, obj.nServices = static_cast<ServiceFlags>(services_tmp));
        } else {
            READWRITE(Using<CustomUintFormatter<8>>(obj.nServices));
        }
        // Invoke V1/V2 serializer for CService parent object.
        const auto ser_params{use_v2 ? CNetAddr::V2 : CNetAddr::V1};
        READWRITE(ser_params(AsBase<CService>(obj)));
    }

    //! Always included in serialization. The behavior is unspecified if the value is not representable as uint32_t.
    NodeSeconds nTime{TIME_INIT};
    //! Serialized as uint64_t in V1, and as CompactSize in V2.
    ServiceFlags nServices{NODE_NONE};

    friend bool operator==(const CAddress& a, const CAddress& b)
    {
        return a.nTime == b.nTime &&
               a.nServices == b.nServices &&
               static_cast<const CService&>(a) == static_cast<const CService&>(b);
    }
};

/** getdata / inv message types.
 * These numbers are defined by the protocol. When adding a new value, be sure
 * to mention it in the respective BIP.
 */
enum GetDataMsg : uint32_t {
    UNDEFINED = 0,
    MSG_TX = 1,
    MSG_BLOCK = 2,
    // The following can only occur in getdata. Invs always use TX or BLOCK.
    MSG_FILTERED_BLOCK = 3,                           //!< Defined in BIP37
    // Dash message types
    // NOTE: we must keep this enum consistent and backwards compatible
    /* MSG_LEGACY_TXLOCK_REQUEST = 4, */              // Legacy InstantSend and not used anymore
    /* MSG_TXLOCK_VOTE = 5, */                        // Legacy InstantSend and not used anymore
    MSG_SPORK = 6,
    /* 7 - 15 were used in old Dash versions and were mainly budget and MN broadcast/ping related*/
    MSG_DSTX = 16,
    MSG_GOVERNANCE_OBJECT = 17,
    MSG_GOVERNANCE_OBJECT_VOTE = 18,
    /* 19 was used for MSG_MASTERNODE_VERIFY and is not supported anymore */
    // Nodes may always request a MSG_CMPCT_BLOCK in a getdata, however,
    // MSG_CMPCT_BLOCK should not appear in any invs except as a part of getdata.
    MSG_CMPCT_BLOCK = 20,                             //!< Defined in BIP152
    MSG_QUORUM_FINAL_COMMITMENT = 21,
    /* MSG_QUORUM_DUMMY_COMMITMENT = 22, */           // was shortly used on testnet/devnet/regtest
    MSG_QUORUM_CONTRIB = 23,
    MSG_QUORUM_COMPLAINT = 24,
    MSG_QUORUM_JUSTIFICATION = 25,
    MSG_QUORUM_PREMATURE_COMMITMENT = 26,
    /* MSG_QUORUM_DEBUG_STATUS = 27, */               // was shortly used on testnet/devnet/regtest
    MSG_QUORUM_RECOVERED_SIG = 28,
    MSG_CLSIG = 29,
    /* MSG_ISLOCK = 30, */                            // Non-deterministic InstantSend and not used anymore
    MSG_ISDLOCK = 31,
    MSG_DSQ = 32,
    MSG_PLATFORM_BAN = 33,                            // Platform service ban (DIP-0031)
    MSG_ASSET_UNLOCK = 34,                            //!< Version 2 asset unlock announced by instance hash; getdata is answered with a `tx` message
};

/** inv message data */
class CInv
{
public:
    CInv();
    CInv(uint32_t typeIn, const uint256& hashIn);

    SERIALIZE_METHODS(CInv, obj) { READWRITE(obj.type, obj.hash); }

    friend bool operator<(const CInv& a, const CInv& b);
    friend bool operator==(const CInv& a, const CInv& b) { return a.type == b.type && a.hash == b.hash; }

    bool IsKnownType() const;
    std::string GetCommand() const;
    std::string ToString() const;

    // Single-message helper methods
    bool IsMsgTx()        const { return type == MSG_TX; }
    bool IsMsgBlk() const { return type == MSG_BLOCK; }
    bool IsMsgDstx()       const { return type == MSG_DSTX; }
    bool IsMsgAssetUnlock() const { return type == MSG_ASSET_UNLOCK; }
    bool IsMsgFilteredBlk() const { return type == MSG_FILTERED_BLOCK; }
    bool IsMsgCmpctBlk() const { return type == MSG_CMPCT_BLOCK; }

    // Combined-message helper methods
    bool IsGenTxMsg() const
    {
        return type == MSG_TX || type == MSG_DSTX || type == MSG_ASSET_UNLOCK;
    }
    bool IsGenBlkMsg() const
    {
        return type == MSG_BLOCK || type == MSG_FILTERED_BLOCK || type == MSG_CMPCT_BLOCK;
    }

private:
    const char* GetCommandInternal() const;

public:
    uint32_t type;
    uint256 hash;
};

#endif // BITCOIN_PROTOCOL_H
