// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LLMQ_CACHE_H
#define BITCOIN_LLMQ_CACHE_H

#include <consensus/params.h>
#include <llmq/params.h>
#include <saltedhasher.h>
#include <uint256.h>
#include <unordered_lru_cache.h>

#include <map>
#include <utility>

namespace llmq {

//! A separate LRU cache per LLMQ type, sized from that type's consensus parameters.
//!
//! Only the types registered for the active chain get a cache. Consensus::LLMQType is a
//! uint8_t enum that arrives over the wire, so callers may pass a type this chain does not
//! use: those lookups miss and those writes are dropped, which is the same answer a cache
//! that has never held such an entry would give.
template <typename Value, typename Key = uint256>
class PerLlmqTypeCache
{
private:
    using CacheType = unordered_lru_cache<Key, Value, StaticSaltedHasher>;

    std::map<Consensus::LLMQType, CacheType> m_caches;

public:
    //! Creates a cache per registered type, sized by size_fn. Must be called before use;
    //! until then every type reads as absent.
    template <typename SizeFn>
    void Init(const Consensus::Params& consensus_params, SizeFn size_fn)
    {
        for (const auto& llmq : consensus_params.llmqs) {
            m_caches.emplace(std::piecewise_construct, std::forward_as_tuple(llmq.type),
                             std::forward_as_tuple(size_fn(llmq)));
        }
    }

    void Init(const Consensus::Params& consensus_params, bool limit_by_connections = true)
    {
        Init(consensus_params, [limit_by_connections](const Consensus::LLMQParams& llmq) {
            return limit_by_connections ? llmq.keepOldConnections : llmq.keepOldKeys;
        });
    }

    bool IsInitialized() const { return !m_caches.empty(); }

    bool get(Consensus::LLMQType llmqType, const Key& key, Value& value)
    {
        auto it = m_caches.find(llmqType);
        return it != m_caches.end() && it->second.get(key, value);
    }

    void insert(Consensus::LLMQType llmqType, const Key& key, const Value& value)
    {
        if (auto it = m_caches.find(llmqType); it != m_caches.end()) {
            it->second.insert(key, value);
        }
    }

    void emplace(Consensus::LLMQType llmqType, const Key& key, Value&& value)
    {
        if (auto it = m_caches.find(llmqType); it != m_caches.end()) {
            it->second.emplace(key, std::move(value));
        }
    }

    void erase(Consensus::LLMQType llmqType, const Key& key)
    {
        if (auto it = m_caches.find(llmqType); it != m_caches.end()) {
            it->second.erase(key);
        }
    }

    //! Drops cached entries but keeps the per-type caches, so Init is not needed again.
    void clear()
    {
        for (auto& [_, cache] : m_caches) {
            cache.clear();
        }
    }

    void clear(Consensus::LLMQType llmqType)
    {
        if (auto it = m_caches.find(llmqType); it != m_caches.end()) {
            it->second.clear();
        }
    }

    //! Capacity of a type's cache, or 0 if this chain does not use it.
    size_t max_size(Consensus::LLMQType llmqType) const
    {
        auto it = m_caches.find(llmqType);
        return it != m_caches.end() ? it->second.max_size() : 0;
    }
};

} // namespace llmq

#endif // BITCOIN_LLMQ_CACHE_H
