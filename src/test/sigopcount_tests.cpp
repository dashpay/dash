// Copyright (c) 2012-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <consensus/tx_verify.h>
#include <key.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <script/standard.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <vector>

#include <boost/test/unit_test.hpp>

// Helpers:
static std::vector<unsigned char>
Serialize(const CScript& s)
{
    std::vector<unsigned char> sSerialized(s.begin(), s.end());
    return sSerialized;
}

BOOST_FIXTURE_TEST_SUITE(sigopcount_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(GetSigOpCount)
{
    // Test CScript::GetSigOpCount()
    CScript s1;
    BOOST_CHECK_EQUAL(s1.GetSigOpCount(false), 0U);
    BOOST_CHECK_EQUAL(s1.GetSigOpCount(true), 0U);

    uint160 dummy;
    s1 << OP_1 << ToByteVector(dummy) << ToByteVector(dummy) << OP_2 << OP_CHECKMULTISIG;
    BOOST_CHECK_EQUAL(s1.GetSigOpCount(true), 2U);
    s1 << OP_IF << OP_CHECKSIG << OP_ENDIF;
    BOOST_CHECK_EQUAL(s1.GetSigOpCount(true), 3U);
    BOOST_CHECK_EQUAL(s1.GetSigOpCount(false), 21U);

    CScript p2sh = GetScriptForDestination(ScriptHash(s1));
    CScript scriptSig;
    scriptSig << OP_0 << Serialize(s1);
    BOOST_CHECK_EQUAL(p2sh.GetSigOpCount(scriptSig), 3U);

    std::vector<CPubKey> keys;
    for (int i = 0; i < 3; i++)
    {
        CKey k = GenerateRandomKey();
        keys.push_back(k.GetPubKey());
    }
    CScript s2 = GetScriptForMultisig(1, keys);
    BOOST_CHECK_EQUAL(s2.GetSigOpCount(true), 3U);
    BOOST_CHECK_EQUAL(s2.GetSigOpCount(false), 20U);

    p2sh = GetScriptForDestination(ScriptHash(s2));
    BOOST_CHECK_EQUAL(p2sh.GetSigOpCount(true), 0U);
    BOOST_CHECK_EQUAL(p2sh.GetSigOpCount(false), 0U);
    CScript scriptSig2;
    scriptSig2 << OP_1 << ToByteVector(dummy) << ToByteVector(dummy) << Serialize(s2);
    BOOST_CHECK_EQUAL(p2sh.GetSigOpCount(scriptSig2), 3U);
}

BOOST_AUTO_TEST_CASE(GetSigOpCountDataSigs)
{
    const CScript script{CScript() << OP_CHECKDATASIG << OP_CHECKDATASIGVERIFY << OP_CHECKSIG};
    BOOST_CHECK_EQUAL(script.GetSigOpCount(/*fAccurate=*/false), 1U);
    BOOST_CHECK_EQUAL(script.GetSigOpCount(/*fAccurate=*/true), 1U);
    BOOST_CHECK_EQUAL(script.GetSigOpCount(/*fAccurate=*/false, /*count_data_sigs=*/true), 3U);
    BOOST_CHECK_EQUAL(script.GetSigOpCount(/*fAccurate=*/true, /*count_data_sigs=*/true), 3U);

    const CScript p2sh{GetScriptForDestination(ScriptHash(script))};
    const CScript scriptSig{CScript() << OP_0 << Serialize(script)};
    BOOST_CHECK_EQUAL(p2sh.GetSigOpCount(scriptSig), 1U);
    BOOST_CHECK_EQUAL(p2sh.GetSigOpCount(scriptSig, /*count_data_sigs=*/true), 3U);
}

static CMutableTransaction SpendP2SH(CCoinsViewCache& coins, const CScript& redeem_script)
{
    const COutPoint prevout{InsecureRand256(), 0};
    coins.AddCoin(prevout, Coin{CTxOut{COIN, GetScriptForDestination(ScriptHash(redeem_script))}, 1, false}, false);
    CMutableTransaction tx;
    tx.vin.emplace_back(prevout, CScript() << Serialize(redeem_script));
    tx.vout.emplace_back(COIN, CScript() << OP_CHECKDATASIG);
    return tx;
}

BOOST_AUTO_TEST_CASE(GetTransactionSigOpCountDataSigs)
{
    CCoinsView view_dummy;
    CCoinsViewCache coins{&view_dummy};
    const CScript redeem_script{CScript() << OP_CHECKDATASIGVERIFY << OP_CHECKDATASIG};
    const CTransaction tx{SpendP2SH(coins, redeem_script)};

    BOOST_CHECK_EQUAL(GetLegacySigOpCount(tx), 0U);
    BOOST_CHECK_EQUAL(GetLegacySigOpCount(tx, /*count_data_sigs=*/true), 1U);
    BOOST_CHECK_EQUAL(GetP2SHSigOpCount(tx, coins), 0U);
    BOOST_CHECK_EQUAL(GetP2SHSigOpCount(tx, coins, /*count_data_sigs=*/true), 2U);
    BOOST_CHECK_EQUAL(GetTransactionSigOpCount(tx, coins, SCRIPT_VERIFY_P2SH), 0U);
    BOOST_CHECK_EQUAL(GetTransactionSigOpCount(tx, coins, SCRIPT_VERIFY_P2SH, /*count_data_sigs=*/true), 3U);
}

BOOST_AUTO_TEST_CASE(P2SHDataSigsAreStandardOnlyWithinSigOpLimit)
{
    CCoinsView view_dummy;
    CCoinsViewCache coins{&view_dummy};
    for (const unsigned int data_sigs : {MAX_P2SH_SIGOPS, MAX_P2SH_SIGOPS + 1}) {
        CScript redeem_script;
        for (unsigned int i{0}; i < data_sigs; ++i) {
            redeem_script << OP_CHECKDATASIGVERIFY;
        }
        const CTransaction tx{SpendP2SH(coins, redeem_script)};
        BOOST_CHECK_EQUAL(AreInputsStandard(tx, coins), data_sigs <= MAX_P2SH_SIGOPS);
    }
}

BOOST_AUTO_TEST_SUITE_END()
