// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <clientversion.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <node/blockstorage.h>
#include <node/context.h>
#include <pow.h>
#include <script/script.h>
#include <streams.h>
#include <util/chaintype.h>
#include <util/fs_helpers.h>
#include <util/zstd.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>
#include <test/util/setup_common.h>

using node::BlockManager;
using node::BLOCK_RECORD_COMPRESSED;
using node::BLOCK_RECORD_SIZE_MASK;
using node::BLOCK_SERIALIZATION_HEADER_SIZE;
using node::MAX_BLOCKFILE_SIZE;

/** Size field of the record whose payload starts at pos */
static uint32_t ReadSizeField(const BlockManager& blockman, const FlatFilePos& pos)
{
    AutoFile file{blockman.OpenBlockFile(FlatFilePos{pos.nFile, pos.nPos - sizeof(uint32_t)}, true)};
    uint32_t size_field;
    file >> size_field;
    return size_field;
}

// use BasicTestingSetup here for the data directory configuration, setup, and cleanup
BOOST_FIXTURE_TEST_SUITE(blockmanager_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(blockmanager_find_block_pos)
{
    const auto params {CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const BlockManager::Options blockman_opts{
        .chainparams = *params,
        .blocks_dir = m_args.GetBlocksDirPath(),
    };
    BlockManager blockman{blockman_opts};
    // simulate adding a genesis block normally
    BOOST_CHECK_EQUAL(blockman.SaveBlockToDisk(params->GenesisBlock(), 0, nullptr).nPos, BLOCK_SERIALIZATION_HEADER_SIZE);
    // simulate what happens during reindex
    // simulate a well-formed genesis block being found at offset 8 in the blk00000.dat file
    // the block is found at offset 8 because there is an 8 byte serialization header
    // consisting of 4 magic bytes + 4 length bytes before each block in a well-formed blk file.
    FlatFilePos pos{0, BLOCK_SERIALIZATION_HEADER_SIZE};
    BOOST_CHECK_EQUAL(blockman.SaveBlockToDisk(params->GenesisBlock(), 0, &pos).nPos, BLOCK_SERIALIZATION_HEADER_SIZE);
    // now simulate what happens after reindex for the first new block processed
    // the actual block contents don't matter, just that it's a block.
    // verify that the write position is right after the stored genesis record.
    // this is a check to make sure that https://github.com/bitcoin/bitcoin/issues/21379 does not recur
    // 8 bytes (for serialization header) + the stored (possibly compressed) genesis block
    // add another 8 bytes for the second block's serialization header
    const uint32_t stored_genesis_size{ReadSizeField(blockman, pos) & BLOCK_RECORD_SIZE_MASK};
    BOOST_CHECK(stored_genesis_size <= ::GetSerializeSize(params->GenesisBlock(), CLIENT_VERSION));
    FlatFilePos actual{blockman.SaveBlockToDisk(params->GenesisBlock(), 1, nullptr)};
    BOOST_CHECK_EQUAL(actual.nPos, BLOCK_SERIALIZATION_HEADER_SIZE + stored_genesis_size + BLOCK_SERIALIZATION_HEADER_SIZE);
}

BOOST_AUTO_TEST_CASE(blockmanager_compressed_record)
{
    if (!util::ZstdAvailable()) return;
    const auto params{CreateChainParams(ArgsManager{}, ChainType::REGTEST)};
    const BlockManager::Options blockman_opts{
        .chainparams = *params,
        .block_compression = 3,
        .blocks_dir = m_args.GetBlocksDirPath(),
    };
    BlockManager blockman{blockman_opts};

    // a block whose coinbase carries 20 KB of repeated bytes, so that it compresses
    CBlock block{params->GenesisBlock()};
    CMutableTransaction coinbase{*block.vtx[0]};
    coinbase.vout[0].scriptPubKey = CScript() << OP_RETURN << std::vector<unsigned char>(20000, 0x42);
    block.vtx[0] = MakeTransactionRef(coinbase);
    block.hashMerkleRoot = BlockMerkleRoot(block);
    while (!CheckProofOfWork(block.GetHash(), block.nBits, params->GetConsensus())) ++block.nNonce;
    const auto serialized_size{static_cast<unsigned int>(::GetSerializeSize(block, CLIENT_VERSION))};

    const FlatFilePos pos{blockman.SaveBlockToDisk(block, 1, nullptr)};
    BOOST_REQUIRE(!pos.IsNull());
    const uint32_t size_field{ReadSizeField(blockman, pos)};
    BOOST_CHECK(size_field & BLOCK_RECORD_COMPRESSED);
    BOOST_CHECK((size_field & BLOCK_RECORD_SIZE_MASK) < serialized_size);
    BOOST_CHECK(blockman.m_have_compressed_records);

    CBlock read_block;
    BOOST_CHECK(blockman.ReadBlockFromDisk(read_block, pos) == block.GetHash());
    BOOST_CHECK_EQUAL(read_block.vtx.size(), 1U);
    BOOST_CHECK(*read_block.vtx[0] == *block.vtx[0]);

    // reindex accounts for the stored record, not the serialized block
    const uint64_t size_after_write{blockman.GetBlockFileInfo(0)->nSize};
    BOOST_CHECK_EQUAL(size_after_write, pos.nPos + (size_field & BLOCK_RECORD_SIZE_MASK));
    FlatFilePos known{pos};
    blockman.m_have_compressed_records = false;
    BOOST_CHECK_EQUAL(blockman.SaveBlockToDisk(block, 1, &known).nPos, pos.nPos);
    BOOST_CHECK_EQUAL(blockman.GetBlockFileInfo(0)->nSize, size_after_write);
    BOOST_CHECK(blockman.m_have_compressed_records);

    // a compressed record that decompresses to more than the largest valid block is not read
    CBlock oversized{block};
    CMutableTransaction oversized_coinbase{coinbase};
    oversized_coinbase.vout[0].scriptPubKey = CScript() << OP_RETURN << std::vector<unsigned char>(MaxBlockSize() + 1000000, 0x42);
    oversized.vtx[0] = MakeTransactionRef(oversized_coinbase);
    oversized.hashMerkleRoot = BlockMerkleRoot(oversized);
    while (!CheckProofOfWork(oversized.GetHash(), oversized.nBits, params->GetConsensus())) ++oversized.nNonce;
    const FlatFilePos oversized_pos{blockman.SaveBlockToDisk(oversized, 2, nullptr)};
    BOOST_REQUIRE(ReadSizeField(blockman, oversized_pos) & BLOCK_RECORD_COMPRESSED);
    BOOST_CHECK(!blockman.ReadBlockFromDisk(read_block, oversized_pos));

    // compression disabled: the record holds the serialized block (written from the start of the block file again)
    const BlockManager::Options plain_opts{
        .chainparams = *params,
        .block_compression = 0,
        .blocks_dir = m_args.GetBlocksDirPath(),
    };
    BlockManager plain_blockman{plain_opts};
    const FlatFilePos plain_pos{plain_blockman.SaveBlockToDisk(block, 2, nullptr)};
    BOOST_CHECK_EQUAL(ReadSizeField(plain_blockman, plain_pos), serialized_size);
    BOOST_CHECK(plain_blockman.ReadBlockFromDisk(read_block, plain_pos) == block.GetHash());

    // a record written by software without compression support is still readable
    FlatFilePos legacy_pos{0, plain_blockman.GetBlockFileInfo(0)->nSize};
    {
        AutoFile file{plain_blockman.OpenBlockFile(legacy_pos)};
        file << params->MessageStart() << serialized_size;
        legacy_pos.nPos = FileTell64(file.Get());
        file << block;
    }
    BOOST_CHECK(plain_blockman.ReadBlockFromDisk(read_block, legacy_pos) == block.GetHash());
}

BOOST_FIXTURE_TEST_CASE(blockmanager_scan_unlink_already_pruned_files, TestChain100Setup)
{
    // Cap last block file size, and mine new block in a new block file.
    auto& chainman{*Assert(m_node.chainman)};
    auto& blockman{chainman.m_blockman};
    const CBlockIndex* old_tip{WITH_LOCK(chainman.GetMutex(), return chainman.ActiveChain().Tip())};
    WITH_LOCK(chainman.GetMutex(), blockman.GetBlockFileInfo(old_tip->GetBlockPos().nFile)->nSize = MAX_BLOCKFILE_SIZE);
    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));

    // Prune the older block file, but don't unlink it
    int file_number;
    {
        LOCK(chainman.GetMutex());
        file_number = old_tip->GetBlockPos().nFile;
        blockman.PruneOneBlockFile(file_number);
    }

    const FlatFilePos pos(file_number, 0);

    // Check that the file is not unlinked after ScanAndUnlinkAlreadyPrunedFiles
    // if m_have_pruned is not yet set
    WITH_LOCK(chainman.GetMutex(), blockman.ScanAndUnlinkAlreadyPrunedFiles());
    BOOST_CHECK(!CAutoFile(blockman.OpenBlockFile(pos, true), CLIENT_VERSION).IsNull());

    // Check that the file is unlinked after ScanAndUnlinkAlreadyPrunedFiles
    // once m_have_pruned is set
    blockman.m_have_pruned = true;
    WITH_LOCK(chainman.GetMutex(), blockman.ScanAndUnlinkAlreadyPrunedFiles());
    BOOST_CHECK(CAutoFile(blockman.OpenBlockFile(pos, true), CLIENT_VERSION).IsNull());

    // Check that calling with already pruned files doesn't cause an error
    WITH_LOCK(chainman.GetMutex(), blockman.ScanAndUnlinkAlreadyPrunedFiles());

    // Check that the new tip file has not been removed
    const CBlockIndex* new_tip{WITH_LOCK(chainman.GetMutex(), return chainman.ActiveChain().Tip())};
    BOOST_CHECK_NE(old_tip, new_tip);
    const int new_file_number{WITH_LOCK(chainman.GetMutex(), return new_tip->GetBlockPos().nFile)};
    const FlatFilePos new_pos(new_file_number, 0);
    BOOST_CHECK(!CAutoFile(blockman.OpenBlockFile(new_pos, true), CLIENT_VERSION).IsNull());
}

BOOST_AUTO_TEST_SUITE_END()
