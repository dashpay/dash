// Copyright (c) 2023 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_STD23_H
#define BITCOIN_UTIL_STD23_H

#include <type_traits>
#include <utility>

namespace std23 {
#if __cplusplus >= 202302L
using std::to_underlying;
#else
/**
 * @tparam E enumeration type, automatically deduced
 * @param e the enumerated value to convert
 * @return the underlying value in base type
 */
template <typename E>
[[nodiscard]] inline constexpr std::underlying_type_t<E> to_underlying(E e) noexcept
{
    return static_cast<std::underlying_type_t<E>>(e);
}
#endif // __cplusplus >= 202302L
} // namespace std23

#endif // BITCOIN_UTIL_STD23_H
