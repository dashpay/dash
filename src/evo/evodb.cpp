// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/evodb.h>

#include <uint256.h>

#include <tuple>

CEvoDBScopedCommitter::CEvoDBScopedCommitter(CEvoDB& _evoDB, EvoDbIdentity identity) :
    evoDB{_evoDB},
    identity{identity}
{
}

CEvoDBScopedCommitter::~CEvoDBScopedCommitter()
{
    if (!didCommitOrRollback)
        Rollback();
}

void CEvoDBScopedCommitter::Commit()
{
    assert(!didCommitOrRollback);
    didCommitOrRollback = true;
    evoDB.CommitCurTransaction(identity);
}

void CEvoDBScopedCommitter::Rollback()
{
    assert(!didCommitOrRollback);
    didCommitOrRollback = true;
    evoDB.RollbackCurTransaction(identity);
}

CEvoDB::CEvoDB(const util::DbWrapperParams& db_params) :
    db{util::MakeDbWrapper({db_params.path / "evodb", db_params.memory, db_params.wipe, /*cache_size=*/64 << 20})}
{
    transaction_contexts.emplace(EvoDbIdentity::NORMAL, std::make_unique<TransactionContext>(*db));
}

CEvoDB::~CEvoDB() = default;

bool CEvoDB::HasActiveTransaction()
{
    LOCK(cs);
    return active_transaction.has_value();
}

CEvoDB::TransactionContext& CEvoDB::GetContext(EvoDbIdentity identity)
{
    auto it = transaction_contexts.find(identity);
    if (it == transaction_contexts.end()) {
        // Construct the context before inserting so a throwing constructor
        // cannot leave a null entry behind for later dereference.
        it = transaction_contexts.emplace(identity, std::make_unique<TransactionContext>(*db)).first;
    }
    return *it->second;
}

const CEvoDB::TransactionContext& CEvoDB::GetContext(EvoDbIdentity identity) const
{
    return *transaction_contexts.at(identity);
}

EvoDbIdentity CEvoDB::GetCurrentIdentity() const
{
    if (active_transaction.has_value() && active_transaction_thread == std::this_thread::get_id()) {
        return *active_transaction;
    }
    return m_default_identity;
}

std::unique_ptr<CEvoDBScopedCommitter> CEvoDB::BeginTransaction(EvoDbIdentity identity)
{
    LOCK(cs);
    assert(!active_transaction.has_value());
    active_transaction = identity;
    active_transaction_thread = std::this_thread::get_id();
    GetContext(identity);
    return std::make_unique<CEvoDBScopedCommitter>(*this, identity);
}

void CEvoDB::CommitCurTransaction(EvoDbIdentity identity)
{
    LOCK(cs);
    assert(active_transaction == identity);
    GetContext(identity).cur_transaction.Commit();
    active_transaction.reset();
}

void CEvoDB::RollbackCurTransaction(EvoDbIdentity identity)
{
    LOCK(cs);
    assert(active_transaction == identity);
    GetContext(identity).cur_transaction.Clear();
    active_transaction.reset();
}

bool CEvoDB::CommitRootTransaction(EvoDbIdentity identity, bool sync)
{
    LOCK(cs);
    auto& context = GetContext(identity);
    assert(context.cur_transaction.IsClean());
    context.root_transaction.Commit();
    bool ret = db->WriteBatch(context.root_batch, sync);
    context.root_batch.Clear();
    return ret;
}

bool CEvoDB::ReadBestBlock(EvoDbIdentity identity, uint256& hash)
{
    LOCK(cs);
    auto& transaction = GetContext(identity).cur_transaction;
    if (identity == EvoDbIdentity::NORMAL) {
        return transaction.Read(EVODB_BEST_BLOCK, hash);
    }
    return transaction.Read(std::make_pair(EVODB_BEST_BLOCK, uint8_t{1}), hash);
}

bool CEvoDB::VerifyBestBlock(EvoDbIdentity identity, const uint256& hash)
{
    // Make sure evodb is consistent.
    // If we already have best block hash saved, the previous block should match it.
    uint256 hash_best_block;
    return ReadBestBlock(identity, hash_best_block) && hash_best_block == hash;
}

void CEvoDB::WriteBestBlock(EvoDbIdentity identity, const uint256& hash)
{
    LOCK(cs);
    auto& transaction = GetContext(identity).cur_transaction;
    if (identity == EvoDbIdentity::NORMAL) {
        transaction.Write(EVODB_BEST_BLOCK, hash);
    } else {
        transaction.Write(std::make_pair(EVODB_BEST_BLOCK, uint8_t{1}), hash);
    }
}

void CEvoDB::WriteDualChainstateMarker()
{
    Write(EVODB_DUAL_CHAINSTATE, uint8_t{1});
}

bool CEvoDB::HasDualChainstateMarker()
{
    LOCK(cs);
    return db->Exists(EVODB_DUAL_CHAINSTATE);
}

void CEvoDB::EraseSnapshotMarkers()
{
    LOCK(cs);
    auto& transaction = GetContext(GetCurrentIdentity()).cur_transaction;
    transaction.Erase(std::make_pair(EVODB_BEST_BLOCK, uint8_t{1}));
    transaction.Erase(EVODB_SNAPSHOT_MNLIST_HASH);
    transaction.Erase(EVODB_BACKGROUND_MNLIST_HASH);
    transaction.Erase(EVODB_DUAL_CHAINSTATE);
}

void CEvoDB::WriteSnapshotBaseMNListHash(const uint256& hash)
{
    Write(EVODB_SNAPSHOT_MNLIST_HASH, hash);
}

bool CEvoDB::ReadSnapshotBaseMNListHash(uint256& hash)
{
    // Lifecycle markers are read at rest (completion after both identities'
    // sync commits, or startup recovery). Read the raw DB so the result cannot
    // depend on which transaction-less default identity happens to be current.
    LOCK(cs);
    return db->Read(EVODB_SNAPSHOT_MNLIST_HASH, hash);
}

void CEvoDB::WriteBackgroundMNListHash(const uint256& block_hash, const uint256& mn_list_hash)
{
    Write(EVODB_BACKGROUND_MNLIST_HASH, std::make_pair(block_hash, mn_list_hash));
}

bool CEvoDB::ReadBackgroundMNListHash(uint256& block_hash, uint256& mn_list_hash)
{
    // See ReadSnapshotBaseMNListHash: at-rest raw read, identity-independent.
    LOCK(cs);
    std::pair<uint256, uint256> value;
    if (!db->Read(EVODB_BACKGROUND_MNLIST_HASH, value)) return false;
    std::tie(block_hash, mn_list_hash) = value;
    return true;
}

void CEvoDB::WriteRequiredWorkMNListHashes(const std::vector<uint256>& block_hashes)
{
    Write(EVODB_REQUIRED_WORK_MNLISTS, block_hashes);
}

bool CEvoDB::ReadRequiredWorkMNListHashes(std::vector<uint256>& block_hashes)
{
    return Read(EVODB_REQUIRED_WORK_MNLISTS, block_hashes);
}

void CEvoDB::WriteBackgroundWorkMNListHash(const uint256& block_hash, const uint256& mn_list_hash)
{
    Write(std::make_pair(EVODB_BACKGROUND_WORK_MNLIST_HASH, block_hash), mn_list_hash);
}

bool CEvoDB::ReadBackgroundWorkMNListHash(const uint256& block_hash, uint256& mn_list_hash)
{
    return Read(std::make_pair(EVODB_BACKGROUND_WORK_MNLIST_HASH, block_hash), mn_list_hash);
}

static void EraseHistoricalMNListMarkers(CDBWrapper& db, CDBBatch& batch)
{
    std::vector<uint256> required;
    if (db.Read(EVODB_REQUIRED_WORK_MNLISTS, required)) {
        for (const auto& block_hash : required) {
            batch.Erase(std::make_pair(EVODB_BACKGROUND_WORK_MNLIST_HASH, block_hash));
        }
    }
    batch.Erase(EVODB_REQUIRED_WORK_MNLISTS);
}

bool CEvoDB::PromoteSnapshotMarkers(const uint256& expected_snapshot_tip)
{
    LOCK(cs);
    assert(!active_transaction.has_value());
    for (const auto& [_, context] : transaction_contexts) {
        if (!context) continue;
        assert(context->cur_transaction.IsClean());
        assert(context->root_transaction.IsClean());
    }

    const auto snapshot_key = std::make_pair(EVODB_BEST_BLOCK, uint8_t{1});
    uint256 snapshot_tip;
    if (!db->Read(snapshot_key, snapshot_tip)) {
        uint256 normal_tip;
        const bool already_promoted = db->Read(EVODB_BEST_BLOCK, normal_tip) && normal_tip == expected_snapshot_tip &&
                                      !db->Exists(EVODB_DUAL_CHAINSTATE) && !db->Exists(EVODB_SNAPSHOT_MNLIST_HASH) &&
                                      !db->Exists(EVODB_BACKGROUND_MNLIST_HASH) &&
                                      !db->Exists(EVODB_REQUIRED_WORK_MNLISTS) &&
                                      !db->Exists(EVODB_SNAPSHOT_EVO_SECTION);
        if (already_promoted) m_default_identity = EvoDbIdentity::NORMAL;
        return already_promoted;
    }
    if (snapshot_tip != expected_snapshot_tip) return false;

    CDBBatch batch{*db};
    batch.Write(EVODB_BEST_BLOCK, snapshot_tip);
    batch.Erase(snapshot_key);
    batch.Erase(EVODB_SNAPSHOT_MNLIST_HASH);
    batch.Erase(EVODB_BACKGROUND_MNLIST_HASH);
    EraseHistoricalMNListMarkers(*db, batch);
    batch.Erase(EVODB_SNAPSHOT_EVO_SECTION);
    batch.Erase(EVODB_DUAL_CHAINSTATE);
    if (!db->WriteBatch(batch, /*fSync=*/true)) return false;
    // The dual-chainstate run is over: the promoted state is the NORMAL
    // identity, so transaction-less access must resolve there again.
    m_default_identity = EvoDbIdentity::NORMAL;
    return true;
}

bool CEvoDB::DiscardSnapshotMarkers()
{
    LOCK(cs);
    assert(!active_transaction.has_value());
    for (const auto& [_, context] : transaction_contexts) {
        if (!context) continue;
        assert(context->cur_transaction.IsClean());
        assert(context->root_transaction.IsClean());
    }

    CDBBatch batch{*db};
    batch.Erase(std::make_pair(EVODB_BEST_BLOCK, uint8_t{1}));
    batch.Erase(EVODB_SNAPSHOT_MNLIST_HASH);
    batch.Erase(EVODB_BACKGROUND_MNLIST_HASH);
    EraseHistoricalMNListMarkers(*db, batch);
    batch.Erase(EVODB_SNAPSHOT_EVO_SECTION);
    batch.Erase(EVODB_DUAL_CHAINSTATE);
    if (!db->WriteBatch(batch, /*fSync=*/true)) return false;
    // The snapshot chainstate is gone; transaction-less access must resolve
    // against the NORMAL identity again.
    m_default_identity = EvoDbIdentity::NORMAL;
    return true;
}
