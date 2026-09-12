// Copyright (c) 2021-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_CHAINSTATE_H
#define BITCOIN_NODE_CHAINSTATE_H

#include <llmq/options.h>
#include <util/fs.h>
#include <validation.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>

class CChainstateHelper;
class CDeterministicMNManager;
class CEvoDB;
class ChainstateManager;
class CMasternodeSync;
class CTxMemPool;
struct LLMQContext;

namespace chainlock { class Chainlocks; }
namespace llmq { class CInstantSendManager; }

namespace node {

struct CacheSizes;

struct ChainstateLoadOptions {
    CTxMemPool* mempool{nullptr};
    llmq::CInstantSendManager* isman{nullptr};
    chainlock::Chainlocks* chainlocks{nullptr};
    const CMasternodeSync* mn_sync{nullptr};
    fs::path data_dir;

    bool block_tree_db_in_memory{false};
    bool coins_db_in_memory{false};
    bool dash_dbs_in_memory{false};
    bool reindex{false};
    bool reindex_chainstate{false};
    bool prune{false};
    int8_t bls_threads{llmq::DEFAULT_BLSCHECK_THREADS};
    int16_t worker_count{llmq::DEFAULT_WORKER_COUNT};
    int64_t max_recsigs_age{llmq::DEFAULT_MAX_RECOVERED_SIGS_AGE};
    int64_t check_blocks{DEFAULT_CHECKBLOCKS};
    int64_t check_level{DEFAULT_CHECKLEVEL};
    std::function<bool()> check_interrupt;
    std::function<void()> coins_error_cb;
};

//! Chainstate load status. Simple applications can just check for the success
//! case, and treat other cases as errors. More complex applications may want to
//! try reindexing in the generic failure case, and pass an interrupt callback
//! and exit cleanly in the interrupted case.
enum class ChainstateLoadStatus {
    SUCCESS,
    FAILURE, //!< Generic failure which reindexing may fix
    FAILURE_FATAL, //!< Fatal error which should not prompt to reindex
    FAILURE_INCOMPATIBLE_DB,
    INTERRUPTED,
};

//! Chainstate load status code and optional error string.
using ChainstateLoadResult = std::tuple<ChainstateLoadStatus, bilingual_str>;

/** This sequence can have 4 types of outcomes:
 *
 *  1. Success
 *  2. Shutdown requested
 *    - nothing failed but a shutdown was triggered in the middle of the
 *      sequence
 *  3. Soft failure
 *    - a failure that might be recovered from with a reindex
 *  4. Hard failure
 *    - a failure that definitively cannot be recovered from with a reindex
 *
 *  LoadChainstate returns a (status code, error string) tuple.
 *
 *  The llmq_ctx and chain_helper arguments are outputs: any instance they hold
 *  is destroyed and replaced with a freshly constructed one.
 */
ChainstateLoadResult LoadChainstate(ChainstateManager& chainman, const CacheSizes& cache_sizes,
                                    const ChainstateLoadOptions& options, CEvoDB& evodb,
                                    CDeterministicMNManager& dmnman, std::unique_ptr<LLMQContext>& llmq_ctx,
                                    std::unique_ptr<CChainstateHelper>& chain_helper);
ChainstateLoadResult VerifyLoadedChainstate(ChainstateManager& chainman, const ChainstateLoadOptions& options, CEvoDB& evodb,
                                            std::function<void(bool)> notify_bls_state = nullptr);
} // namespace node

#endif // BITCOIN_NODE_CHAINSTATE_H
