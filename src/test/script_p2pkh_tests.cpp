// Public Resource ℗ 2021 𝖡𝗂𝗍𝖼𝗈𝗂𝗇 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌
// Public Resource ℗ 2020 𝖠𝗆𝖾𝗋𝗈 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌
// THIS REPOSITORY IS LICENSED UNDER THE AMERO PUBLIC RESOURCE LICENSE.

#include <script/script.h>
#include <test/test_amero.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(script_p2pkh_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(IsPayToPublicKeyHash)
{
    // Test CScript::IsPayToPublicKeyHash()
    uint160 dummy;
    CScript p2pkh;
    p2pkh << OP_DUP << OP_HASH160 << ToByteVector(dummy) << OP_EQUALVERIFY << OP_CHECKSIG;
    BOOST_CHECK(p2pkh.IsPayToPublicKeyHash());

    static const unsigned char direct[] = {
        OP_DUP, OP_HASH160, 20, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, OP_EQUALVERIFY, OP_CHECKSIG
    };
    BOOST_CHECK(CScript(direct, direct+sizeof(direct)).IsPayToPublicKeyHash());

    static const unsigned char notp2pkh1[] = {
        OP_DUP, OP_HASH160, 20, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, OP_EQUALVERIFY, OP_CHECKSIG, OP_CHECKSIG
    };
    BOOST_CHECK(!CScript(notp2pkh1, notp2pkh1+sizeof(notp2pkh1)).IsPayToPublicKeyHash());

    static const unsigned char p2sh[] = {
        OP_HASH160, 20, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, OP_EQUAL
    };
    BOOST_CHECK(!CScript(p2sh, p2sh+sizeof(p2sh)).IsPayToPublicKeyHash());

    static const unsigned char extra[] = {
        OP_DUP, OP_HASH160, 20, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, OP_EQUALVERIFY, OP_CHECKSIG, OP_CHECKSIG
    };
    BOOST_CHECK(!CScript(extra, extra+sizeof(extra)).IsPayToPublicKeyHash());

    static const unsigned char missing[] = {
        OP_HASH160, 20, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, OP_EQUALVERIFY, OP_CHECKSIG, OP_RETURN
    };
    BOOST_CHECK(!CScript(missing, missing+sizeof(missing)).IsPayToPublicKeyHash());

    static const unsigned char missing2[] = {
        OP_DUP, OP_HASH160, 20, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
    };
    BOOST_CHECK(!CScript(missing2, missing2+sizeof(missing2)).IsPayToPublicKeyHash());

    static const unsigned char tooshort[] = {
        OP_DUP, OP_HASH160, 2, 0,0, OP_EQUALVERIFY, OP_CHECKSIG
    };
    BOOST_CHECK(!CScript(tooshort, tooshort+sizeof(tooshort)).IsPayToPublicKeyHash());

}

BOOST_AUTO_TEST_SUITE_END()
