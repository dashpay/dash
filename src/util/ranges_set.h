// Copyright (c) 2023 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_RANGES_SET_H
#define BITCOIN_UTIL_RANGES_SET_H

#include <hash.h>
#include <saltedhasher.h>
#include <serialize.h>

#include <ios>
#include <limits>
#include <set>
#include <utility>

/**
 * The CRangesSet is a datastructure that keeps efficiently numbers as set of
 * continuous ranges of numbers.
 * CRangesSet let's to keep elements with gaps of any size while CSkipList has
 * limited capacity (total size of all gaps)
 *
 * The CRangesSet provides transaction guarantees: element can be added or
 * removed and data structure will be consistent. For case if any of these
 * operation failed (out-memory for example), the `assert` will be called to
 * terminate program.
 */
class CRangesSet
{
    // internal datastructure, doesn't have a reason to be publicly available
    struct Range
    {
        uint64_t begin;
        uint64_t end;
        Range();
        Range(uint64_t begin_in, uint64_t end_in);
        bool operator<(const Range& other) const
        {
            if (begin != other.begin) return begin < other.begin;
            return end < other.end;
        }

        SERIALIZE_METHODS(Range, obj)
        {
            READWRITE(obj.begin);
            READWRITE(obj.end);
        }
    };

    std::set<Range> ranges;

public:
    static constexpr uint64_t DEFAULT_MAX_RANGES{MAX_SIZE};

    /**
     * this function adds `value` to the datastructure.
     * it returns true if `add` succeed
     */
    [[nodiscard]] bool Add(uint64_t value);

    /**
     * this function returns true if `value` exists in the datastructure
     */
    [[nodiscard]] bool Contains(uint64_t value) const noexcept;

    /**
     * this function removes `value` from the datastructure.
     * it returns `false` if element didn't existed or removing failed by any reason
     */
    [[nodiscard]] bool Remove(uint64_t value);

    /**
     * Size() works with complexity O(N) times, avoid calling it without a good reason
     * Instead prefer to use IsEmpty()
     */
    [[nodiscard]] size_t Size() const noexcept;

    /**
     * IsEmpty() returns true if there's no any elements added
     */
    [[nodiscard]] bool IsEmpty() const noexcept;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        // Preserve the established canonical set encoding.
        s << ranges;
    }

    template <typename Stream>
    void UnserializeBounded(Stream& s, uint64_t max_ranges)
    {
        std::set<Range> decoded;
        const uint64_t count{ReadCompactSize(s)};
        if (count > max_ranges) throw std::ios_base::failure("oversized CRangesSet range count");
        uint64_t previous_end{0};
        bool have_previous{false};
        for (uint64_t i{0}; i < count; ++i) {
            Range range;
            s >> range;
            const bool wrapped_max{range.end == 0};
            if (wrapped_max && range.begin == 0) {
                throw std::ios_base::failure("unrepresentable full-domain CRangesSet range");
            }
            if (!wrapped_max && range.begin >= range.end) {
                throw std::ios_base::failure("invalid empty CRangesSet range");
            }
            // Equality is adjacent and must have been merged; less-than is
            // overlapping or unordered. Both are noncanonical and could make
            // Size() underflow.
            if (have_previous && (previous_end == 0 || range.begin <= previous_end)) {
                throw std::ios_base::failure("noncanonical CRangesSet ranges");
            }
            if (wrapped_max && i + 1 != count) {
                throw std::ios_base::failure("wrapped CRangesSet range must be last");
            }
            previous_end = range.end;
            have_previous = true;
            decoded.emplace(range);
        }
        ranges = std::move(decoded);
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        UnserializeBounded(s, DEFAULT_MAX_RANGES);
    }
};

#endif // BITCOIN_UTIL_RANGES_SET_H
