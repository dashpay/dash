// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/providertx.h>

#include <bls/bls.h>
#include <evo/dmn_types.h>
#include <evo/netinfo.h>
#include <key.h>
#include <script/script.h>
#include <streams.h>
#include <uint256.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

// DIP0026 multi-party payout data-model / serialization tests.
BOOST_FIXTURE_TEST_SUITE(evo_providertx_tests, BasicTestingSetup)

static CScript P2PKHScript(uint8_t fill)
{
    return CScript() << OP_DUP << OP_HASH160 << std::vector<unsigned char>(20, fill) << OP_EQUALVERIFY << OP_CHECKSIG;
}

template <typename T>
static T SerRoundtrip(const T& in)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << in;
    T out;
    ss >> out;
    return out;
}

BOOST_AUTO_TEST_CASE(payoutshare_roundtrip)
{
    const PayoutShare in{P2PKHScript(0x11), 6000};
    const PayoutShare out = SerRoundtrip(in);
    BOOST_CHECK(in == out);
    BOOST_CHECK(out.scriptPayout == in.scriptPayout);
    BOOST_CHECK_EQUAL(out.payoutShareReward, 6000);
}

BOOST_AUTO_TEST_CASE(payoutshares_vector_wireformat)
{
    // The DIP0026 wire layout is a CompactSize count followed by that many shares.
    const std::vector<PayoutShare> shares{{P2PKHScript(0x11), 6000}, {P2PKHScript(0x22), 4000}};
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << shares;
    BOOST_CHECK_EQUAL(static_cast<uint8_t>(ss[0]), 0x02); // CompactSize count == 2

    std::vector<PayoutShare> out;
    ss >> out;
    BOOST_CHECK_EQUAL(out.size(), 2U);
    BOOST_CHECK(out == shares);
}

// Build a minimal but serializable ProRegTx at the requested version.
static CProRegTx MakeProReg(uint16_t version)
{
    CProRegTx p;
    p.nVersion = version;
    p.nType = MnType::Regular;
    p.netInfo = NetInfoInterface::MakeNetInfo(version);
    BOOST_CHECK_EQUAL(p.netInfo->AddEntry(NetInfoPurpose::CORE_P2P, "1.2.3.4:9999"), NetInfoStatus::Success);
    CBLSSecretKey sk;
    sk.MakeNewKey();
    p.pubKeyOperator.Set(sk.GetPublicKey(), /*specificLegacyScheme=*/version == ProTxVersion::LegacyBLS);
    CKey owner;
    owner.MakeNewKey(true);
    p.keyIDOwner = owner.GetPubKey().GetID();
    p.keyIDVoting = owner.GetPubKey().GetID();
    p.inputsHash = uint256S("aa");
    return p;
}

BOOST_AUTO_TEST_CASE(proregtx_v3_uses_scriptpayout)
{
    CProRegTx in = MakeProReg(ProTxVersion::ExtAddr);
    in.scriptPayout = P2PKHScript(0x33);

    const CProRegTx out = SerRoundtrip(in);
    BOOST_CHECK_EQUAL(out.nVersion, ProTxVersion::ExtAddr);
    BOOST_CHECK(out.scriptPayout == in.scriptPayout);
    BOOST_CHECK(out.payoutShares.empty());
    // Uniform accessor synthesizes a single full share from the legacy field.
    const auto shares = out.GetPayoutShares();
    BOOST_CHECK_EQUAL(shares.size(), 1U);
    BOOST_CHECK(shares[0].scriptPayout == in.scriptPayout);
    BOOST_CHECK_EQUAL(shares[0].payoutShareReward, PayoutShare::TOTAL_BASIS_POINTS);
}

BOOST_AUTO_TEST_CASE(proregtx_v4_uses_payoutshares)
{
    CProRegTx in = MakeProReg(ProTxVersion::MultiPayout);
    in.payoutShares = {{P2PKHScript(0x44), 7000}, {P2PKHScript(0x55), 3000}};

    const CProRegTx out = SerRoundtrip(in);
    BOOST_CHECK_EQUAL(out.nVersion, ProTxVersion::MultiPayout);
    BOOST_CHECK(out.scriptPayout.empty());
    BOOST_CHECK_EQUAL(out.payoutShares.size(), 2U);
    BOOST_CHECK(out.payoutShares == in.payoutShares);
    // Uniform accessor returns the stored shares verbatim for v4.
    BOOST_CHECK(out.GetPayoutShares() == in.payoutShares);
}

// Build a minimal but serializable ProUpRegTx at the requested version.
static CProUpRegTx MakeProUpReg(uint16_t version)
{
    CProUpRegTx p;
    p.nVersion = version;
    p.proTxHash = uint256S("bb");
    CBLSSecretKey sk;
    sk.MakeNewKey();
    p.pubKeyOperator.Set(sk.GetPublicKey(), /*specificLegacyScheme=*/version == ProTxVersion::LegacyBLS);
    CKey voting;
    voting.MakeNewKey(true);
    p.keyIDVoting = voting.GetPubKey().GetID();
    p.inputsHash = uint256S("cc");
    return p;
}

BOOST_AUTO_TEST_CASE(proupregtx_v2_uses_scriptpayout)
{
    CProUpRegTx in = MakeProUpReg(ProTxVersion::BasicBLS);
    in.scriptPayout = P2PKHScript(0x66);

    const CProUpRegTx out = SerRoundtrip(in);
    BOOST_CHECK_EQUAL(out.nVersion, ProTxVersion::BasicBLS);
    BOOST_CHECK(out.scriptPayout == in.scriptPayout);
    BOOST_CHECK(out.payoutShares.empty());
}

BOOST_AUTO_TEST_CASE(proupregtx_v4_uses_payoutshares)
{
    CProUpRegTx in = MakeProUpReg(ProTxVersion::MultiPayout);
    in.payoutShares = {{P2PKHScript(0x77), 5000}, {P2PKHScript(0x88), 2500}, {P2PKHScript(0x99), 2500}};

    const CProUpRegTx out = SerRoundtrip(in);
    BOOST_CHECK_EQUAL(out.nVersion, ProTxVersion::MultiPayout);
    BOOST_CHECK(out.scriptPayout.empty());
    BOOST_CHECK_EQUAL(out.payoutShares.size(), 3U);
    BOOST_CHECK(out.payoutShares == in.payoutShares);
}

BOOST_AUTO_TEST_SUITE_END()
