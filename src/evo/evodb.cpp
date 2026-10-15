// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/evodb.h>

#include <logging.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/check.h>
#include <util/strencodings.h>
#include <util/translation.h>

#include <limits>
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

namespace {
DataStream NamespaceTag()
{
    DataStream tag{};
    tag << EVODB_GENERATION_NAMESPACE;
    return tag;
}

bool StartsWith(const DataStream& key, const DataStream& prefix)
{
    return key.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), key.begin());
}

bool SameBytes(const DataStream& a, const DataStream& b) { return std::equal(a.begin(), a.end(), b.begin(), b.end()); }

auto GenerationRecordKey(uint32_t generation) { return std::make_pair(EVODB_GENERATION_RECORD, generation); }
} // namespace

DataStream CEvoDBStore::GenerationPrefix(uint32_t generation)
{
    DataStream prefix{};
    if (generation != 0) prefix << EVODB_GENERATION_NAMESPACE << generation;
    return prefix;
}

bool CEvoDBStore::Owns(const DataStream& raw_key) const
{
    if (m_prefix.empty()) return !StartsWith(raw_key, NamespaceTag());
    return StartsWith(raw_key, m_prefix);
}

void CEvoDBStore::Iterator::SkipForeign()
{
    // Generation namespaces form one contiguous key range. The NORMAL view
    // steps over it as a whole; a generation view ends where its prefix ends.
    if (!m_store.m_prefix.empty() || !m_it->Valid()) return;
    const DataStream tag{NamespaceTag()};
    if (!StartsWith(m_it->GetKey(), tag)) return;
    DataStream after_tag{tag};
    std::byte& last{after_tag[after_tag.size() - 1]};
    last = std::byte{static_cast<uint8_t>(std::to_integer<uint8_t>(last) + 1)};
    m_it->Seek(after_tag);
}

CEvoDB::CEvoDB(const util::DbWrapperParams& db_params) :
    db{util::MakeDbWrapper({db_params.path / "evodb", db_params.memory, db_params.wipe, /*cache_size=*/64 << 20, db_params.options})}
{
    transaction_contexts.emplace(EvoDbIdentity::NORMAL, std::make_unique<TransactionContext>(*db, DataStream{}));
    ++g_view_cache_epoch;
}

CEvoDB::~CEvoDB() = default;

CEvoDB::TransactionContext& CEvoDB::GetContext(EvoDbIdentity identity)
{
    auto it = transaction_contexts.find(identity);
    if (it == transaction_contexts.end()) {
        if (identity == EvoDbIdentity::SNAPSHOT && !m_snapshot_generation) {
            throw EvoViewBindingError("EvoDB: the snapshot identity has no generation bound");
        }
        const uint32_t generation{identity == EvoDbIdentity::SNAPSHOT ? m_snapshot_generation->generation : 0};
        // Construct the context before inserting so a throwing constructor
        // cannot leave a null entry behind for later dereference.
        auto context{std::make_unique<TransactionContext>(*db, CEvoDBStore::GenerationPrefix(generation))};
        it = transaction_contexts.emplace(identity, std::move(context)).first;
    }
    return *it->second;
}

const CEvoDB::TransactionContext& CEvoDB::GetContext(EvoDbIdentity identity) const
{
    return *transaction_contexts.at(identity);
}

namespace {
thread_local const EvoViewScope* g_innermost_scope{nullptr};
} // namespace

EvoViewScope::EvoViewScope(const EvoViewToken& token) :
    m_token{token},
    m_outer{g_innermost_scope}
{
    if (m_token.db == nullptr) throw EvoViewBindingError("EvoViewScope: token names no EvoDB");
    m_token.db->CheckScopeAgainstTransaction(m_token);
    g_innermost_scope = this;
}

EvoViewScope::~EvoViewScope()
{
    Assume(g_innermost_scope == this);
    g_innermost_scope = m_outer;
}

const EvoViewToken* EvoViewScope::Innermost(const CEvoDB& db)
{
    for (const EvoViewScope* scope{g_innermost_scope}; scope != nullptr; scope = scope->m_outer) {
        if (scope->m_token.db == &db) return &scope->m_token;
    }
    return nullptr;
}

std::optional<EvoViewToken> EvoViewScope::InnermostAny()
{
    if (g_innermost_scope == nullptr) return std::nullopt;
    return g_innermost_scope->m_token;
}

bool CEvoDB::IsBound() const
{
    LOCK(cs);
    return HoldsTransaction() || EvoViewScope::Innermost(*this) != nullptr;
}

void CEvoDB::UpdateLiveGeneration()
{
    m_has_live_generation = m_snapshot_generation && (m_snapshot_generation->state == EvoGenerationState::PUBLISHED ||
                                                      m_snapshot_generation->state == EvoGenerationState::PROMOTING);
}

EvoViewToken CEvoDB::ActiveView() const
{
    LOCK(cs);
    return MakeView(m_default_identity);
}

bool CEvoDB::IsLive(const EvoViewToken& view) const
{
    LOCK(cs);
    return view.identity == EvoDbIdentity::NORMAL || MakeView(EvoDbIdentity::SNAPSHOT) == view;
}

EvoViewToken CEvoDB::MakeView(EvoDbIdentity identity) const
{
    if (identity == EvoDbIdentity::SNAPSHOT && m_snapshot_generation) {
        return EvoViewToken{.db = this,
                            .identity = identity,
                            .generation = m_snapshot_generation->generation,
                            .base = m_snapshot_generation->base,
                            .base_height = m_snapshot_generation->base_height};
    }
    return EvoViewToken{.db = this, .identity = identity};
}

EvoViewToken CEvoDB::ResolveView() const
{
    EvoViewToken view;
    if (HoldsTransaction()) {
        view = MakeView(*active_transaction);
    } else if (const EvoViewToken * scoped{EvoViewScope::Innermost(*this)}) {
        view = *scoped;
    } else if (m_has_live_generation) {
        // With two chainstates, an absent binding says nothing about which one
        // the access serves; never guess.
        throw EvoViewBindingError("unbound Evo access during dual-chainstate operation");
    } else {
        return MakeView(m_default_identity);
    }
    if (view.identity == EvoDbIdentity::SNAPSHOT && view != MakeView(EvoDbIdentity::SNAPSHOT)) {
        throw EvoViewBindingError(strprintf("Evo access bound to retired snapshot generation %u", view.generation));
    }
    return view;
}

bool CEvoDB::IsCurrentView(const EvoViewToken& view) const
{
    LOCK(cs);
    try {
        return ResolveView() == view;
    } catch (const EvoViewBindingError&) {
        return false;
    }
}

EvoViewToken CEvoDB::CurrentView() const
{
    LOCK(cs);
    return ResolveView();
}

EvoViewToken CEvoDB::ViewFor(EvoDbIdentity identity) const
{
    LOCK(cs);
    return MakeView(identity);
}

void CEvoDB::CheckScopeAgainstTransaction(const EvoViewToken& token) const
{
    LOCK(cs);
    if (HoldsTransaction() && MakeView(*active_transaction) != token) {
        throw EvoViewBindingError("EvoViewScope conflicts with this thread's open EvoDB transaction");
    }
}

std::unique_ptr<CEvoDBScopedCommitter> CEvoDB::BeginTransaction(EvoDbIdentity identity)
{
    AssertLockHeld(::cs_main);
    LOCK(cs);
    assert(!active_transaction.has_value());
    if (const EvoViewToken * scoped{EvoViewScope::Innermost(*this)}; scoped && *scoped != MakeView(identity)) {
        throw EvoViewBindingError("EvoDB transaction conflicts with this thread's EvoViewScope");
    }
    GetContext(identity);
    active_transaction = identity;
    active_transaction_thread = std::this_thread::get_id();
    return std::make_unique<CEvoDBScopedCommitter>(*this, identity);
}

void CEvoDB::CommitCurTransaction(EvoDbIdentity identity)
{
    AssertLockHeld(::cs_main);
    LOCK(cs);
    assert(active_transaction == identity);
    GetContext(identity).cur_transaction.Commit();
    active_transaction.reset();
}

void CEvoDB::RollbackCurTransaction(EvoDbIdentity identity)
{
    AssertLockHeld(::cs_main);
    LOCK(cs);
    assert(active_transaction == identity);
    // The open transaction's context exists. Never create one here: that can
    // throw, and this runs from CEvoDBScopedCommitter's destructor.
    if (const auto it{transaction_contexts.find(identity)}; it != transaction_contexts.end()) {
        it->second->cur_transaction.Clear();
    }
    active_transaction.reset();
}

bool CEvoDB::CommitRootTransaction(EvoDbIdentity identity, bool sync)
{
    AssertLockHeld(::cs_main);
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
    if (identity == EvoDbIdentity::SNAPSHOT && !m_snapshot_generation) return false;
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
    LOCK(cs);
    db->Write(EVODB_DUAL_CHAINSTATE, uint8_t{1});
}

bool CEvoDB::HasDualChainstateMarker()
{
    LOCK(cs);
    return db->Exists(EVODB_DUAL_CHAINSTATE);
}

void CEvoDB::WriteSnapshotBaseMNListHash(const uint256& hash)
{
    LOCK(cs);
    db->Write(EVODB_SNAPSHOT_MNLIST_HASH, hash);
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

std::optional<EvoGenerationRecord> CEvoDB::GetSnapshotGeneration() const
{
    LOCK(cs);
    return m_snapshot_generation;
}

void CEvoDB::BindSnapshotGeneration(std::optional<EvoGenerationRecord> record)
{
    // A context's store is keyed by the generation it was built for.
    if (auto it{transaction_contexts.find(EvoDbIdentity::SNAPSHOT)}; it != transaction_contexts.end()) {
        Assume(it->second->cur_transaction.IsClean() && it->second->root_transaction.IsClean());
        transaction_contexts.erase(it);
    }
    m_snapshot_generation = std::move(record);
    UpdateLiveGeneration();
    if (!m_snapshot_generation) m_default_identity = EvoDbIdentity::NORMAL;
}

bool CEvoDB::TransactionsClean() const
{
    if (active_transaction.has_value()) return false;
    return std::ranges::all_of(transaction_contexts, [](const auto& entry) {
        return !entry.second || (entry.second->cur_transaction.IsClean() && entry.second->root_transaction.IsClean());
    });
}

namespace {
bool IsKnown(EvoGenerationState state) { return state <= EvoGenerationState::PROMOTING; }

bool IsValid(const EvoPromotionJournal& journal)
{
    return journal.schema_version == EvoGenerationRecord::SCHEMA_VERSION && journal.phase <= EvoPromotionPhase::MERGED;
}
} // namespace

bool CEvoDB::CreateSnapshotGeneration(const uint256& base, int base_height)
{
    LOCK(cs);
    if (m_snapshot_generation) return false;
    uint32_t generation{1};
    db->Read(EVODB_GENERATION_NEXT, generation);
    // Generation 0 names the NORMAL keyspace; never wrap into it.
    if (generation == 0 || generation == std::numeric_limits<uint32_t>::max()) {
        LogPrintf("ERROR: %s: snapshot EvoDB generation ids are exhausted\n", __func__);
        return false;
    }
    EvoGenerationRecord record;
    record.generation = generation;
    record.state = EvoGenerationState::UNPUBLISHED;
    record.base = base;
    record.base_height = base_height;
    record.tip = base;
    CDBBatch batch{*db};
    batch.Write(EVODB_GENERATION_NEXT, generation + 1);
    batch.Write(GenerationRecordKey(generation), record);
    if (!db->WriteBatch(batch, /*fSync=*/true)) return false;
    BindSnapshotGeneration(record);
    return true;
}

bool CEvoDB::PublishSnapshotGeneration(const uint256& config_fingerprint, const std::optional<uint256>& base_mn_list_hash,
                                       const std::vector<uint8_t>& evo_section)
{
    LOCK(cs);
    if (!m_snapshot_generation || m_snapshot_generation->state != EvoGenerationState::UNPUBLISHED) return false;
    EvoGenerationRecord record{*m_snapshot_generation};
    record.state = EvoGenerationState::PUBLISHED;
    record.config_fingerprint = config_fingerprint;
    CDBBatch batch{*db};
    batch.Write(GenerationRecordKey(record.generation), record);
    batch.Write(EVODB_DUAL_CHAINSTATE, uint8_t{1});
    if (base_mn_list_hash) batch.Write(EVODB_SNAPSHOT_MNLIST_HASH, *base_mn_list_hash);
    if (!evo_section.empty()) batch.Write(EVODB_SNAPSHOT_EVO_SECTION, evo_section);
    if (!db->WriteBatch(batch, /*fSync=*/true)) return false;
    m_snapshot_generation = record;
    UpdateLiveGeneration();
    return true;
}

std::optional<std::vector<uint8_t>> CEvoDB::ReadRetainedEvoSection() const
{
    LOCK(cs);
    std::vector<uint8_t> evo_section;
    if (!db->Read(EVODB_SNAPSHOT_EVO_SECTION, evo_section)) return std::nullopt;
    return evo_section;
}

util::Result<void> CEvoDB::LoadSnapshotGenerations(const uint256& config_fingerprint)
{
    LOCK(cs);
    if (!Assume(TransactionsClean())) return util::Error{_("Evo database transactions are pending at startup.")};
    BindSnapshotGeneration(std::nullopt);
    std::vector<EvoGenerationRecord> records;
    {
        std::unique_ptr<CDBIterator> it{db->NewIterator()};
        it->Seek(GenerationRecordKey(0));
        for (; it->Valid(); it->Next()) {
            std::pair<std::string, uint32_t> key;
            if (!it->GetKey(key) || key.first != EVODB_GENERATION_RECORD) break;
            EvoGenerationRecord record;
            if (!it->GetValue(record) || record.schema_version != EvoGenerationRecord::SCHEMA_VERSION ||
                !IsKnown(record.state) || record.generation == 0 || record.generation != key.second) {
                return util::Error{_("The Evo database holds assumeutxo snapshot state in an unsupported format. "
                                     "Restart with -reindex-chainstate to discard it.")};
            }
            records.push_back(record);
        }
    }

    std::optional<EvoGenerationRecord> live;
    for (const EvoGenerationRecord& record : records) {
        switch (record.state) {
        case EvoGenerationState::UNPUBLISHED:
            if (!CollectGeneration(record.generation, /*erase_record=*/true)) {
                return util::Error{_("Failed to remove discarded assumeutxo snapshot state from the Evo database.")};
            }
            break;
        case EvoGenerationState::REJECTED:
            // Kept until startup recovery has invalidated its coins directory.
            break;
        case EvoGenerationState::PUBLISHED:
        case EvoGenerationState::PROMOTING:
            if (live) {
                return util::Error{_("The Evo database holds more than one live assumeutxo snapshot state. "
                                     "Restart with -reindex-chainstate to discard them.")};
            }
            live = record;
            break;
        }
    }

    if (live && live->config_fingerprint != config_fingerprint) {
        return util::Error{_("The consensus configuration changed since the assumeutxo snapshot was loaded, so its "
                             "Evo state can no longer be trusted. Restart with the previous configuration, or with "
                             "-reindex-chainstate to discard the snapshot.")};
    }
    // Once a promotion has merged and collected its generation, only the
    // journal remains to finish it.
    EvoPromotionJournal journal;
    const bool finishing_promotion{db->Read(EVODB_PROMOTION_JOURNAL, journal)};
    if (db->Exists(EVODB_PROMOTION_JOURNAL) && (!finishing_promotion || !IsValid(journal))) {
        return util::Error{_("The Evo database holds an assumeutxo promotion journal in an unsupported format. "
                             "Restart with -reindex-chainstate to discard it.")};
    }
    if (!live && !finishing_promotion &&
        (db->Exists(EVODB_DUAL_CHAINSTATE) || db->Exists(std::make_pair(EVODB_BEST_BLOCK, uint8_t{1})))) {
        return util::Error{_("An assumeutxo snapshot in progress was loaded by an older version that did not "
                             "isolate its Evo state. Restart with -reindex-chainstate to discard it.")};
    }
    BindSnapshotGeneration(live);
    return {};
}

bool CEvoDB::HasRejectedGeneration() const
{
    LOCK(cs);
    return !RejectedGenerations().empty();
}

std::vector<uint32_t> CEvoDB::RejectedGenerations() const
{
    std::vector<uint32_t> rejected;
    std::unique_ptr<CDBIterator> it{db->NewIterator()};
    for (it->Seek(GenerationRecordKey(0)); it->Valid(); it->Next()) {
        std::pair<std::string, uint32_t> key;
        if (!it->GetKey(key) || key.first != EVODB_GENERATION_RECORD) break;
        EvoGenerationRecord record;
        if (it->GetValue(record) && record.state == EvoGenerationState::REJECTED) rejected.push_back(key.second);
    }
    return rejected;
}

bool CEvoDB::CollectRejectedGenerations()
{
    LOCK(cs);
    for (const uint32_t generation : RejectedGenerations()) {
        if (!CollectGeneration(generation, /*erase_record=*/true)) return false;
    }
    return true;
}

bool CEvoDB::CollectGeneration(uint32_t generation, bool erase_record)
{
    if (!Assume(generation != 0)) return false;
    const DataStream prefix{CEvoDBStore::GenerationPrefix(generation)};
    std::unique_ptr<CDBIterator> it{db->NewIterator()};
    CDBBatch batch{*db};
    size_t pending{0};
    for (it->Seek(prefix); it->Valid() && StartsWith(it->GetKey(), prefix); it->Next()) {
        batch.Erase(it->GetKey());
        if (++pending >= m_merge_batch_size) {
            if (!db->WriteBatch(batch)) return false;
            batch.Clear();
            pending = 0;
        }
    }
    if (erase_record) batch.Erase(GenerationRecordKey(generation));
    return db->WriteBatch(batch, /*fSync=*/true);
}

bool CEvoDB::DiscardBoundGeneration()
{
    CDBBatch batch{*db};
    batch.Erase(EVODB_SNAPSHOT_MNLIST_HASH);
    batch.Erase(EVODB_BACKGROUND_MNLIST_HASH);
    batch.Erase(EVODB_SNAPSHOT_EVO_SECTION);
    batch.Erase(EVODB_DUAL_CHAINSTATE);
    batch.Erase(EVODB_PROMOTION_JOURNAL);
    if (m_snapshot_generation) {
        EvoGenerationRecord record{*m_snapshot_generation};
        record.state = EvoGenerationState::REJECTED;
        batch.Write(GenerationRecordKey(record.generation), record);
    }
    // A failure to persist the rejection leaves the generation live, and the
    // dual-chainstate markers with it; startup recovery then retries. The
    // REJECTED record stays until the snapshot's coins directory is
    // invalidated (CollectRejectedGenerations), so a crash in between can
    // still finish the invalidation.
    if (!db->WriteBatch(batch, /*fSync=*/true)) return false;
    BindSnapshotGeneration(std::nullopt);
    ++g_view_cache_epoch;
    return true;
}

bool CEvoDB::AbandonSnapshotGeneration()
{
    AssertLockHeld(::cs_main);
    LOCK(cs);
    if (!Assume(!active_transaction.has_value())) return false;
    if (auto it{transaction_contexts.find(EvoDbIdentity::SNAPSHOT)}; it != transaction_contexts.end()) {
        it->second->cur_transaction.Clear();
        it->second->root_transaction.Clear();
        it->second->root_batch.Clear();
    }
    return DiscardBoundGeneration();
}

bool CEvoDB::DiscardSnapshotMarkers()
{
    LOCK(cs);
    if (!Assume(TransactionsClean())) return false;
    return DiscardBoundGeneration();
}

bool CEvoDB::PrepareSnapshotPromotion(const uint256& snapshot_tip)
{
    LOCK(cs);
    if (!m_snapshot_generation) return false;
    // The coins tip being promoted must be the tip the generation's Evo state
    // was flushed at.
    uint256 evo_tip;
    if (!GetContext(EvoDbIdentity::SNAPSHOT).cur_transaction.Read(std::make_pair(EVODB_BEST_BLOCK, uint8_t{1}), evo_tip) ||
        evo_tip != snapshot_tip) {
        return false;
    }
    EvoPromotionJournal journal;
    if (db->Read(EVODB_PROMOTION_JOURNAL, journal)) {
        // Only a promotion that has not renamed anything yet may be redone.
        if (!IsValid(journal) || journal.generation != m_snapshot_generation->generation ||
            journal.phase != EvoPromotionPhase::PREPARED) {
            return false;
        }
    }
    journal = EvoPromotionJournal{};
    journal.generation = m_snapshot_generation->generation;
    journal.base = m_snapshot_generation->base;
    journal.tip = snapshot_tip;
    uint256 base_mn_list_hash;
    if (db->Read(EVODB_SNAPSHOT_MNLIST_HASH, base_mn_list_hash)) journal.base_mn_list_hash = base_mn_list_hash;
    EvoGenerationRecord record{*m_snapshot_generation};
    record.state = EvoGenerationState::PROMOTING;
    record.tip = snapshot_tip;
    CDBBatch batch{*db};
    batch.Write(EVODB_PROMOTION_JOURNAL, journal);
    batch.Write(GenerationRecordKey(record.generation), record);
    if (!db->WriteBatch(batch, /*fSync=*/true)) return false;
    m_snapshot_generation = record;
    UpdateLiveGeneration();
    return true;
}

std::optional<EvoPromotionJournal> CEvoDB::ReadPromotionJournal() const
{
    LOCK(cs);
    EvoPromotionJournal journal;
    if (!db->Read(EVODB_PROMOTION_JOURNAL, journal)) return std::nullopt;
    return journal;
}

bool CEvoDB::MergeGeneration(EvoPromotionJournal& journal)
{
    const DataStream prefix{CEvoDBStore::GenerationPrefix(journal.generation)};
    DataStream best_block_key{};
    best_block_key << std::make_pair(EVODB_BEST_BLOCK, uint8_t{1});
    DataStream llmq_best_block_key{};
    llmq_best_block_key << EVODB_LLMQ_BEST_BLOCK;

    std::unique_ptr<CDBIterator> it{db->NewIterator()};
    DataStream start{prefix};
    start.write(MakeByteSpan(journal.cursor));
    it->Seek(start);
    // The cursor names the last merged key; resume after it.
    if (!journal.cursor.empty() && it->Valid() && SameBytes(it->GetKey(), start)) it->Next();

    CDBBatch batch{*db};
    size_t pending{0};
    for (; it->Valid(); it->Next()) {
        const DataStream raw_key{it->GetKey()};
        if (!StartsWith(raw_key, prefix)) break;
        DataStream key{raw_key};
        key.ignore(prefix.size());
        // The promoted best block is published with the final markers.
        if (!SameBytes(key, best_block_key)) {
            CDataStream value{SER_DISK, CLIENT_VERSION};
            if (!db->ReadDataStream(raw_key, value)) return false;
            CDataStream normal_value{SER_DISK, CLIENT_VERSION};
            if (db->ReadDataStream(key, normal_value) && !SameBytes(key, llmq_best_block_key) &&
                !std::equal(normal_value.begin(), normal_value.end(), value.begin(), value.end())) {
                // Both chainstates derived this immutable record from the same
                // block; differing bytes mean one of them is corrupt.
                LogPrintf("ERROR: %s: snapshot generation %u conflicts with NORMAL state for key %s\n", __func__,
                          journal.generation, HexStr(key));
                return false;
            }
            batch.Write(key, value);
        }
        journal.cursor.assign(UCharCast(key.data()), UCharCast(key.data() + key.size()));
        if (++pending >= m_merge_batch_size) {
            batch.Write(EVODB_PROMOTION_JOURNAL, journal);
            if (!db->WriteBatch(batch, /*fSync=*/true)) return false;
            batch.Clear();
            pending = 0;
            if (m_promotion_failpoint == PromotionFailpoint::MID_MERGE) return false;
        }
    }
    journal.phase = EvoPromotionPhase::MERGED;
    journal.cursor.clear();
    batch.Write(EVODB_PROMOTION_JOURNAL, journal);
    return db->WriteBatch(batch, /*fSync=*/true);
}

bool CEvoDB::PromoteSnapshotMarkers(const uint256& expected_snapshot_tip)
{
    LOCK(cs);
    if (!Assume(TransactionsClean())) return false;

    EvoPromotionJournal journal;
    if (!db->Read(EVODB_PROMOTION_JOURNAL, journal)) {
        uint256 normal_tip;
        const bool already_promoted = db->Read(EVODB_BEST_BLOCK, normal_tip) && normal_tip == expected_snapshot_tip &&
                                      !m_snapshot_generation && !db->Exists(EVODB_DUAL_CHAINSTATE) &&
                                      !db->Exists(EVODB_SNAPSHOT_MNLIST_HASH) &&
                                      !db->Exists(EVODB_BACKGROUND_MNLIST_HASH);
        if (already_promoted) m_default_identity = EvoDbIdentity::NORMAL;
        return already_promoted;
    }
    if (!IsValid(journal) || journal.tip != expected_snapshot_tip) return false;

    if (journal.phase == EvoPromotionPhase::PREPARED) {
        journal.phase = EvoPromotionPhase::MERGING;
        if (!db->Write(EVODB_PROMOTION_JOURNAL, journal, /*fSync=*/true)) return false;
    }
    if (journal.phase == EvoPromotionPhase::MERGING && !MergeGeneration(journal)) return false;
    if (m_promotion_failpoint == PromotionFailpoint::AFTER_MERGE) return false;

    // Everything is merged: collecting the namespace (idempotent) cannot lose
    // data, and the journal still carries the promoted tip. The PROMOTING
    // record, with the configuration fingerprint startup checks, stays until
    // the final batch.
    BindSnapshotGeneration(std::nullopt);
    if (!CollectGeneration(journal.generation, /*erase_record=*/false)) return false;
    if (m_promotion_failpoint == PromotionFailpoint::AFTER_COLLECT) return false;

    CDBBatch batch{*db};
    batch.Write(EVODB_BEST_BLOCK, journal.tip);
    batch.Erase(EVODB_SNAPSHOT_MNLIST_HASH);
    batch.Erase(EVODB_BACKGROUND_MNLIST_HASH);
    batch.Erase(EVODB_SNAPSHOT_EVO_SECTION);
    batch.Erase(EVODB_DUAL_CHAINSTATE);
    batch.Erase(EVODB_PROMOTION_JOURNAL);
    batch.Erase(GenerationRecordKey(journal.generation));
    if (!db->WriteBatch(batch, /*fSync=*/true)) return false;
    // The dual-chainstate run is over: the promoted state is the NORMAL
    // identity, so transaction-less access must resolve there again.
    m_default_identity = EvoDbIdentity::NORMAL;
    ++g_view_cache_epoch;
    return true;
}
