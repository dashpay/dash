// Copyright (c) 2021-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_HELPERS_H
#define BITCOIN_UTIL_HELPERS_H

#include <memory>
#include <optional>
#include <ranges>

namespace util {
template <typename X, typename Z>
constexpr inline auto find_if_opt(const X& ds, const Z& fn)
{
    const auto it = std::ranges::find_if(ds, fn);
    if (it != std::end(ds)) {
        return std::make_optional(*it);
    }
    return std::optional<std::decay_t<decltype(*it)>>{};
}

template <typename T>
constexpr inline auto irange(const T& max)
{
    return std::views::iota(T{0}, max);
}

template <typename T>
inline bool shared_ptr_equal(const std::shared_ptr<T>& lhs, const std::shared_ptr<T>& rhs)
    requires requires { *lhs == *rhs; }
{
    if (lhs == rhs) return true;    // Same object or both blank
    if (!lhs || !rhs) return false; // Inequal initialization state
    return *lhs == *rhs;            // Deep comparison
}

template <typename T>
inline bool shared_ptr_not_equal(const std::shared_ptr<T>& lhs, const std::shared_ptr<T>& rhs)
    requires requires { *lhs != *rhs; }
{
    if (lhs == rhs) return false;  // Same object or both blank
    if (!lhs || !rhs) return true; // Inequal initialization state
    return *lhs != *rhs;           // Deep comparison
}
} // namespace util

#endif // BITCOIN_UTIL_HELPERS_H
