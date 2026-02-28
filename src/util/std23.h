// Copyright (c) 2022-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_STD23_H
#define BITCOIN_UTIL_STD23_H

#include <algorithm>
#include <functional>
#include <iterator>
#include <ranges>
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
namespace ranges {
#if __cplusplus >= 202302L
using std::ranges::contains;
using std::ranges::fold_left;
#else
/**
 * @tparam R range type, automatically deduced
 * @tparam T value type to search for, automatically deduced
 * @tparam Proj projection type
 * @param range the range to search in
 * @param value the value to search for
 * @param proj optional projection to apply to elements before comparison
 * @return true if the range contains the value, false otherwise
 *
 * @see https://github.com/llvm/llvm-project/blob/llvmorg-22.1.0/libcxx/include/__algorithm/ranges_contains.h
 */
template <typename R, typename T, typename Proj = std::identity>
inline constexpr bool contains(R&& range, const T& value, Proj proj = {})
{
    return std::ranges::find(std::ranges::begin(range), std::ranges::end(range), value, proj) != std::ranges::end(range);
}

/**
 * @tparam R range type, automatically deduced
 * @tparam T initial value type, automatically deduced
 * @tparam F binary operation type, automatically deduced
 * @param range the range to fold
 * @param init the initial value
 * @param f binary operation to apply
 * @return the result of left-folding the range with f
 *
 * @see https://github.com/llvm/llvm-project/blob/llvmorg-22.1.0/libcxx/include/__algorithm/ranges_fold.h
 */
template <typename R, typename T, typename F>
inline constexpr auto fold_left(R&& range, T init, F f)
{
    using I = decltype(std::ranges::begin(range));
    using U = std::decay_t<std::invoke_result_t<F&, T, std::iter_reference_t<I>>>;

    auto first = std::ranges::begin(range);
    auto last = std::ranges::end(range);
    if (first == last) {
        return U(std::move(init));
    }

    U accum = std::invoke(f, std::move(init), *first);
    for (++first; first != last; ++first) {
        accum = std::invoke(f, std::move(accum), *first);
    }
    return accum;
}
#endif // __cplusplus >= 202302L
namespace views {
#if __cplusplus >= 202302L
using std::ranges::views::enumerate;
#else
/**
 * @tparam T type of iterable, automatically deduced
 * @tparam TIter begin of container
 * @param iterable an iterable object, can be a temporary
 * @return struct containing a size_t index, and it's element in iterable
 */
template <typename T,
          typename TIter = decltype(std::begin(std::declval<T>())),
          typename = decltype(std::end(std::declval<T>()))>
inline constexpr auto enumerate(T && iterable)
{
    struct iterator {
        size_t i;
        TIter iter;
        bool operator!=(const iterator& other) const { return iter != other.iter; }
        void operator++()
        {
            ++i;
            ++iter;
        }
        auto operator*() const { return std::tie(i, *iter); }
    };
    struct iterable_wrapper {
        T iterable;
        auto begin() { return iterator{0, std::begin(iterable)}; }
        auto end() { return iterator{0, std::end(iterable)}; }
    };
    return iterable_wrapper{std::forward<T>(iterable)};
}
#endif // __cplusplus >= 202302L
} // namespace views
} // namespace ranges
namespace views {
using namespace std23::ranges::views;
} // namespace views
} // namespace std23

#endif // BITCOIN_UTIL_STD23_H
