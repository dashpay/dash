// Copyright (c) 2018 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DASH_QUORUMS_SIGNING_H
#define DASH_QUORUMS_SIGNING_H

#include "llmq/quorums.h"

#include "net.h"
#include "chainparams.h"

#include <boost/lockfree/queue.hpp>
#include <utility>

namespace cxxtimer {
    class Timer;
}

class CScheduler;

namespace llmq
{

// <signHash, quorumMember>
typedef std::pair<uint256, uint16_t> SigShareKey;

// this one does not get transmitted over the wire as it is batched inside CBatchedSigShares
class CSigShare
{
public:
    uint8_t llmqType;
    uint256 quorumHash;
    uint16_t quorumMember;
    uint256 id;
    uint256 msgHash;
    CBLSSignature sigShare;

    // only in-memory
    SigShareKey key;

public:
    ADD_SERIALIZE_METHODS

    template<typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(llmqType);
        READWRITE(quorumHash);
        READWRITE(quorumMember);
        READWRITE(id);
        READWRITE(msgHash);
        READWRITE(sigShare);
        if (ser_action.ForRead()) {
            UpdateKey();
        }
    };

    void UpdateKey();
    const SigShareKey& GetKey() const
    {
        return key;
    }
    const uint256& GetSignHash() const
    {
        assert(!key.first.IsNull());
        return key.first;
    }
};

class CSigSharesInv
{
public:
    uint8_t llmqType;
    uint256 signHash;
    std::vector<bool> inv;

public:
    template<typename Stream>
    void Serialize(Stream& s) const {
        auto& consensus = Params().GetConsensus();
        auto it = consensus.llmqs.find((Consensus::LLMQType)llmqType);
        assert(it != consensus.llmqs.end());
        assert(inv.size() == it->second.size);
        size_t cnt = CountSet();
        size_t s1 = (inv.size() + 7) / 8; // as bitset
        size_t s2 = 1; // as series of var int diffs with 0 as stop signal
        for (size_t i = 0; i < inv.size(); i++) {
            if (inv[i]) {
                s2 += GetSizeOfVarInt(i + 1);
            }
        }
        if (s1 < s2) {
            s << llmqType;
            s << signHash;
            s << FIXEDBITSET(inv, inv.size());
        } else {
            s << (uint8_t)(llmqType | 0x80);
            s << signHash;
            size_t last = 0;
            for (size_t i = 0; i < inv.size(); i++) {
                if (inv[i]) {
                    s << VARINT((i - last) + 1); // +1 because 0 is the stopper
                    last = i;
                }
            }
            WriteVarInt(s, 0); // stopper
        }
    }
    template<typename Stream>
    void Unserialize(Stream& s) {
        s >> llmqType;
        s >> signHash;

        bool isBitset = true;
        if (llmqType & 0x80) {
            llmqType &= 0x7F;
            isBitset = false;
        }
        auto& consensus = Params().GetConsensus();
        auto it = consensus.llmqs.find((Consensus::LLMQType)llmqType);
        if (it == consensus.llmqs.end()) {
            throw std::ios_base::failure(strprintf("invalid llmqType %d", llmqType));
        }

        if (isBitset) {
            s >> FIXEDBITSET(inv, (size_t)it->second.size);
        } else {
            inv.resize((size_t)it->second.size);
            size_t last = 0;
            while(true) {
                uint64_t v;
                s >> VARINT(v);
                if (v == 0) {
                    break;
                }
                uint64_t idx = last + v - 1;
                if (idx >= inv.size()) {
                    throw std::ios_base::failure(strprintf("out of bounds index %d, last=%d", idx, last));
                }
                if (last != 0 && idx <= last) {
                    throw std::ios_base::failure(strprintf("unexpected index %d, last=%d", idx, last));
                }
                if (inv[idx]) {
                    throw std::ios_base::failure(strprintf("duplicate index %d, last=%d", idx, last));
                }
                inv[idx] = true;
                last = idx;
            }
        }
    }

    void Init(Consensus::LLMQType _llmqType, const uint256& _signHash);
    bool IsMarked(uint16_t quorumMember) const;
    void Set(uint16_t quorumMember, bool v);
    void Merge(const CSigSharesInv& inv2);

    size_t CountSet() const;
    std::string ToString() const;
};

// sent through the message QBSIGSHARES as a vector of multiple batches
class CBatchedSigShares
{
public:
    uint8_t llmqType;
    uint256 quorumHash;
    uint256 id;
    uint256 msgHash;
    std::vector<std::pair<uint16_t, CBLSSignature>> sigShares;

public:
    template<typename Stream, typename Operation>
    inline void SerializationOpBase(Stream& s, Operation ser_action)
    {
        READWRITE(llmqType);
        READWRITE(quorumHash);
        READWRITE(id);
        READWRITE(msgHash);
    }

    template<typename Stream>
    inline void Serialize(Stream& s) const
    {
        s << llmqType;
        s << quorumHash;
        s << id;
        s << msgHash;
        s << sigShares;
    }
    template<typename Stream>
    inline void Unserialize(Stream& s)
    {
        s >> llmqType;
        s >> quorumHash;
        s >> id;
        s >> msgHash;

        // we do custom deserialization here with the malleability check skipped for signatures
        // we can do this here because we never use the hash of a sig share for identification and are only interested
        // in validity
        uint64_t nSize = ReadCompactSize(s);
        if (nSize > 400) { // we don't support larger quorums, so this is the limit
            throw std::ios_base::failure(strprintf("too many elements (%d) in CBatchedSigShares", nSize));
        }
        sigShares.resize(nSize);
        for (size_t i = 0; i < nSize; i++) {
            s >> sigShares[i].first;
            sigShares[i].second.Unserialize(s, false);
        }
    };

    CSigShare RebuildSigShare(size_t idx) const
    {
        assert(idx < sigShares.size());
        auto& s = sigShares[idx];
        CSigShare sigShare;
        sigShare.llmqType = llmqType;
        sigShare.quorumHash = quorumHash;
        sigShare.quorumMember = s.first;
        sigShare.id = id;
        sigShare.msgHash = msgHash;
        sigShare.sigShare = s.second;
        sigShare.UpdateKey();
        return sigShare;
    }

    CSigSharesInv ToInv() const;
};

class CRecoveredSig
{
public:
    uint8_t llmqType;
    uint256 quorumHash;
    uint256 id;
    uint256 msgHash;
    CBLSSignature sig;

    // only in-memory
    uint256 hash;

public:
    ADD_SERIALIZE_METHODS

    template<typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(llmqType);
        READWRITE(quorumHash);
        READWRITE(id);
        READWRITE(msgHash);
        READWRITE(sig);
        if (ser_action.ForRead()) {
            UpdateHash();
        }
    };

    void UpdateHash()
    {
        hash = ::SerializeHash(*this);
    }

    const uint256& GetHash() const
    {
        assert(!hash.IsNull());
        return hash;
    }
};

class CSigSharesNodeState
{
public:
    struct Session {
        CSigSharesInv announced;
        CSigSharesInv requested;
        CSigSharesInv knows;
    };
    // TODO limit number of sessions per node
    std::map<uint256, Session> sessions;

    std::map<SigShareKey, CSigShare> pendingIncomingSigShares;
    std::map<uint256, CRecoveredSig> pendingIncomingRecSigs; // k = signHash
    std::map<SigShareKey, int64_t> requestedSigShares;

    // elements are added whenever we receive a valid sig share from this node
    // this triggers us to send inventory items to him as he seems to be interested in these
    std::set<std::pair<Consensus::LLMQType, uint256>> interestedIn;

    Session& GetOrCreateSession(Consensus::LLMQType llmqType, const uint256& signHash);

    void MarkAnnounced(const uint256& signHash, const CSigSharesInv& inv);
    void MarkRequested(const uint256& signHash, const CSigSharesInv& inv);
    void MarkKnows(const uint256& signHash, const CSigSharesInv& inv);

    void MarkAnnounced(Consensus::LLMQType llmqType, const uint256& signHash, uint16_t quorumMember);
    void MarkRequested(Consensus::LLMQType llmqType, const uint256& signHash, uint16_t quorumMember);
    void MarkKnows(Consensus::LLMQType llmqType, const uint256& signHash, uint16_t quorumMember);

    bool Announced(const uint256& signHash, uint16_t quorumMember) const;
    bool Requested(const uint256& signHash, uint16_t quorumMember) const;
    bool Knows(const uint256& signHash, uint16_t quorumMember) const;

    void Erase(const uint256& signHash, uint16_t quorumMember);
    void Erase(const uint256& signHash);
};

class CSigningManager
{
    static const int64_t SIGNING_SESSION_TIMEOUT = 60 * 1000;
    static const int64_t SIG_SHARE_REQUEST_TIMEOUT = 5 * 1000;
private:
    CCriticalSection cs;
    CEvoDB& evoDb;
    CBLSWorker& blsWorker;

    // TODO cleanup (also stuff from deleted quorums)
    std::map<SigShareKey, CSigShare> sigShares;
    std::map<uint256, CRecoveredSig> recoveredSigs;

    std::map<uint256, int64_t> firstSeenForSessions;
    std::map<std::pair<Consensus::LLMQType, uint256>, uint256> recoveredSigsForIds;
    std::set<uint256> recoveredSessions;

    std::map<std::pair<Consensus::LLMQType, uint256>, uint256> votedOnIds;

    struct WorkQueueItem {
        int64_t at;
        std::function<void()> func;
    };
    std::list<WorkQueueItem> workQueue;
    std::mutex workQueueMutex;
    std::thread workThread;
    std::atomic<bool> stopWorkThread{false};

    std::map<NodeId, CSigSharesNodeState> nodeStates;
    std::map<SigShareKey, std::pair<NodeId, int64_t>> sigSharesRequested;
    std::set<SigShareKey> sigSharesToAnnounce;

    // must be protected by cs
    FastRandomContext rnd;

    int64_t lastCleanupTime{0};

public:
    CSigningManager(CEvoDB& _evoDb, CBLSWorker& _blsWorker);
    ~CSigningManager();

public:
    bool AlreadyHave(const CInv& inv);
    bool GetRecoveredSig(const uint256& hash, CRecoveredSig& ret);

    void ProcessMessage(CNode* pnode, const std::string& strCommand, CDataStream& vRecv, CConnman& connman);

private:
    void ProcessMessageSigSharesInv(CNode* pfrom, const CSigSharesInv& inv, CConnman& connman);
    void ProcessMessageGetSigShares(CNode* pfrom, const CSigSharesInv& inv, CConnman& connman);
    void ProcessMessageBatchedSigShares(CNode* pfrom, const CBatchedSigShares& batchedSigShares, CConnman& connman);
    void ProcessMessageRecoveredSig(CNode* pfrom, const CRecoveredSig& recoveredSig, CConnman& connman);
    bool VerifySigSharesInv(NodeId from, const CSigSharesInv& inv);

    bool PreVerifyBatchedSigShares(NodeId nodeId, const CBatchedSigShares& batchedSigShares, bool& retBan);
    bool PreVerifyRecoveredSig(NodeId nodeId, const CRecoveredSig& recoveredSig, bool& retBan);

    void ProcessPendingIncomingSigs(CConnman& connman);
    void ProcessPendingSigShares(CConnman& connman);
    void ProcessPendingRecoveredSigs(CConnman& connman);

    bool VerifyPendingSigShares(const std::vector<CSigShare>& sigShares, const std::map<std::pair<Consensus::LLMQType, uint256>, CQuorumCPtr>& quorums);
    void ProcessPendingSigSharesFromNode(NodeId nodeId, const std::vector<CSigShare>& sigShares, const std::map<std::pair<Consensus::LLMQType, uint256>, CQuorumCPtr>& quorums, CConnman& connman);
    void ProcessPendingRecoveredSigsFromNode(NodeId nodeId, const std::list<CRecoveredSig>& recoveredSigs, const std::map<std::pair<Consensus::LLMQType, uint256>, CQuorumCPtr>& quorums, CConnman& connman);

    void ProcessSigShare(NodeId nodeId, const CSigShare& sigShare, CConnman& connman, const CQuorumCPtr& quorum);

    void TryRecoverSig(const CQuorumCPtr& quorum, const uint256& id, const uint256& msgHash, CConnman& connman);
    void ProcessRecoveredSig(NodeId nodeId, const CRecoveredSig& recoveredSig, const CQuorumCPtr& quorum, CConnman& connman);

    bool IsQuorumActive(Consensus::LLMQType llmqType, const uint256& quorumHash);

public:
    // public interface
    void AsyncSignIfMember(Consensus::LLMQType llmqType, const uint256& id, const uint256& msgHash);
    void SignIfMember(Consensus::LLMQType llmqType, const uint256& id, const uint256& msgHash);
    void Sign(const CQuorumCPtr& quorum, const uint256& id, const uint256& msgHash);
    bool HasRecoveredSig(Consensus::LLMQType llmqType, const uint256& id, const uint256& msgHash);
    bool IsConflicting(Consensus::LLMQType llmqType, const uint256& id, const uint256& msgHash);

private:
    void Cleanup();
    void RemoveSigSharesForSession(const uint256& signHash);
    void RemoveBannedNodeStates();

    template<typename Callable>
    void PushWork(int64_t delay, Callable&& func) {
        if (delay != 0) {
            delay += GetTimeMillis();
        }
        WorkQueueItem wi;
        wi.at = delay;
        wi.func = func;
        std::unique_lock<std::mutex> l(workQueueMutex);
        workQueue.emplace_back(std::move(wi));
    }
    void SendMessages();
    void CollectSigSharesToRequest(std::map<NodeId, std::map<uint256, CSigSharesInv>>& sigSharesToRequest);
    void CollectSigSharesToSend(std::map<NodeId, std::map<uint256, CBatchedSigShares>>& sigSharesToSend);
    void CollectSigSharesToAnnounce(std::map<NodeId, std::map<uint256, CSigSharesInv>>& sigSharesToAnnounce);
    void WorkThreadMain();
};

extern CSigningManager* quorumsSigningManager;

}

#endif //DASH_QUORUMS_SIGNING_H
