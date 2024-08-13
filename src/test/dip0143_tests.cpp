// Copyright (c) 2024 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <key.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <script/script_error.h>
#include <test/util/setup_common.h>
#include <uint256.h>


#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(dip0143_tests, BasicTestingSetup)

static bool SignP2PKH(const CKey& privKey, CMutableTransaction& txTo, const CScript& redeemScript, int nIn, CAmount amount, int sigHashType, SigVersion sigVersion)
{
    CPubKey pubkey = privKey.GetPubKey();
    std::vector<unsigned char> vchSig;
    uint256 hash = SignatureHash(redeemScript, txTo, nIn, sigHashType, amount, sigVersion);
    if (!privKey.Sign(hash, vchSig)) return false;
    vchSig.push_back((unsigned char)(sigHashType));
    txTo.vin[nIn].scriptSig << vchSig;
    txTo.vin[nIn].scriptSig << ToByteVector(pubkey);
    return true;
}

static bool SignMultiSig(const std::vector<CKey>& keys, CMutableTransaction& txTo, const CScript& redeemScript, int nIn, CAmount amount, const std::vector<int>& sigHashTypes, const std::vector<SigVersion>& sigVersions)
{
    if (sigHashTypes.size() != keys.size() || keys.size() != sigVersions.size()) return false;

    txTo.vin[nIn].scriptSig << OP_0;

    for (size_t i = 0; i < keys.size(); i++) {
        uint256 hash = SignatureHash(redeemScript, txTo, nIn, sigHashTypes[i], amount, sigVersions[i]);
        std::vector<unsigned char> vchSig;
        if (!keys[i].Sign(hash, vchSig)) return false;
        vchSig.push_back((unsigned char)sigHashTypes[i]);
        txTo.vin[nIn].scriptSig << vchSig;
    }
    return true;
}

BOOST_AUTO_TEST_CASE(dip0143_verify_script_p2pkh)
{
    unsigned int flags = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_STRICTENC | SCRIPT_ENABLE_DIP0143;

    // Create a private/public key pair
    CKey privKey;
    privKey.MakeNewKey(true);

    // Create a normal P2PKH transaction
    CScript redeemScript;
    redeemScript << OP_DUP << OP_HASH160 << ToByteVector(privKey.GetPubKey().GetID()) << OP_EQUALVERIFY << OP_CHECKSIG;

    CMutableTransaction txFrom;
    CAmount amount = 55;
    txFrom.vout.resize(1);
    txFrom.vout[0].scriptPubKey = redeemScript;
    txFrom.vout[0].nValue = amount;

    // Create txTo that spends txFrom
    CMutableTransaction txTo;
    txTo.vin.resize(1);
    txTo.vout.resize(1);
    txTo.vin[0].prevout.n = 0;
    txTo.vin[0].prevout.hash = txFrom.GetHash();
    txTo.vout[0].nValue = 1;

    // Sign txTo using DIP0143
    BOOST_CHECK(SignP2PKH(privKey, txTo, redeemScript, 0, amount, SIGHASH_ALL | SIGHASH_DIP0143, SigVersion::DIP0143));

    ScriptError err;
    // Script verification must pass only if the flag SCRIPT_ENABLE_DIP0143 is set
    BOOST_CHECK(VerifyScript(txTo.vin[0].scriptSig, redeemScript, flags, MutableTransactionSignatureChecker(&txTo, 0, amount), &err));
    BOOST_CHECK(!VerifyScript(txTo.vin[0].scriptSig, redeemScript, flags & (~SCRIPT_ENABLE_DIP0143), MutableTransactionSignatureChecker(&txTo, 0, amount), &err));
    BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_SIGHASHTYPE_DIP0143, ScriptErrorString(err));

    // Sign again the txTo but this time forget about the sigHashType SIGHASH_DIP0143
    BOOST_CHECK(SignP2PKH(privKey, txTo, redeemScript, 0, amount, SIGHASH_ALL, SigVersion::DIP0143));
    // This time CHECKSIGNATURE will fail regardless of the flag SCRIPT_ENABLE_DIP0143
    BOOST_CHECK(!VerifyScript(txTo.vin[0].scriptSig, redeemScript, flags, MutableTransactionSignatureChecker(&txTo, 0, amount), &err));
    BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_EVAL_FALSE, ScriptErrorString(err));
    BOOST_CHECK(!VerifyScript(txTo.vin[0].scriptSig, redeemScript, flags & (~SCRIPT_ENABLE_DIP0143), MutableTransactionSignatureChecker(&txTo, 0, amount), &err));
    BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_EVAL_FALSE, ScriptErrorString(err));

    // Sign again with SIGHASH_DIP0143 but with SigVersion::BASE
    BOOST_CHECK(SignP2PKH(privKey, txTo, redeemScript, 0, amount, SIGHASH_ALL | SIGHASH_DIP0143, SigVersion::BASE));
    // Script verification must always fail.
    BOOST_CHECK(!VerifyScript(txTo.vin[0].scriptSig, redeemScript, flags, MutableTransactionSignatureChecker(&txTo, 0, amount), &err));
    BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_EVAL_FALSE, ScriptErrorString(err));
    BOOST_CHECK(!VerifyScript(txTo.vin[0].scriptSig, redeemScript, flags & (~SCRIPT_ENABLE_DIP0143), MutableTransactionSignatureChecker(&txTo, 0, amount), &err));
    BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_SIGHASHTYPE_DIP0143, ScriptErrorString(err));

    // Finally verify that SIGHASH_DIP0143 cannot be used alone
    BOOST_CHECK(SignP2PKH(privKey, txTo, redeemScript, 0, amount, SIGHASH_DIP0143, SigVersion::DIP0143));
    BOOST_CHECK(!VerifyScript(txTo.vin[0].scriptSig, redeemScript, flags, MutableTransactionSignatureChecker(&txTo, 0, amount), &err));
    BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_SIG_HASHTYPE, ScriptErrorString(err));
}

BOOST_AUTO_TEST_CASE(dip0143_verify_script_multisig)
{
    unsigned int flags = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_STRICTENC | SCRIPT_ENABLE_DIP0143;

    // Create two private/public key pairs
    std::vector<CKey> privKeys(2, CKey());
    for (size_t i = 0; i < 2; i++) {
        privKeys[i].MakeNewKey(true);
    }

    // Create a normal 2-of-2 multi sig transaction
    CScript redeemScript;
    redeemScript << OP_2 << ToByteVector(privKeys[0].GetPubKey()) << ToByteVector(privKeys[1].GetPubKey()) << OP_2 << OP_CHECKMULTISIG;

    CAmount amount = 55;
    CMutableTransaction txFrom;
    txFrom.vout.resize(1);
    txFrom.vout[0].scriptPubKey = redeemScript;
    txFrom.vout[0].nValue = amount;

    // Create txTo that spends txFrom
    CMutableTransaction txTo;
    txTo.vin.resize(1);
    txTo.vout.resize(1);
    txTo.vin[0].prevout.n = 0;
    txTo.vin[0].prevout.hash = txFrom.GetHash();
    txTo.vout[0].nValue = 1;

    // Sign both needed signatures using DIP0143
    BOOST_CHECK(SignMultiSig(privKeys, txTo, redeemScript, 0, amount, {SIGHASH_ALL | SIGHASH_DIP0143, SIGHASH_ALL | SIGHASH_DIP0143}, {SigVersion::DIP0143, SigVersion::DIP0143}));
    ScriptError err;
    // Script verification must pass only if the flag SCRIPT_ENABLE_DIP0143 is set
    BOOST_CHECK(VerifyScript(txTo.vin[0].scriptSig, redeemScript, flags, MutableTransactionSignatureChecker(&txTo, 0, amount), &err));
    BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_OK, ScriptErrorString(err));
    BOOST_CHECK(!VerifyScript(txTo.vin[0].scriptSig, redeemScript, flags & ~SCRIPT_ENABLE_DIP0143, MutableTransactionSignatureChecker(&txTo, 0, amount), &err));
    BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_SIGHASHTYPE_DIP0143, ScriptErrorString(err));

    // Now consider the case in which one signature is done with DIP0143 and the other with BASE version
    BOOST_CHECK(SignMultiSig(privKeys, txTo, redeemScript, 0, amount, {SIGHASH_ALL | SIGHASH_DIP0143, SIGHASH_ALL}, {SigVersion::DIP0143, SigVersion::BASE}));
    // Script verification must pass only if the flag SCRIPT_ENABLE_DIP0143 is set
    BOOST_CHECK(VerifyScript(txTo.vin[0].scriptSig, redeemScript, flags, MutableTransactionSignatureChecker(&txTo, 0, amount), &err));
    BOOST_CHECK(!VerifyScript(txTo.vin[0].scriptSig, redeemScript, flags & ~SCRIPT_ENABLE_DIP0143, MutableTransactionSignatureChecker(&txTo, 0, amount), &err));
    BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_SIGHASHTYPE_DIP0143, ScriptErrorString(err));

    // Just to be sure consider also the case in which one signature forget about the sigHashType SIGHASH_DIP0143
    BOOST_CHECK(SignMultiSig(privKeys, txTo, redeemScript, 0, amount, {SIGHASH_ALL | SIGHASH_DIP0143, SIGHASH_ALL}, {SigVersion::DIP0143, SigVersion::DIP0143}));
    // Script verification must always fail
    BOOST_CHECK(!VerifyScript(txTo.vin[0].scriptSig, redeemScript, flags, MutableTransactionSignatureChecker(&txTo, 0, amount), &err));
    BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_EVAL_FALSE, ScriptErrorString(err));
    BOOST_CHECK(!VerifyScript(txTo.vin[0].scriptSig, redeemScript, flags & (~SCRIPT_ENABLE_DIP0143), MutableTransactionSignatureChecker(&txTo, 0, amount), &err));
    BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_SIGHASHTYPE_DIP0143, ScriptErrorString(err));
}


BOOST_AUTO_TEST_SUITE_END()
