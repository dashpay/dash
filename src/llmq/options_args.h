// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LLMQ_OPTIONS_ARGS_H
#define BITCOIN_LLMQ_OPTIONS_ARGS_H

#include <algorithm>
#include <cstdint>
#include <map>
#include <thread>

class ArgsManager;
namespace Consensus {
enum class LLMQType : uint8_t;
} // namespace Consensus

namespace llmq {
enum class QvvecSyncMode : int8_t {
    Invalid = -1,
    Always = 0,
    OnlyIfTypeMember = 1,
};

// Keep recovered signatures for a week. This is a "-maxrecsigsage" option default.
static constexpr int64_t DEFAULT_MAX_RECOVERED_SIGS_AGE{60 * 60 * 24 * 7};

/** -llmq-data-recovery default */
static constexpr bool DEFAULT_ENABLE_QUORUM_DATA_RECOVERY{true};
/** -watchquorums default, if true, we will connect to all new quorums and watch their communication */
static constexpr bool DEFAULT_WATCH_QUORUMS{false};
/** -parbls default (number of bls-checking threads, 0 = auto) */
static constexpr int8_t DEFAULT_BLSCHECK_THREADS{0};
/** Number of workers allocated per worker pool */
inline const int16_t DEFAULT_WORKER_COUNT{std::clamp<int16_t>(std::thread::hardware_concurrency() / 2, 1, 4)};
/** Maximum number of dedicated script-checking threads allowed */
static constexpr int8_t MAX_BLSCHECK_THREADS{33};

using QvvecSyncModeMap = std::map<Consensus::LLMQType, QvvecSyncMode>;

/// Returns the parsed entries given by `-llmq-qvvec-sync`
QvvecSyncModeMap GetEnabledQuorumVvecSyncEntries(const ArgsManager& args);
} // namespace llmq

#endif // BITCOIN_LLMQ_OPTIONS_ARGS_H
