// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_DETERMINISTICMNS_H
#define BITCOIN_EVO_DETERMINISTICMNS_H

#include <evo/dmn_types.h>
#include <evo/dmnstate.h>
#include <evo/providertx.h>
#include <evo/types.h>

#include <arith_uint256.h>
#include <clientversion.h>
#include <consensus/params.h>
#include <crypto/common.h>
#include <saltedhasher.h>
#include <scheduler.h>
#include <sync.h>

#include <gsl/pointers.h>
#include <immer/map.hpp>

#include <algorithm>
#include <atomic>
#include <limits>
#include <list>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

class CBlock;
class CBlockIndex;
class CCoinsViewCache;
class ChainstateManager;
class CEvoDB;
class CSimplifiedMNList;
class CSimplifiedMNListEntry;
class CMasternodeMetaMan;
class CSpecialTxProcessor;
struct RPCResult;

extern RecursiveMutex cs_main; // NOLINT(readability-redundant-declaration)

class CDeterministicMN
{
private:
    uint64_t internalId{std::numeric_limits<uint64_t>::max()};

public:
    static constexpr uint16_t MN_VERSION_FORMAT = 2;
    static constexpr uint16_t MN_CURRENT_FORMAT = MN_VERSION_FORMAT;

    uint256 proTxHash;
    COutPoint collateralOutpoint;
    uint16_t nOperatorReward{0};
    MnType nType{MnType::Regular};
    std::shared_ptr<const CDeterministicMNState> pdmnState;

    CDeterministicMN() = delete; // no default constructor, must specify internalId
    explicit CDeterministicMN(uint64_t _internalId, MnType mnType = MnType::Regular) :
        internalId(_internalId),
        nType(mnType)
    {
        // only non-initial values
        assert(_internalId != std::numeric_limits<uint64_t>::max());
    }
    template <typename Stream>
    CDeterministicMN(deserialize_type, Stream& s) { s >> *this; }

    SERIALIZE_METHODS(CDeterministicMN, obj)
    {
        READWRITE(obj.proTxHash);
        READWRITE(VARINT(obj.internalId));
        READWRITE(obj.collateralOutpoint);
        READWRITE(obj.nOperatorReward);
        READWRITE(obj.pdmnState);
        // We can't know if we are serialising for the Disk or for the Network here (s.GetType() is not accessible)
        // Therefore if s.GetVersion() == CLIENT_VERSION -> Then we know we are serialising for the Disk
        // Otherwise, we can safely check with protocol versioning logic so we won't break old clients
        if (s.GetVersion() == CLIENT_VERSION || s.GetVersion() >= DMN_TYPE_PROTO_VERSION) {
            READWRITE(obj.nType);
        } else {
            SER_READ(obj, obj.nType = MnType::Regular);
        }
    }

    [[nodiscard]] uint64_t GetInternalId() const;

    [[nodiscard]] CSimplifiedMNListEntry to_sml_entry() const;
    [[nodiscard]] std::string ToString() const;

    [[nodiscard]] static RPCResult GetJsonHelp(const std::string& key, bool optional);
    [[nodiscard]] UniValue ToJson() const;
};

class CDeterministicMNListDiff;

template <typename Stream, typename K, typename T, typename Hash, typename Equal>
void SerializeImmerMap(Stream& os, const immer::map<K, T, Hash, Equal>& m)
{
    WriteCompactSize(os, m.size());
    for (typename immer::map<K, T, Hash, Equal>::const_iterator mi = m.begin(); mi != m.end(); ++mi)
        Serialize(os, (*mi));
}

template <typename Stream, typename K, typename T, typename Hash, typename Equal>
void UnserializeImmerMap(Stream& is, immer::map<K, T, Hash, Equal>& m)
{
    m = immer::map<K, T, Hash, Equal>();
    unsigned int nSize = ReadCompactSize(is);
    for (unsigned int i = 0; i < nSize; i++) {
        std::pair<K, T> item;
        Unserialize(is, item);
        m = m.set(item.first, item.second);
    }
}

// For some reason the compiler is not able to choose the correct Serialize/Deserialize methods without a specialized
// version of SerReadWrite. It otherwise always chooses the version that calls a.Serialize()
template<typename Stream, typename K, typename T, typename Hash, typename Equal>
inline void SerReadWrite(Stream& s, const immer::map<K, T, Hash, Equal>& m, CSerActionSerialize ser_action)
{
    ::SerializeImmerMap(s, m);
}

template<typename Stream, typename K, typename T, typename Hash, typename Equal>
inline void SerReadWrite(Stream& s, immer::map<K, T, Hash, Equal>& obj, CSerActionUnserialize ser_action)
{
    ::UnserializeImmerMap(s, obj);
}


class CDeterministicMNList
{
private:
    struct ImmerHasher
    {
        size_t operator()(const uint256& hash) const { return ReadLE64(hash.begin()); }
    };

public:
    using MnMap = immer::map<uint256, CDeterministicMNCPtr, ImmerHasher>;
    using MnInternalIdMap = immer::map<uint64_t, uint256>;
    using MnUniquePropertyMap = immer::map<uint256, std::pair<uint256, uint32_t>, ImmerHasher>;

    struct Counts {
        size_t m_total_evo{0};
        size_t m_total_mn{0};
        size_t m_total_weighted{0};
        size_t m_valid_evo{0};
        size_t m_valid_mn{0};
        size_t m_valid_weighted{0};

        [[nodiscard]] size_t total() const { return m_total_mn + m_total_evo; }
        [[nodiscard]] size_t enabled() const { return m_valid_mn + m_valid_evo; }
    };

private:
    uint256 blockHash;
    int nHeight{-1};
    uint32_t nTotalRegisteredCount{0};
    MnMap mnMap;
    MnInternalIdMap mnInternalIdMap;

    // map of unique properties like address and keys
    // we keep track of this as checking for duplicates would otherwise be painfully slow
    MnUniquePropertyMap mnUniquePropertyMap;

    // This SML could be null
    // This cache is used to improve performance and meant to be reused
    // for multiple CDeterministicMNList until mnMap is actually changed.
    // Calls of AddMN, RemoveMN and (in some cases) UpdateMN reset this cache;
    // it happens also for indirect calls such as ApplyDiff
    // Thread safety: Protected by its own mutex for thread-safe access
    mutable Mutex m_cached_sml_mutex;
    mutable std::shared_ptr<const CSimplifiedMNList> m_cached_sml GUARDED_BY(m_cached_sml_mutex);

    // Private helper method to invalidate SML cache
    void InvalidateSMLCache() EXCLUSIVE_LOCKS_REQUIRED(!m_cached_sml_mutex)
    {
        LOCK(m_cached_sml_mutex);
        m_cached_sml = nullptr;
    }

public:
    CDeterministicMNList() = default;
    explicit CDeterministicMNList(const uint256& _blockHash, int _height, uint32_t _totalRegisteredCount) :
        blockHash(_blockHash),
        nHeight(_height),
        nTotalRegisteredCount(_totalRegisteredCount)
    {
        assert(nHeight >= 0);
    }

    // Copy constructor
    CDeterministicMNList(const CDeterministicMNList& other) :
        blockHash(other.blockHash),
        nHeight(other.nHeight),
        nTotalRegisteredCount(other.nTotalRegisteredCount),
        mnMap(other.mnMap),
        mnInternalIdMap(other.mnInternalIdMap),
        mnUniquePropertyMap(other.mnUniquePropertyMap)
    {
        LOCK(other.m_cached_sml_mutex);
        m_cached_sml = other.m_cached_sml;
    }

    // Assignment operator
    CDeterministicMNList& operator=(const CDeterministicMNList& other)
        EXCLUSIVE_LOCKS_REQUIRED(!m_cached_sml_mutex, !other.m_cached_sml_mutex)
    {
        if (this != &other) {
            blockHash = other.blockHash;
            nHeight = other.nHeight;
            nTotalRegisteredCount = other.nTotalRegisteredCount;
            mnMap = other.mnMap;
            mnInternalIdMap = other.mnInternalIdMap;
            mnUniquePropertyMap = other.mnUniquePropertyMap;

            LOCK2(m_cached_sml_mutex, other.m_cached_sml_mutex);
            m_cached_sml = other.m_cached_sml;
        }
        return *this;
    }

    template <typename Stream, typename Operation>
    inline void SerializationOpBase(Stream& s, Operation ser_action)
    {
        READWRITE(blockHash);
        READWRITE(nHeight);
        READWRITE(nTotalRegisteredCount);
    }

    template<typename Stream>
    void Serialize(Stream& s) const
    {
        const_cast<CDeterministicMNList*>(this)->SerializationOpBase(s, CSerActionSerialize());

        // Serialize the map as a vector
        WriteCompactSize(s, mnMap.size());
        for (const auto& [_, dmn] : mnMap) {
            s << *dmn;
        }
    }

    template <typename Stream>
    void Unserialize(Stream& s) EXCLUSIVE_LOCKS_REQUIRED(!m_cached_sml_mutex)
    {
        Clear();

        SerializationOpBase(s, CSerActionUnserialize());

        for (size_t to_read = ReadCompactSize(s); to_read > 0; --to_read) {
            AddMN(std::make_shared<CDeterministicMN>(deserialize, s), /*fBumpTotalCount=*/false);
        }
    }

    void Clear() EXCLUSIVE_LOCKS_REQUIRED(!m_cached_sml_mutex)
    {
        blockHash = uint256{};
        nHeight = -1;
        nTotalRegisteredCount = 0;
        mnMap = MnMap();
        mnUniquePropertyMap = MnUniquePropertyMap();
        mnInternalIdMap = MnInternalIdMap();
        InvalidateSMLCache();
    }

    [[nodiscard]] Counts GetCounts() const
    {
        Counts ret;
        for (const auto& [_, dmn] : mnMap) {
            const bool is_evo = dmn->nType == MnType::Evo;
            const bool is_valid = !dmn->pdmnState->IsBanned();
            const auto weight = GetMnType(dmn->nType).voting_weight;
            if (is_evo) {
                ret.m_total_evo++;
                if (is_valid) {
                    ret.m_valid_evo++;
                }
            } else {
                ret.m_total_mn++;
                if (is_valid) {
                    ret.m_valid_mn++;
                }
            }
            if (is_valid) {
                ret.m_valid_weighted += weight;
            }
            ret.m_total_weighted += weight;
        }
        return ret;
    }

    /**
     * Execute a callback on all masternodes in the mnList. This will pass a reference
     * of each masternode to the callback function. This should be preferred over ForEachMNShared.
     * @param onlyValid Run on all masternodes, or only "valid" (not banned) masternodes
     * @param cb callback to execute
     */
    void ForEachMN(bool onlyValid, std::function<void(const CDeterministicMN&)> cb) const
    {
        for (const auto& p : mnMap) {
            if (!onlyValid || !p.second->pdmnState->IsBanned()) {
                cb(*p.second);
            }
        }
    }

    /**
     * Prefer ForEachMN. Execute a callback on all masternodes in the mnList.
     * This will pass a non-null shared_ptr of each masternode to the callback function.
     * Use this function only when a shared_ptr is needed in order to take shared ownership.
     * @param onlyValid Run on all masternodes, or only "valid" (not banned) masternodes
     * @param cb callback to execute
     */
    void ForEachMNShared(bool onlyValid, std::function<void(const CDeterministicMNCPtr&)> cb) const
    {
        for (const auto& p : mnMap) {
            if (!onlyValid || !p.second->pdmnState->IsBanned()) {
                cb(p.second);
            }
        }
    }

    [[nodiscard]] const uint256& GetBlockHash() const
    {
        return blockHash;
    }
    void SetBlockHash(const uint256& _blockHash)
    {
        blockHash = _blockHash;
    }
    [[nodiscard]] int GetHeight() const
    {
        assert(nHeight >= 0);
        return nHeight;
    }
    void SetHeight(int _height)
    {
        assert(_height >= 0);
        nHeight = _height;
    }
    [[nodiscard]] uint32_t GetTotalRegisteredCount() const
    {
        return nTotalRegisteredCount;
    }

    [[nodiscard]] bool IsMNValid(const uint256& proTxHash) const;
    [[nodiscard]] bool IsMNPoSeBanned(const uint256& proTxHash) const;

    [[nodiscard]] bool HasMN(const uint256& proTxHash) const
    {
        return GetMN(proTxHash) != nullptr;
    }
    [[nodiscard]] bool HasMNByCollateral(const COutPoint& collateralOutpoint) const
    {
        return GetMNByCollateral(collateralOutpoint) != nullptr;
    }
    [[nodiscard]] bool HasValidMNByCollateral(const COutPoint& collateralOutpoint) const
    {
        return GetValidMNByCollateral(collateralOutpoint) != nullptr;
    }
    [[nodiscard]] CDeterministicMNCPtr GetMN(const uint256& proTxHash) const;
    [[nodiscard]] CDeterministicMNCPtr GetValidMN(const uint256& proTxHash) const;
    [[nodiscard]] CDeterministicMNCPtr GetMNByOperatorKey(const CBLSPublicKey& pubKey) const;
    [[nodiscard]] CDeterministicMNCPtr GetMNByCollateral(const COutPoint& collateralOutpoint) const;
    [[nodiscard]] CDeterministicMNCPtr GetValidMNByCollateral(const COutPoint& collateralOutpoint) const;
    [[nodiscard]] CDeterministicMNCPtr GetMNByService(const CService& service) const;
    [[nodiscard]] CDeterministicMNCPtr GetMNByInternalId(uint64_t internalId) const;
    [[nodiscard]] CDeterministicMNCPtr GetMNPayee(gsl::not_null<const CBlockIndex*> pindexPrev) const;

    /**
     * Calculates the projected MN payees for the next *count* blocks. The result is not guaranteed to be correct
     * as PoSe banning might occur later
     * @param nCount the number of payees to return. "nCount = max()"" means "all", use it to avoid calling GetCounts twice.
     */
    [[nodiscard]] std::vector<CDeterministicMNCPtr> GetProjectedMNPayees(gsl::not_null<const CBlockIndex* const> pindexPrev, int nCount = std::numeric_limits<int>::max()) const;

    /**
     * Calculates CSimplifiedMNList for current list and cache it
     * Thread safety: Uses internal mutex for thread-safe cache access
     */
    gsl::not_null<std::shared_ptr<const CSimplifiedMNList>> to_sml() const EXCLUSIVE_LOCKS_REQUIRED(!m_cached_sml_mutex);

    /**
     * Calculates the maximum penalty which is allowed at the height of this MN list. It is dynamic and might change
     * for every block.
     */
    [[nodiscard]] int CalcMaxPoSePenalty() const;

    /**
     * Returns a the given percentage from the max penalty for this MN list. Always use this method to calculate the
     * value later passed to PoSePunish. The percentage should be high enough to take per-block penalty decreasing for MNs
     * into account. This means, if you want to accept 2 failures per payment cycle, you should choose a percentage that
     * is higher then 50%, e.g. 66%.
     */
    [[nodiscard]] int CalcPenalty(int percent) const;

    /**
     * Punishes a MN for misbehavior. If the resulting penalty score of the MN reaches the max penalty, it is banned.
     * Penalty scores are only increased when the MN is not already banned, which means that after banning the penalty
     * might appear lower then the current max penalty, while the MN is still banned.
     */
    void PoSePunish(const uint256& proTxHash, int penalty, bool debugLogs) EXCLUSIVE_LOCKS_REQUIRED(!m_cached_sml_mutex);

    void DecreaseScores() EXCLUSIVE_LOCKS_REQUIRED(!m_cached_sml_mutex);
    /**
     * Decrease penalty score of MN by 1.
     * Only allowed on non-banned MNs.
     */
    void PoSeDecrease(const CDeterministicMN& dmn) EXCLUSIVE_LOCKS_REQUIRED(!m_cached_sml_mutex);

    [[nodiscard]] CDeterministicMNListDiff BuildDiff(const CDeterministicMNList& to) const;
    /**
     * Apply Diff modifies current object.
     * It is more efficient than creating a copy due to heavy copy constructor.
     * Calculating for old block may require up to {DISK_SNAPSHOT_PERIOD} object copy & destroy.
     */
    void ApplyDiff(gsl::not_null<const CBlockIndex*> pindex, const CDeterministicMNListDiff& diff)
        EXCLUSIVE_LOCKS_REQUIRED(!m_cached_sml_mutex);

    void AddMN(const CDeterministicMNCPtr& dmn, bool fBumpTotalCount = true) EXCLUSIVE_LOCKS_REQUIRED(!m_cached_sml_mutex);
    void UpdateMN(const CDeterministicMN& oldDmn, const std::shared_ptr<const CDeterministicMNState>& pdmnState)
        EXCLUSIVE_LOCKS_REQUIRED(!m_cached_sml_mutex);
    void UpdateMN(const uint256& proTxHash, const std::shared_ptr<const CDeterministicMNState>& pdmnState)
        EXCLUSIVE_LOCKS_REQUIRED(!m_cached_sml_mutex);
    void UpdateMN(const CDeterministicMN& oldDmn, const CDeterministicMNStateDiff& stateDiff)
        EXCLUSIVE_LOCKS_REQUIRED(!m_cached_sml_mutex);
    void RemoveMN(const uint256& proTxHash) EXCLUSIVE_LOCKS_REQUIRED(!m_cached_sml_mutex);

    template <typename T>
    [[nodiscard]] bool HasUniqueProperty(const T& v) const
    {
        return mnUniquePropertyMap.count(GetUniquePropertyHash(v)) != 0;
    }
    template <typename T>
    [[nodiscard]] CDeterministicMNCPtr GetUniquePropertyMN(const T& v) const
    {
        auto p = mnUniquePropertyMap.find(GetUniquePropertyHash(v));
        if (!p) {
            return nullptr;
        }
        return GetMN(p->first);
    }

    /**
     * Is this operator public key already held by a masternode other than `self`, under *either* BLS
     * encoding?
     *
     * mnUniquePropertyMap is keyed by GetUniquePropertyHash(), which serializes its argument, and a
     * BLS key serializes differently under the legacy and basic schemes. So one public key sits in
     * one of two possible slots and a single lookup sees only one of them, which is why operator key
     * uniqueness is otherwise enforced per encoding rather than per key. There are exactly two
     * schemes, so probing both is O(1) rather than a scan.
     *
     * Both slots are probed even when one resolves to `self`: where a cross-scheme duplicate pair
     * already exists, returning early on a self-match would miss the other member.
     *
     * Pass uint256() as `self` to exclude nothing. An invalid pubkey is never considered held:
     * it is rejected up front rather than relying on the map never containing a null entry.
     */
    [[nodiscard]] bool HasOperatorKeyUnderAnyScheme(const CBLSPublicKey& pubkey, const uint256& self) const
    {
        if (!pubkey.IsValid()) return false;
        for (const bool legacy_scheme : {true, false}) {
            CBLSLazyPublicKey wrapped;
            wrapped.Set(pubkey, legacy_scheme);
            if (!HasUniqueProperty(wrapped)) continue;
            const auto holder = GetUniquePropertyMN(wrapped);
            if (holder && holder->proTxHash != self) return true;
        }
        return false;
    }

    // Compare two masternode lists for equality, ignoring non-deterministic members.
    // Non-deterministic members (nTotalRegisteredCount, internalId) can differ between
    // nodes due to different sync histories, but don't affect consensus validity.
    bool IsEqual(const CDeterministicMNList& rhs) const
    {
        // Compare deterministic metadata
        if (blockHash != rhs.blockHash ||
            nHeight != rhs.nHeight ||
            mnUniquePropertyMap != rhs.mnUniquePropertyMap) {
            return false;
        }

        // Compare map sizes (actual entries compared below)
        // Note: Not comparing nTotalRegisteredCount (non-deterministic)
        if (mnMap.size() != rhs.mnMap.size() ||
            mnInternalIdMap.size() != rhs.mnInternalIdMap.size()) {
            return false;
        }

        // Compare each masternode entry
        for (const auto& [proTxHash, dmn] : mnMap) {
            auto dmn_rhs = rhs.mnMap.find(proTxHash);
            if (dmn_rhs == nullptr) {
                return false;
            }

            // Compare deterministic masternode fields
            // Note: Not comparing internalId (non-deterministic)
            if (dmn->proTxHash != dmn_rhs->get()->proTxHash ||
                dmn->collateralOutpoint != dmn_rhs->get()->collateralOutpoint ||
                dmn->nOperatorReward != dmn_rhs->get()->nOperatorReward ||
                dmn->nType != dmn_rhs->get()->nType ||
                // Use SerializeHash for pdmnState to avoid enumerating all state fields
                SerializeHash(*dmn->pdmnState) != SerializeHash(*dmn_rhs->get()->pdmnState)) {
                return false;
            }
        }
        return true;
    }

private:
    template <typename T>
    [[nodiscard]] static uint256 GetUniquePropertyHash(const T& v)
    {
#define DMNL_NO_TEMPLATE(name) \
    static_assert(!std::is_same_v<std::decay_t<T>, name>, "GetUniquePropertyHash cannot be templated against " #name)
        DMNL_NO_TEMPLATE(CBLSPublicKey);
        DMNL_NO_TEMPLATE(ExtNetInfo);
        DMNL_NO_TEMPLATE(MnNetInfo);
        DMNL_NO_TEMPLATE(NetInfoEntry);
        DMNL_NO_TEMPLATE(NetInfoInterface);
        DMNL_NO_TEMPLATE(std::shared_ptr<NetInfoInterface>);
#undef DMNL_NO_TEMPLATE
        int ser_version{PROTOCOL_VERSION};
        if constexpr (std::is_same_v<std::decay_t<T>, CService>) {
            // Special handling is required if we're using addresses that can only be (de)serialized using
            // ADDRv2. Without this step, the address gets truncated, the hashmap gets contaminated with
            // an invalid entry and subsequent attempts at registering ADDRv2 entries get blocked. We cannot
            // apply this treatment ADDRv1 compatible addresses for backwards compatibility with the existing map.
            if (!v.IsAddrV1Compatible()) {
                ser_version |= ADDRV2_FORMAT;
            }
        }
        return ::SerializeHash(v, /*nType=*/SER_GETHASH, /*nVersion=*/ser_version);
    }
    template <typename T>
    [[nodiscard]] bool AddUniqueProperty(const CDeterministicMN& dmn, const T& v)
    {
        static const T nullValue{};
        if (v == nullValue) {
            return false;
        }

        auto hash = GetUniquePropertyHash(v);
        auto oldEntry = mnUniquePropertyMap.find(hash);
        if (oldEntry != nullptr && oldEntry->first != dmn.proTxHash) {
            return false;
        }
        std::pair<uint256, uint32_t> newEntry(dmn.proTxHash, 1);
        if (oldEntry != nullptr) {
            newEntry.second = oldEntry->second + 1;
        }
        mnUniquePropertyMap = mnUniquePropertyMap.set(hash, newEntry);
        return true;
    }
    template <typename T>
    [[nodiscard]] bool DeleteUniqueProperty(const CDeterministicMN& dmn, const T& oldValue)
    {
        static const T nullValue{};
        if (oldValue == nullValue) {
            return false;
        }

        auto oldHash = GetUniquePropertyHash(oldValue);
        auto p = mnUniquePropertyMap.find(oldHash);
        if (p == nullptr || p->first != dmn.proTxHash) {
            return false;
        }
        if (p->second == 1) {
            mnUniquePropertyMap = mnUniquePropertyMap.erase(oldHash);
        } else {
            mnUniquePropertyMap = mnUniquePropertyMap.set(oldHash, std::make_pair(dmn.proTxHash, p->second - 1));
        }
        return true;
    }
    template <typename T>
    [[nodiscard]] bool UpdateUniqueProperty(const CDeterministicMN& dmn, const T& oldValue, const T& newValue)
    {
        // A BLS operator key can keep the same point while its serialized encoding (legacy<->basic)
        // changes on a version transition. The map is keyed by GetUniquePropertyHash(), so only that
        // hash reveals the entry must be re-keyed to the new scheme; CBLSLazyPublicKey::operator==
        // compares the point and ignores the scheme, so it would wrongly short-circuit. Compare the
        // serialized hashes for BLS keys and the plain value for every other unique property.
        if constexpr (std::is_same_v<std::decay_t<T>, CBLSLazyPublicKey>) {
            if (GetUniquePropertyHash(oldValue) == GetUniquePropertyHash(newValue)) {
                return true;
            }
        } else if (oldValue == newValue) {
            return true;
        }
        static const T nullValue{};

        if (oldValue != nullValue && !DeleteUniqueProperty(dmn, oldValue)) {
            return false;
        }

        if (newValue != nullValue && !AddUniqueProperty(dmn, newValue)) {
            return false;
        }
        return true;
    }

    friend bool operator==(const CDeterministicMNList& a, const CDeterministicMNList& b)
    {
        return  a.blockHash == b.blockHash &&
                a.nHeight == b.nHeight &&
                a.nTotalRegisteredCount == b.nTotalRegisteredCount &&
                a.mnMap == b.mnMap &&
                a.mnInternalIdMap == b.mnInternalIdMap &&
                a.mnUniquePropertyMap == b.mnUniquePropertyMap;
    }
};

class CDeterministicMNListDiff
{
public:
    int nHeight{-1}; //memory only

    std::vector<CDeterministicMNCPtr> addedMNs;
    // keys are all relating to the internalId of MNs
    std::unordered_map<uint64_t, CDeterministicMNStateDiff> updatedMNs;
    std::set<uint64_t> removedMns;

    template<typename Stream>
    void Serialize(Stream& s) const
    {
        s << addedMNs;

        WriteCompactSize(s, updatedMNs.size());
        std::vector<uint64_t> updatedMNsInternalIds;
        updatedMNsInternalIds.reserve(updatedMNs.size());
        for (const auto& entry : updatedMNs) {
            updatedMNsInternalIds.emplace_back(entry.first);
        }
        std::sort(updatedMNsInternalIds.begin(), updatedMNsInternalIds.end());
        for (const auto& internalId : updatedMNsInternalIds) {
            WriteVarInt<Stream, VarIntMode::DEFAULT, uint64_t>(s, internalId);
            s << updatedMNs.at(internalId);
        }

        WriteCompactSize(s, removedMns.size());
        for (const auto& internalId : removedMns) {
            WriteVarInt<Stream, VarIntMode::DEFAULT, uint64_t>(s, internalId);
        }
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        UnserializeImpl(s, false); // Default: new format
    }

    // Legacy-aware unserialize method
    template <typename Stream>
    void UnserializeLegacyFormat(Stream& s)
    {
        UnserializeImpl(s, true); // Legacy format
    }

private:
    template <typename Stream>
    void UnserializeImpl(Stream& s, bool isLegacyFormat)
    {
        // Reset all collections before reading
        addedMNs.clear();
        updatedMNs.clear();
        removedMns.clear();

        for (size_t to_read = ReadCompactSize(s); to_read > 0; --to_read) {
            addedMNs.push_back(std::make_shared<CDeterministicMN>(deserialize, s));
        }

        for (size_t to_read = ReadCompactSize(s); to_read > 0; --to_read) {
            uint64_t internalId = ReadVarInt<Stream, VarIntMode::DEFAULT, uint64_t>(s);
            // CDeterministicMNState can have newer fields but doesn't need migration logic here as
            // CDeterministicMNStateDiff is always serialised using a bitmask and new fields have a new bit guide value,
            // so we are good to continue. We do need migration logic to change field order though.
            if (isLegacyFormat) {
                // Use legacy deserializer for old format
                CDeterministicMNStateDiffLegacy legacyDiff(deserialize, s);
                // Convert to new format and store
                updatedMNs.emplace(internalId, legacyDiff.ToNewFormat());
            } else {
                // Use current deserializer for new format
                updatedMNs.emplace(internalId, CDeterministicMNStateDiff(deserialize, s));
            }
        }

        for (size_t to_read = ReadCompactSize(s); to_read > 0; --to_read) {
            uint64_t internalId = ReadVarInt<Stream, VarIntMode::DEFAULT, uint64_t>(s);
            removedMns.emplace(internalId);
        }
    }

public:
    bool HasChanges() const
    {
        return !addedMNs.empty() || !updatedMNs.empty() || !removedMns.empty();
    }
};


constexpr int llmq_max_blocks() {
    int max_blocks{0};
    for (const auto& llmq : Consensus::available_llmqs) {
        int blocks = (llmq.useRotation ? 1 : llmq.signingActiveQuorumCount) * llmq.dkgInterval;
        max_blocks = std::max(max_blocks, blocks);
    }
    return max_blocks;
}

struct MNListUpdates
{
    CDeterministicMNList old_list;
    CDeterministicMNList new_list;
    CDeterministicMNListDiff diff;
};

/** Sentinel suffix carried by serving-failure messages that report locally
 *  missing block data (pruned, or below an unvalidated snapshot base) rather
 *  than peer misbehavior. Every producer must append this verbatim so that
 *  IsBlockDataUnavailableError() keeps recognizing the condition; matching on
 *  this constant is what keeps such requests from penalizing the peer. */
inline constexpr std::string_view BLOCK_DATA_UNAVAILABLE_SUFFIX{
    "is not available (pruned or below an unvalidated snapshot base)"};

/** Thrown when the masternode list for a block cannot be reconstructed because
 *  the data is not on this node yet (pruned, or below an unvalidated snapshot
 *  base, or pending in another chainstate's unflushed EvoDB overlay). Distinct
 *  from the plain std::runtime_error that CDeterministicMNList::ApplyDiff
 *  raises for genuine local corruption, which must never be swallowed.
 *  The message carries BLOCK_DATA_UNAVAILABLE_SUFFIX, matched by
 *  IsBlockDataUnavailableError(). */
class BlockDataUnavailableError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

class CDeterministicMNManager
{
    static constexpr int DISK_SNAPSHOT_PERIOD = 576; // once per day
    // keep cache for enough disk snapshots to have all active quourms covered
    static constexpr int DISK_SNAPSHOTS = llmq_max_blocks() / DISK_SNAPSHOT_PERIOD + 1;
    static constexpr int LIST_DIFFS_CACHE_SIZE = DISK_SNAPSHOT_PERIOD * DISK_SNAPSHOTS;

public:
    // Hard caps on the in-memory caches. CleanupCache() alone is not enough: it only
    // runs once a new block has arrived, so between blocks an unauthenticated peer
    // spamming getmnlistd for historical blocks could append entries without bound
    // (a full mainnet list is several MB). List admission uses two tiers:
    //   - Recent tier (mnListsCache): tip-recent heights only, lowest-height-first
    //     eviction, cap MAX_CACHE_LISTS. Attacker stale traffic never enters here.
    //   - Stale tier (mnStaleListsCache): LRU, cap MAX_STALE_CACHE_LISTS. Keeps a
    //     bounded working set of historical mini-snapshots so repeated stale
    //     getmnlistd requests do not re-read a multi-MB disk snapshot on every call.
    // Lists are rebuilt by applying up to DISK_SNAPSHOT_PERIOD - 1 diffs from the
    // previous on-disk snapshot, so block validation / invalidation spanning a
    // snapshot boundary can legitimately keep up to two snapshot periods of lists
    // (per-block lists plus mini-snapshots) resident. Size the recent cap to hold
    // that whole window so bounding admission never slows the (dis)connect hot path;
    // it is well above honest steady-state usage (tip + live quorum bases +
    // mini-snapshots within LIST_DIFFS_CACHE_SIZE of the tip).
    static constexpr size_t MAX_CACHE_LISTS = static_cast<size_t>(DISK_SNAPSHOT_PERIOD) * 2;
    // One 576-block snapshot interval of mini-snapshots (18) plus margin.
    static constexpr size_t MAX_STALE_CACHE_LISTS = 32;
    // Diffs are small; allow a full recency window plus a margin for one rebuild walk.
    static constexpr size_t MAX_CACHE_DIFFS = static_cast<size_t>(LIST_DIFFS_CACHE_SIZE) + 64;

private:
    struct StaleListEntry {
        CDeterministicMNList list;
        std::list<uint256>::iterator lru_it{};
    };
    Mutex cs;
    Mutex cs_cleanup;
    // We have performed CleanupCache() on this height.
    int did_cleanup GUARDED_BY(cs_cleanup) {0};

    // Main thread has indicated we should perform cleanup up to this height
    std::atomic<int> to_cleanup {0};

    CEvoDB& m_evoDb;
    CMasternodeMetaMan& m_mn_metaman;

    Uint256HashMap<CDeterministicMNList> mnListsCache GUARDED_BY(cs);
    Uint256HashMap<CDeterministicMNListDiff> mnListDiffsCache GUARDED_BY(cs);
    std::list<uint256> mnStaleListsLru GUARDED_BY(cs);
    Uint256HashMap<StaleListEntry> mnStaleListsCache GUARDED_BY(cs);
    const CBlockIndex* tipIndex GUARDED_BY(cs) {nullptr};
    const CBlockIndex* m_initial_snapshot_index GUARDED_BY(cs) {nullptr};

public:
    CDeterministicMNManager() = delete;
    CDeterministicMNManager(const CDeterministicMNManager&) = delete;
    CDeterministicMNManager& operator=(const CDeterministicMNManager&) = delete;
    explicit CDeterministicMNManager(CEvoDB& evoDb, CMasternodeMetaMan& mn_metaman);
    ~CDeterministicMNManager();

    bool ProcessBlock(const CBlock& block, gsl::not_null<const CBlockIndex*> pindex, BlockValidationState& state,
                      const CDeterministicMNList& newList, std::optional<MNListUpdates>& updatesRet)
        EXCLUSIVE_LOCKS_REQUIRED(!cs, ::cs_main);
    bool UndoBlock(gsl::not_null<const CBlockIndex*> pindex, std::optional<MNListUpdates>& updatesRet) EXCLUSIVE_LOCKS_REQUIRED(!cs);

    void UpdatedBlockTip(gsl::not_null<const CBlockIndex*> pindex) EXCLUSIVE_LOCKS_REQUIRED(!cs);

    CDeterministicMNList GetListForBlock(gsl::not_null<const CBlockIndex*> pindex) EXCLUSIVE_LOCKS_REQUIRED(!cs) {
        LOCK(cs);
        return GetListForBlockInternal(pindex);
    };
    CDeterministicMNList GetListAtChainTip() EXCLUSIVE_LOCKS_REQUIRED(!cs);

    void SetListForBlockForTesting(const CDeterministicMNList& list) EXCLUSIVE_LOCKS_REQUIRED(!cs)
    {
        LOCK(cs);
        mnListsCache.insert_or_assign(list.GetBlockHash(), list);
    }

    // In-memory list/diff cache sizes (for tests and diagnostics).
    size_t GetListCacheSize() EXCLUSIVE_LOCKS_REQUIRED(!cs)
    {
        LOCK(cs);
        return mnListsCache.size();
    }
    size_t GetListDiffsCacheSize() EXCLUSIVE_LOCKS_REQUIRED(!cs)
    {
        LOCK(cs);
        return mnListDiffsCache.size();
    }
    size_t GetStaleListCacheSize() EXCLUSIVE_LOCKS_REQUIRED(!cs)
    {
        LOCK(cs);
        return mnStaleListsCache.size();
    }

    // Test if given TX is a ProRegTx which also contains the collateral at index n
    static bool IsProTxWithCollateral(const CTransactionRef& tx, uint32_t n);

    void DoMaintenance() EXCLUSIVE_LOCKS_REQUIRED(!cs, !cs_cleanup);

    // Recalculate and optionally repair diffs between snapshots
    struct RecalcDiffsResult {
        int start_height{0};
        int stop_height{0};
        int diffs_recalculated{0};
        int snapshots_verified{0};
        std::vector<std::string> verification_errors;
        std::vector<std::string> repair_errors;
    };

    // Callback type for building a new MN list from a block
    using BuildListFromBlockFunc = std::function<bool(
        const CBlock& block,
        gsl::not_null<const CBlockIndex*> pindexPrev,
        const CDeterministicMNList& prevList,
        const CCoinsViewCache& view,
        bool debugLogs,
        BlockValidationState& state,
        CDeterministicMNList& mnListRet)>;

    [[nodiscard]] RecalcDiffsResult RecalculateAndRepairDiffs(const CBlockIndex* start_index,
                                                              const CBlockIndex* stop_index,
                                                              BuildListFromBlockFunc build_list_func, bool repair)
        EXCLUSIVE_LOCKS_REQUIRED(!cs);
    [[nodiscard]] bool IsRepaired() const;
    void CompleteRepair();

    // Migration support for nVersion-first CDeterministicMNStateDiff format
    [[nodiscard]] bool IsMigrationRequired() const EXCLUSIVE_LOCKS_REQUIRED(!cs, ::cs_main);
    [[nodiscard]] bool MigrateLegacyDiffs(const CBlockIndex* const tip_index) EXCLUSIVE_LOCKS_REQUIRED(!cs, ::cs_main);

private:
    void CleanupCache(int nHeight) EXCLUSIVE_LOCKS_REQUIRED(cs);
    CDeterministicMNList GetListForBlockInternal(gsl::not_null<const CBlockIndex*> pindex) EXCLUSIVE_LOCKS_REQUIRED(cs);
    // Retain only tip-recent heights (same window CleanupCache uses for "too old").
    [[nodiscard]] bool ShouldRetainCacheHeight(int height) EXCLUSIVE_LOCKS_REQUIRED(cs);
    void CacheMNList(const uint256& block_hash, const CDeterministicMNList& list) EXCLUSIVE_LOCKS_REQUIRED(cs);
    void CacheMNListDiff(const uint256& block_hash, CDeterministicMNListDiff diff) EXCLUSIVE_LOCKS_REQUIRED(cs);
    void CacheStaleMNList(const uint256& block_hash, const CDeterministicMNList& list) EXCLUSIVE_LOCKS_REQUIRED(cs);
    [[nodiscard]] std::optional<CDeterministicMNList> GetStaleList(const uint256& block_hash) EXCLUSIVE_LOCKS_REQUIRED(cs);
    void EraseStaleList(const uint256& block_hash) EXCLUSIVE_LOCKS_REQUIRED(cs);
    void EnforceListsCacheLimit() EXCLUSIVE_LOCKS_REQUIRED(cs);
    void EnforceDiffsCacheLimit() EXCLUSIVE_LOCKS_REQUIRED(cs);

    // Helper methods for RecalculateAndRepairDiffs
    static std::vector<const CBlockIndex*> CollectSnapshotBlocks(const CBlockIndex* start_index,
                                                                 const CBlockIndex* stop_index,
                                                                 const Consensus::Params& consensus_params);
    bool VerifySnapshotPair(const CBlockIndex* from_index, const CBlockIndex* to_index,
                            const CDeterministicMNList& from_snapshot, const CDeterministicMNList& to_snapshot,
                            RecalcDiffsResult& result);
    static std::vector<std::pair<uint256, CDeterministicMNListDiff>> RepairSnapshotPair(
        const CBlockIndex* from_index, const CBlockIndex* to_index, const CDeterministicMNList& from_snapshot,
        const CDeterministicMNList& to_snapshot, BuildListFromBlockFunc build_list_func, RecalcDiffsResult& result);
    void WriteRepairedDiffs(const std::vector<std::pair<uint256, CDeterministicMNListDiff>>& recalculated_diffs)
        EXCLUSIVE_LOCKS_REQUIRED(!cs);
};
#endif // BITCOIN_EVO_DETERMINISTICMNS_H
