// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NET_PROCESSING_H
#define BITCOIN_NET_PROCESSING_H

#include <net.h>
#include <validationinterface.h>
#include <version.h>

#include <msg_result.h>

#include <atomic>

class AddrMan;
class CActiveMasternodeManager;
class CCoinJoinQueue;
class CDeterministicMNManager;
class CDSTXManager;
class ChainstateManager;
class CInv;
class CJWalletManager;
class CMasternodeMetaMan;
class CMasternodeSync;
class CNetMsgMaker;
class CSporkManager;
class CTransaction;
class CTxMemPool;
struct LLMQContext;
namespace chainlock {
class Chainlocks;
class ChainlockHandler;
} // namespace chainlock

/** Default for -maxorphantxsize, maximum size in megabytes the orphan map can grow before entries are removed */
static const unsigned int DEFAULT_MAX_ORPHAN_TRANSACTIONS_SIZE = 10; // this allows around 100 TXs of max size (and many more of normal size)
/** Default number of orphan+recently-replaced txn to keep around for block reconstruction */
static const unsigned int DEFAULT_BLOCK_RECONSTRUCTION_EXTRA_TXN = 100;
static const bool DEFAULT_PEERBLOOMFILTERS = true;
static const bool DEFAULT_PEERBLOCKFILTERS = false;
/** Threshold for marking a node to be discouraged, e.g. disconnected and added to the discouragement filter. */
static const int DISCOURAGEMENT_THRESHOLD{100};
/** Maximum number of outstanding CMPCTBLOCK requests for the same block. */
static const unsigned int MAX_CMPCTBLOCKS_INFLIGHT_PER_BLOCK = 3;

/** Outcome of authorising an incoming object against what we asked a peer for.
 *
 * Used by object types that only ever travel inv -> getdata -> object, to tell an unsolicited push
 * apart from an honest answer to a GETDATA of ours that no longer has a tracker entry. The latter
 * is routine: a request expires after GetObjectInterval() and is re-pointed at another peer, and
 * accepting the object from any source erases every announcement for it (see ForgetTxHash). */
enum class GetDataResponse {
    //! We had an in-flight GETDATA for this object; it has now been consumed.
    REQUESTED,
    //! No in-flight request, but we did send this peer a GETDATA for this object recently, so the
    //! answer is merely late or was superseded. Process it, but do not treat it as unsolicited.
    LATE,
    //! We have no record of asking this peer for this object. Usually that is because we never did,
    //! which is what makes it worth scoring -- but a record we did keep is also gone once it ages
    //! out or is evicted, so an answer late enough is treated the same as one never asked for.
    UNREQUESTED,
};

struct CNodeStateStats {
    int m_misbehavior_score = 0;
    int nSyncHeight = -1;
    int nCommonHeight = -1;
    int m_starting_height = -1;
    std::chrono::microseconds m_ping_wait;
    std::vector<int> vHeightInFlight;
    bool m_relay_txs;
    uint64_t m_addr_processed = 0;
    uint64_t m_addr_rate_limited = 0;
    bool m_addr_relay_enabled{false};
    ServiceFlags their_services;
};

class PeerManagerInternal
{
public:
    virtual void PeerMisbehaving(const NodeId pnode, const int howmuch, const std::string& message = "") = 0;
    /** Whether the peer is no longer connected or has crossed the discouragement threshold. */
    virtual bool PeerIsDisconnectedOrDiscouraged(const NodeId node_id) = 0;
    /** Complete this peer's pending announcement of the inv, so it is not requested from them
     *  again. Announcements of the same inv by other peers are unaffected: an invalid or unusable
     *  object must not stop us from fetching it from honest peers.
     *  Requires ::cs_main (see the PeerManagerImpl override). */
    virtual void PeerEraseObjectRequest(const NodeId nodeid, const CInv& inv) = 0;
    /** Consume this peer's pending request for the inv and return whether such a request existed
     *  (the peer announced the inv, or we requested it from the peer). Consuming completes the
     *  announcement, so a second call without a re-announcement in between returns false.
     *  Requires ::cs_main (see the PeerManagerImpl override). */
    virtual bool PeerConsumeObjectRequest(NodeId nodeid, const CInv& inv) = 0;
    /** Consume this peer's in-flight GETDATA for the inv and report how the object was authorised.
     *  Stricter than PeerConsumeObjectRequest: a bare announcement does not qualify, only a request
     *  we actually sent. Use for object types that are only ever sent in reply to a GETDATA, so a
     *  peer cannot authorise its own payload by announcing it first. Only UNREQUESTED leaves us with
     *  nothing to show we asked; see GetDataResponse for why LATE is a normal outcome for an honest
     *  peer, and for what else can produce UNREQUESTED besides never having asked.
     *  Requires ::cs_main (see the PeerManagerImpl override). */
    virtual GetDataResponse PeerConsumeGetDataResponse(NodeId nodeid, const CInv& inv) = 0;
    /** Delete all peers' announcements of the inv. Call once the object is accepted (AlreadyHave
     *  turns true), so it is not requested from anyone anymore.
     *  Requires ::cs_main (see the PeerManagerImpl override). */
    virtual void PeerForgetObjectRequest(const CInv& inv) = 0;
    virtual void PeerPushInventory(NodeId nodeid, const CInv& inv) = 0;
    virtual void PeerRelayInv(const CInv& inv) = 0;
    virtual void PeerRelayInvFiltered(const CInv& inv, const CTransaction& relatedTx) = 0;
    virtual void PeerRelayInvFiltered(const CInv& inv, const uint256& relatedTxHash) = 0;
    virtual void PeerRelayTransaction(const uint256& txid) = 0;
    virtual void PeerRelayDSQ(const CCoinJoinQueue& queue) = 0;
    virtual void PeerRelayRecoveredSig(const llmq::CRecoveredSig& sig, bool proactive_relay) = 0;
    virtual void PeerAskPeersForTransaction(const uint256& txid) = 0;
    virtual size_t PeerGetRequestedObjectCount(NodeId nodeid) const = 0;
    virtual void PeerPostProcessMessage(MessageProcessingResult&& ret) = 0;
};

class NetHandler
{
public:
    NetHandler(PeerManagerInternal* peer_manager) : m_peer_manager{Assert(peer_manager)} {}
    virtual ~NetHandler() {
        Interrupt();
        Stop();
    }

    virtual void Start() {}
    virtual void Stop() {}
    virtual void Interrupt() {}
    virtual void Schedule(CScheduler& scheduler) {}

    virtual void ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv) {}

    // It returns true, if NetHandler has a responsibility about having this type of inventory and has corresponding data.
    virtual bool AlreadyHave(const CInv& inv) { return false; }

    // It should return true, if there's data has been pushed
    virtual bool ProcessGetData(CNode& pfrom, const CInv& inv, const CNetMsgMaker& msgMaker) { return false; }
protected:
    PeerManagerInternal* m_peer_manager;
};


class PeerManager : public CValidationInterface, public NetEventsInterface, public PeerManagerInternal
{
public:
    /**
     * @param nodeman Non-null iff masternode mode is on; null otherwise. Used both
     *                as the masternode-mode indicator and for direct access.
     */
    static std::unique_ptr<PeerManager> make(CConnman& connman, AddrMan& addrman,
                                             BanMan* banman, CDSTXManager& dstxman, ChainstateManager& chainman,
                                             CTxMemPool& pool, CMasternodeMetaMan& mn_metaman, CMasternodeSync& mn_sync,
                                             CSporkManager& sporkman,
                                             const chainlock::Chainlocks& chainlocks,
                                             chainlock::ChainlockHandler& clhandler,
                                             CActiveMasternodeManager* nodeman,
                                             const std::unique_ptr<CDeterministicMNManager>& dmnman,
                                             const std::unique_ptr<CJWalletManager>& cj_walletman,
                                             const std::unique_ptr<LLMQContext>& llmq_ctx, bool ignore_incoming_txs);
    virtual ~PeerManager() { }

    /**
     * Attempt to manually fetch block from a given peer. We must already have the header.
     *
     * @param[in]  peer_id      The peer id
     * @param[in]  block_index  The blockindex
     * @returns std::nullopt if a request was successfully made, otherwise an error message
     */
    virtual std::optional<std::string> FetchBlock(NodeId peer_id, const CBlockIndex& block_index) = 0;

    /** Begin running background tasks, should only be called once */
    virtual void StartScheduledTasks(CScheduler& scheduler) = 0;

    /** Get statistics from node state */
    virtual bool GetNodeStateStats(NodeId nodeid, CNodeStateStats& stats) const = 0;

    /** Whether this node ignores txs received over p2p. */
    virtual bool IgnoresIncomingTxs() = 0;

    /** Send ping message to all peers */
    virtual void SendPings() = 0;

    /** Broadcast inventory message to a specific peer. */
    virtual void PushInventory(NodeId nodeid, const CInv& inv) = 0;

    /** Relay DSQ based on peer preference */
    virtual void RelayDSQ(const CCoinJoinQueue& queue) = 0;

    /** Relay inventories to all peers */
    virtual void RelayInv(const CInv& inv) = 0;
    virtual void RelayInv(const CInv& inv, const int minProtoVersion) = 0;

    /** Relay transaction to all peers. */
    virtual void RelayTransaction(const uint256& txid) = 0;

    /** Relay recovered sigs to all interested peers */
    virtual void RelayRecoveredSig(const llmq::CRecoveredSig& sig, bool proactive_relay) = 0;

    /** Set the best height */
    virtual void SetBestHeight(int height) = 0;

    /* Public for unit testing. */
    virtual void UnitTestMisbehaving(NodeId peer_id, int howmuch) = 0;

    /**
     * Evict extra outbound peers. If we think our tip may be stale, connect to an extra outbound.
     * Public for unit testing.
     */
    virtual void CheckForStaleTipAndEvictPeers() = 0;

    /** Process a single message from a peer. Public for fuzz testing */
    virtual void ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv,
                                const std::chrono::microseconds time_received, const std::atomic<bool>& interruptMsgProc) EXCLUSIVE_LOCKS_REQUIRED(g_msgproc_mutex) = 0;

    /** Finish message processing. Used for some specific messages */
    virtual void PostProcessMessage(MessageProcessingResult&& ret, NodeId node = -1) = 0;

    /** This function is used for testing the stale tip eviction logic, see denialofservice_tests.cpp */
    virtual void UpdateLastBlockAnnounceTime(NodeId node, int64_t time_in_seconds) = 0;

    virtual size_t GetRequestedObjectCount(NodeId nodeid) const = 0;

    virtual void AddExtraHandler(std::unique_ptr<NetHandler>&& handler) = 0;
    virtual void RemoveHandlers() = 0;
    virtual void StartHandlers() = 0;
    virtual void StopHandlers() = 0;
    virtual void InterruptHandlers() = 0;
    virtual void ScheduleHandlers(CScheduler& scheduler) = 0;
};

#endif // BITCOIN_NET_PROCESSING_H
