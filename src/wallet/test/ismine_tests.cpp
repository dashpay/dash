// Copyright (c) 2017-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <key.h>
#include <key_io.h>
#include <script/script.h>
#include <script/script_error.h>
#include <script/standard.h>
#include <test/util/setup_common.h>
#include <wallet/ismine.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>

using node::NodeContext;

namespace wallet {
BOOST_FIXTURE_TEST_SUITE(ismine_tests, BasicTestingSetup)

wallet::ScriptPubKeyMan* CreateDescriptor(CWallet& keystore, const std::string& desc_str, const bool success)
{
    keystore.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);

    FlatSigningProvider keys;
    std::string error;
    std::unique_ptr<Descriptor> parsed_desc = Parse(desc_str, keys, error, false);
    BOOST_CHECK(success == (parsed_desc != nullptr));
    if (!success) return nullptr;

    const int64_t range_start = 0, range_end = 1, next_index = 0, timestamp = 1;

    WalletDescriptor w_desc(std::move(parsed_desc), timestamp, range_start, range_end, next_index);

    LOCK(keystore.cs_wallet);

    return Assert(keystore.AddWalletDescriptor(w_desc, keys,/*label=*/"", /*internal=*/false));
};

BOOST_AUTO_TEST_CASE(ismine_standard)
{
    CKey keys[2];
    CPubKey pubkeys[2];
    for (int i = 0; i < 2; i++) {
        keys[i].MakeNewKey(true);
        pubkeys[i] = keys[i].GetPubKey();
    }

    CKey uncompressedKey = GenerateRandomKey(/*compressed=*/false);
    CPubKey uncompressedPubkey = uncompressedKey.GetPubKey();
    NodeContext node;
    auto& chain = m_node.chain;

    CScript scriptPubKey;
    isminetype result;

    // P2PK compressed - Legacy
    {
        CWallet keystore(chain.get(), /*coinjoin_loader=*/nullptr, "", m_args, CreateDummyWalletDatabase());
        keystore.SetupLegacyScriptPubKeyMan();
        LOCK(keystore.GetLegacyScriptPubKeyMan()->cs_KeyStore);
        scriptPubKey = GetScriptForRawPubKey(pubkeys[0]);

        // Keystore does not have key
        result = keystore.GetLegacyScriptPubKeyMan()->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_NO);
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->GetScriptPubKeys().count(scriptPubKey) == 0);

        // Keystore has key
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->AddKey(keys[0]));
        result = keystore.GetLegacyScriptPubKeyMan()->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_SPENDABLE);
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->GetScriptPubKeys().count(scriptPubKey) == 1);
    }

    // P2PK compressed - Descriptor
    {
        CWallet keystore(chain.get(), /*coinjoin_loader=*/nullptr, "", m_args, CreateDummyWalletDatabase());
        std::string desc_str = "pk(" + EncodeSecret(keys[0]) + ")";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        scriptPubKey = GetScriptForRawPubKey(pubkeys[0]);
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_SPENDABLE);
    }

    // P2PK uncompressed - Legacy
    {
        CWallet keystore(chain.get(), /*coinjoin_loader=*/nullptr, "", m_args, CreateDummyWalletDatabase());
        keystore.SetupLegacyScriptPubKeyMan();
        LOCK(keystore.GetLegacyScriptPubKeyMan()->cs_KeyStore);
        scriptPubKey = GetScriptForRawPubKey(uncompressedPubkey);

        // Keystore does not have key
        result = keystore.GetLegacyScriptPubKeyMan()->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_NO);
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->GetScriptPubKeys().count(scriptPubKey) == 0);

        // Keystore has key
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->AddKey(uncompressedKey));
        result = keystore.GetLegacyScriptPubKeyMan()->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_SPENDABLE);
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->GetScriptPubKeys().count(scriptPubKey) == 1);
    }

    // P2PK uncompressed - Descriptor
    {
        CWallet keystore(chain.get(), /*coinjoin_loader=*/nullptr, "", m_args, CreateDummyWalletDatabase());
        std::string desc_str = "pk(" + EncodeSecret(uncompressedKey) + ")";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        scriptPubKey = GetScriptForRawPubKey(uncompressedPubkey);
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_SPENDABLE);
    }

    // P2PKH compressed - Legacy
    {
        CWallet keystore(chain.get(), /*coinjoin_loader=*/nullptr, "", m_args, CreateDummyWalletDatabase());
        keystore.SetupLegacyScriptPubKeyMan();
        LOCK(keystore.GetLegacyScriptPubKeyMan()->cs_KeyStore);
        scriptPubKey = GetScriptForDestination(PKHash(pubkeys[0]));

        // Keystore does not have key
        result = keystore.GetLegacyScriptPubKeyMan()->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_NO);
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->GetScriptPubKeys().count(scriptPubKey) == 0);

        // Keystore has key
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->AddKey(keys[0]));
        result = keystore.GetLegacyScriptPubKeyMan()->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_SPENDABLE);
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->GetScriptPubKeys().count(scriptPubKey) == 1);
    }

    // P2PKH compressed - Descriptor
    {
        CWallet keystore(chain.get(), /*coinjoin_loader=*/nullptr, "", m_args, CreateDummyWalletDatabase());
        std::string desc_str = "pkh(" + EncodeSecret(keys[0]) + ")";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        scriptPubKey = GetScriptForDestination(PKHash(pubkeys[0]));
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_SPENDABLE);
    }

    // P2PKH uncompressed - Legacy
    {
        CWallet keystore(chain.get(), /*coinjoin_loader=*/nullptr, "", m_args, CreateDummyWalletDatabase());
        keystore.SetupLegacyScriptPubKeyMan();
        LOCK(keystore.GetLegacyScriptPubKeyMan()->cs_KeyStore);
        scriptPubKey = GetScriptForDestination(PKHash(uncompressedPubkey));

        // Keystore does not have key
        result = keystore.GetLegacyScriptPubKeyMan()->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_NO);
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->GetScriptPubKeys().count(scriptPubKey) == 0);

        // Keystore has key
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->AddKey(uncompressedKey));
        result = keystore.GetLegacyScriptPubKeyMan()->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_SPENDABLE);
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->GetScriptPubKeys().count(scriptPubKey) == 1);
    }

    // P2PKH uncompressed - Descriptor
    {
        CWallet keystore(chain.get(), /*coinjoin_loader=*/nullptr, "", m_args, CreateDummyWalletDatabase());
        std::string desc_str = "pkh(" + EncodeSecret(uncompressedKey) + ")";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        scriptPubKey = GetScriptForDestination(PKHash(uncompressedPubkey));
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_SPENDABLE);
    }

    // P2SH - Legacy
    {
        CWallet keystore(chain.get(), /*coinjoin_loader=*/nullptr, "", m_args, CreateDummyWalletDatabase());
        keystore.SetupLegacyScriptPubKeyMan();
        LOCK(keystore.GetLegacyScriptPubKeyMan()->cs_KeyStore);

        CScript redeemScript = GetScriptForDestination(PKHash(pubkeys[0]));
        scriptPubKey = GetScriptForDestination(ScriptHash(redeemScript));

        // Keystore does not have redeemScript or key
        result = keystore.GetLegacyScriptPubKeyMan()->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_NO);
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->GetScriptPubKeys().count(scriptPubKey) == 0);

        // Keystore has redeemScript but no key
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->AddCScript(redeemScript));
        result = keystore.GetLegacyScriptPubKeyMan()->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_NO);
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->GetScriptPubKeys().count(scriptPubKey) == 0);

        // Keystore has redeemScript and key
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->AddKey(keys[0]));
        result = keystore.GetLegacyScriptPubKeyMan()->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_SPENDABLE);
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->GetScriptPubKeys().count(scriptPubKey) == 1);
    }

    // P2SH - Descriptor
    {
        CWallet keystore(chain.get(), /*coinjoin_loader=*/nullptr, "", m_args, CreateDummyWalletDatabase());
        std::string desc_str = "sh(pkh(" + EncodeSecret(keys[0]) + "))";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        CScript redeemScript = GetScriptForDestination(PKHash(pubkeys[0]));
        scriptPubKey = GetScriptForDestination(ScriptHash(redeemScript));
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_SPENDABLE);
    }

    // (P2PKH inside) P2SH inside P2SH (invalid) - Legacy
    {
        CWallet keystore(chain.get(), /*coinjoin_loader=*/nullptr, "", m_args, CreateDummyWalletDatabase());
        keystore.SetupLegacyScriptPubKeyMan();
        LOCK(keystore.GetLegacyScriptPubKeyMan()->cs_KeyStore);

        CScript redeemscript_inner = GetScriptForDestination(PKHash(pubkeys[0]));
        CScript redeemscript = GetScriptForDestination(ScriptHash(redeemscript_inner));
        scriptPubKey = GetScriptForDestination(ScriptHash(redeemscript));

        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->AddCScript(redeemscript));
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->AddCScript(redeemscript_inner));
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->AddCScript(scriptPubKey));
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->AddKey(keys[0]));
        result = keystore.GetLegacyScriptPubKeyMan()->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_NO);
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->GetScriptPubKeys().count(scriptPubKey) == 0);
    }

    // (P2PKH inside) P2SH inside P2SH (invalid) - Descriptor
    {
        CWallet keystore(chain.get(), /*coinjoin_loader=*/nullptr, "", m_args, CreateDummyWalletDatabase());
        std::string desc_str = "sh(sh(" + EncodeSecret(keys[0]) + "))";

        auto spk_manager = CreateDescriptor(keystore, desc_str, false);
        BOOST_CHECK_EQUAL(spk_manager, nullptr);
    }

    // P2WPKH uncompressed (invalid) - Descriptor
    // Not applicable for Dash Core because there is no segwit

    // scriptPubKey multisig - Legacy
    {
        CWallet keystore(chain.get(), /*coinjoin_loader=*/nullptr, "", m_args, CreateDummyWalletDatabase());
        keystore.SetupLegacyScriptPubKeyMan();
        LOCK(keystore.GetLegacyScriptPubKeyMan()->cs_KeyStore);

        scriptPubKey = GetScriptForMultisig(2, {uncompressedPubkey, pubkeys[1]});

        // Keystore does not have any keys
        result = keystore.GetLegacyScriptPubKeyMan()->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_NO);
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->GetScriptPubKeys().count(scriptPubKey) == 0);

        // Keystore has 1/2 keys
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->AddKey(uncompressedKey));

        result = keystore.GetLegacyScriptPubKeyMan()->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_NO);
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->GetScriptPubKeys().count(scriptPubKey) == 0);

        // Keystore has 2/2 keys
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->AddKey(keys[1]));

        result = keystore.GetLegacyScriptPubKeyMan()->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_NO);
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->GetScriptPubKeys().count(scriptPubKey) == 0);

        // Keystore has 2/2 keys and the script
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->AddCScript(scriptPubKey));

        result = keystore.GetLegacyScriptPubKeyMan()->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_NO);
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->GetScriptPubKeys().count(scriptPubKey) == 0);
    }

    // scriptPubKey multisig - Descriptor
    {
        CWallet keystore(chain.get(), /*coinjoin_loader=*/nullptr, "", m_args, CreateDummyWalletDatabase());
        std::string desc_str = "multi(2, " + EncodeSecret(uncompressedKey) + ", " + EncodeSecret(keys[1]) + ")";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        scriptPubKey = GetScriptForMultisig(2, {uncompressedPubkey, pubkeys[1]});
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_SPENDABLE);
    }

    // P2SH multisig - Legacy
    {
        CWallet keystore(chain.get(), /*coinjoin_loader=*/nullptr, "", m_args, CreateDummyWalletDatabase());
        keystore.SetupLegacyScriptPubKeyMan();
        LOCK(keystore.GetLegacyScriptPubKeyMan()->cs_KeyStore);
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->AddKey(uncompressedKey));
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->AddKey(keys[1]));

        CScript redeemScript = GetScriptForMultisig(2, {uncompressedPubkey, pubkeys[1]});
        scriptPubKey = GetScriptForDestination(ScriptHash(redeemScript));

        // Keystore has no redeemScript
        result = keystore.GetLegacyScriptPubKeyMan()->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_NO);
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->GetScriptPubKeys().count(scriptPubKey) == 0);

        // Keystore has redeemScript
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->AddCScript(redeemScript));
        result = keystore.GetLegacyScriptPubKeyMan()->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_SPENDABLE);
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->GetScriptPubKeys().count(scriptPubKey) == 1);
    }

    // P2SH multisig - Descriptor
    {
        CWallet keystore(chain.get(), /*coinjoin_loader=*/nullptr, "", m_args, CreateDummyWalletDatabase());

        std::string desc_str = "sh(multi(2, " + EncodeSecret(uncompressedKey) + ", " + EncodeSecret(keys[1]) + "))";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        CScript redeemScript = GetScriptForMultisig(2, {uncompressedPubkey, pubkeys[1]});
        scriptPubKey = GetScriptForDestination(ScriptHash(redeemScript));
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_SPENDABLE);
    }

    // Combo - Descriptor
    {
        CWallet keystore(chain.get(), /*coinjoin_loader=*/nullptr, "", m_args, CreateDummyWalletDatabase());

        std::string desc_str = "combo(" + EncodeSecret(keys[0]) + ")";

        auto spk_manager = CreateDescriptor(keystore, desc_str, true);

        // Test P2PK
        result = spk_manager->IsMine(GetScriptForRawPubKey(pubkeys[0]));
        BOOST_CHECK_EQUAL(result, ISMINE_SPENDABLE);

        // Test P2PKH
        result = spk_manager->IsMine(GetScriptForDestination(PKHash(pubkeys[0])));
        BOOST_CHECK_EQUAL(result, ISMINE_SPENDABLE);

        // Test P2SH-P2PKH (Dash's combo descriptor describes it in place of upstream's P2SH-P2WPKH)
        CScript redeemScript = GetScriptForDestination(PKHash(pubkeys[0]));
        scriptPubKey = GetScriptForDestination(ScriptHash(redeemScript));
        result = spk_manager->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_SPENDABLE);
    }

    // OP_RETURN
    {
        CWallet keystore(chain.get(), /*coinjoin_loader=*/nullptr, "", m_args, CreateDummyWalletDatabase());
        keystore.SetupLegacyScriptPubKeyMan();
        LOCK(keystore.GetLegacyScriptPubKeyMan()->cs_KeyStore);
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->AddKey(keys[0]));

        scriptPubKey.clear();
        scriptPubKey << OP_RETURN << ToByteVector(pubkeys[0]);

        result = keystore.GetLegacyScriptPubKeyMan()->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_NO);
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->GetScriptPubKeys().count(scriptPubKey) == 0);
    }

    // Nonstandard
    {
        CWallet keystore(chain.get(), /*coinjoin_loader=*/nullptr, "", m_args, CreateDummyWalletDatabase());
        keystore.SetupLegacyScriptPubKeyMan();
        LOCK(keystore.GetLegacyScriptPubKeyMan()->cs_KeyStore);
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->AddKey(keys[0]));

        scriptPubKey.clear();
        scriptPubKey << OP_9 << OP_ADD << OP_11 << OP_EQUAL;

        result = keystore.GetLegacyScriptPubKeyMan()->IsMine(scriptPubKey);
        BOOST_CHECK_EQUAL(result, ISMINE_NO);
        BOOST_CHECK(keystore.GetLegacyScriptPubKeyMan()->GetScriptPubKeys().count(scriptPubKey) == 0);
    }
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
