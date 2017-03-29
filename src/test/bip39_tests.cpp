// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "base58.h"
#include "data/bip39_vectors.json.h"
#include "key.h"
#include "util.h"
#include "utilstrencodings.h"
#include "test/test_dash.h"
#include "bip39.h"

#include <boost/test/unit_test.hpp>

#include <univalue.h>

// In script_tests.cpp
extern UniValue read_json(const std::string& jsondata);

BOOST_FIXTURE_TEST_SUITE(bip39_tests, BasicTestingSetup)

// https://github.com/trezor/python-mnemonic/blob/b502451a33a440783926e04428115e0bed87d01f/vectors.json
BOOST_AUTO_TEST_CASE(bip39_vectors)
{
    UniValue tests = read_json(std::string(json_tests::bip39_vectors, json_tests::bip39_vectors + sizeof(json_tests::bip39_vectors)));

    for (unsigned int i = 0; i < tests.size(); i++) {
        // printf("%d\n", i);
        UniValue test = tests[i];
        std::string strTest = test.write();
        if (test.size() < 4) // Allow for extra stuff (useful for comments)
        {
            BOOST_ERROR("Bad test: " << strTest);
            continue;
        }

        const char *m;
        std::vector<uint8_t> vData = ParseHex(test[0].get_str());
        m = mnemonic_from_data(&vData[0], vData.size());

        const char *mnemonic = test[1].get_str().c_str();

        // printf("%s\n%s\n", m, mnemonic);
        BOOST_CHECK(*m == *mnemonic);

        BOOST_CHECK(mnemonic_check(mnemonic));

        uint8_t seed[64];
        const char *passphrase = "TREZOR";

        mnemonic_to_seed(mnemonic, passphrase, seed, 0);
        // printf("seed: %s\n", HexStr(seed, seed + 64).c_str());
        BOOST_CHECK(HexStr(seed, seed + 64) == test[2].get_str());

        CExtKey key;
        CExtPubKey pubkey;

        key.SetMaster(seed, 64);
        pubkey = key.Neuter();

        CBitcoinExtKey b58key;
        b58key.SetKey(key);
        // printf("CBitcoinExtKey: %s\n", b58key.ToString().c_str());
        BOOST_CHECK(b58key.ToString() == test[3].get_str());
    }
}

BOOST_AUTO_TEST_SUITE_END()
