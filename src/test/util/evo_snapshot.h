// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_UTIL_EVO_SNAPSHOT_H
#define BITCOIN_TEST_UTIL_EVO_SNAPSHOT_H

#include <clientversion.h>
#include <evo/snapshot.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <streams.h>
#include <version.h>

#include <cstdint>
#include <vector>

inline std::vector<unsigned char> SerializeTransactionForTest(const CTransaction& tx)
{
    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    stream << tx;
    return {UCharCast(stream.data()), UCharCast(stream.data() + stream.size())};
}

//! The wire bytes of a canonically ordered snapshot, assembled field by field
//! in the order of the format table rather than through EvoSnapshot's own
//! serializer, so a pinned hash of them is independent of the codec under
//! test. Version 3 omits every field v4 added.
inline std::vector<unsigned char> AssembleWireBytes(const evo::EvoSnapshot& snapshot, uint16_t version)
{
    CDataStream s{SER_DISK, CLIENT_VERSION};
    s << version << snapshot.base_block_hash;
    evo::SerializeCanonicalMNList(s, snapshot.mn_list);
    if (version >= 4) {
        if (snapshot.base_coinbase) {
            const auto coinbase{SerializeTransactionForTest(*snapshot.base_coinbase)};
            WriteCompactSize(s, coinbase.size());
            s.write(MakeByteSpan(coinbase));
        } else {
            WriteCompactSize(s, 0);
        }
        WriteCompactSize(s, snapshot.base_coinbase_branch.size());
        for (const auto& node : snapshot.base_coinbase_branch)
            s << node;
    }
    WriteCompactSize(s, snapshot.quorums.size());
    for (const auto& data : snapshot.quorums) {
        s << static_cast<uint8_t>(data.llmq_type) << uint8_t{data.rotation_enabled};
        for (const auto* commitments : {&data.active_commitments, &data.safety_commitments}) {
            WriteCompactSize(s, commitments->size());
            for (const auto& entry : *commitments) {
                s << entry.quorum_base_block_hash << entry.work_block_hash << entry.commitment << entry.mined_block_hash;
            }
        }
        WriteCompactSize(s, data.rotation_snapshots.size());
        for (const auto& entry : data.rotation_snapshots)
            evo::WriteRotationSnapshot(s, entry);
        if (version >= 4) {
            WriteCompactSize(s, data.bootstrap_work.size());
            for (const auto& entry : data.bootstrap_work)
                s << int32_t{entry.cycle_base_height} << entry.work_block_hash;
        }
    }
    WriteCompactSize(s, snapshot.historical_mn_list_diffs.size());
    for (const auto& entry : snapshot.historical_mn_list_diffs) {
        s << entry.previous_block_hash << entry.block_hash << int32_t{entry.height} << entry.total_registered_count
          << entry.canonical_list_hash;
        evo::SerializeCanonicalMNListDiff(s, entry.diff);
    }
    WriteCompactSize(s, snapshot.quorum_modifiers.size());
    for (const auto& entry : snapshot.quorum_modifiers) {
        s << static_cast<uint8_t>(entry.llmq_type) << entry.work_block_hash << entry.modifier;
    }
    s << snapshot.credit_pool.locked << snapshot.credit_pool.currentLimit << snapshot.credit_pool.latelyUnlocked
      << snapshot.credit_pool.indexes;
    if (version >= 4) {
        WriteCompactSize(s, snapshot.credit_history.size());
        for (const auto& entry : snapshot.credit_history)
            s << entry.credit_pool << entry.unlocked;
    }
    WriteCompactSize(s, snapshot.mnhf_signals.size());
    for (const auto& [bit, height] : snapshot.mnhf_signals)
        s << bit << int32_t{height};
    return {UCharCast(s.data()), UCharCast(s.data() + s.size())};
}

#endif // BITCOIN_TEST_UTIL_EVO_SNAPSHOT_H
