// Copyright (c) 2019-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_UTIL_MINING_H
#define BITCOIN_TEST_UTIL_MINING_H

#include <consensus/validation.h>
#include <node/miner.h>
#include <uint256.h>
#include <validationinterface.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

class CBlock;
class CChainParams;
class COutPoint;
class CScript;
namespace node {
struct NodeContext;
} // namespace node

/** Records BlockChecked for one block hash. */
struct BlockValidationStateCatcher : public CValidationInterface {
    const uint256 m_hash;
    std::optional<BlockValidationState> m_state;

    explicit BlockValidationStateCatcher(const uint256& hash) :
        m_hash{hash}
    {
    }

protected:
    void BlockChecked(const CBlock& block, const BlockValidationState& state) override;
};

/** Create a blockchain, starting from genesis */
std::vector<std::shared_ptr<CBlock>> CreateBlockChain(size_t total_height, const CChainParams& params);

/** Returns the generated coin */
COutPoint MineBlock(const node::NodeContext&, const CScript& coinbase_scriptPubKey);

/**
 * Returns the generated coin (or Null if the block was invalid).
 * It is recommended to call RegenerateCommitments before mining the block to avoid merkle tree mismatches.
 **/
COutPoint MineBlock(const node::NodeContext&, std::shared_ptr<CBlock>& block);

/** Prepare a block to be mined */
std::shared_ptr<CBlock> PrepareBlock(const node::NodeContext&, const CScript& coinbase_scriptPubKey);
std::shared_ptr<CBlock> PrepareBlock(const node::NodeContext& node, const CScript& coinbase_scriptPubKey,
                                     const node::BlockAssembler::Options& assembler_options);

/** RPC-like helper function, returns the generated coin */
COutPoint generatetoaddress(const node::NodeContext&, const std::string& address);

#endif // BITCOIN_TEST_UTIL_MINING_H
