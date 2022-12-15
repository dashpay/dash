// Copyright (c) 2023 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_RESULT_H
#define BITCOIN_UTIL_RESULT_H

#include <cassert>
#include <utility> // for std::move()
#include <variant>

template<typename T>
struct Ok {
    constexpr explicit Ok(T val) : val_(std::move(val)) {}
    const T val_;
};

// Specialization of the Ok struct for void type
template<>
struct Ok<void> {
    constexpr Ok() = default;
};

template<typename E>
struct Err {
    constexpr explicit Err(E val) : val_(std::move(val)) {}

    template<typename... Ts>
    constexpr explicit Err(Ts...t) : val_{t...} {}

    const E val_;
};

template<typename T, typename E>
class Result {
public:
    constexpr Result(Ok<T> val) : result{val} {}
    template<typename... Ts>
    constexpr Result(Ts... t) : result{Ok<T>{t...}} {}
    constexpr Result(Err<E> err) : result{err.val_} {}

    [[nodiscard]] constexpr bool is_ok() const { return std::holds_alternative<Ok<T>>(result); }
    [[nodiscard]] constexpr bool is_err() const { return !is_ok(); }

    [[nodiscard]] constexpr explicit operator bool() const {
        return is_ok();
    }

    template<typename U = T>
    [[nodiscard]] constexpr const U& operator*() const {
        return unwrap();
    }

    template<typename U = T>
    [[nodiscard]] constexpr const U& unwrap() const {
        assert(is_ok());
        return std::get<Ok<U>>(result).val_;
    }

    [[nodiscard]] constexpr const E& err() const {
        assert(is_err());
        return std::get<E>(result);
    }

private:
    std::variant<Ok<T>, E> result;
};

#endif
