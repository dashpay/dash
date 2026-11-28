// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/consensus.h>
#include <evo/assetlocktx.h>
#include <evo/specialtx.h>
#include <hash.h>
#include <instantsend/instantsend.h>
#include <instantsend/lock.h>
#include <instantsend/net_instantsend.h>
#include <interfaces/handler.h>
#include <llmq/context.h>
#include <llmq/quorumsman.h>
#include <llmq/signhash.h>
#include <llmq/signing.h>
#include <primitives/transaction.h>
#include <protocol.h>
#include <script/solver.h>
#include <spork.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <validation.h>

#include <optional>
#include <string_view>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(evo_islock_tests)

BOOST_FIXTURE_TEST_CASE(chainlocked_conflict_overrides_confirmed_locks, TestChain100Setup)
{
    constexpr const char* REGTEST_SPORK_PRIVKEY{"cP4EKFyJsHT39LDqgdcB43Y3YXjNyjb5Fuas1GQSeAtjnZWmZEQK"};
    BOOST_REQUIRE(m_node.sporkman->SetSporkAddress(Params().SporkAddress()));
    BOOST_REQUIRE(m_node.sporkman->SetPrivKey(REGTEST_SPORK_PRIVKEY));
    BOOST_REQUIRE(m_node.sporkman->UpdateSpork(SPORK_2_INSTANTSEND_ENABLED, 0).has_value());

    // Confirming a lock keeps it only in the db caches; a ChainLocked transaction that later
    // overrides such locks must leave neither a conflict nor their transactions reported as locked
    auto& isman = *m_node.isman;
    CMutableTransaction conflict;
    conflict.vin.emplace_back(COutPoint{GetRandHash(), 0});
    conflict.vin.emplace_back(COutPoint{GetRandHash(), 1});
    conflict.vout.emplace_back(COIN, CScript{});
    const CTransaction conflict_tx{conflict};

    const auto* tip = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip());
    std::vector<uint256> locked_txids;
    // The second lock takes over the cached index entry of the input both locks share
    const COutPoint& shared_input{conflict_tx.vin[1].prevout};
    for (const auto& inputs : {std::vector{conflict_tx.vin[0].prevout, shared_input}, std::vector{shared_input}}) {
        auto islock = std::make_shared<instantsend::InstantSendLock>();
        islock->txid = GetRandHash();
        islock->inputs = inputs;
        isman.WriteNewISLock(::SerializeHash(*islock), islock, tip->nHeight);
        locked_txids.push_back(islock->txid);
    }
    isman.RemoveConfirmedInstantSendLocks(tip);
    for (const auto& txid : locked_txids) {
        BOOST_REQUIRE(isman.IsLocked(txid));
    }
    BOOST_REQUIRE(isman.ConflictingISLockIfAny(conflict_tx));

    isman.RemoveConflictingISLocksByTx(conflict_tx);
    BOOST_CHECK(!isman.ConflictingISLockIfAny(conflict_tx));
    for (const auto& txid : locked_txids) {
        BOOST_CHECK(!isman.IsLocked(txid));
    }
}

//! A lock discarded for a block it conflicts with, whether stored or still
//! waiting for its transaction, is no longer served, and is not requested again.
BOOST_FIXTURE_TEST_CASE(discarded_lock_is_not_served, TestChain100Setup)
{
    constexpr const char* REGTEST_SPORK_PRIVKEY{"cP4EKFyJsHT39LDqgdcB43Y3YXjNyjb5Fuas1GQSeAtjnZWmZEQK"};
    BOOST_REQUIRE(m_node.sporkman->SetSporkAddress(Params().SporkAddress()));
    BOOST_REQUIRE(m_node.sporkman->SetPrivKey(REGTEST_SPORK_PRIVKEY));
    BOOST_REQUIRE(m_node.sporkman->UpdateSpork(SPORK_2_INSTANTSEND_ENABLED, 0).has_value());

    auto& isman = *m_node.isman;
    for (const bool stored : {true, false}) {
        auto islock = std::make_shared<instantsend::InstantSendLock>();
        islock->txid = GetRandHash();
        islock->inputs = {COutPoint{GetRandHash(), 0}};
        const uint256 hash{::SerializeHash(*islock)};
        if (stored) {
            isman.WriteNewISLock(hash, islock, /*minedHeight=*/std::nullopt);
        } else {
            isman.AddPendingISLock(hash, islock, /*from=*/0);
        }
        instantsend::InstantSendLock served;
        BOOST_REQUIRE(isman.GetInstantSendLockByHash(hash, served));
        BOOST_CHECK_EQUAL(isman.HasTxForLock(hash), stored);

        isman.RemoveConflictingLock(hash, *islock);
        isman.RemovePendingNoTxLock(hash);
        BOOST_CHECK(!isman.GetInstantSendLockByHash(hash, served));
        BOOST_CHECK(isman.HasTxForLock(hash));
        BOOST_CHECK(isman.AlreadyHave(CInv{MSG_ISDLOCK, hash}));
    }
}

uint256 CalculateRequestId(const std::vector<COutPoint>& inputs)
{
    HashWriter hw{};
    hw << std::string_view("islock");
    hw << inputs;
    return hw.GetHash();
}

BOOST_AUTO_TEST_CASE(getrequestid)
{
    // Create an empty InstantSendLock
    instantsend::InstantSendLock islock;

    // Compute expected hash for an empty inputs vector.
    // Note: InstantSendLock::GetRequestId() serializes the prefix "islock"
    // followed by the 'inputs' vector.
    {
        const uint256 expected = CalculateRequestId(islock.inputs);

        BOOST_CHECK(islock.GetRequestId() == expected);
    }

    // Now add two dummy inputs to the lock
    islock.inputs.clear();
    // Construct two dummy outpoints (using uint256S for a dummy hash)
    COutPoint op1(uint256::ONE, 0);
    COutPoint op2(uint256::TWO, 1);
    islock.inputs.push_back(op1);
    islock.inputs.push_back(op2);

    const uint256 expected = CalculateRequestId(islock.inputs);

    BOOST_CHECK(islock.GetRequestId() == expected);
}

BOOST_AUTO_TEST_CASE(deserialize_instantlock_from_realdata2)
{
    // Expected values from the provided getislocks output:
    const std::string_view expectedTxidStr = "7b33968effa613e8ea9c1b5734c9bbbe467ff4650f8060caf8a5c213c6059d5b";
    const std::string_view expectedCycleHashStr = "000000000000000bbd0b1bb95540351e7ee99c5b08efde076b3d712a57ea74d6";
    const std::string_view expectedSignatureStr =
        "997d0b36738a9eef46ceeb4405998ff7235317708f277402799ffe05258015cae9b6bae"
        "43683f992b2f50f70f8f0cb9c0f26af340b00903e93995c1345d1b2c5b697ebecdbe581"
        "1dd112e11889101dcb4553b2bc206ab304026b96c07dec4f24";
    const std::string quorumHashStr = "0000000000000019756ecc9c9c5f476d3f66876b1dcfa5dde1ea82f0d99334a2";
    const std::string_view expectedSignHashStr = "6a3c37bc610c4efd5babd8941068a8eca9e7bec942fe175b8ca9cae31b67e838";
    // The serialized InstantSend lock from the "hex" field of getislocks:
    const std::string_view islockHex =
        "0101497915895c30eebfad0c5fcfb9e0e72308c7e92cd3749be2fd49c8320c4c58b6010000005b9d05c613c2a5f8ca60800f65f47f46be"
        "bbc934571b9ceae813a6ff8e96337bd674ea572a713d6b07deef085b9ce97e1e354055b91b0bbd0b00000000000000997d0b36738a9eef"
        "46ceeb4405998ff7235317708f277402799ffe05258015cae9b6bae43683f992b2f50f70f8f0cb9c0f26af340b00903e93995c1345d1b2"
        "c5b697ebecdbe5811dd112e11889101dcb4553b2bc206ab304026b96c07dec4f24";

    // This InstantSend lock's signature is basic. An un-annotated decode reads it as basic.
    // Convert hex string to a byte vector and deserialize.
    std::vector<unsigned char> islockData = ParseHex(islockHex);
    DataStream ss{islockData};
    instantsend::InstantSendLock islock;
    ss >> islock;

    // Verify the calculated signHash
    auto signHash =
        llmq::SignHash(Consensus::LLMQType::LLMQ_60_75, uint256S(quorumHashStr), islock.GetRequestId(), islock.txid).Get();
    BOOST_CHECK_EQUAL(signHash.ToString(), expectedSignHashStr);

    // Verify the txid field.
    BOOST_CHECK_EQUAL(islock.txid.ToString(), expectedTxidStr);

    // Verify the cycleHash field.
    BOOST_CHECK_EQUAL(islock.cycleHash.ToString(), expectedCycleHashStr);

    // Verify the inputs vector has exactly one element.
    BOOST_REQUIRE_EQUAL(islock.inputs.size(), 1U);
    const COutPoint& input = islock.inputs.front();
    const std::string expectedInputTxid = "b6584c0c32c849fde29b74d32ce9c70823e7e0b9cf5f0cadbfee305c89157949";
    const unsigned int expectedInputN = 1;
    BOOST_CHECK_EQUAL(input.hash.ToString(), expectedInputTxid);
    BOOST_CHECK_EQUAL(input.n, expectedInputN);

    // Compute the expected request ID: it is the hash of the constant prefix "islock" followed by the inputs.
    uint256 expectedRequestId = CalculateRequestId(islock.inputs);
    BOOST_CHECK_EQUAL(islock.GetRequestId().ToString(), expectedRequestId.ToString());

    // Verify the signature field.
    BOOST_CHECK_EQUAL(islock.sig.Get().ToString(), expectedSignatureStr);
}

BOOST_AUTO_TEST_CASE(geninputlockrequestid_serialization_format)
{
    // Test that the serialization format is: SerializeHash(pair("inlock", outpoint))
    const uint256 txHash = uint256S("0x0000000000000000000000000000000000000000000000000000000000000001");
    const uint32_t outputIndex = 0;

    COutPoint outpoint(txHash, outputIndex);

    // Calculate the expected hash manually
    const uint256 expectedHash = ::SerializeHash(std::make_pair(std::string_view("inlock"), outpoint));

    // Get the actual hash from the function
    const uint256 actualHash = instantsend::GenInputLockRequestId(outpoint);

    BOOST_CHECK(actualHash == expectedHash);

    // A null prevout hash and a null output index still hash as outpoints. Both
    // ids stay non-null, and they stay distinct from each other.
    const COutPoint null_outpoint{uint256{}, 0};
    const COutPoint null_index_outpoint{uint256::ONE, COutPoint::NULL_INDEX};
    const uint256 null_id{instantsend::GenInputLockRequestId(null_outpoint)};
    const uint256 null_index_id{instantsend::GenInputLockRequestId(null_index_outpoint)};
    BOOST_CHECK(!null_id.IsNull());
    BOOST_CHECK(!null_index_id.IsNull());
    BOOST_CHECK(null_id != null_index_id);
    BOOST_CHECK(null_id == ::SerializeHash(std::make_pair(std::string_view("inlock"), null_outpoint)));
    BOOST_CHECK(null_index_id == ::SerializeHash(std::make_pair(std::string_view("inlock"), null_index_outpoint)));
}

// Regression test for the islock input cap: an oversized input vector must be
// rejected by TriviallyValid(), while the cap is derived so it can never reject a valid lock.
BOOST_AUTO_TEST_CASE(trivially_valid_input_cap)
{
    // A lock with a non-null txid and a few unique inputs is trivially valid.
    instantsend::InstantSendLock islock;
    islock.txid = uint256::ONE;
    islock.inputs = {COutPoint(uint256::ONE, 0), COutPoint(uint256::ONE, 1)};
    BOOST_CHECK(islock.TriviallyValid());

    // MAX_INPUTS is derived from MaxBlockSize(): a consensus-valid transaction must fit in a
    // block and each input is >=41 bytes on the wire, so it can never carry more than
    // MaxBlockSize() / 41 inputs. The cap thus cannot reject a legitimate islock, and it
    // tracks any future block-size change.
    BOOST_CHECK_EQUAL(instantsend::InstantSendLock::MAX_INPUTS, MaxBlockSize() / 41);

    // A lock carrying more than MAX_INPUTS inputs is rejected up front, before any O(n)
    // hashing/dedup work, so a peer cannot pin an oversized input vector.
    instantsend::InstantSendLock oversized;
    oversized.txid = uint256::ONE;
    oversized.inputs.assign(instantsend::InstantSendLock::MAX_INPUTS + 1, COutPoint(uint256::ONE, 0));
    BOOST_CHECK(!oversized.TriviallyValid());
}

BOOST_AUTO_TEST_CASE(lock_inputs_of_asset_unlock)
{
    // An asset unlock has no inputs; its lock pins the synthetic outpoint {DIP-27 request id, 0}
    const auto make_unlock = [](uint8_t version, uint64_t index, uint32_t requested_height) {
        CMutableTransaction mtx;
        mtx.nVersion = 3;
        mtx.nType = TRANSACTION_ASSET_UNLOCK;
        mtx.vout.emplace_back(COIN, CScript{});
        SetTxPayload(mtx, CAssetUnlockPayload{version, index, /*fee=*/2000, requested_height, uint256::ONE,
                                              CBLSSignature{}});
        return CTransaction{mtx};
    };
    const uint64_t index{0x0011223344556677ULL};
    const uint256 request_id{::SerializeHash(std::make_pair(std::string{"plwdtx"}, index))};

    const CTransaction unlock{make_unlock(2, index, 500)};
    BOOST_CHECK(instantsend::HasLockInputs(unlock));
    const auto inputs = instantsend::GetLockInputs(unlock);
    BOOST_REQUIRE_EQUAL(inputs.size(), 1U);
    BOOST_CHECK(inputs[0] == COutPoint(request_id, 0));

    // Every instance of one withdrawal maps to the same outpoint: re-signs (same txid), and
    // version 1 instances (different txid) - so any two claimants of the index conflict
    BOOST_CHECK(instantsend::GetLockInputs(make_unlock(2, index, 700)) == inputs);
    const CTransaction unlock_v1{make_unlock(1, index, 500)};
    BOOST_CHECK(unlock_v1.GetHash() != unlock.GetHash());
    BOOST_CHECK(instantsend::GetLockInputs(unlock_v1) == inputs);
    // A different withdrawal maps elsewhere
    BOOST_CHECK(instantsend::GetLockInputs(make_unlock(2, index + 1, 500)) != inputs);

    // Ordinary transactions pin their prevouts; input-less non-unlock transactions pin nothing
    CMutableTransaction spend;
    spend.vin.emplace_back(COutPoint(uint256::TWO, 3));
    spend.vin.emplace_back(COutPoint(uint256::ONE, 1));
    BOOST_CHECK(instantsend::HasLockInputs(CTransaction{spend}));
    BOOST_CHECK((instantsend::GetLockInputs(CTransaction{spend}) ==
                 std::vector<COutPoint>{COutPoint(uint256::TWO, 3), COutPoint(uint256::ONE, 1)}));
    CMutableTransaction commitment;
    commitment.nVersion = 3;
    commitment.nType = TRANSACTION_QUORUM_COMMITMENT;
    BOOST_CHECK(!instantsend::HasLockInputs(CTransaction{commitment}));
    BOOST_CHECK(instantsend::GetLockInputs(CTransaction{commitment}).empty());
    CMutableTransaction coinbase;
    coinbase.vin.emplace_back(COutPoint{});
    BOOST_CHECK(!instantsend::HasLockInputs(CTransaction{coinbase}));
}

BOOST_FIXTURE_TEST_CASE(nonlocked_asset_unlock_refresh_is_retried, TestChain100Setup)
{
    // A re-signed instance of a tracked version 2 asset unlock shares its txid; the retry queue
    // must hand out the fresh instance, not the stale one first tracked
    auto& isman = *m_node.isman;
    const auto make_unlock = [](uint32_t requested_height) {
        CMutableTransaction mtx;
        mtx.nVersion = 3;
        mtx.nType = TRANSACTION_ASSET_UNLOCK;
        mtx.vout.emplace_back(COIN, CScript{});
        SetTxPayload(mtx, CAssetUnlockPayload{2, /*index=*/7, /*fee=*/2000, requested_height, uint256::ONE,
                                              CBLSSignature{}});
        return MakeTransactionRef(mtx);
    };
    const auto unlock = make_unlock(500);
    const auto unlock_resigned = make_unlock(700);
    BOOST_REQUIRE(unlock->GetHash() == unlock_resigned->GetHash());
    BOOST_REQUIRE(unlock->GetInstanceHash() != unlock_resigned->GetInstanceHash());

    isman.AddNonLockedTx(unlock, nullptr);
    isman.AddNonLockedTx(unlock_resigned, nullptr);
    isman.RetryUnminedAssetUnlocks();
    const auto retry = isman.PrepareTxToRetry();
    BOOST_REQUIRE_EQUAL(retry.size(), 1U);
    BOOST_CHECK(retry[0]->GetInstanceHash() == unlock_resigned->GetInstanceHash());

    // Once handed out, an unlock is not retried again until the next block or refresh re-queues
    // it; otherwise the worker would re-verify its signature on every iteration
    BOOST_CHECK(isman.PrepareTxToRetry().empty());
    isman.RetryUnminedAssetUnlocks();
    BOOST_CHECK_EQUAL(isman.PrepareTxToRetry().size(), 1U);

    // A stale instance arriving after the fresh one is likewise recorded: the mempool (not this
    // tracker) decides which instance is held, and the tracker mirrors the last admitted one
    isman.RemoveNonLockedTx(unlock->GetHash(), false);
    BOOST_CHECK(isman.PrepareTxToRetry().empty());
}

BOOST_FIXTURE_TEST_CASE(nonlocked_asset_unlock_dropped_when_removed_from_mempool, TestChain100Setup)
{
    // An unlocked asset unlock leaving the mempool has no inputs for the conflict cleanup to key
    // on; the removal hook must drop it from the tracker or it is re-queued on every block
    auto& isman = *m_node.isman;
    CMutableTransaction mtx;
    mtx.nVersion = 3;
    mtx.nType = TRANSACTION_ASSET_UNLOCK;
    mtx.vout.emplace_back(COIN, CScript{});
    SetTxPayload(mtx, CAssetUnlockPayload{2, /*index=*/9, /*fee=*/2000, /*requestedHeight=*/500, uint256::ONE,
                                          CBLSSignature{}});
    const auto unlock = MakeTransactionRef(mtx);

    isman.AddNonLockedTx(unlock, nullptr);
    isman.RetryUnminedAssetUnlocks();
    BOOST_REQUIRE_EQUAL(isman.PrepareTxToRetry().size(), 1U);
    isman.TransactionIsRemoved(unlock);
    BOOST_CHECK_EQUAL(isman.GetCounts().m_unprotected_tx, 0U);
    isman.RetryUnminedAssetUnlocks();
    BOOST_CHECK(isman.PrepareTxToRetry().empty());
}

BOOST_FIXTURE_TEST_CASE(nonlocked_ordinary_tx_released_on_mempool_removal, TestChain100Setup)
{
    auto& isman = *m_node.isman;
    auto net = std::make_shared<NetInstantSend>(m_node.peerman.get(), isman, nullptr, *m_node.llmq_ctx->sigman,
                                                *m_node.llmq_ctx->qman, *m_node.chainlocks, *m_node.chainman,
                                                *m_node.mempool, *m_node.mn_sync);
    RegisterSharedValidationInterface(net);
    const auto unregister = interfaces::MakeCleanupHandler([&] { UnregisterSharedValidationInterface(net); });
    for (const bool expire : {true, false}) {
        auto mtx = CreateValidMempoolTransaction(m_coinbase_txns.front(), 0, 1, coinbaseKey,
                                                 GetScriptForRawPubKey(coinbaseKey.GetPubKey()),
                                                 m_coinbase_txns.front()->vout[0].nValue - (expire ? 1000 : 2000));
        const auto tx = WITH_LOCK(m_node.mempool->cs, return m_node.mempool->get(mtx.GetHash()));
        BOOST_REQUIRE(tx);
        isman.AddNonLockedTx(tx, nullptr);
        if (expire) {
            WITH_LOCK(m_node.mempool->cs,
                      m_node.mempool->Expire(GetTime<std::chrono::seconds>() + std::chrono::seconds{1}));
        } else {
            WITH_LOCK(m_node.mempool->cs, m_node.mempool->TrimToSize(0));
        }
        SyncWithValidationInterfaceQueue();
        BOOST_CHECK_EQUAL(tx.use_count(), 1);
    }
    unregister->disconnect();

    // Block removals do not emit TransactionRemovedFromMempool. Mined tracking must still
    // retain the transaction and report a conflict until confirmation cleanup removes it.
    CMutableTransaction mined;
    mined.vin.emplace_back(COutPoint{GetRandHash(), 0});
    mined.vout.emplace_back(COIN, CScript{});
    const auto tx = MakeTransactionRef(mined);
    const auto* tip = WITH_LOCK(cs_main, return m_node.chainman->ActiveTip());
    isman.AddNonLockedTx(tx, tip);
    // Peer rejection also reaches this entry point; it must preserve still-mined conflicts.
    isman.TransactionIsRemoved(tx);
    BOOST_CHECK_EQUAL(tx.use_count(), 2);
    // An unmined spend of the same input, e.g. admitted after a reorg, must not drop the mined
    // transaction from the outpoint index when it leaves the mempool.
    CMutableTransaction spend;
    spend.vin = mined.vin;
    spend.vout.emplace_back(COIN / 2, CScript{});
    const auto spend_tx = MakeTransactionRef(spend);
    isman.AddNonLockedTx(spend_tx, nullptr);
    isman.TransactionIsRemoved(spend_tx);
    instantsend::InstantSendLock conflict;
    conflict.txid = GetRandHash();
    conflict.inputs.push_back(mined.vin.front().prevout);
    {
        const auto conflicts = isman.RetrieveISConflicts(GetRandHash(), conflict);
        BOOST_REQUIRE_EQUAL(conflicts.size(), 1U);
        BOOST_CHECK(conflicts.at(tip).contains(tx->GetHash()));
    }
    isman.RemoveNonLockedTx(tx->GetHash(), false);
    BOOST_CHECK_EQUAL(tx.use_count(), 1);
}

BOOST_AUTO_TEST_SUITE_END()
