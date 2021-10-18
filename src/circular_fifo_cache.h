// Copyright (c) 2019-2021 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CIRCULAR_FIFO_CACHE_H
#define BITCOIN_CIRCULAR_FIFO_CACHE_H

template <typename T, size_t MaxSize = 0, size_t TruncateThreshold = 0>
class circular_fifo_cache
{
private:
    std::vector<T> internalCache;
    const size_t maxSize;

public:
    explicit circular_fifo_cache(size_t _maxSize = MaxSize, size_t _truncateThreshold = TruncateThreshold) :
        maxSize(_maxSize)
    {
        // either specify maxSize through template arguments or the constructor and fail otherwise
        assert(_maxSize != 0);

        internalCache.reserve(maxSize + 1);
    }

    size_t max_size() const { return maxSize; }
    size_t size() const { return internalCache.size(); }

    void emplace(T&& v)
    {
        internalCache.emplace_back(v);
        truncate_if_needed();
    }

    void insert(const T& v)
    {
        internalCache.push_back(v);
        truncate_if_needed();
    }

    void clear()
    {
        internalCache.clear();
    }

    void get(std::vector<T>& v)
    {
        std::copy(internalCache.begin(),
                  internalCache.end(),
                  std::back_inserter(v));
    }

private:
    void truncate_if_needed()
    {
        if (internalCache.size() > maxSize) {
            auto itThreshold = internalCache.begin() + maxSize;
            std::rotate(internalCache.begin(), internalCache.begin() + 1, internalCache.end());
            internalCache.erase(itThreshold, internalCache.end());
        }
    }
};

#endif //BITCOIN_CIRCULAR_FIFO_CACHE_H
