// Copyright (c) 2021-2022 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_RANGES_H
#define BITCOIN_UTIL_RANGES_H

#include <algorithm>
#include <optional>

//#if __cplusplus > 201703L // C++20 compiler
//namespace ranges = std::ranges;
//#else

#define MK_RANGE(FUN)                                        \
    template <typename X, typename Z>                        \
    inline auto FUN(const X& ds, const Z& fn)                \
    {                                                        \
        return std::FUN(std::cbegin(ds), std::cend(ds), fn); \
    }                                                        \
    template <typename X, typename Z>                        \
    inline auto FUN(X& ds, const Z& fn)                      \
    {                                                        \
        return std::FUN(std::begin(ds), std::end(ds), fn);   \
    }

#define MK_RANGE2(FUN)                                                                                     \
    template <typename X, typename Y, typename Z>                                                          \
    inline auto FUN(const X& first, const Y& second, const Z& fn)                                          \
    {                                                                                                      \
        return std::FUN(std::cbegin(first), std::cend(first), std::cbegin(second), std::cend(second), fn); \
    }                                                                                                      \
    template <typename X, typename Y, typename Z>                                                          \
    inline auto FUN(X& first, Y& second, const Z& fn)                                                      \
    {                                                                                                      \
        return std::FUN(std::begin(first), std::end(first), std::begin(second), std::end(second), fn);     \
    }                                                                                                      \
    template <typename X, typename Y>                                                                      \
    inline auto FUN(const X& first, const Y& second)                                                       \
    {                                                                                                      \
        return std::FUN(std::cbegin(first), std::cend(first), std::cbegin(second), std::cend(second));     \
    }                                                                                                      \
    template <typename X, typename Y>                                                                      \
    inline auto FUN(X& first, Y& second)                                                                   \
    {                                                                                                      \
        return std::FUN(std::begin(first), std::end(first), std::begin(second), std::end(second));         \
    }

namespace ranges {
    MK_RANGE(all_of)
    MK_RANGE(any_of)
    MK_RANGE(count_if)
    MK_RANGE(find_if)

    MK_RANGE2(equal)

    template <typename X, typename Z>
    constexpr inline auto find_if_opt(const X& ds, const Z& fn) {
        const auto it = ranges::find_if(ds, fn);
        if (it != end(ds)) {
            return std::make_optional(*it);
        }
        return std::optional<std::decay_t<decltype(*it)>>{};
    }

}

//#endif // C++20 compiler
#endif // BITCOIN_UTIL_RANGES_H
