// Copyright (c) 2023-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <consensus/amount.h>
#include <consensus/tx_check.h>
#include <consensus/validation.h>
#include <evo/assetlocktx.h>
#include <evo/creditpool.h>
#include <evo/specialtx.h>
#include <llmq/context.h>
#include <policy/policy.h>
#include <script/script.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <util/ranges_set.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

// Helpers:
static bool IsStandardTx(const CTransaction& tx, std::string& reason)
{
    return IsStandardTx(tx, MAX_OP_RETURN_RELAY, DEFAULT_PERMIT_BAREMULTISIG, CFeeRate{DUST_RELAY_TX_FEE}, reason);
}

// Create two dummy transactions, each with two outputs.  The first has 11 and 50 CENT outputs
// paid to a TX_PUBKEY, the second 21 and 22 CENT outputs paid to a TX_PUBKEYHASH.
static std::vector<CMutableTransaction>
SetupDummyInputs(FillableSigningProvider& keystoreRet, CCoinsViewCache& coinsRet)
{
    std::vector<CMutableTransaction> dummyTransactions;
    dummyTransactions.resize(2);

    // Add some keys to the keystore:
    std::array<CKey, 4> key;
    {
        bool flip = true;
        for (auto& k : key) {
            k.MakeNewKey(flip);
            keystoreRet.AddKey(k);
            flip = !flip;
        }
    }

    // Create some dummy input transactions
    dummyTransactions[0].vout.resize(2);
    dummyTransactions[0].vout[0].nValue = 11*CENT;
    dummyTransactions[0].vout[0].scriptPubKey = GetScriptForRawPubKey(key[0].GetPubKey());
    dummyTransactions[0].vout[1].nValue = 50*CENT;
    dummyTransactions[0].vout[1].scriptPubKey = GetScriptForRawPubKey(key[1].GetPubKey());
    AddCoins(coinsRet, CTransaction(dummyTransactions[0]), 0);

    dummyTransactions[1].vout.resize(2);
    dummyTransactions[1].vout[0].nValue = 21*CENT;
    dummyTransactions[1].vout[0].scriptPubKey = GetScriptForDestination(PKHash(key[2].GetPubKey()));
    dummyTransactions[1].vout[1].nValue = 22*CENT;
    dummyTransactions[1].vout[1].scriptPubKey = GetScriptForDestination(PKHash(key[3].GetPubKey()));
    AddCoins(coinsRet, CTransaction(dummyTransactions[1]), 0);

    return dummyTransactions;
}

static CMutableTransaction CreateAssetLockTx(FillableSigningProvider& keystore, CCoinsViewCache& coins, CKey& key)
{
    std::vector<CMutableTransaction> dummyTransactions = SetupDummyInputs(keystore, coins);

    std::vector<CTxOut> creditOutputs(2);
    creditOutputs[0].nValue = 17 * CENT;
    creditOutputs[0].scriptPubKey = GetScriptForDestination(PKHash(key.GetPubKey()));
    creditOutputs[1].nValue = 13 * CENT;
    creditOutputs[1].scriptPubKey = GetScriptForDestination(PKHash(key.GetPubKey()));

    CAssetLockPayload assetLockTx(creditOutputs, CAssetLockPayload::INITIAL_VERSION);

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_ASSET_LOCK;
    SetTxPayload(tx, assetLockTx);

    tx.vin.resize(1);
    tx.vin[0].prevout.hash = dummyTransactions[0].GetHash();
    tx.vin[0].prevout.n = 1;
    tx.vin[0].scriptSig << std::vector<unsigned char>(65, 0);

    tx.vout.resize(2);
    tx.vout[0].nValue = 30 * CENT;
    tx.vout[0].scriptPubKey = CScript() << OP_RETURN << ParseHex("");

    tx.vout[1].nValue = 20 * CENT;
    tx.vout[1].scriptPubKey = GetScriptForDestination(PKHash(key.GetPubKey()));

    return tx;
}

static CMutableTransaction CreateAssetUnlockTx(FillableSigningProvider& keystore, CKey& key)
{
    int nVersion = 1;
    // just a big number bigger than uint32_t
    uint64_t index = 0x001122334455667788L;
    // big enough to overflow int32_t
    uint32_t fee = 2000'000'000u;
    // just big enough to overflow uint16_t
    uint32_t requestedHeight = 1000'000;
    uint256 quorumHash;
    CBLSSignature quorumSig;
    CAssetUnlockPayload assetUnlockTx(nVersion, index, fee, requestedHeight, quorumHash, quorumSig);

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_ASSET_UNLOCK;
    SetTxPayload(tx, assetUnlockTx);

    tx.vin.resize(0);

    tx.vout.resize(2);
    tx.vout[0].nValue = 10 * CENT;
    tx.vout[0].scriptPubKey = GetScriptForDestination(PKHash(key.GetPubKey()));

    tx.vout[1].nValue = 20 * CENT;
    tx.vout[1].scriptPubKey = GetScriptForDestination(PKHash(key.GetPubKey()));

    return tx;
}

static CTransactionRef CreateCreditPoolUnlockTx(uint64_t index, CAmount amount)
{
    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_ASSET_UNLOCK;
    tx.vout.emplace_back(amount, CScript{});
    SetTxPayload(tx, CAssetUnlockPayload{1, index, 0, 0, {}, {}});
    return MakeTransactionRef(std::move(tx));
}

BOOST_FIXTURE_TEST_SUITE(evo_assetlocks_tests, TestChain100Setup)

static void CheckAssetLockCommon(uint8_t version, bool is_v24_active)
{
    LOCK(cs_main);
    FillableSigningProvider keystore;
    CCoinsView coinsDummy;
    CCoinsViewCache coins(&coinsDummy);

    CKey key;
    key.MakeNewKey(true);

    CMutableTransaction baseTx = CreateAssetLockTx(keystore, coins, key);
    const auto basePayload = GetTxPayload<CAssetLockPayload>(CTransaction(baseTx));
    const std::vector<CTxOut> creditOutputs = basePayload->getCreditOutputs();

    SetTxPayload(baseTx, CAssetLockPayload(creditOutputs, version));

    const CTransaction tx{baseTx};
    std::string reason;
    BOOST_CHECK(IsStandardTx(CTransaction(tx), reason));

    TxValidationState tx_state;
    std::string strTest;
    BOOST_CHECK_MESSAGE(CheckTransaction(CTransaction(tx), tx_state), strTest);
    BOOST_CHECK(tx_state.IsValid());

    BOOST_CHECK(CheckAssetLockTx(CTransaction(tx), tx_state, is_v24_active));

    BOOST_CHECK(AreInputsStandard(CTransaction(tx), coins));

    // Check version
    {
        BOOST_CHECK(tx.IsSpecialTxVersion());

        const auto opt_payload = GetTxPayload<CAssetLockPayload>(tx);

        BOOST_CHECK(opt_payload.has_value());
        BOOST_CHECK(opt_payload->getVersion() == version);
    }

    {
        // Wrong type "Asset Unlock TX" instead "Asset Lock TX"
        CMutableTransaction txWrongType(tx);
        txWrongType.nType = TRANSACTION_ASSET_UNLOCK;
        BOOST_CHECK(!CheckAssetLockTx(CTransaction(txWrongType), tx_state, is_v24_active));
        BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetlocktx-type");
    }

    {
        CAmount inSum = 0;
        for (const auto& vin : tx.vin) {
            inSum += coins.AccessCoin(vin.prevout).out.nValue;
        }

        auto outSum = CTransaction(tx).GetValueOut();
        BOOST_CHECK(inSum == outSum);

        // Outputs should not be bigger than inputs
        CMutableTransaction txBigOutput(baseTx);
        txBigOutput.vout[0].nValue += 1;
        BOOST_CHECK(!CheckAssetLockTx(CTransaction(txBigOutput), tx_state, is_v24_active));
        BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetlocktx-creditamount");

        // Smaller outputs are allown
        CMutableTransaction txSmallOutput(baseTx);
        txSmallOutput.vout[1].nValue -= 1;
        BOOST_CHECK(CheckAssetLockTx(CTransaction(txSmallOutput), tx_state, is_v24_active));
    }

    {
        // Sum of credit output greater than OP_RETURN
        std::vector<CTxOut> wrongOutput = creditOutputs;
        wrongOutput[0].nValue += CENT;

        CMutableTransaction txGreaterCredits(baseTx);
        SetTxPayload(txGreaterCredits, CAssetLockPayload(wrongOutput, version));

        BOOST_CHECK(!CheckAssetLockTx(CTransaction(txGreaterCredits), tx_state, is_v24_active));
        BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetlocktx-creditamount");

        // Sum of credit output less than OP_RETURN
        wrongOutput[1].nValue -= 2 * CENT;

        CMutableTransaction txLessCredits(baseTx);
        SetTxPayload(txLessCredits, CAssetLockPayload(wrongOutput, version));

        BOOST_CHECK(!CheckAssetLockTx(CTransaction(txLessCredits), tx_state, is_v24_active));
        BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetlocktx-creditamount");
    }

    {
        // Credit output is out-of-range
        std::vector<CTxOut> creditOutputsOutOfRange = creditOutputs;
        creditOutputsOutOfRange[0].nValue = 0;

        CMutableTransaction txInvalidOutputs(baseTx);
        SetTxPayload(txInvalidOutputs, CAssetLockPayload(creditOutputsOutOfRange, version));

        BOOST_CHECK(!CheckAssetLockTx(CTransaction(txInvalidOutputs), tx_state, is_v24_active));
        BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetlocktx-credit-outofrange");

        // one of output is out of range
        creditOutputsOutOfRange[0].nValue = MAX_MONEY + 1;
        SetTxPayload(txInvalidOutputs, CAssetLockPayload(creditOutputsOutOfRange, version));
        BOOST_CHECK(!CheckAssetLockTx(CTransaction(txInvalidOutputs), tx_state, is_v24_active));
        BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetlocktx-credit-outofrange");

        // sum of some of output is out of range
        creditOutputsOutOfRange[0].nValue = MAX_MONEY + 1 - creditOutputsOutOfRange[1].nValue;
        SetTxPayload(txInvalidOutputs, CAssetLockPayload(creditOutputsOutOfRange, version));
        BOOST_CHECK(!CheckAssetLockTx(CTransaction(txInvalidOutputs), tx_state, is_v24_active));
        BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetlocktx-credit-outofrange");

    }
    {
        // One credit output keys is not pub key
        std::vector<CTxOut> creditOutputsNotPubkey = creditOutputs;
        creditOutputsNotPubkey[0].scriptPubKey = CScript() << OP_1;

        CMutableTransaction txNotPubkey(baseTx);
        SetTxPayload(txNotPubkey, CAssetLockPayload(creditOutputsNotPubkey, version));

        BOOST_CHECK(!CheckAssetLockTx(CTransaction(txNotPubkey), tx_state, is_v24_active));
        if (version >= 2) {
            BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetlocktx-script-pubkey");
        } else {
            BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetlocktx-pubKeyHash");
        }

    }

    {
        // OP_RETURN must be only one, not more
        CMutableTransaction txMultipleReturn(baseTx);
        txMultipleReturn.vout[1].scriptPubKey = CScript() << OP_RETURN << ParseHex("");

        BOOST_CHECK(!CheckAssetLockTx(CTransaction(txMultipleReturn), tx_state, is_v24_active));
        BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetlocktx-multiple-return");

    }

    {
        // zero/negative OP_RETURN
        CMutableTransaction txReturnOutOfRange(baseTx);
        txReturnOutOfRange.vout[0].nValue = 0;

        BOOST_CHECK(!CheckAssetLockTx(CTransaction(txReturnOutOfRange), tx_state, is_v24_active));
        BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetlocktx-opreturn-outofrange");

        txReturnOutOfRange.vout[0].nValue = MAX_MONEY + 1;

        BOOST_CHECK(!CheckAssetLockTx(CTransaction(txReturnOutOfRange), tx_state, is_v24_active));
        BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetlocktx-opreturn-outofrange");
    }


    {
        // OP_RETURN is missing
        CMutableTransaction txNoReturn(baseTx);
        txNoReturn.vout[0].scriptPubKey = GetScriptForDestination(PKHash(key.GetPubKey()));

        BOOST_CHECK(!CheckAssetLockTx(CTransaction(txNoReturn), tx_state, is_v24_active));
        BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetlocktx-no-return");
    }

    {
        // OP_RETURN should not have any data
        CMutableTransaction txReturnData(baseTx);
        txReturnData.vout[0].scriptPubKey = CScript() << OP_RETURN << ParseHex("abcd");

        BOOST_CHECK(!CheckAssetLockTx(CTransaction(txReturnData), tx_state, is_v24_active));
        BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetlocktx-non-empty-return");
    }
}

BOOST_FIXTURE_TEST_CASE(evo_assetlock, TestChain100Setup)
{
    CheckAssetLockCommon(CAssetLockPayload::INITIAL_VERSION, /*is_v24_active=*/false);
    CheckAssetLockCommon(CAssetLockPayload::INITIAL_VERSION, /*is_v24_active=*/true);
    CheckAssetLockCommon(CAssetLockPayload::CURRENT_VERSION, /*is_v24_active=*/true);
}

BOOST_FIXTURE_TEST_CASE(evo_assetlock_v2, TestChain100Setup)
{
    LOCK(cs_main);
    FillableSigningProvider keystore;
    CCoinsView coinsDummy;
    CCoinsViewCache coins(&coinsDummy);

    CKey key;
    key.MakeNewKey(true);

    CMutableTransaction tx = CreateAssetLockTx(keystore, coins, key);
    TxValidationState tx_state;

    const auto v1Payload = GetTxPayload<CAssetLockPayload>(CTransaction(tx));
    const std::vector<CTxOut> creditOutputs = v1Payload->getCreditOutputs();

    // Build v2 payload with same P2PKH credit outputs
    CAssetLockPayload v2Payload(creditOutputs);
    BOOST_CHECK(v2Payload.getVersion() == CAssetLockPayload::CURRENT_VERSION);
    SetTxPayload(tx, v2Payload);

    {
        // v2 P2PKH: accepted with is_v24_active=true
        BOOST_CHECK(CheckAssetLockTx(CTransaction(tx), tx_state, /*is_v24_active=*/true));
    }

    {
        // v2 P2PKH: rejected pre-v24
        BOOST_CHECK(!CheckAssetLockTx(CTransaction(tx), tx_state, /*is_v24_active=*/false));
        BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetlocktx-version-2");
    }

    {
        // v2 with P2SH credit output: accepted with is_v24_active=true
        CScript p2sh_script = GetScriptForDestination(ScriptHash(CScript() << OP_1));
        std::vector<CTxOut> p2shOutputs(1);
        p2shOutputs[0].nValue = 30 * CENT;
        p2shOutputs[0].scriptPubKey = p2sh_script;

        CMutableTransaction txP2SH(tx);
        SetTxPayload(txP2SH, CAssetLockPayload(p2shOutputs));

        BOOST_CHECK(CheckAssetLockTx(CTransaction(txP2SH), tx_state, /*is_v24_active=*/true));
    }

    {
        // v1 with P2SH credit output: rejected even post-v24
        CScript p2sh_script = GetScriptForDestination(ScriptHash(CScript() << OP_1));
        std::vector<CTxOut> p2shOutputs(1);
        p2shOutputs[0].nValue = 30 * CENT;
        p2shOutputs[0].scriptPubKey = p2sh_script;

        CMutableTransaction txV1P2SH(tx);
        SetTxPayload(txV1P2SH, CAssetLockPayload(p2shOutputs, CAssetLockPayload::INITIAL_VERSION));

        BOOST_CHECK(!CheckAssetLockTx(CTransaction(txV1P2SH), tx_state, /*is_v24_active=*/true));
        BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetlocktx-pubKeyHash");
    }

    {
        // v2 with non-P2PKH/non-P2SH credit output: rejected
        std::vector<CTxOut> badOutputs(1);
        badOutputs[0].nValue = 30 * CENT;
        badOutputs[0].scriptPubKey = CScript() << OP_1;

        CMutableTransaction txBad(tx);
        SetTxPayload(txBad, CAssetLockPayload(badOutputs));

        BOOST_CHECK(!CheckAssetLockTx(CTransaction(txBad), tx_state, /*is_v24_active=*/true));
        BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetlocktx-script-pubkey");
    }

    {
        // v1 still works post-v24
        CMutableTransaction txV1(tx);
        SetTxPayload(txV1, CAssetLockPayload(creditOutputs, CAssetLockPayload::INITIAL_VERSION));

        BOOST_CHECK(CheckAssetLockTx(CTransaction(txV1), tx_state, /*is_v24_active=*/true));
    }

    {
        // version 3 (future): rejected even post-v24
        CMutableTransaction txV3(tx);
        SetTxPayload(txV3, CAssetLockPayload(creditOutputs, /*nVersion=*/3));

        BOOST_CHECK(!CheckAssetLockTx(CTransaction(txV3), tx_state, /*is_v24_active=*/true));
        BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetlocktx-version");
    }
}

BOOST_FIXTURE_TEST_CASE(evo_assetunlock, TestChain100Setup)
{
    LOCK(cs_main);
    FillableSigningProvider keystore;

    CKey key;
    key.MakeNewKey(true);

    const CTransaction tx{CreateAssetUnlockTx(keystore, key)};
    std::string reason;
    BOOST_CHECK(IsStandardTx(CTransaction(tx), reason));

    TxValidationState tx_state;
    std::string strTest;
    BOOST_CHECK_MESSAGE(CheckTransaction(CTransaction(tx), tx_state), strTest);
    BOOST_CHECK(tx_state.IsValid());

    auto& blockman = Assert(m_node.chainman)->m_blockman;
    auto& qman = *Assert(m_node.llmq_ctx)->qman;

    const CBlockIndex *block_index = m_node.chainman->ActiveChain().Tip();
    BOOST_CHECK(!CheckAssetUnlockTx(blockman, qman, CTransaction(tx), block_index, std::nullopt, /*is_v24_active=*/true, tx_state));
    BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetunlock-quorum-hash");

    {
        // Any input should be a reason to fail CheckAssetUnlockTx()
        CCoinsView coinsDummy;
        CCoinsViewCache coins(&coinsDummy);
        std::vector<CMutableTransaction> dummyTransactions = SetupDummyInputs(keystore, coins);

        CMutableTransaction txNonemptyInput(tx);
        txNonemptyInput.vin.resize(1);
        txNonemptyInput.vin[0].prevout.hash = dummyTransactions[0].GetHash();
        txNonemptyInput.vin[0].prevout.n = 1;
        txNonemptyInput.vin[0].scriptSig << std::vector<unsigned char>(65, 0);

        std::string reason;
        BOOST_CHECK(IsStandardTx(CTransaction(tx), reason));

        BOOST_CHECK(!CheckAssetUnlockTx(blockman, qman, CTransaction(txNonemptyInput), block_index, std::nullopt, /*is_v24_active=*/true, tx_state));
        BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetunlocktx-have-input");
    }

    {
        const auto unlockPayload = GetTxPayload<CAssetUnlockPayload>(tx);
        BOOST_CHECK(unlockPayload.has_value());
        BOOST_CHECK(unlockPayload->getVersion() == 1);
        BOOST_CHECK(unlockPayload->getRequestedHeight() == 1000'000);
        BOOST_CHECK(unlockPayload->getFee() == 2000'000'000u);
        BOOST_CHECK(unlockPayload->getIndex() == 0x001122334455667788L);

        // Wrong type "Asset Lock TX" instead "Asset Unlock TX"
        CMutableTransaction txWrongType(tx);
        txWrongType.nType = TRANSACTION_ASSET_LOCK;
        BOOST_CHECK(!CheckAssetUnlockTx(blockman, qman, CTransaction(txWrongType), block_index, std::nullopt, /*is_v24_active=*/true, tx_state));
        BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetunlocktx-type");

        // Check version of tx and payload
        BOOST_CHECK(tx.IsSpecialTxVersion());
        for (uint8_t payload_version : {0, 1, 2, 255}) {
            CAssetUnlockPayload unlockPayload_tmp{payload_version,
                unlockPayload->getIndex(),
                unlockPayload->getFee(),
                unlockPayload->getRequestedHeight(),
                unlockPayload->getQuorumHash(),
                unlockPayload->getQuorumSig()};
            CMutableTransaction txWrongVersion(tx);
            SetTxPayload(txWrongVersion, unlockPayload_tmp);
            if (payload_version == 0 || payload_version > CAssetUnlockPayload::CURRENT_VERSION) {
                BOOST_CHECK(!CheckAssetUnlockTx(blockman, qman, CTransaction(txWrongVersion), block_index, std::nullopt, /*is_v24_active=*/true, tx_state));
                BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetunlocktx-version");
            } else if (payload_version == 2) {
                BOOST_CHECK(!CheckAssetUnlockTx(blockman, qman, CTransaction(txWrongVersion), block_index, std::nullopt, /*is_v24_active=*/false, tx_state));
                BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetunlocktx-version-2");
                BOOST_CHECK(!CheckAssetUnlockTx(blockman, qman, CTransaction(txWrongVersion), block_index, std::nullopt, /*is_v24_active=*/true, tx_state));
                BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetunlock-quorum-hash");
            } else {
                // Version 1 must not hit the version-2 gate regardless of v24 state
                BOOST_CHECK(!CheckAssetUnlockTx(blockman, qman, CTransaction(txWrongVersion), block_index, std::nullopt, /*is_v24_active=*/false, tx_state));
                BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetunlock-quorum-hash");
                BOOST_CHECK(!CheckAssetUnlockTx(blockman, qman, CTransaction(txWrongVersion), block_index, std::nullopt, /*is_v24_active=*/true, tx_state));
                BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetunlock-quorum-hash");
            }
        }
    }

    {
        // The txid of a version 2 payload is invariant across the quorum signing info - the fields
        // Platform changes on re-sign - and nothing else; the instance hash still covers them
        const auto unlockPayload = GetTxPayload<CAssetUnlockPayload>(tx);
        BOOST_CHECK(unlockPayload.has_value());

        auto make_unlock_tx = [&](uint8_t version, uint64_t index, uint32_t fee, uint32_t requested_height,
                                  const uint256& quorum_hash) {
            CMutableTransaction tx_tmp(tx);
            SetTxPayload(tx_tmp, CAssetUnlockPayload{version, index, fee, requested_height, quorum_hash,
                                                     unlockPayload->getQuorumSig()});
            return CTransaction(tx_tmp);
        };

        // Version 1 hashes the full serialization
        const CTransaction tx_v1{make_unlock_tx(1, 0x11, 2000, 500, uint256::ONE)};
        BOOST_CHECK(!IsAssetUnlockWithStableTxid(tx_v1));
        BOOST_CHECK(tx_v1.GetHash() == tx_v1.GetInstanceHash());
        BOOST_CHECK(tx_v1.GetHash() == ::SerializeHash(tx_v1));

        const CTransaction tx_v2{make_unlock_tx(2, 0x11, 2000, 500, uint256::ONE)};
        BOOST_CHECK(IsAssetUnlockWithStableTxid(tx_v2));
        BOOST_CHECK(tx_v2.vExtraPayload.size() == ASSET_UNLOCK_PAYLOAD_SIZE);
        BOOST_CHECK(tx_v2.GetHash() != tx_v2.GetInstanceHash());
        BOOST_CHECK(tx_v2.GetInstanceHash() == ::SerializeHash(tx_v2));

        // Re-signing changes requestedHeight and quorumHash but not the txid; the instance
        // hash tells the two instances apart
        const CTransaction tx_v2_resigned{make_unlock_tx(2, 0x11, 2000, 700, uint256::TWO)};
        BOOST_CHECK(tx_v2_resigned.GetHash() == tx_v2.GetHash());
        BOOST_CHECK(tx_v2_resigned.GetInstanceHash() != tx_v2.GetInstanceHash());
        // A different withdrawal (index or fee) has a different txid
        BOOST_CHECK(make_unlock_tx(2, 0x12, 2000, 500, uint256::ONE).GetHash() != tx_v2.GetHash());
        BOOST_CHECK(make_unlock_tx(2, 0x11, 3000, 500, uint256::ONE).GetHash() != tx_v2.GetHash());

        // CMutableTransaction applies the same rule
        BOOST_CHECK(CMutableTransaction(tx_v2).GetHash() == tx_v2.GetHash());

        // The txid equals the full-serialization hash with the quorum signing info zeroed
        CMutableTransaction tx_zeroed(tx_v2);
        SetTxPayload(tx_zeroed, CAssetUnlockPayload{2, 0x11, 2000, /*requestedHeight=*/0,
                                                    /*quorumHash=*/uint256(), CBLSSignature{}});
        BOOST_CHECK(::SerializeHash(tx_zeroed) == tx_v2.GetHash());

        // The signed message zeroes only quorumSig: it commits to the signing info even though
        // the txid does not, and must never be computed with GetHash()
        auto msg_hash = [](const CTransaction& t) {
            const auto p = GetTxPayload<CAssetUnlockPayload>(t);
            CMutableTransaction copy(t);
            SetTxPayload(copy, CAssetUnlockPayload{p->getVersion(), p->getIndex(), p->getFee(),
                                                   p->getRequestedHeight(), p->getQuorumHash(), CBLSSignature{}});
            return ::SerializeHash(copy);
        };
        BOOST_CHECK(msg_hash(tx_v2) != msg_hash(tx_v2_resigned));
        BOOST_CHECK(msg_hash(tx_v2) != tx_v2.GetHash());
    }

    {
        // DIP-0027 worked examples for the version 2 txid (dip-0027/dip-0027-txid-calc.py)
        auto dip_example_tx = [](uint64_t index) {
            CMutableTransaction tx_dip;
            tx_dip.nVersion = 3;
            tx_dip.nType = TRANSACTION_ASSET_UNLOCK;
            uint160 pubkey_hash;
            std::fill(pubkey_hash.begin(), pubkey_hash.end(), 0x11);
            const CScript script{GetScriptForDestination(PKHash{pubkey_hash})};
            tx_dip.vout.emplace_back(100000000, script);
            SetTxPayload(tx_dip, CAssetUnlockPayload{2, index, /*fee=*/70000, /*requestedHeight=*/500,
                                                     uint256::ONE, CBLSSignature{}});
            return CTransaction(tx_dip);
        };
        BOOST_CHECK_EQUAL(dip_example_tx(101).GetHash().ToString(),
                          "3c4db73c8356407a5d7c78df5045bd280f2dc4fd644b06c4bfbdead3d5ae41cf");
        BOOST_CHECK_EQUAL(dip_example_tx(123456789).GetHash().ToString(),
                          "a67e1107ae6e04b813bc8e81348266f5206d1ca93d305dc4323940e18cdbaf34");
    }

    {
        // Exactly 32 withdrawal is fine
        CMutableTransaction txManyOutputs(tx);
        int outputsLimit = 32;
        txManyOutputs.vout.resize(outputsLimit);
        for (auto& out : txManyOutputs.vout) {
            out.nValue = CENT;
            out.scriptPubKey = GetScriptForDestination(PKHash(key.GetPubKey()));
        }

        BOOST_CHECK(!CheckAssetUnlockTx(blockman, qman, CTransaction(txManyOutputs), block_index, std::nullopt, /*is_v24_active=*/true, tx_state));
        BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetunlock-quorum-hash");

        // Basic checks for CRangesSet
        CRangesSet indexes;
        BOOST_CHECK(!CheckAssetUnlockTx(blockman, qman, CTransaction(txManyOutputs), block_index, indexes, /*is_v24_active=*/true, tx_state));
        BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetunlock-quorum-hash");
        BOOST_CHECK(indexes.Add(0x001122334455667788L));
        BOOST_CHECK(!CheckAssetUnlockTx(blockman, qman, CTransaction(txManyOutputs), block_index, indexes, /*is_v24_active=*/true, tx_state));
        BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetunlock-duplicated-index");


        // Should not be more than 32 withdrawal in one transaction
        txManyOutputs.vout.resize(outputsLimit + 1);
        txManyOutputs.vout.back().nValue = CENT;
        txManyOutputs.vout.back().scriptPubKey = GetScriptForDestination(PKHash(key.GetPubKey()));
        BOOST_CHECK(!CheckAssetUnlockTx(blockman, qman, CTransaction(txManyOutputs), block_index, std::nullopt, /*is_v24_active=*/true, tx_state));
        BOOST_CHECK(tx_state.GetRejectReason() == "bad-assetunlocktx-too-many-outs");
    }

}

BOOST_FIXTURE_TEST_CASE(credit_pool_package_atomicity, TestChain100Setup)
{
    LOCK(cs_main);
    const auto unlock_seven = CreateCreditPoolUnlockTx(1, 7 * COIN);
    const auto unlock_four = CreateCreditPoolUnlockTx(2, 4 * COIN);
    const auto* tip = m_node.chainman->ActiveChain().Tip();
    CCreditPoolDiff diff{CCreditPool{100 * COIN, 10 * COIN}, tip, m_node.chainman->GetConsensus(), 0};

    TxValidationState package_state;
    BOOST_CHECK(!diff.ProcessLockUnlockTransactions({unlock_seven, unlock_four}, package_state));
    BOOST_CHECK_EQUAL(package_state.GetRejectReason(), "failed-creditpool-unlock-too-much");
    BOOST_CHECK_EQUAL(diff.GetTotalLocked(), 100 * COIN);

    TxValidationState retry_state;
    BOOST_CHECK(diff.ProcessLockUnlockTransaction(*unlock_seven, retry_state));
    BOOST_CHECK_EQUAL(diff.GetTotalLocked(), 93 * COIN);

    // A later package's rollback must erase only its own indexes: index 1 was
    // committed above and must survive the failed package below.
    const auto unlock_two = CreateCreditPoolUnlockTx(3, 2 * COIN);
    const auto unlock_dup = CreateCreditPoolUnlockTx(1, COIN);
    TxValidationState dup_state;
    BOOST_CHECK(!diff.ProcessLockUnlockTransactions({unlock_two, unlock_dup}, dup_state));
    BOOST_CHECK_EQUAL(dup_state.GetRejectReason(), "failed-creditpool-unlock-duplicated-index");
    BOOST_CHECK_EQUAL(diff.GetTotalLocked(), 93 * COIN);

    // index 1 must still be known as used...
    TxValidationState still_dup_state;
    BOOST_CHECK(!diff.ProcessLockUnlockTransactions({unlock_dup}, still_dup_state));
    BOOST_CHECK_EQUAL(still_dup_state.GetRejectReason(), "failed-creditpool-unlock-duplicated-index");

    // ...while index 3 was rolled back and is usable again
    TxValidationState reuse_state;
    BOOST_CHECK(diff.ProcessLockUnlockTransactions({unlock_two}, reuse_state));
    BOOST_CHECK_EQUAL(diff.GetTotalLocked(), 91 * COIN);
}

BOOST_AUTO_TEST_SUITE_END()
