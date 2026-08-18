// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/wallet.h>

#include <bls/bls.h>
#include <chainparams.h>
#include <evo/providertx.h>
#include <evo/specialtx.h>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace wallet {
namespace {

uint32_t MasternodeOperatorCoinType()
{
    // 5 on mainnet, 1 everywhere else - the DashSync convention of using
    // testnet's coin type for every non-mainnet network.
    return static_cast<uint32_t>(Params().ExtCoinType());
}

std::vector<unsigned char> CanonicalOperatorKeyBytes(const CBLSPublicKey& public_key)
{
    return public_key.ToByteVector(/*specificLegacyScheme=*/false);
}

} // namespace

bool CWallet::LoadMasternodeOperatorKeyCounter(const MasternodeOperatorKeyCounter& counter)
{
    AssertLockHeld(cs_wallet);
    if (counter.source_id.empty() || counter.next_index > MASTERNODE_OPERATOR_MAX_INDEX) return false;
    m_mn_operator_key_counter = counter;
    return true;
}

bool CWallet::LoadMasternodeOperatorLookahead(const MasternodeOperatorLookahead& lookahead)
{
    AssertLockHeld(cs_wallet);
    if (lookahead.source_id.empty() || lookahead.public_keys.empty() ||
        lookahead.public_keys.size() > MASTERNODE_OPERATOR_GAP_LIMIT ||
        lookahead.base_index > MASTERNODE_OPERATOR_MAX_INDEX - lookahead.public_keys.size()) {
        return false;
    }
    for (const auto& public_key : lookahead.public_keys) {
        CBLSPublicKey parsed;
        parsed.SetBytes(public_key, /*specificLegacyScheme=*/false);
        if (!parsed.IsValid() || CanonicalOperatorKeyBytes(parsed) != public_key) return false;
    }
    m_mn_operator_lookahead = lookahead;
    return true;
}

std::optional<CWallet::MasternodeOperatorKeySource> CWallet::GetMasternodeOperatorKeySource() const
{
    AssertLockHeld(cs_wallet);
    if (IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS) || IsWalletFlagSet(WALLET_FLAG_EXTERNAL_SIGNER)) {
        return std::nullopt;
    }

    std::optional<MasternodeOperatorKeySource> source;
    for (const auto& [id, spk_man] : m_spk_managers) {
        std::vector<unsigned char> candidate;
        const auto status{spk_man->GetMasternodeOperatorKeySource(candidate)};
        if (status == MasternodeOperatorKeySourceStatus::AMBIGUOUS) return std::nullopt;
        if (status == MasternodeOperatorKeySourceStatus::NONE) continue;
        if (!source) {
            source = MasternodeOperatorKeySource{spk_man.get(), std::move(candidate)};
        } else if (source->id != candidate) {
            return std::nullopt;
        }
    }
    return source;
}

bool CWallet::HasMasternodeOperatorKeySource() const
{
    LOCK(cs_wallet);
    return GetMasternodeOperatorKeySource().has_value();
}

uint32_t CWallet::GetMasternodeOperatorNextIndex(const std::vector<unsigned char>& source_id) const
{
    AssertLockHeld(cs_wallet);
    if (!m_mn_operator_key_counter || m_mn_operator_key_counter->source_id != source_id) return 0;
    return m_mn_operator_key_counter->next_index;
}

bool CWallet::RaiseMasternodeOperatorKeyCounter(const std::vector<unsigned char>& source_id, uint32_t next_index,
                                                WalletBatch& batch)
{
    AssertLockHeld(cs_wallet);
    if (next_index <= GetMasternodeOperatorNextIndex(source_id)) return true;
    const MasternodeOperatorKeyCounter updated{source_id, next_index};
    if (!batch.WriteMasternodeOperatorKeyCounter(updated)) return false;
    m_mn_operator_key_counter = updated;
    return true;
}

MasternodeOperatorKeyStatus CWallet::TopUpMasternodeOperatorLookahead()
{
    AssertLockHeld(cs_wallet);
    WalletBatch batch(GetDatabase());
    return TopUpMasternodeOperatorLookahead(batch);
}

MasternodeOperatorKeyStatus CWallet::TopUpMasternodeOperatorLookahead(WalletBatch& batch)
{
    AssertLockHeld(cs_wallet);
    const auto source{GetMasternodeOperatorKeySource()};
    if (!source) return MasternodeOperatorKeyStatus::NOT_SUPPORTED;
    if (IsLocked()) return MasternodeOperatorKeyStatus::WALLET_LOCKED;
    const auto& source_id{source->id};

    const uint32_t next{GetMasternodeOperatorNextIndex(source_id)};
    const uint32_t lookahead_end{
        std::min<uint32_t>(next + MASTERNODE_OPERATOR_GAP_LIMIT, MASTERNODE_OPERATOR_MAX_INDEX)};
    if (next >= lookahead_end) return MasternodeOperatorKeyStatus::EXHAUSTED;
    if (m_mn_operator_lookahead && m_mn_operator_lookahead->source_id == source_id &&
        m_mn_operator_lookahead->base_index == next &&
        m_mn_operator_lookahead->public_keys.size() == lookahead_end - next) {
        return MasternodeOperatorKeyStatus::SUCCESS;
    }

    MasternodeOperatorLookahead lookahead{source_id, next, {}};
    lookahead.public_keys.reserve(lookahead_end - next);
    const auto status{source->spk_man->DeriveMasternodeOperatorKeys(
        MasternodeOperatorCoinType(), next, lookahead_end, [&lookahead](uint32_t, const CBLSSecretKey& secret) {
            lookahead.public_keys.push_back(CanonicalOperatorKeyBytes(secret.GetPublicKey()));
            return true;
        })};
    if (status != MasternodeOperatorKeyStatus::SUCCESS) return status;
    if (lookahead.public_keys.size() != lookahead_end - next) return MasternodeOperatorKeyStatus::DERIVATION_ERROR;
    if (!batch.WriteMasternodeOperatorLookahead(lookahead)) {
        return MasternodeOperatorKeyStatus::DATABASE_ERROR;
    }
    m_mn_operator_lookahead = std::move(lookahead);
    return MasternodeOperatorKeyStatus::SUCCESS;
}

void CWallet::MaybeAdvanceMasternodeOperatorKeyCounter(const CTransaction& tx, WalletBatch& batch)
{
    AssertLockHeld(cs_wallet);
    if (tx.nType != TRANSACTION_PROVIDER_REGISTER && tx.nType != TRANSACTION_PROVIDER_UPDATE_REGISTRAR &&
        tx.nType != TRANSACTION_PROVIDER_UPDATE_SHARED_REGISTRAR) {
        return;
    }
    // Without a lookahead nothing can be matched; recognition is best-effort
    // by design, so nothing is tracked for later either.
    if (!m_mn_operator_lookahead) return;

    CBLSPublicKey public_key;
    if (tx.nType == TRANSACTION_PROVIDER_REGISTER) {
        const auto payload{GetTxPayload<CProRegTx>(tx)};
        if (!payload) return;
        public_key = payload->pubKeyOperator.Get();
    } else if (tx.nType == TRANSACTION_PROVIDER_UPDATE_REGISTRAR) {
        const auto payload{GetTxPayload<CProUpRegTx>(tx)};
        if (!payload) return;
        public_key = payload->pubKeyOperator.Get();
    } else {
        const auto payload{GetTxPayload<CProUpSharedRegTx>(tx)};
        if (!payload) return;
        public_key = payload->pubKeyOperator.Get();
    }
    if (!public_key.IsValid()) return;

    // Canonical bytes make this a key-value comparison: a legacy-scheme wire
    // encoding of a lookahead key cannot cause a miss.
    const auto target{CanonicalOperatorKeyBytes(public_key)};
    const auto& keys{m_mn_operator_lookahead->public_keys};
    const auto it{std::find(keys.begin(), keys.end(), target)};
    if (it == keys.end()) return;
    const uint32_t index{m_mn_operator_lookahead->base_index + static_cast<uint32_t>(it - keys.begin())};

    // Only trust a lookahead that belongs to the active seed.
    const auto source{GetMasternodeOperatorKeySource()};
    if (!source || source->id != m_mn_operator_lookahead->source_id) return;
    const auto& source_id{source->id};
    if (index < GetMasternodeOperatorNextIndex(source_id)) return; // already consumed

    // The write lands before the in-memory key counter moves; a failed write is
    // retried the next time a transaction with this key is synced.
    if (!RaiseMasternodeOperatorKeyCounter(source_id, index + 1, batch)) {
        WalletLogPrintf("Failed to persist masternode operator key counter for index %u\n", index);
        return;
    }
    WalletLogPrintf("Masternode operator key index %u seen in provider transaction %s; key counter advanced to %u\n",
                    index, tx.GetHash().ToString(), index + 1);
    // Rebase the lookahead onto the new key counter so a contiguous run of
    // used keys is followed past the current window.
    if (const auto lookahead_status{TopUpMasternodeOperatorLookahead(batch)};
        lookahead_status != MasternodeOperatorKeyStatus::SUCCESS) {
        WalletLogPrintf("Failed to refresh masternode operator lookahead (status %d)\n",
                        static_cast<int>(lookahead_status));
    }
}

interfaces::MasternodeOperatorKeyResult CWallet::GetNewMasternodeOperatorKey(
    const std::function<bool(const CBLSPublicKey&)>& is_in_use)
{
    interfaces::MasternodeOperatorKeyResult result;
    const uint32_t coin_type{MasternodeOperatorCoinType()};

    uint32_t candidate{0};
    {
        LOCK(cs_wallet);
        const auto source{GetMasternodeOperatorKeySource()};
        if (!source) {
            result.status = MasternodeOperatorKeyStatus::NOT_SUPPORTED;
            return result;
        }
        if (IsLocked()) {
            result.status = MasternodeOperatorKeyStatus::WALLET_LOCKED;
            return result;
        }
        candidate = GetMasternodeOperatorNextIndex(source->id);
    }

    if (is_in_use) {
        // Gap-limit scan: consult the caller's predicate (typically the
        // current masternode list) from the key counter upward and issue only
        // above every index it reports in use, so a restored wallet cannot
        // re-issue a key that is demonstrably active. The predicate is
        // queried without cs_wallet held because it may read node chainstate.
        uint32_t scan_index{candidate};
        uint32_t consecutive_misses{0};
        while (consecutive_misses < MASTERNODE_OPERATOR_GAP_LIMIT && scan_index < MASTERNODE_OPERATOR_MAX_INDEX) {
            const uint32_t chunk_end{
                std::min(scan_index + MASTERNODE_OPERATOR_GAP_LIMIT, MASTERNODE_OPERATOR_MAX_INDEX)};
            std::vector<CBLSPublicKey> chunk;
            chunk.reserve(chunk_end - scan_index);
            {
                LOCK(cs_wallet);
                const auto source{GetMasternodeOperatorKeySource()};
                if (!source) {
                    result.status = MasternodeOperatorKeyStatus::NOT_SUPPORTED;
                    return result;
                }
                const auto status{source->spk_man->DeriveMasternodeOperatorKeys(
                    coin_type, scan_index, chunk_end, [&chunk](uint32_t, const CBLSSecretKey& secret) {
                        chunk.push_back(secret.GetPublicKey());
                        return true;
                    })};
                if (status != MasternodeOperatorKeyStatus::SUCCESS) {
                    result.status = status;
                    return result;
                }
            }
            for (uint32_t offset{0}; offset < chunk.size() && consecutive_misses < MASTERNODE_OPERATOR_GAP_LIMIT;
                 ++offset) {
                if (is_in_use(chunk[offset])) {
                    candidate = scan_index + offset + 1;
                    consecutive_misses = 0;
                } else {
                    ++consecutive_misses;
                }
            }
            scan_index = chunk_end;
        }
    }

    LOCK(cs_wallet);
    const auto source{GetMasternodeOperatorKeySource()};
    if (!source) {
        result.status = MasternodeOperatorKeyStatus::NOT_SUPPORTED;
        return result;
    }
    const auto& source_id{source->id};
    // The wallet may have been locked while the predicate ran without
    // cs_wallet; nothing has been consumed yet, so fail without burning an
    // index the seed can no longer derive.
    if (IsLocked()) {
        result.status = MasternodeOperatorKeyStatus::WALLET_LOCKED;
        return result;
    }
    // The sync hook may have advanced the key counter while the predicate ran;
    // never issue below it. A candidate raised this way skips a fresh
    // predicate query, matching the accepted race with concurrent
    // registrations (consensus rejects a genuine duplicate).
    candidate = std::max(candidate, GetMasternodeOperatorNextIndex(source_id));
    if (candidate >= MASTERNODE_OPERATOR_MAX_INDEX) {
        result.status = MasternodeOperatorKeyStatus::EXHAUSTED;
        return result;
    }

    CBLSSecretKey secret;
    const auto status{source->spk_man->DeriveMasternodeOperatorKey(coin_type, candidate, secret)};
    if (status != MasternodeOperatorKeyStatus::SUCCESS) {
        result.status = status;
        return result;
    }

    // The key counter is recorded before the secret is ever returned;
    // consumption is permanent and never rolled back.
    WalletBatch batch(GetDatabase());
    if (!RaiseMasternodeOperatorKeyCounter(source_id, candidate + 1, batch)) {
        secret.Reset();
        result.status = MasternodeOperatorKeyStatus::DATABASE_ERROR;
        return result;
    }
    // Keep the recognition lookahead ahead of the new key counter; a failure
    // only degrades opportunistic recognition, never the issued key.
    if (const auto lookahead_status{TopUpMasternodeOperatorLookahead()};
        lookahead_status != MasternodeOperatorKeyStatus::SUCCESS) {
        WalletLogPrintf("Failed to refresh masternode operator lookahead (status %d)\n",
                        static_cast<int>(lookahead_status));
    }

    result.key.secret_key.resize(CBLSSecretKey::SerSize);
    if (!secret.SerializeTo(result.key.secret_key)) {
        secret.Reset();
        result.key.secret_key.clear();
        result.status = MasternodeOperatorKeyStatus::DERIVATION_ERROR;
        return result;
    }
    result.key.public_key = CanonicalOperatorKeyBytes(secret.GetPublicKey());
    secret.Reset();
    result.key.path = MasternodeOperatorKeyPath(coin_type, candidate);
    result.status = MasternodeOperatorKeyStatus::SUCCESS;
    return result;
}

interfaces::MasternodeOperatorKeyResult CWallet::GetMasternodeOperatorKey(const CBLSPublicKey& public_key)
{
    interfaces::MasternodeOperatorKeyResult result;
    if (!public_key.IsValid()) {
        result.status = MasternodeOperatorKeyStatus::INVALID_KEY;
        return result;
    }

    LOCK(cs_wallet);
    const auto source{GetMasternodeOperatorKeySource()};
    if (!source) {
        result.status = MasternodeOperatorKeyStatus::NOT_SUPPORTED;
        return result;
    }
    const auto& source_id{source->id};
    if (IsLocked()) {
        result.status = MasternodeOperatorKeyStatus::WALLET_LOCKED;
        return result;
    }
    // Only consumed indexes - those below the key counter - are addressable:
    // exposure equals consumption, and nothing above the key counter has been
    // exposed.
    const uint32_t next{GetMasternodeOperatorNextIndex(source_id)};
    if (next == 0) {
        result.status = MasternodeOperatorKeyStatus::NOT_FOUND;
        return result;
    }

    const uint32_t coin_type{MasternodeOperatorCoinType()};
    const auto target{CanonicalOperatorKeyBytes(public_key)};
    CBLSSecretKey found;
    uint32_t found_index{0};
    const auto status{source->spk_man->DeriveMasternodeOperatorKeys(
        coin_type, 0, next, [&](uint32_t index, const CBLSSecretKey& secret) {
            if (CanonicalOperatorKeyBytes(secret.GetPublicKey()) != target) return true;
            found = secret;
            found_index = index;
            return false;
        })};
    if (status != MasternodeOperatorKeyStatus::SUCCESS) {
        result.status = status;
        return result;
    }
    if (!found.IsValid()) {
        result.status = MasternodeOperatorKeyStatus::NOT_FOUND;
        return result;
    }

    result.key.secret_key.resize(CBLSSecretKey::SerSize);
    if (!found.SerializeTo(result.key.secret_key)) {
        found.Reset();
        result.key.secret_key.clear();
        result.status = MasternodeOperatorKeyStatus::DERIVATION_ERROR;
        return result;
    }
    found.Reset();
    result.key.public_key = target;
    result.key.path = MasternodeOperatorKeyPath(coin_type, found_index);
    result.status = MasternodeOperatorKeyStatus::SUCCESS;
    return result;
}

} // namespace wallet
