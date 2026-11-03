// Copyright (c) 2021-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_THREAD_H
#define BITCOIN_UTIL_THREAD_H

#include <functional>
#include <string>

namespace ctpl {
class thread_pool;
} // namespace ctpl

namespace util {
/**
 * A wrapper for do-something-once thread functions.
 */
void TraceThread(std::string_view thread_name, std::function<void()> thread_func);

} // namespace util

/** Give every thread of the pool the name "<baseName>-<n>". */
void RenameThreadPool(ctpl::thread_pool& tp, const char* baseName);

#endif // BITCOIN_UTIL_THREAD_H
