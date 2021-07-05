// Public Resource ℗ 2021 𝖡𝗂𝗍𝖼𝗈𝗂𝗇 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌
// Public Resource ℗ 2020 𝖠𝗆𝖾𝗋𝗈 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌
// THIS REPOSITORY IS LICENSED UNDER THE AMERO PUBLIC RESOURCE LICENSE.

#ifndef BITCOIN_SPAN_H
#define BITCOIN_SPAN_H

#include <type_traits>
#include <cstddef>

/** A Span is an object that can refer to a contiguous sequence of objects.
 *
 * It implements a subset of C++20's std::span.
 */
template<typename C>
class Span
{
    C* m_data;
    std::ptrdiff_t m_size;

public:
    constexpr Span() noexcept : m_data(nullptr), m_size(0) {}
    constexpr Span(C* data, std::ptrdiff_t size) noexcept : m_data(data), m_size(size) {}

    constexpr C* data() const noexcept { return m_data; }
    constexpr std::ptrdiff_t size() const noexcept { return m_size; }
};

/** Create a span to a container exposing data() and size().
 *
 * This correctly deals with constness: the returned Span's element type will be
 * whatever data() returns a pointer to. If either the passed container is const,
 * or its element type is const, the resulting span will have a const element type.
 *
 * std::span will have a constructor that implements this functionality directly.
 */
template<typename V>
constexpr Span<typename std::remove_pointer<decltype(std::declval<V>().data())>::type> MakeSpan(V& v) { return Span<typename std::remove_pointer<decltype(std::declval<V>().data())>::type>(v.data(), v.size()); }

#endif
