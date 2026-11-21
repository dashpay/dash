// Copyright (c) 2023 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_KERNEL_NOTIFICATIONS_INTERFACE_H
#define BITCOIN_KERNEL_NOTIFICATIONS_INTERFACE_H

#include <cstdint>
#include <string>

class CBlockIndex;
class CDeterministicMNList;
enum class SynchronizationState;
struct bilingual_str;

namespace kernel {

/**
 * A base class defining functions for notifying about certain kernel
 * events.
 */
class Notifications
{
public:
    virtual ~Notifications(){};

    virtual void blockTip(SynchronizationState state, CBlockIndex& index) {}
    virtual void headerTip(SynchronizationState state, int64_t height, int64_t timestamp, bool presync) {}
    virtual void progress(const bilingual_str& title, int progress_percent, bool resume_possible) {}
    virtual void warning(const bilingual_str& warning) {}
    virtual void masternodeListChanged(const CDeterministicMNList& list, const CBlockIndex* index) {}
    virtual void evoStateWarningChanged() {}
};
} // namespace kernel

#endif // BITCOIN_KERNEL_NOTIFICATIONS_INTERFACE_H
