// Copyright (c) 2019-2021 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CIRCULAR_FIFO_CACHE_H
#define BITCOIN_CIRCULAR_FIFO_CACHE_H

template <typename T, size_t MaxSize = 0>
class circular_fifo_cache
{
private:
    std::vector<T> internalCache;
    const size_t maxSize;
    const size_t truncateThreshold;

public:
    explicit circular_fifo_cache(size_t _maxSize = MaxSize) :
        maxSize(_maxSize), truncateThreshold(_maxSize * 2)
    {
        // either specify maxSize through template arguments or the constructor and fail otherwise
        assert(_maxSize != 0);

        internalCache.reserve(truncateThreshold);
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
        //Get always at most maxSize last inserted items
        if (internalCache.size() < maxSize) {
            std::copy(internalCache.begin(),
                      internalCache.end(),
                      std::back_inserter(v));
        } else {
            std::copy(internalCache.begin() + (internalCache.size() - maxSize),
                      internalCache.end(),
                      std::back_inserter(v));
        }
    }

    bool back(T& value)
    {
        if (internalCache.empty())
            return false;
        value = internalCache.back();
        return true;
    }

private:
    void truncate_if_needed()
    {
        if (internalCache.size() == truncateThreshold) {
            std::rotate(internalCache.begin(), internalCache.begin() + (internalCache.size() - maxSize), internalCache.end());
            internalCache.erase(internalCache.begin() + maxSize, internalCache.end());
        }
    }
};

#endif //BITCOIN_CIRCULAR_FIFO_CACHE_H
