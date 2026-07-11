// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/evodb.h>

#include <uint256.h>

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

CEvoDB::TransactionContext& CEvoDB::GetContext(EvoDbIdentity identity)
{
    auto [it, inserted] = transaction_contexts.try_emplace(identity);
    if (inserted) it->second = std::make_unique<TransactionContext>(*db);
    return *it->second;
}

const CEvoDB::TransactionContext& CEvoDB::GetContext(EvoDbIdentity identity) const
{
    return *transaction_contexts.at(identity);
}

EvoDbIdentity CEvoDB::GetCurrentIdentity() const { return active_transaction.value_or(active_chainstate_identity); }

void CEvoDB::SetActiveChainstateIdentity(EvoDbIdentity identity)
{
    LOCK(cs);
    assert(!active_transaction.has_value());
    active_chainstate_identity = identity;
}

std::unique_ptr<CEvoDBScopedCommitter> CEvoDB::BeginTransaction(EvoDbIdentity identity)
{
    LOCK(cs);
    assert(!active_transaction.has_value());
    active_transaction = identity;
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

bool CEvoDB::CommitRootTransaction(EvoDbIdentity identity)
{
    LOCK(cs);
    auto& context = GetContext(identity);
    assert(context.cur_transaction.IsClean());
    context.root_transaction.Commit();
    bool ret = db->WriteBatch(context.root_batch);
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
