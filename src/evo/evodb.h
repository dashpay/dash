// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_EVODB_H
#define BITCOIN_EVO_EVODB_H

#include <dbwrapper.h>
#include <evo/snapshot_types.h>
#include <kernel/cs_main.h>
#include <logging.h>
#include <serialize.h>
#include <streams.h>
#include <sync.h>
#include <uint256.h>
#include <util/result.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <thread>

namespace util {
struct DbWrapperParams;
} // namespace util

// "b_b" was used in the initial version of deterministic MN storage
// "b_b2" was used after compact diffs were introduced
// "b_b3" was used after masternode type introduction in evoDB
// "b_b4" was used after storing protx version for each masternode in evoDB
static const std::string EVODB_BEST_BLOCK = "b_b4";
// Released Dash software has no snapshot-chainstate detection, ignores the
// chainstate_snapshot directory, and loads the chainstate directory together
// with this legacy marker. That pair is the background chainstate's own coins
// and marker, so downgrading mid-snapshot safely reverts to background IBD.
static const std::string EVODB_DUAL_CHAINSTATE = "b_dcs";
static const std::string EVODB_SNAPSHOT_MNLIST_HASH = "b_dcs_mn";
static const std::string EVODB_BACKGROUND_MNLIST_HASH = "b_dcs_bg_mn";
//! The evo section the live snapshot generation was seeded from.
static const std::string EVODB_SNAPSHOT_EVO_SECTION = "b_dcs_evo";
// Snapshot-identity lifecycle metadata. It lives in the legacy keyspace and is
// never part of a generation namespace.
static const std::string EVODB_GENERATION_RECORD = "b_evo_gen";
static const std::string EVODB_GENERATION_NEXT = "b_evo_gen_next";
static const std::string EVODB_PROMOTION_JOURNAL = "b_evo_promote";
// Every key the snapshot identity writes is stored as
// (EVODB_GENERATION_NAMESPACE, generation, key).
static const std::string EVODB_GENERATION_NAMESPACE = "S";
// Mutable singleton records whose snapshot-identity value supersedes the
// NORMAL one when a generation is promoted.
static const std::string EVODB_LLMQ_BEST_BLOCK = "q_bbu2";

enum class EvoDbIdentity {
    NORMAL,
    SNAPSHOT,
};

/** Thrown when an independently derived block payload disagrees with the copy
 *  already recorded in EvoDB. This is local state corruption (or a
 *  cross-chainstate divergence bug), never evidence about the block being
 *  processed, so it must abort the node rather than invalidate the block. */
class EvoDbInconsistencyError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

class CEvoDB;

/** Names the Evo state one operation reads and writes: the NORMAL view, or
 *  one generation of the SNAPSHOT view, whose snapshot starts at `base`. */
struct EvoViewToken {
    const CEvoDB* db{nullptr};
    EvoDbIdentity identity{EvoDbIdentity::NORMAL};
    uint32_t generation{0};
    uint256 base{};
    int base_height{-1};

    bool operator==(const EvoViewToken&) const = default;
};

enum class EvoGenerationState : uint8_t {
    UNPUBLISHED = 0,
    PUBLISHED = 1,
    REJECTED = 2,
    PROMOTING = 3,
};

/** Lifecycle record of one snapshot-identity generation. */
struct EvoGenerationRecord {
    //! Local on-disk schema of generations and the promotion journal. It is
    //! independent of the snapshot wire format.
    static constexpr uint16_t SCHEMA_VERSION{1};

    uint16_t schema_version{SCHEMA_VERSION};
    uint32_t generation{0};
    EvoGenerationState state{EvoGenerationState::UNPUBLISHED};
    uint256 base;
    int32_t base_height{-1};
    uint256 tip;
    //! Fingerprint of the consensus configuration the generation was
    //! published under; see EvoSnapshotConsensusConfig.
    uint256 config_fingerprint;

    SERIALIZE_METHODS(EvoGenerationRecord, obj)
    {
        uint8_t state{static_cast<uint8_t>(obj.state)};
        READWRITE(obj.schema_version, obj.generation, state, obj.base, obj.base_height, obj.tip, obj.config_fingerprint);
        SER_READ(obj, obj.state = static_cast<EvoGenerationState>(state));
    }
};

enum class EvoPromotionPhase : uint8_t {
    //! Decided, before the first coins-directory rename.
    PREPARED = 0,
    //! Both coins-directory renames are durable; merging into NORMAL keys.
    MERGING = 1,
    //! Every record is merged; the generation namespace may be collected.
    MERGED = 2,
};

/** Journal of a snapshot-generation promotion, written before the first
 *  coins-directory rename and removed together with the final markers. */
struct EvoPromotionJournal {
    uint16_t schema_version{EvoGenerationRecord::SCHEMA_VERSION};
    uint32_t generation{0};
    uint256 base;
    uint256 tip;
    uint256 base_mn_list_hash;
    EvoPromotionPhase phase{EvoPromotionPhase::PREPARED};
    //! Generation-relative key the merge resumes after; empty to start over.
    std::vector<uint8_t> cursor;

    SERIALIZE_METHODS(EvoPromotionJournal, obj)
    {
        uint8_t phase{static_cast<uint8_t>(obj.phase)};
        READWRITE(obj.schema_version, obj.generation, obj.base, obj.tip, obj.base_mn_list_hash, phase, obj.cursor);
        SER_READ(obj, obj.phase = static_cast<EvoPromotionPhase>(phase));
    }
};

/** Read side of one view of the EvoDB keyspace: every key is stored behind
 *  `prefix`. The NORMAL view has an empty prefix and skips all generation
 *  namespaces, so neither view ever sees the other's records. */
class CEvoDBStore
{
public:
    class Iterator
    {
    public:
        Iterator(const CEvoDBStore& store, CDBIterator* it) :
            m_store{store},
            m_it{it}
        {
        }
        void SeekToFirst()
        {
            m_it->Seek(m_store.m_prefix);
            SkipForeign();
        }
        void Seek(const DataStream& key)
        {
            m_it->Seek(m_store.Key(key));
            SkipForeign();
        }
        void Next()
        {
            m_it->Next();
            SkipForeign();
        }
        bool Valid() { return m_it->Valid() && m_store.Owns(m_it->GetKey()); }
        DataStream GetKey()
        {
            DataStream key{m_it->GetKey()};
            key.ignore(m_store.m_prefix.size());
            return key;
        }
        unsigned int GetKeySize() { return m_it->GetKeySize() - m_store.m_prefix.size(); }

    private:
        void SkipForeign();

        const CEvoDBStore& m_store;
        std::unique_ptr<CDBIterator> m_it;
    };

    CEvoDBStore(CDBWrapper& db, DataStream prefix) :
        m_db{db},
        m_prefix{std::move(prefix)}
    {
    }

    //! Prefix of generation `generation`'s namespace; generation 0 names the
    //! NORMAL view, whose prefix is empty.
    static DataStream GenerationPrefix(uint32_t generation);

    DataStream Key(const DataStream& key) const
    {
        DataStream out{m_prefix};
        out.write(Span<const std::byte>{key.data(), key.size()});
        return out;
    }
    bool Owns(const DataStream& raw_key) const;

    template <typename V>
    bool Read(const DataStream& key, V& value) const
    {
        return m_db.Read(Key(key), value);
    }
    bool Exists(const DataStream& key) const { return m_db.Exists(Key(key)); }
    Iterator* NewIterator() const { return new Iterator(*this, m_db.NewIterator()); }

private:
    CDBWrapper& m_db;
    const DataStream m_prefix;
};

/** Write side of one view: a commit target that stores keys behind the view's
 *  prefix. */
class CEvoDBStoreBatch
{
public:
    CEvoDBStoreBatch(CDBBatch& batch, const CEvoDBStore& store) :
        m_batch{batch},
        m_store{store}
    {
    }
    template <typename V>
    void Write(const DataStream& key, const V& value)
    {
        m_batch.Write(m_store.Key(key), value);
    }
    void Erase(const DataStream& key) { m_batch.Erase(m_store.Key(key)); }

private:
    CDBBatch& m_batch;
    const CEvoDBStore& m_store;
};

/** Binds the calling thread's Evo accesses on `token.db` to `token` until
 *  destruction, which restores the enclosing binding. Scopes nest; the
 *  innermost one for a CEvoDB wins. A scope must agree with a transaction the
 *  same thread holds open on that CEvoDB, or construction raises
 *  EvoViewBindingError. */
class EvoViewScope
{
public:
    explicit EvoViewScope(const EvoViewToken& token);
    ~EvoViewScope();
    EvoViewScope(const EvoViewScope&) = delete;
    EvoViewScope& operator=(const EvoViewScope&) = delete;

    //! The innermost token this thread bound for `db`, if any.
    static const EvoViewToken* Innermost(const CEvoDB& db);
    //! The innermost token this thread bound for any CEvoDB, if any.
    static std::optional<EvoViewToken> InnermostAny();

private:
    const EvoViewToken m_token;
    const EvoViewScope* const m_outer;
};

class CEvoDBScopedCommitter
{
private:
    CEvoDB& evoDB;
    const EvoDbIdentity identity;
    bool didCommitOrRollback{false};

public:
    CEvoDBScopedCommitter(CEvoDB& _evoDB, EvoDbIdentity identity);
    ~CEvoDBScopedCommitter();

    void Commit() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    void Rollback() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
};

class CEvoDB
{
public:
    mutable Mutex cs;
private:
    std::unique_ptr<CDBWrapper> db;

    using RootTransaction = CDBTransaction<CEvoDBStore, CEvoDBStoreBatch>;
    using CurTransaction = CDBTransaction<RootTransaction, RootTransaction>;

    struct TransactionContext {
        CEvoDBStore store;
        CDBBatch root_batch;
        CEvoDBStoreBatch commit_target;
        RootTransaction root_transaction;
        CurTransaction cur_transaction;

        TransactionContext(CDBWrapper& db, DataStream prefix) :
            store{db, std::move(prefix)},
            root_batch{db},
            commit_target{root_batch, store},
            root_transaction{store, commit_target},
            cur_transaction{root_transaction, root_transaction}
        {
        }
    };

    std::map<EvoDbIdentity, std::unique_ptr<TransactionContext>> transaction_contexts GUARDED_BY(cs);
    // The open transaction belongs to the thread that began it: it is the
    // identity of one validation execution context, not of the process. Other
    // threads reading concurrently (RPC, P2P serving, quorum construction after
    // it has released cs_main) must keep resolving against m_default_identity,
    // or a background NORMAL transaction would silently redirect them away from
    // the active snapshot's overlay.
    std::optional<EvoDbIdentity> active_transaction GUARDED_BY(cs);
    std::thread::id active_transaction_thread GUARDED_BY(cs);
    // Identity used by reads and writes outside any transaction. Tracks the
    // active chainstate so transaction-less consumers (RPC, mempool, miner,
    // P2P serving) resolve against active-chain state rather than always
    // falling back to NORMAL.
    EvoDbIdentity m_default_identity GUARDED_BY(cs){EvoDbIdentity::NORMAL};

    TransactionContext& GetContext(EvoDbIdentity identity) EXCLUSIVE_LOCKS_REQUIRED(cs);
    const TransactionContext& GetContext(EvoDbIdentity identity) const EXCLUSIVE_LOCKS_REQUIRED(cs);
    EvoDbIdentity GetCurrentIdentity() const EXCLUSIVE_LOCKS_REQUIRED(cs) { return ResolveView().identity; }
    EvoViewToken MakeView(EvoDbIdentity identity) const EXCLUSIVE_LOCKS_REQUIRED(cs);
    bool IsLiveLocked(const EvoViewToken& view) const EXCLUSIVE_LOCKS_REQUIRED(cs);
    EvoViewToken ResolveView() const EXCLUSIVE_LOCKS_REQUIRED(cs);
    bool HoldsTransaction() const EXCLUSIVE_LOCKS_REQUIRED(cs)
    {
        return active_transaction.has_value() && active_transaction_thread == std::this_thread::get_id();
    }

public:
    CEvoDB() = delete;
    CEvoDB(const CEvoDB&) = delete;
    CEvoDB& operator=(const CEvoDB&) = delete;
    explicit CEvoDB(const util::DbWrapperParams& db_params);
    ~CEvoDB();

    std::unique_ptr<CEvoDBScopedCommitter> BeginTransaction(EvoDbIdentity identity = EvoDbIdentity::NORMAL)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !cs);

    /** Whether a block-scoped transaction is open. Writes performed outside one are never
     *  committed and trip the clean-transaction assertion at the next root commit, so callers
     *  reachable from transaction-less contexts (mempool acceptance, mining, RPC) must skip
     *  optional persistence when this is false. */
    bool HasActiveTransaction() const EXCLUSIVE_LOCKS_REQUIRED(!cs)
    {
        LOCK(cs);
        return HoldsTransaction();
    }

    //! Changes whenever cached Evo-derived state of any view may have gone
    //! stale: a CEvoDB was opened, or a snapshot generation was rejected or
    //! promoted. EvoViewCache drops every partition when it changes.
    static uint64_t ViewCacheEpoch() { return g_view_cache_epoch.load(); }
    //! Run `f(ViewCacheEpoch())` if `view` is a live view of this CEvoDB (NORMAL,
    //! or the snapshot generation bound now, published or not), and return
    //! whether it ran. A snapshot view is checked and `f` run under the lock
    //! that retires generations, so this CEvoDB cannot retire the generation
    //! before `f` returns. `f` must not call back into this CEvoDB.
    template <typename F>
    bool WithLiveView(const EvoViewToken& view, F&& f) const EXCLUSIVE_LOCKS_REQUIRED(!cs)
    {
        if (view.db != this) return false;
        // NORMAL never retires, so its views need no lock.
        if (view.identity == EvoDbIdentity::NORMAL) {
            f(g_view_cache_epoch.load());
            return true;
        }
        LOCK(cs);
        if (!IsLiveLocked(view)) return false;
        f(g_view_cache_epoch.load());
        return true;
    }

    //! The view this thread's Evo accesses resolve to: its open transaction,
    //! else its innermost EvoViewScope, else the default identity. While a
    //! published snapshot generation is live there is no default: an unbound access, or
    //! one bound to a retired generation, raises EvoViewBindingError before it
    //! touches any state. So does every read, write, iterator and cache lookup
    //! that resolves its view through here.
    EvoViewToken CurrentView() const EXCLUSIVE_LOCKS_REQUIRED(!cs);
    //! Whether this thread's accesses currently resolve to exactly `view`.
    bool IsCurrentView(const EvoViewToken& view) const EXCLUSIVE_LOCKS_REQUIRED(!cs);
    //! The current view of a chainstate with the given identity.
    EvoViewToken ViewFor(EvoDbIdentity identity) const EXCLUSIVE_LOCKS_REQUIRED(!cs);
    //! Whether this thread already binds its accesses to this CEvoDB, through
    //! an open transaction or an EvoViewScope.
    bool IsBound() const EXCLUSIVE_LOCKS_REQUIRED(!cs);
    //! Whether a published snapshot generation is live, so that two views exist.
    bool HasLiveGeneration() const { return m_has_live_generation.load(); }
    //! The view of the active chainstate.
    EvoViewToken ActiveView() const EXCLUSIVE_LOCKS_REQUIRED(!cs);
    //! Whether `view` still names a view of this CEvoDB: NORMAL, or the snapshot
    //! generation bound now, whether or not it is published yet.
    bool IsLive(const EvoViewToken& view) const EXCLUSIVE_LOCKS_REQUIRED(!cs);
    //! Raise EvoViewBindingError if this thread holds a transaction on this
    //! CEvoDB that disagrees with `token`.
    void CheckScopeAgainstTransaction(const EvoViewToken& token) const EXCLUSIVE_LOCKS_REQUIRED(!cs);

    CurTransaction& GetCurTransaction() EXCLUSIVE_LOCKS_REQUIRED(cs)
    {
        AssertLockHeld(cs); // lock must be held from outside as long as the DB transaction is used
        return GetContext(GetCurrentIdentity()).cur_transaction;
    }

    template <typename K, typename V>
    bool Read(const K& key, V& value) EXCLUSIVE_LOCKS_REQUIRED(!cs)
    {
        LOCK(cs);
        return GetContext(GetCurrentIdentity()).cur_transaction.Read(key, value);
    }

    template <typename K, typename V>
    void Write(const K& key, const V& value) EXCLUSIVE_LOCKS_REQUIRED(!cs)
    {
        LOCK(cs);
        GetContext(GetCurrentIdentity()).cur_transaction.Write(key, value);
    }

    /**
     * Write immutable block-derived data, accepting an identical existing value
     * in the current view. V must have canonical serialization: its bytes must
     * be a pure function of its logical content. Audited call-site types are
     * CDeterministicMNListDiff, CDeterministicMNList,
     * AbstractEHFManager::Signals, the mined-commitment pair, and CCreditPool.
     */
    template <typename K, typename V>
    bool WriteDerived(const K& key, const V& value) EXCLUSIVE_LOCKS_REQUIRED(!cs)
    {
        LOCK(cs);
        auto& transaction = GetContext(GetCurrentIdentity()).cur_transaction;
        V existing;
        if (transaction.Read(key, existing)) {
            if (!SameSerialization(existing, value)) {
                LogPrintf("ERROR: CEvoDB::WriteDerived: block-derived payload mismatch in EvoDB\n");
                return false;
            }
            return true;
        }
        transaction.Write(key, value);
        return true;
    }

    /** Write `value` under `key` in the current view immediately and durably,
     *  outside any transaction. */
    template <typename K, typename V>
    bool WriteDurable(const K& key, const V& value) EXCLUSIVE_LOCKS_REQUIRED(!cs)
    {
        LOCK(cs);
        const auto& store{GetContext(GetCurrentIdentity()).store};
        CDBBatch batch{*db};
        DataStream ssKey{};
        ssKey << key;
        batch.Write(store.Key(ssKey), value);
        return db->WriteBatch(batch);
    }

    template <typename K>
    bool Exists(const K& key) EXCLUSIVE_LOCKS_REQUIRED(!cs)
    {
        LOCK(cs);
        return GetContext(GetCurrentIdentity()).cur_transaction.Exists(key);
    }

    template <typename K>
    void Erase(const K& key) EXCLUSIVE_LOCKS_REQUIRED(!cs)
    {
        LOCK(cs);
        GetContext(GetCurrentIdentity()).cur_transaction.Erase(key);
    }

    /** The raw database, keyed in the NORMAL view's legacy keyspace. Only
     *  NORMAL-only lifecycle operations (DMN migration and repair, quorum
     *  cleanup) may use it: it bypasses view isolation. */
    CDBWrapper& GetRawDB()
    {
        return *db;
    }

    [[nodiscard]] size_t GetMemoryUsage() const EXCLUSIVE_LOCKS_REQUIRED(!cs)
    {
        LOCK(cs);
        size_t result{0};
        for (const auto& [_, context] : transaction_contexts) {
            result += context->root_transaction.GetMemoryUsage();
        }
        return result;
    }

    bool CommitRootTransaction(EvoDbIdentity identity = EvoDbIdentity::NORMAL, bool sync = false)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !cs);

    bool IsEmpty() { return db->IsEmpty(); }

    //! Set the identity used by reads/writes outside any transaction. Must
    //! track the active chainstate: snapshot activation sets SNAPSHOT;
    //! PromoteSnapshotMarkers/DiscardSnapshotMarkers reset it to NORMAL.
    void SetDefaultIdentity(EvoDbIdentity identity) EXCLUSIVE_LOCKS_REQUIRED(!cs)
    {
        LOCK(cs);
        m_default_identity = identity;
    }

    bool ReadBestBlock(EvoDbIdentity identity, uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(!cs);
    bool VerifyBestBlock(EvoDbIdentity identity, const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(!cs);
    void WriteBestBlock(EvoDbIdentity identity, const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(!cs);
    bool HasDualChainstateMarker() EXCLUSIVE_LOCKS_REQUIRED(!cs);
    //! Lifecycle markers are written to the legacy keyspace immediately, never
    //! into a transaction or a generation namespace.
    void WriteDualChainstateMarker() EXCLUSIVE_LOCKS_REQUIRED(!cs);
    void WriteSnapshotBaseMNListHash(const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(!cs);
    bool ReadSnapshotBaseMNListHash(uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(!cs);
    void WriteBackgroundMNListHash(const uint256& block_hash, const uint256& mn_list_hash) EXCLUSIVE_LOCKS_REQUIRED(!cs);
    bool ReadBackgroundMNListHash(uint256& block_hash, uint256& mn_list_hash) EXCLUSIVE_LOCKS_REQUIRED(!cs);

    //! The live (published or promoting) snapshot generation, if any.
    std::optional<EvoGenerationRecord> GetSnapshotGeneration() const EXCLUSIVE_LOCKS_REQUIRED(!cs);

    /** Start a new, unpublished snapshot generation for a snapshot whose base
     *  block is `base`, and bind the SNAPSHOT identity to it. Fails if a
     *  generation is already live. */
    bool CreateSnapshotGeneration(const uint256& base, int base_height) EXCLUSIVE_LOCKS_REQUIRED(!cs);

    /** Publish the bound generation together with the dual-chainstate markers,
     *  the consensus-configuration fingerprint it is valid for and the encoded
     *  evo section it was seeded from (empty for a UTXO-only snapshot). */
    bool PublishSnapshotGeneration(const uint256& config_fingerprint, const std::optional<uint256>& base_mn_list_hash,
                                   const std::vector<uint8_t>& evo_section = {}) EXCLUSIVE_LOCKS_REQUIRED(!cs);
    //! The evo section the live generation was published with, if any.
    std::optional<std::vector<uint8_t>> ReadRetainedEvoSection() const EXCLUSIVE_LOCKS_REQUIRED(!cs);

    /** Startup: collect unpublished and rejected generations, then bind the
     *  live one. Fails closed if the live generation was published under a
     *  different consensus configuration, or if dual-chainstate markers
     *  predate generation isolation. */
    util::Result<void> LoadSnapshotGenerations(const uint256& config_fingerprint) EXCLUSIVE_LOCKS_REQUIRED(!cs);

    /** Abandon the bound generation of a snapshot whose activation failed. */
    bool AbandonSnapshotGeneration() EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !cs);

    /** Record the decision to promote the bound generation at `snapshot_tip`.
     *  Must precede the first coins-directory rename. */
    bool PrepareSnapshotPromotion(const uint256& snapshot_tip) EXCLUSIVE_LOCKS_REQUIRED(!cs);
    std::optional<EvoPromotionJournal> ReadPromotionJournal() const EXCLUSIVE_LOCKS_REQUIRED(!cs);

    /**
     * Merge the promoted generation into the NORMAL keyspace, then atomically
     * write the promoted best block and remove the journal and every
     * dual-chainstate marker. Resumable from any crash after
     * PrepareSnapshotPromotion. Both identity transaction trees must already be
     * fully committed by the caller.
     */
    bool PromoteSnapshotMarkers(const uint256& expected_snapshot_tip) EXCLUSIVE_LOCKS_REQUIRED(!cs);

    /** Reject the bound generation and remove the dual-chainstate markers in
     *  one synced batch, preserving NORMAL. The REJECTED record remains until
     *  CollectRejectedGenerations(), after the coins directory is invalidated. */
    bool DiscardSnapshotMarkers() EXCLUSIVE_LOCKS_REQUIRED(!cs);
    //! Whether a rejected generation still awaits its coins invalidation.
    bool HasRejectedGeneration() const EXCLUSIVE_LOCKS_REQUIRED(!cs);
    //! Remove every rejected generation's records and namespace (idempotent).
    bool CollectRejectedGenerations() EXCLUSIVE_LOCKS_REQUIRED(!cs);

    enum class PromotionFailpoint {
        NONE,
        MID_MERGE,
        AFTER_MERGE,
        AFTER_COLLECT,
    };
    //! Make PromoteSnapshotMarkers stop with failure at `failpoint`, as if the
    //! node crashed there.
    void SetPromotionFailpointForTesting(PromotionFailpoint failpoint) EXCLUSIVE_LOCKS_REQUIRED(!cs)
    {
        LOCK(cs);
        m_promotion_failpoint = failpoint;
    }
    //! Records merged per batch, so tests can stop a merge part-way.
    void SetMergeBatchSizeForTesting(size_t size) EXCLUSIVE_LOCKS_REQUIRED(!cs)
    {
        LOCK(cs);
        m_merge_batch_size = size;
    }

    bool VerifyBestBlock(const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(!cs) { return VerifyBestBlock(EvoDbIdentity::NORMAL, hash); }
    void WriteBestBlock(const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(!cs) { WriteBestBlock(EvoDbIdentity::NORMAL, hash); }

private:
    // only CEvoDBScopedCommitter is allowed to invoke these
    friend class CEvoDBScopedCommitter;
    void CommitCurTransaction(EvoDbIdentity identity) EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !cs);
    void RollbackCurTransaction(EvoDbIdentity identity) EXCLUSIVE_LOCKS_REQUIRED(::cs_main, !cs);

    template <typename V>
    static bool SameSerialization(const V& a, const V& b)
    {
        CDataStream a_stream{SER_DISK, CLIENT_VERSION};
        CDataStream b_stream{SER_DISK, CLIENT_VERSION};
        a_stream << a;
        b_stream << b;
        return a_stream.size() == b_stream.size() && std::equal(a_stream.begin(), a_stream.end(), b_stream.begin());
    }

    void BindSnapshotGeneration(std::optional<EvoGenerationRecord> record) EXCLUSIVE_LOCKS_REQUIRED(cs);
    void UpdateLiveGeneration() EXCLUSIVE_LOCKS_REQUIRED(cs);
    //! Whether no transaction is open and every transaction tree is committed.
    bool TransactionsClean() const EXCLUSIVE_LOCKS_REQUIRED(cs);
    bool CollectGeneration(uint32_t generation, bool erase_record) EXCLUSIVE_LOCKS_REQUIRED(cs);
    std::vector<uint32_t> RejectedGenerations() const EXCLUSIVE_LOCKS_REQUIRED(cs);
    bool MergeGeneration(EvoPromotionJournal& journal) EXCLUSIVE_LOCKS_REQUIRED(cs);
    bool DiscardBoundGeneration() EXCLUSIVE_LOCKS_REQUIRED(cs);

    static inline std::atomic<uint64_t> g_view_cache_epoch{0};
    std::atomic<bool> m_has_live_generation{false};

    //! The live generation the SNAPSHOT identity reads and writes.
    std::optional<EvoGenerationRecord> m_snapshot_generation GUARDED_BY(cs);
    PromotionFailpoint m_promotion_failpoint GUARDED_BY(cs){PromotionFailpoint::NONE};
    size_t m_merge_batch_size GUARDED_BY(cs){10000};
};

/** Binds an active-chain dispatch (an RPC call, a P2P message, an interfaces
 *  call, a service work round) to the active chainstate's Evo view for its
 *  duration. Binds nothing while only one view exists, or when the thread is
 *  already bound (a captured background token, an open transaction). */
class ActiveEvoViewScope
{
public:
    explicit ActiveEvoViewScope(const CEvoDB* evodb)
    {
        if (evodb == nullptr || !evodb->HasLiveGeneration() || evodb->IsBound()) return;
        m_scope.emplace(evodb->ActiveView());
    }

private:
    std::optional<EvoViewScope> m_scope;
};

//! Run one round of active-chain service work bound to the active view. A
//! local Evo state error ends the round, never the thread.
template <typename F>
void RunActiveEvoRound(const CEvoDB* evodb, const char* what, F&& round)
{
    try {
        const ActiveEvoViewScope evo_view{evodb};
        round();
    } catch (const evo::LocalStateError& e) {
        LogPrintf("ERROR: %s: Evo state unavailable: %s\n", what, e.what());
    }
}

/** The Evo view binding of a task handed to another thread: captured from the
 *  enqueuing thread's innermost scope and reinstalled when the task runs, so
 *  the executing thread's own binding never replaces it. A task whose
 *  snapshot generation was retired in between is dropped. */
class EvoViewCapture
{
public:
    EvoViewCapture() :
        m_view{EvoViewScope::InnermostAny()}
    {
    }

    enum class Outcome {
        RAN,     //!< the task ran to completion
        FAILED,  //!< the task started and ended in a local Evo state error
        DROPPED, //!< the task never started
    };

    //! Run `task` under the captured binding. A task whose generation was
    //! retired, or whose binding cannot be installed, is dropped.
    template <typename F>
    Outcome Run(F&& task) const
    {
        std::optional<EvoViewScope> scope;
        if (m_view) {
            if (!m_view->db->IsLive(*m_view)) {
                LogPrint(BCLog::VALIDATION, "Dropping a task bound to retired snapshot generation %u\n",
                         m_view->generation);
                return Outcome::DROPPED;
            }
            try {
                scope.emplace(*m_view);
            } catch (const evo::LocalStateError& e) {
                LogPrintf("ERROR: dropping a queued task: %s\n", e.what());
                return Outcome::DROPPED;
            }
        }
        try {
            task();
        } catch (const evo::LocalStateError& e) {
            LogPrintf("ERROR: Evo state unavailable to a queued task: %s\n", e.what());
            return Outcome::FAILED;
        }
        return Outcome::RAN;
    }

private:
    std::optional<EvoViewToken> m_view;
};

//! Wrap `task` so that it runs under the calling thread's current Evo view
//! binding wherever it is eventually executed. `on_drop` releases whatever
//! the enqueuer claimed for the task when it is dropped without starting; a
//! task that started releases its claims itself.
template <typename F, typename D = void (*)()>
auto WithCapturedEvoView(F task, D on_drop = [] {})
{
    return [task = std::move(task), on_drop = std::move(on_drop), evo_view = EvoViewCapture{}](auto&&... args) mutable {
        if (evo_view.Run([&] { task(std::forward<decltype(args)>(args)...); }) == EvoViewCapture::Outcome::DROPPED) {
            on_drop();
        }
    };
}

#endif // BITCOIN_EVO_EVODB_H
