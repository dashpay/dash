// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <clientversion.h>
#include <consensus/merkle.h>
#include <node/blockstorage.h>
#include <node/context.h>
#include <pow.h>
#include <script/script.h>
#include <streams.h>
#include <undo.h>
#include <util/fs.h>
#include <util/string.h>
#include <util/system.h>
#include <util/zstd.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>
#include <test/util/setup_common.h>

#include <algorithm>

using node::BlockManager;
using node::BLOCK_RECORD_COMPRESSED;
using node::BLOCK_RECORD_SIZE_MASK;
using node::BLOCK_SERIALIZATION_HEADER_SIZE;
using node::MAX_BLOCKFILE_SIZE;
using node::OpenBlockFile;
using node::ReadBlockFromDisk;
using node::UndoReadFromDisk;

/** Size field of the record whose payload starts at payload_pos in file */
static uint32_t ReadSizeField(FILE* file, unsigned int payload_pos)
{
    AutoFile filein{file};
    BOOST_REQUIRE_EQUAL(fseek(filein.Get(), payload_pos - sizeof(uint32_t), SEEK_SET), 0);
    uint32_t size_field;
    filein >> size_field;
    return size_field;
}

/** Size field of the block record whose payload starts at pos */
static uint32_t ReadSizeField(const FlatFilePos& pos)
{
    return ReadSizeField(OpenBlockFile(pos, true), pos.nPos);
}

/** Size field of the undo record whose payload starts at pos */
static uint32_t ReadUndoSizeField(const FlatFilePos& pos)
{
    return ReadSizeField(fsbridge::fopen(gArgs.GetBlocksDirPath() / fs::PathFromString(strprintf("rev%05u.dat", pos.nFile)), "rb"), pos.nPos);
}

// use BasicTestingSetup here for the data directory configuration, setup, and cleanup
BOOST_FIXTURE_TEST_SUITE(blockmanager_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(blockmanager_find_block_pos)
{
    const auto params {CreateChainParams(ArgsManager{}, CBaseChainParams::MAIN)};
    node::BlockManager::Options blockman_opts{
        .chainparams = *params,
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
    const uint32_t stored_genesis_size{ReadSizeField(pos) & BLOCK_RECORD_SIZE_MASK};
    BOOST_CHECK(stored_genesis_size <= ::GetSerializeSize(params->GenesisBlock(), CLIENT_VERSION));
    FlatFilePos actual{blockman.SaveBlockToDisk(params->GenesisBlock(), 1, nullptr)};
    BOOST_CHECK_EQUAL(actual.nPos, BLOCK_SERIALIZATION_HEADER_SIZE + stored_genesis_size + BLOCK_SERIALIZATION_HEADER_SIZE);
}

BOOST_AUTO_TEST_CASE(blockmanager_compressed_record)
{
    if (!util::ZstdAvailable()) return;
    const auto params{CreateChainParams(ArgsManager{}, CBaseChainParams::REGTEST)};
    node::BlockManager::Options blockman_opts{
        .chainparams = *params,
    };
    BlockManager blockman{blockman_opts};
    gArgs.ForceSetArg("-blockcompression", "3");

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
    const uint32_t size_field{ReadSizeField(pos)};
    BOOST_CHECK(size_field & BLOCK_RECORD_COMPRESSED);
    BOOST_CHECK((size_field & BLOCK_RECORD_SIZE_MASK) < serialized_size);

    CBlock read_block;
    BOOST_CHECK(ReadBlockFromDisk(read_block, pos, params->GetConsensus()) == block.GetHash());
    BOOST_CHECK_EQUAL(read_block.vtx.size(), 1U);
    BOOST_CHECK(*read_block.vtx[0] == *block.vtx[0]);

    // reindex accounts for the stored record, not the serialized block
    const unsigned int size_after_write{blockman.GetBlockFileInfo(0)->nSize};
    BOOST_CHECK_EQUAL(size_after_write, pos.nPos + (size_field & BLOCK_RECORD_SIZE_MASK));
    FlatFilePos known{pos};
    BOOST_CHECK_EQUAL(blockman.SaveBlockToDisk(block, 1, &known).nPos, pos.nPos);
    BOOST_CHECK_EQUAL(blockman.GetBlockFileInfo(0)->nSize, size_after_write);

    // compression disabled: the record holds the serialized block
    gArgs.ForceSetArg("-blockcompression", "0");
    const FlatFilePos plain_pos{blockman.SaveBlockToDisk(block, 2, nullptr)};
    BOOST_CHECK_EQUAL(ReadSizeField(plain_pos), serialized_size);
    BOOST_CHECK(ReadBlockFromDisk(read_block, plain_pos, params->GetConsensus()) == block.GetHash());
    gArgs.ForceSetArg("-blockcompression", ToString(util::DEFAULT_ZSTD_LEVEL));

    // a record written by software without compression support is still readable
    FlatFilePos legacy_pos{0, blockman.GetBlockFileInfo(0)->nSize};
    {
        AutoFile file{OpenBlockFile(legacy_pos)};
        file << params->MessageStart() << serialized_size;
        legacy_pos.nPos = ftell(file.Get());
        file << block;
    }
    BOOST_CHECK(ReadBlockFromDisk(read_block, legacy_pos, params->GetConsensus()) == block.GetHash());
}

BOOST_AUTO_TEST_CASE(blockmanager_compressed_undo_record)
{
    if (!util::ZstdAvailable()) return;
    const auto params{CreateChainParams(ArgsManager{}, CBaseChainParams::REGTEST)};
    node::BlockManager::Options blockman_opts{
        .chainparams = *params,
    };
    BlockManager blockman{blockman_opts};
    gArgs.ForceSetArg("-blockcompression", "3");
    // undo data goes next to the block file, so block file 0 has to exist
    BOOST_REQUIRE(!blockman.SaveBlockToDisk(params->GenesisBlock(), 0, nullptr).IsNull());

    // undo data spending 200 outputs with the same script, so that it compresses
    CBlockUndo undo;
    undo.vtxundo.emplace_back();
    for (int i{0}; i < 200; ++i) {
        undo.vtxundo.back().vprevout.emplace_back(CTxOut{i * COIN, CScript() << OP_RETURN << std::vector<unsigned char>(50, 0x42)}, /*nHeightIn=*/100, /*fCoinBaseIn=*/false);
    }
    DataStream serialized_undo;
    serialized_undo << undo;

    const uint256 prev_hash{params->GenesisBlock().GetHash()};
    CBlockIndex prev{params->GenesisBlock()};
    prev.phashBlock = &prev_hash;
    const auto write_undo{[&](CBlockIndex& index) {
        index.pprev = &prev;
        index.nFile = 0;
        BlockValidationState state;
        LOCK(::cs_main);
        BOOST_REQUIRE(blockman.WriteUndoDataForBlock(undo, state, index));
        return index.GetUndoPos();
    }};
    const auto read_undo{[&](const CBlockIndex& index) {
        CBlockUndo read_undo;
        BOOST_CHECK(UndoReadFromDisk(read_undo, &index));
        DataStream serialized_read_undo;
        serialized_read_undo << read_undo;
        BOOST_CHECK(std::ranges::equal(serialized_read_undo, serialized_undo));
    }};

    CBlockIndex index{params->GenesisBlock()};
    const FlatFilePos pos{write_undo(index)};
    const uint32_t size_field{ReadUndoSizeField(pos)};
    BOOST_CHECK(size_field & BLOCK_RECORD_COMPRESSED);
    BOOST_CHECK((size_field & BLOCK_RECORD_SIZE_MASK) < serialized_undo.size());
    // the undo file accounts for the stored record followed by its checksum
    BOOST_CHECK_EQUAL(blockman.GetBlockFileInfo(0)->nUndoSize, pos.nPos + (size_field & BLOCK_RECORD_SIZE_MASK) + uint256::size());
    read_undo(index);

    // compression disabled: the record holds the serialized undo data
    gArgs.ForceSetArg("-blockcompression", "0");
    CBlockIndex plain_index{params->GenesisBlock()};
    const FlatFilePos plain_pos{write_undo(plain_index)};
    BOOST_CHECK_EQUAL(ReadUndoSizeField(plain_pos), serialized_undo.size());
    read_undo(plain_index);
    gArgs.ForceSetArg("-blockcompression", ToString(util::DEFAULT_ZSTD_LEVEL));
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
    BOOST_CHECK(!CAutoFile(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION).IsNull());

    // Check that the file is unlinked after ScanAndUnlinkAlreadyPrunedFiles
    // once m_have_pruned is set
    blockman.m_have_pruned = true;
    WITH_LOCK(chainman.GetMutex(), blockman.ScanAndUnlinkAlreadyPrunedFiles());
    BOOST_CHECK(CAutoFile(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION).IsNull());

    // Check that calling with already pruned files doesn't cause an error
    WITH_LOCK(chainman.GetMutex(), blockman.ScanAndUnlinkAlreadyPrunedFiles());

    // Check that the new tip file has not been removed
    const CBlockIndex* new_tip{WITH_LOCK(chainman.GetMutex(), return chainman.ActiveChain().Tip())};
    BOOST_CHECK_NE(old_tip, new_tip);
    const int new_file_number{WITH_LOCK(chainman.GetMutex(), return new_tip->GetBlockPos().nFile)};
    const FlatFilePos new_pos(new_file_number, 0);
    BOOST_CHECK(!CAutoFile(OpenBlockFile(new_pos, true), SER_DISK, CLIENT_VERSION).IsNull());
}

BOOST_AUTO_TEST_SUITE_END()
