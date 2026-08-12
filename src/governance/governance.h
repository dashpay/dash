// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_GOVERNANCE_GOVERNANCE_H
#define BITCOIN_GOVERNANCE_GOVERNANCE_H

#include <cachemap.h>
#include <cachemultimap.h>
#include <governance/vote.h>
#include <primitives/transaction.h>
#include <sync.h>
#include <util/time.h>

#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

class CBloomFilter;
class CBlockIndex;
class CDataStream;
class CDeterministicMNList;
class CDeterministicMNManager;
class ChainstateManager;
template<typename T>
class CFlatDB;
class CGovernanceException;
class CGovernanceObject;
class CInv;
class CMasternodeMetaMan;
class CMasternodeSync;
class CService;
struct RPCResult;
class UniValue;

namespace governance {
class SuperblockManager;

struct OrphanVote {
    CGovernanceVote vote;
    NodeSeconds expiration;

    OrphanVote() = default;
    OrphanVote(const CGovernanceVote& vote, NodeSeconds expiration) : vote(vote), expiration(expiration) {}

    SERIALIZE_METHODS(OrphanVote, obj)
    {
        // Preserve the historical integer Unix-seconds representation on disk.
        READWRITE(obj.vote, Using<ChronoFormatter<int64_t>>(obj.expiration));
    }
};

inline bool operator<(const OrphanVote& lhs, const OrphanVote& rhs)
{
    return lhs.vote < rhs.vote;
}

/** Bounded holding area for votes whose parent object has not arrived yet. Entries are
 *  peer-supplied and evicted oldest-first, so two bounds are enforced together: a global one that
 *  caps memory, and a per-masternode one so that a single voting key cannot mint votes naming
 *  invented parents and flush everyone else's orphans on demand. */
class OrphanVoteCache
{
public:
    using cache_t = CacheMultiMap<uint256, OrphanVote>;

    enum class InsertResult {
        OK,
        DUPLICATE, //!< this (parent, vote) pair is already cached
        MN_LIMIT,  //!< the vote's masternode already holds its full share of the cache
    };

    OrphanVoteCache(size_t max_total, size_t max_per_mn) :
        m_max_total{max_total},
        m_max_per_mn{max_per_mn},
        // Eviction happens here, before the inner map is ever full, so its own self-pruning
        // (which would bypass the per-masternode accounting) can never trigger.
        m_cache{static_cast<cache_t::size_type>(max_total + 1)}
    {
    }

    InsertResult Insert(const uint256& parent_hash, const OrphanVote& orphan_vote)
    {
        // A pair we already hold is reported as a duplicate even when its masternode is over its
        // share: it costs no capacity, and the caller treats a repeat relay as fresh evidence that
        // the sending peer has the missing parent.
        if (m_cache.HasEntry(parent_hash, orphan_vote)) {
            return InsertResult::DUPLICATE;
        }
        const COutPoint outpoint{orphan_vote.vote.GetMasternodeOutpoint()};
        if (const auto it{m_counts.find(outpoint)}; it != m_counts.end() && it->second >= m_max_per_mn) {
            return InsertResult::MN_LIMIT;
        }
        if (!m_cache.Insert(parent_hash, orphan_vote)) {
            return InsertResult::DUPLICATE;
        }
        ++m_counts[outpoint];
        if (m_cache.GetSize() > m_max_total) {
            // Insert prepends, so the back is the oldest entry cache-wide and never the one just
            // added. Copied, not referenced: Erase destroys the node.
            const auto oldest{m_cache.GetItemList().back()};
            Erase(oldest.key, oldest.value);
        }
        return InsertResult::OK;
    }

    void Erase(const uint256& parent_hash, const OrphanVote& orphan_vote)
    {
        // Callers may pass a reference into the cache's own item list (e.g. the expiry sweep),
        // which the erase below invalidates; take what we need first.
        const COutPoint outpoint{orphan_vote.vote.GetMasternodeOutpoint()};
        const auto size_before{m_cache.GetSize()};
        m_cache.Erase(parent_hash, orphan_vote);
        if (m_cache.GetSize() == size_before) return;
        if (const auto it{m_counts.find(outpoint)}; it != m_counts.end() && --it->second == 0) {
            m_counts.erase(it);
        }
    }

    //! Drop every cached vote from one masternode, e.g. because its keys changed and the votes
    //! can no longer validate on replay. Returns how many were dropped.
    size_t EraseAllForMasternode(const COutPoint& outpoint)
    {
        if (m_counts.find(outpoint) == m_counts.end()) return 0;
        // Collect first: Erase destroys the nodes being iterated.
        std::vector<std::pair<uint256, OrphanVote>> expelled;
        for (const auto& item : m_cache.GetItemList()) {
            if (item.value.vote.GetMasternodeOutpoint() == outpoint) {
                expelled.emplace_back(item.key, item.value);
            }
        }
        for (const auto& [parent_hash, orphan_vote] : expelled) {
            Erase(parent_hash, orphan_vote);
        }
        return expelled.size();
    }

    bool GetAll(const uint256& parent_hash, std::vector<OrphanVote>& votes)
    {
        return m_cache.GetAll(parent_hash, votes);
    }

    void Clear()
    {
        m_cache.Clear();
        m_counts.clear();
    }

    size_t GetSize() const { return m_cache.GetSize(); }
    const cache_t::list_t& GetItemList() const { return m_cache.GetItemList(); }
    //! The inner map, for writing the legacy on-disk field.
    const cache_t& Store() const { return m_cache; }

private:
    const size_t m_max_total;
    const size_t m_max_per_mn;
    cache_t m_cache;
    std::map<COutPoint, size_t> m_counts;
};
} // namespace governance

static constexpr int RATE_BUFFER_SIZE = 5;
static constexpr bool DEFAULT_GOVERNANCE_ENABLE{true};

extern RecursiveMutex cs_main; // NOLINT(readability-redundant-declaration)

class CRateCheckBuffer
{
private:
    std::vector<int64_t> vecTimestamps;

    int nDataStart{0};
    int nDataEnd{0};
    bool fBufferEmpty{true};

public:
    CRateCheckBuffer() :
        vecTimestamps(RATE_BUFFER_SIZE)
    {
    }

    void AddTimestamp(int64_t nTimestamp)
    {
        if ((nDataEnd == nDataStart) && !fBufferEmpty) {
            // Buffer full, discard 1st element
            nDataStart = (nDataStart + 1) % RATE_BUFFER_SIZE;
        }
        vecTimestamps[nDataEnd] = nTimestamp;
        nDataEnd = (nDataEnd + 1) % RATE_BUFFER_SIZE;
        fBufferEmpty = false;
    }

    int64_t GetMinTimestamp() const
    {
        int nIndex = nDataStart;
        int64_t nMin = std::numeric_limits<int64_t>::max();
        if (fBufferEmpty) {
            return nMin;
        }
        do {
            if (vecTimestamps[nIndex] < nMin) {
                nMin = vecTimestamps[nIndex];
            }
            nIndex = (nIndex + 1) % RATE_BUFFER_SIZE;
        } while (nIndex != nDataEnd);
        return nMin;
    }

    int64_t GetMaxTimestamp() const
    {
        int nIndex = nDataStart;
        int64_t nMax = 0;
        if (fBufferEmpty) {
            return nMax;
        }
        do {
            if (vecTimestamps[nIndex] > nMax) {
                nMax = vecTimestamps[nIndex];
            }
            nIndex = (nIndex + 1) % RATE_BUFFER_SIZE;
        } while (nIndex != nDataEnd);
        return nMax;
    }

    int GetCount() const
    {
        if (fBufferEmpty) {
            return 0;
        }
        if (nDataEnd > nDataStart) {
            return nDataEnd - nDataStart;
        }
        return RATE_BUFFER_SIZE - nDataStart + nDataEnd;
    }

    double GetRate() const
    {
        int nCount = GetCount();
        if (nCount < RATE_BUFFER_SIZE) {
            return 0.0;
        }
        int64_t nMin = GetMinTimestamp();
        int64_t nMax = GetMaxTimestamp();
        if (nMin == nMax) {
            // multiple objects with the same timestamp => infinite rate
            return 1.0e10;
        }
        return double(nCount) / double(nMax - nMin);
    }

    SERIALIZE_METHODS(CRateCheckBuffer, obj)
    {
        READWRITE(obj.vecTimestamps, obj.nDataStart, obj.nDataEnd, obj.fBufferEmpty);
    }
};

class GovernanceStore
{
protected:
    struct last_object_rec {
        explicit last_object_rec(bool fStatusOKIn = true) :
            triggerBuffer(),
            fStatusOK(fStatusOKIn)
        {
        }

        SERIALIZE_METHODS(last_object_rec, obj)
        {
            READWRITE(obj.triggerBuffer, obj.fStatusOK);
        }

        CRateCheckBuffer triggerBuffer;
        bool fStatusOK;
    };

    using txout_m_t = std::map<COutPoint, last_object_rec>;
    using vote_cmm_t = CacheMultiMap<uint256, governance::OrphanVote>;

public:
    /** Bounds for the orphan-vote cache, which is filled from the network by any peer with a
     *  parent object we do not have. The per-masternode bound is the security control: eviction
     *  is oldest-first, and every entry costs its sender a registered masternode voting key, so
     *  one key can occupy at most its share instead of flushing the whole cache; the value is
     *  far above the number of in-flight objects one masternode can plausibly have voted on.
     *  The global bound caps memory: at roughly 400 bytes per entry this allows ~40 MB, and
     *  filling it takes 200 distinct masternode keys. */
    static constexpr size_t MAX_ORPHAN_VOTES = 100'000;
    static constexpr size_t MAX_ORPHAN_VOTES_PER_MN = 500;

protected:
    static constexpr int MAX_CACHE_SIZE = 1000000;
    static const std::string SERIALIZATION_VERSION_STRING;

protected:
    // critical section to protect the inner data structures
    mutable Mutex cs_store;

    // keep track of the scanning errors
    std::map<uint256, std::shared_ptr<CGovernanceObject>> mapObjects GUARDED_BY(cs_store);
    // mapErasedGovernanceObjects contains key-value pairs, where
    //   key   - governance object's hash
    //   value - expiration time for deleted objects
    std::map<uint256, int64_t> mapErasedGovernanceObjects GUARDED_BY(cs_store);
    governance::OrphanVoteCache m_orphan_votes GUARDED_BY(cs_store);
    txout_m_t mapLastMasternodeObject GUARDED_BY(cs_store);
    // used to check for changed voting keys
    std::shared_ptr<CDeterministicMNList> lastMNListForVotingKeys GUARDED_BY(cs_store);

public:
    GovernanceStore();
    ~GovernanceStore() = default;

    template<typename Stream>
    void Serialize(Stream &s) const EXCLUSIVE_LOCKS_REQUIRED(!cs_store)
    {
        LOCK(cs_store);
        // TODO: Remove the historical invalid-vote-cache field on the next disk-format version bump.
        const CacheMap<uint256, CGovernanceVote> empty_invalid_votes{MAX_CACHE_SIZE};
        s   << SERIALIZATION_VERSION_STRING
            << mapErasedGovernanceObjects
            << empty_invalid_votes
            << m_orphan_votes.Store()
            << mapObjects
            << mapLastMasternodeObject
            << *lastMNListForVotingKeys;
    }

    template<typename Stream>
    void Unserialize(Stream &s) EXCLUSIVE_LOCKS_REQUIRED(!cs_store)
    {
        Clear();

        LOCK(cs_store);
        std::string strVersion;
        s >> strVersion;
        if (strVersion != SERIALIZATION_VERSION_STRING) {
            return;
        }

        // TODO: Stop consuming the historical invalid-vote-cache field on the next disk-format version bump.
        CacheMap<uint256, CGovernanceVote> discarded_invalid_votes;
        // The historical format stores CacheMultiMap's capacity with its entries. Consume that
        // field to preserve the format, but keep both the stale orphan votes and their disk-supplied
        // capacity out of the live cache. Orphans are a ten-minute recovery window invalidated by
        // the restart; the live capacity is fixed at construction.
        vote_cmm_t discarded_orphan_votes;

        s   >> mapErasedGovernanceObjects
            >> discarded_invalid_votes
            >> discarded_orphan_votes
            >> mapObjects
            >> mapLastMasternodeObject
            >> *lastMNListForVotingKeys;
    }

    void Clear()
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);

    std::string ToString() const
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);
};

//
// Governance Manager : Contains all proposals for the budget
//
class CGovernanceManager : public GovernanceStore
{
private:
    using db_type = CFlatDB<GovernanceStore>;
    using object_ref_cm_t = CacheMap<uint256, std::shared_ptr<CGovernanceObject>>;

private:
    const std::unique_ptr<db_type> m_db;
    bool is_loaded{false};

    CMasternodeMetaMan& m_mn_metaman;
    const ChainstateManager& m_chainman;
    governance::SuperblockManager& m_superblocks;
    CDeterministicMNManager& m_dmnman;
    CMasternodeSync& m_mn_sync;

    int64_t nTimeLastDiff{0};
    // keep track of current block height
    int nCachedBlockHeight{0};
    object_ref_cm_t cmapVoteToObject;
    std::map<uint256, std::shared_ptr<CGovernanceObject>> mapPostponedObjects;
    std::set<uint256> setAdditionalRelayObjects;
    bool fRateChecksEnabled{true};

    mutable Mutex cs_relay;
    std::vector<CInv> m_relay_invs GUARDED_BY(cs_relay);

public:
    CGovernanceManager() = delete;
    CGovernanceManager(const CGovernanceManager&) = delete;
    CGovernanceManager& operator=(const CGovernanceManager&) = delete;
    explicit CGovernanceManager(CMasternodeMetaMan& mn_metaman, const ChainstateManager& chainman,
                                governance::SuperblockManager& superblocks, CDeterministicMNManager& dmnman,
                                CMasternodeSync& mn_sync);
    ~CGovernanceManager();

    // Basic initialization and querying
    bool IsValid() const { return is_loaded; }
    bool LoadCache(bool load_cache)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);
    [[nodiscard]] static RPCResult GetJsonHelp(const std::string& key, bool optional);
    std::string ToString() const
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);
    [[nodiscard]] UniValue ToJson() const
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);
    void Clear()
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);

    // CGovernanceObject
    bool AreRateChecksEnabled() const { return fRateChecksEnabled; }

    // Getters/Setters
    int GetCachedBlockHeight() const { return nCachedBlockHeight; }
    int64_t GetLastDiffTime() const { return nTimeLastDiff; }
    std::vector<CGovernanceVote> GetCurrentVotes(const uint256& nParentHash, const COutPoint& mnCollateralOutpointFilter) const
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);
    void GetAllNewerThan(std::vector<CGovernanceObject>& objs, int64_t nMoreThanTime,
                         bool include_postponed = false) const
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);
    void UpdateLastDiffTime(int64_t nTimeIn) { nTimeLastDiff = nTimeIn; }

    // Networking
    /**
     * This is called by AlreadyHave in net_processing.cpp as part of the inventory
     * retrieval process.  Returns true if we want to retrieve the object, otherwise
     * false. (Note logic is inverted in AlreadyHave).
     */
    bool ConfirmInventoryRequest(const CInv& inv)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);
    bool ProcessVoteAndRelay(const CGovernanceVote& vote, CGovernanceException& exception)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store, !cs_relay);
    void RelayObject(const CGovernanceObject& obj)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_relay);
    void RelayVote(const CGovernanceVote& vote)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_relay);

    // Notification interface trigger
    void UpdatedBlockTip(const CBlockIndex* pindex)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store, !cs_relay);

    // Signer interface
    bool MasternodeRateCheck(const CGovernanceObject& govobj, bool fUpdateFailStatus = false)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);
    std::shared_ptr<CGovernanceObject> FindGovernanceObjectByDataHash(const uint256& nDataHash)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);
    std::vector<std::shared_ptr<const CGovernanceObject>> GetApprovedProposals(const CDeterministicMNList& tip_mn_list)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);
    void AddGovernanceObject(CGovernanceObject& govobj, const std::string& peer_str)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store, !cs_relay);
    /** Test-only helper: inserts an object into the syncable object store without
     *  running collateral or chain validation. */
    void AddGovernanceObjectForTesting(const CGovernanceObject& govobj)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);

    // Thread-safe accessors
    bool HaveObjectForHash(const uint256& nHash) const
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);
    bool HaveObjectForFetch(const uint256& nHash) const
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);
    bool HaveSyncableObjectForHash(const uint256& nHash) const
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);
    bool HaveVoteForHash(const uint256& nHash) const
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);
    bool SerializeObjectForHash(const uint256& nHash, CDataStream& ss) const
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);
    bool SerializeVoteForHash(const uint256& nHash, CDataStream& ss) const
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);
    std::shared_ptr<CGovernanceObject> FindGovernanceObject(const uint256& nHash) EXCLUSIVE_LOCKS_REQUIRED(!cs_store);
    int GetVoteCount() const
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);
    void AddPostponedObject(const CGovernanceObject& govobj)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);

    std::shared_ptr<const CGovernanceObject> FindConstGovernanceObject(const uint256& nHash) const
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);

    // Used by NetGovernance
    std::vector<CInv> FetchRelayInventory() EXCLUSIVE_LOCKS_REQUIRED(!cs_relay);
    void CheckAndRemove() EXCLUSIVE_LOCKS_REQUIRED(!cs_store);
    /** Number of orphan votes currently held, so the MAX_ORPHAN_VOTES bound can be asserted. */
    [[nodiscard]] size_t GetOrphanVoteCount() const EXCLUSIVE_LOCKS_REQUIRED(!cs_store);
    std::pair<std::vector<uint256>, std::vector<uint256>> FetchGovernanceObjectVotes(
        size_t peers_per_hash_max, int64_t now, std::map<uint256, std::map<CService, int64_t>>& map_asked_recently) const
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);
    /** Build bloom filter of existing votes for a governance object (for sync requests) */
    [[nodiscard]] CBloomFilter GetVoteBloomFilter(const uint256& nHash) const EXCLUSIVE_LOCKS_REQUIRED(!cs_store);
    /** Returns inventory items for all syncable (non-deleted, non-expired) governance objects */
    [[nodiscard]] std::vector<CInv> GetSyncableObjectInvs() const EXCLUSIVE_LOCKS_REQUIRED(!cs_store);
    /** Returns inventory items for syncable votes on a specific object, filtered by bloom filter */
    [[nodiscard]] std::vector<CInv> GetSyncableVoteInvs(const uint256& nProp, const CBloomFilter& filter) const
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);
    bool ProcessObject(const std::string& peer_str, const uint256& hash, CGovernanceObject& govobj)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !cs_store, !cs_relay);

    CDeterministicMNManager& GetMNManager();
    /** Process a governance vote. Returns true on success.
     *  If the vote is for an unknown object (orphan), hashToRequest is set to the object hash. */
    bool ProcessVote(const CGovernanceVote& vote, CGovernanceException& exception, uint256& hashToRequest)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);


private:
    // Internal counterparts to "Thread-safe accessors"
    void AddPostponedObjectInternal(const CGovernanceObject& govobj)
        EXCLUSIVE_LOCKS_REQUIRED(cs_store);
    std::shared_ptr<CGovernanceObject> FindGovernanceObjectInternal(const uint256& nHash)
        EXCLUSIVE_LOCKS_REQUIRED(cs_store);

    std::shared_ptr<const CGovernanceObject> FindConstGovernanceObjectInternal(const uint256& nHash) const
        EXCLUSIVE_LOCKS_REQUIRED(cs_store);

    // Internal counterpart to "Signer interface"
    void AddGovernanceObjectInternal(CGovernanceObject& govobj, const std::string& peer_str)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main, cs_store, !cs_relay);

    // ...
    void MasternodeRateUpdate(const CGovernanceObject& govobj)
        EXCLUSIVE_LOCKS_REQUIRED(cs_store);

    bool MasternodeRateCheck(const CGovernanceObject& govobj, bool fUpdateFailStatus, bool fForce, bool& fRateCheckBypassed)
        EXCLUSIVE_LOCKS_REQUIRED(cs_store);

    void CheckPostponedObjects()
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main, cs_store, !cs_relay);

    void InitOnLoad()
        EXCLUSIVE_LOCKS_REQUIRED(!cs_store);

    void CheckOrphanVotes(CGovernanceObject& govobj)
        EXCLUSIVE_LOCKS_REQUIRED(cs_store, !cs_relay);

    /** Drop orphan votes whose parent object never arrived within GOVERNANCE_ORPHAN_EXPIRATION_TIME. */
    void ExpireOrphanVotes()
        EXCLUSIVE_LOCKS_REQUIRED(cs_store);

    void RebuildIndexes()
        EXCLUSIVE_LOCKS_REQUIRED(cs_store);

    void RemoveInvalidVotes()
        EXCLUSIVE_LOCKS_REQUIRED(cs_store);
};

#endif // BITCOIN_GOVERNANCE_GOVERNANCE_H
