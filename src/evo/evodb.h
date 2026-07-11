// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_EVODB_H
#define BITCOIN_EVO_EVODB_H

#include <dbwrapper.h>
#include <sync.h>

#include <algorithm>
#include <map>
#include <optional>

class uint256;
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

// TODO(assumeutxo): snapshot completion must promote the SNAPSHOT marker to
// the legacy key when chainstate_snapshot is renamed over chainstate.
enum class EvoDbIdentity {
    NORMAL,
    SNAPSHOT,
};

class CEvoDB;

class CEvoDBScopedCommitter
{
private:
    CEvoDB& evoDB;
    const EvoDbIdentity identity;
    bool didCommitOrRollback{false};

public:
    CEvoDBScopedCommitter(CEvoDB& _evoDB, EvoDbIdentity identity);
    ~CEvoDBScopedCommitter();

    void Commit();
    void Rollback();
};

class CEvoDB
{
public:
    Mutex cs;
private:
    std::unique_ptr<CDBWrapper> db;

    using RootTransaction = CDBTransaction<CDBWrapper, CDBBatch>;
    using CurTransaction = CDBTransaction<RootTransaction, RootTransaction>;

    struct TransactionContext {
        CDBBatch root_batch;
        RootTransaction root_transaction;
        CurTransaction cur_transaction;

        explicit TransactionContext(CDBWrapper& db) :
            root_batch{db},
            root_transaction{db, root_batch},
            cur_transaction{root_transaction, root_transaction}
        {
        }
    };

    std::map<EvoDbIdentity, std::unique_ptr<TransactionContext>> transaction_contexts;
    std::optional<EvoDbIdentity> active_transaction;
    EvoDbIdentity active_chainstate_identity{EvoDbIdentity::NORMAL};

    TransactionContext& GetContext(EvoDbIdentity identity);
    const TransactionContext& GetContext(EvoDbIdentity identity) const;
    EvoDbIdentity GetCurrentIdentity() const;

public:
    CEvoDB() = delete;
    CEvoDB(const CEvoDB&) = delete;
    CEvoDB& operator=(const CEvoDB&) = delete;
    explicit CEvoDB(const util::DbWrapperParams& db_params);
    ~CEvoDB();

    /** Select the overlay used by reads outside a scoped transaction. */
    void SetActiveChainstateIdentity(EvoDbIdentity identity) EXCLUSIVE_LOCKS_REQUIRED(!cs);
    std::unique_ptr<CEvoDBScopedCommitter> BeginTransaction(EvoDbIdentity identity = EvoDbIdentity::NORMAL) EXCLUSIVE_LOCKS_REQUIRED(!cs);

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
     * Write immutable block-derived data, accepting an identical existing value.
     * TODO(assumeutxo): WriteDerived spot-checks are not the holistic base-state
     * comparison required at snapshot completion.
     */
    template <typename K, typename V>
    bool WriteDerived(const K& key, const V& value) EXCLUSIVE_LOCKS_REQUIRED(!cs)
    {
        LOCK(cs);
        const EvoDbIdentity identity = GetCurrentIdentity();
        auto& transaction = GetContext(identity).cur_transaction;
        V existing;
        bool write{true};
        if (transaction.Read(key, existing)) {
            CDataStream existing_stream{SER_DISK, CLIENT_VERSION};
            CDataStream value_stream{SER_DISK, CLIENT_VERSION};
            existing_stream << existing;
            value_stream << value;
            const bool matches = existing_stream.size() == value_stream.size() &&
                                 std::equal(existing_stream.begin(), existing_stream.end(), value_stream.begin());
            if (!matches) {
                LogPrintf("ERROR: CEvoDB::WriteDerived: block-derived payload mismatch in EvoDB\n");
                return false;
            }
            write = false;
        }

        // Cross-identity writes of the same key are disjoint by construction
        // (background validates blocks <= snapshot base; the snapshot chainstate
        // validates blocks > base; seeded data is flushed at activation). This check is
        // verification-only insurance for that invariant and must never suppress the
        // caller's own write.
        for (const auto& [other_identity, context] : transaction_contexts) {
            if (other_identity == identity || !context) continue;
            V pending;
            if (!context->root_transaction.ReadPending(key, pending)) continue;

            CDataStream pending_stream{SER_DISK, CLIENT_VERSION};
            CDataStream value_stream{SER_DISK, CLIENT_VERSION};
            pending_stream << pending;
            value_stream << value;
            const bool matches = pending_stream.size() == value_stream.size() &&
                                 std::equal(pending_stream.begin(), pending_stream.end(), value_stream.begin());
            if (!matches) {
                LogPrintf("ERROR: CEvoDB::WriteDerived: cross-identity block-derived payload mismatch in EvoDB\n");
                return false;
            }
        }

        if (write) transaction.Write(key, value);
        return true;
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

    CDBWrapper& GetRawDB()
    {
        return *db;
    }

    [[nodiscard]] size_t GetMemoryUsage() const
    {
        size_t result{0};
        for (const auto& [_, context] : transaction_contexts) {
            result += context->root_transaction.GetMemoryUsage();
        }
        return result;
    }

    bool CommitRootTransaction(EvoDbIdentity identity = EvoDbIdentity::NORMAL) EXCLUSIVE_LOCKS_REQUIRED(!cs);

    bool IsEmpty() { return db->IsEmpty(); }

    bool ReadBestBlock(EvoDbIdentity identity, uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(!cs);
    bool VerifyBestBlock(EvoDbIdentity identity, const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(!cs);
    void WriteBestBlock(EvoDbIdentity identity, const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(!cs);
    void WriteDualChainstateMarker() EXCLUSIVE_LOCKS_REQUIRED(!cs);
    bool HasDualChainstateMarker() EXCLUSIVE_LOCKS_REQUIRED(!cs);

    bool VerifyBestBlock(const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(!cs) { return VerifyBestBlock(EvoDbIdentity::NORMAL, hash); }
    void WriteBestBlock(const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(!cs) { WriteBestBlock(EvoDbIdentity::NORMAL, hash); }

private:
    // only CEvoDBScopedCommitter is allowed to invoke these
    friend class CEvoDBScopedCommitter;
    void CommitCurTransaction(EvoDbIdentity identity) EXCLUSIVE_LOCKS_REQUIRED(!cs);
    void RollbackCurTransaction(EvoDbIdentity identity) EXCLUSIVE_LOCKS_REQUIRED(!cs);
};

#endif // BITCOIN_EVO_EVODB_H
