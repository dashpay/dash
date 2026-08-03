// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <chainparams.h>
#include <llmq/blockprocessor.h>
#include <llmq/cache.h>
#include <llmq/context.h>
#include <llmq/params.h>
#include <llmq/quorumsman.h>
#include <uint256.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

// Consensus::LLMQType is a uint8_t enum serialized verbatim over the wire, so a peer can name
// either an unknown enum value or a known type that this chain does not use. PerLlmqTypeCache
// holds a cache only for the types registered on the active chain and answers for the rest as
// misses; before it existed these were plain std::map<LLMQType, ...>, and operator[] on an
// unregistered key default-constructed a zero-capacity LRU whose constructor asserts.
//
// These tests cover the cache contract itself plus the two lookup paths a wire-supplied type
// reaches: HasMinedCommitment directly, and GetQuorum, which funnels into it via HasQuorum.

BOOST_AUTO_TEST_SUITE(llmq_invalid_type_tests)

// Not a named enumerator, and never present in any chain's consensus params.
static constexpr Consensus::LLMQType UNKNOWN_LLMQ_TYPE{static_cast<Consensus::LLMQType>(0)};
// A real enumerator registered on testnet but not regtest.
static constexpr Consensus::LLMQType UNREGISTERED_LLMQ_TYPE{Consensus::LLMQType::LLMQ_25_67};

BOOST_FIXTURE_TEST_CASE(per_llmq_type_cache_ignores_unknown_types, BasicTestingSetup)
{
    llmq::PerLlmqTypeCache<bool> cache;
    BOOST_CHECK(!cache.IsInitialized());
    cache.Init(Params().GetConsensus());
    BOOST_REQUIRE(cache.IsInitialized());

    BOOST_REQUIRE(!Params().GetLLMQ(UNKNOWN_LLMQ_TYPE).has_value());
    BOOST_CHECK_EQUAL(cache.max_size(UNKNOWN_LLMQ_TYPE), 0U);

    // Writes for an unregistered type are dropped rather than sized into existence, and reads
    // stay misses -- the value below must be left alone.
    bool value{true};
    cache.insert(UNKNOWN_LLMQ_TYPE, uint256::ONE, false);
    BOOST_CHECK(!cache.get(UNKNOWN_LLMQ_TYPE, uint256::ONE, value));
    BOOST_CHECK(value);
    cache.erase(UNKNOWN_LLMQ_TYPE, uint256::ONE);
    cache.clear(UNKNOWN_LLMQ_TYPE);
    BOOST_CHECK_EQUAL(cache.max_size(UNKNOWN_LLMQ_TYPE), 0U);

    for (const auto& llmq : Params().GetConsensus().llmqs) {
        BOOST_CHECK_GT(cache.max_size(llmq.type), 0U);

        value = false;
        cache.insert(llmq.type, uint256::ONE, true);
        BOOST_CHECK(cache.get(llmq.type, uint256::ONE, value));
        BOOST_CHECK(value);

        cache.erase(llmq.type, uint256::ONE);
        BOOST_CHECK(!cache.get(llmq.type, uint256::ONE, value));
    }
}

// The crash site itself: an unregistered type must answer false, not abort.
BOOST_FIXTURE_TEST_CASE(has_mined_commitment_unknown_llmq_type_is_safe, RegTestingSetup)
{
    auto& qbp = *Assert(m_node.llmq_ctx)->quorum_block_processor;
    const uint256 hash = WITH_LOCK(::cs_main, return m_node.chainman->ActiveTip()->GetBlockHash());

    BOOST_REQUIRE(!Params().GetLLMQ(UNKNOWN_LLMQ_TYPE).has_value());
    BOOST_REQUIRE(!Params().GetLLMQ(UNREGISTERED_LLMQ_TYPE).has_value());

    BOOST_CHECK(!qbp.HasMinedCommitment(UNKNOWN_LLMQ_TYPE, hash));
    BOOST_CHECK(!qbp.HasMinedCommitment(UNREGISTERED_LLMQ_TYPE, hash));

    for (const auto& llmq : Params().GetConsensus().llmqs) {
        BOOST_CHECK(!qbp.HasMinedCommitment(llmq.type, hash));
    }
}

// The path a wire-supplied type actually travels: GetQuorum resolves the block index from the
// hash, then consults HasQuorum -> HasMinedCommitment. Must return nullptr, not abort.
BOOST_FIXTURE_TEST_CASE(get_quorum_unknown_llmq_type_is_safe, RegTestingSetup)
{
    auto& qman = *Assert(m_node.llmq_ctx)->qman;
    const uint256 tip_hash = WITH_LOCK(::cs_main, return m_node.chainman->ActiveTip()->GetBlockHash());

    BOOST_REQUIRE(!Params().GetLLMQ(UNKNOWN_LLMQ_TYPE).has_value());
    BOOST_REQUIRE(!Params().GetLLMQ(UNREGISTERED_LLMQ_TYPE).has_value());

    BOOST_CHECK(qman.GetQuorum(UNKNOWN_LLMQ_TYPE, tip_hash) == nullptr);
    BOOST_CHECK(qman.GetQuorum(UNREGISTERED_LLMQ_TYPE, tip_hash) == nullptr);

    // Same map, reached by the QDATA handler with a wire-supplied type.
    BOOST_CHECK(qman.GetCachedMutableQuorum(UNKNOWN_LLMQ_TYPE, tip_hash) == nullptr);
    BOOST_CHECK(qman.GetCachedMutableQuorum(UNREGISTERED_LLMQ_TYPE, tip_hash) == nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
