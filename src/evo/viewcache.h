// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_VIEWCACHE_H
#define BITCOIN_EVO_VIEWCACHE_H

#include <evo/evodb.h>

#include <cstdint>
#include <functional>
#include <map>
#include <tuple>
#include <utility>

/** A cache of Evo-derived state, partitioned by Evo view so that state derived
 *  under one view is never served to another. Every partition is dropped when
 *  any snapshot generation is rejected or promoted. Callers resolve the view
 *  once per operation and use the returned partition throughout it. Not
 *  thread-safe: guard it with the owner's lock.
 *
 *  For() may drop every partition, so a reference it returned is valid only
 *  until the next For() on the same cache: never hold one across another
 *  For() call, and hand out shared_ptr partitions when a reference must
 *  outlive the owner's lock. */
template <typename T>
class EvoViewCache
{
public:
    explicit EvoViewCache(std::function<T()> make = [] { return T{}; }) :
        m_make{std::move(make)}
    {
    }

    T& For(const EvoViewToken& view)
    {
        if (const uint64_t epoch{CEvoDB::ViewCacheEpoch()}; epoch != m_epoch) {
            m_parts.clear();
            m_epoch = epoch;
        }
        const Key key{view.db, view.identity, view.generation};
        auto it{m_parts.find(key)};
        if (it == m_parts.end()) it = m_parts.emplace(key, m_make()).first;
        return it->second;
    }

    template <typename F>
    void ForEach(F&& f)
    {
        for (auto& [_, part] : m_parts)
            f(part);
    }

    void Clear() { m_parts.clear(); }

private:
    using Key = std::tuple<const CEvoDB*, EvoDbIdentity, uint32_t>;

    std::function<T()> m_make;
    std::map<Key, T> m_parts;
    uint64_t m_epoch{CEvoDB::ViewCacheEpoch()};
};

#endif // BITCOIN_EVO_VIEWCACHE_H
