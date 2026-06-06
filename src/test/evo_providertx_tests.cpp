// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/providertx.h>

#include <bls/bls.h>
#include <clientversion.h>
#include <evo/dmn_types.h>
#include <evo/dmnstate.h>
#include <evo/netinfo.h>
#include <key.h>
#include <key_io.h>
#include <masternode/payments.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/standard.h>
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

// ---- DIP0026 deterministic MN state (P3) ----

static CDeterministicMNState MakeMNState(uint16_t version)
{
    CDeterministicMNState s;
    s.nVersion = version;
    s.netInfo = NetInfoInterface::MakeNetInfo(version);
    BOOST_CHECK_EQUAL(s.netInfo->AddEntry(NetInfoPurpose::CORE_P2P, "1.2.3.4:9999"), NetInfoStatus::Success);
    CBLSSecretKey sk;
    sk.MakeNewKey();
    s.pubKeyOperator.Set(sk.GetPublicKey(), /*specificLegacyScheme=*/version == ProTxVersion::LegacyBLS);
    CKey k;
    k.MakeNewKey(true);
    s.keyIDOwner = k.GetPubKey().GetID();
    s.keyIDVoting = k.GetPubKey().GetID();
    return s;
}

BOOST_AUTO_TEST_CASE(mnstate_v4_payoutshares_roundtrip)
{
    CDeterministicMNState in = MakeMNState(ProTxVersion::MultiPayout);
    in.payoutShares = {{P2PKHScript(0xa1), 5000}, {P2PKHScript(0xa2), 5000}};

    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << in;
    CDeterministicMNState out;
    ss >> out;

    BOOST_CHECK_EQUAL(out.nVersion, ProTxVersion::MultiPayout);
    BOOST_CHECK(out.payoutShares == in.payoutShares);
    BOOST_CHECK(out.scriptPayout.empty());
    BOOST_CHECK(out.GetPayoutShares() == in.payoutShares);
}

BOOST_AUTO_TEST_CASE(mnstate_v3_scriptpayout_roundtrip)
{
    CDeterministicMNState in = MakeMNState(ProTxVersion::ExtAddr);
    in.scriptPayout = P2PKHScript(0xa3);

    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << in;
    CDeterministicMNState out;
    ss >> out;

    BOOST_CHECK_EQUAL(out.nVersion, ProTxVersion::ExtAddr);
    BOOST_CHECK(out.scriptPayout == in.scriptPayout);
    BOOST_CHECK(out.payoutShares.empty());
}

BOOST_AUTO_TEST_CASE(mnstatediff_tracks_payoutshares)
{
    CDeterministicMNState a = MakeMNState(ProTxVersion::MultiPayout);
    a.payoutShares = {{P2PKHScript(0xb1), PayoutShare::TOTAL_BASIS_POINTS}};
    CDeterministicMNState b = a;
    b.payoutShares = {{P2PKHScript(0xb2), 6000}, {P2PKHScript(0xb3), 4000}};

    CDeterministicMNStateDiff diff(a, b);
    BOOST_CHECK(diff.fields & CDeterministicMNStateDiff::Field_payoutShares);

    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << diff;
    CDeterministicMNStateDiff out;
    ss >> out;

    CDeterministicMNState applied = a;
    out.ApplyToState(applied);
    BOOST_CHECK(applied.payoutShares == b.payoutShares);
}

// ---- DIP0026 payout validation reject matrix (P4) ----

static CScript P2SHScript(uint8_t fill)
{
    return CScript() << OP_HASH160 << std::vector<unsigned char>(20, fill) << OP_EQUAL;
}

// Returns "" if CheckPayoutShares accepts, otherwise the reject reason string.
static std::string PayoutReject(uint16_t version, const CScript& script, const std::vector<PayoutShare>& shares)
{
    TxValidationState state;
    if (CheckPayoutShares(version, script, shares, state)) return "";
    return state.GetRejectReason();
}

BOOST_AUTO_TEST_CASE(checkpayoutshares_accepts_valid)
{
    // v<4: a single p2pkh or p2sh scriptPayout is accepted (payoutShares ignored/empty).
    BOOST_CHECK_EQUAL(PayoutReject(ProTxVersion::ExtAddr, P2PKHScript(0x01), {}), "");
    BOOST_CHECK_EQUAL(PayoutReject(ProTxVersion::ExtAddr, P2SHScript(0x01), {}), "");
    // v4: shares summing to exactly 10000, unique, nonzero, p2pkh/p2sh.
    const std::vector<PayoutShare> ok{{P2PKHScript(0x01), 6000}, {P2SHScript(0x02), 4000}};
    BOOST_CHECK_EQUAL(PayoutReject(ProTxVersion::MultiPayout, CScript(), ok), "");
    // exactly MAX_PAYOUT_SHARES (32) summing to 10000.
    std::vector<PayoutShare> many;
    int total = 0;
    for (size_t i = 0; i < PayoutShare::MAX_PAYOUT_SHARES; ++i) {
        const uint16_t r = (i + 1 < PayoutShare::MAX_PAYOUT_SHARES) ? 312 : uint16_t(10000 - total);
        many.push_back({P2PKHScript(uint8_t(i + 1)), r});
        total += r;
    }
    BOOST_CHECK_EQUAL(PayoutReject(ProTxVersion::MultiPayout, CScript(), many), "");
}

BOOST_AUTO_TEST_CASE(checkpayoutshares_reject_matrix)
{
    // v<4 non-standard script
    BOOST_CHECK_EQUAL(PayoutReject(ProTxVersion::ExtAddr, CScript() << OP_TRUE, {}), "bad-protx-payee");
    // v4 empty set
    BOOST_CHECK_EQUAL(PayoutReject(ProTxVersion::MultiPayout, CScript(), {}), "bad-protx-payout-shares-count");
    // v4 too many (33)
    std::vector<PayoutShare> tooMany;
    for (size_t i = 0; i < PayoutShare::MAX_PAYOUT_SHARES + 1; ++i) tooMany.push_back({P2PKHScript(uint8_t(i + 1)), 303});
    BOOST_CHECK_EQUAL(PayoutReject(ProTxVersion::MultiPayout, CScript(), tooMany), "bad-protx-payout-shares-count");
    // v4 sum != 10000
    BOOST_CHECK_EQUAL(PayoutReject(ProTxVersion::MultiPayout, CScript(),
                                   {{P2PKHScript(0x01), 6000}, {P2PKHScript(0x02), 3999}}),
                      "bad-protx-payout-shares-sum");
    // v4 a reward exceeding 10000
    BOOST_CHECK_EQUAL(PayoutReject(ProTxVersion::MultiPayout, CScript(), {{P2PKHScript(0x01), 10001}}),
                      "bad-protx-payout-share-reward");
    // v4 a zero reward
    BOOST_CHECK_EQUAL(PayoutReject(ProTxVersion::MultiPayout, CScript(),
                                   {{P2PKHScript(0x01), 0}, {P2PKHScript(0x02), 10000}}),
                      "bad-protx-payout-share-reward");
    // v4 duplicate script
    BOOST_CHECK_EQUAL(PayoutReject(ProTxVersion::MultiPayout, CScript(),
                                   {{P2PKHScript(0x01), 5000}, {P2PKHScript(0x01), 5000}}),
                      "bad-protx-payout-share-duplicate");
    // v4 non-standard script in a share
    BOOST_CHECK_EQUAL(PayoutReject(ProTxVersion::MultiPayout, CScript(), {{CScript() << OP_TRUE, 10000}}),
                      "bad-protx-payee");
    // cross-version mixing: v4 must not also carry a legacy scriptPayout
    BOOST_CHECK_EQUAL(PayoutReject(ProTxVersion::MultiPayout, P2PKHScript(0x09), {{P2PKHScript(0x01), 10000}}),
                      "bad-protx-payout-script-unexpected");
    // cross-version mixing: v<4 must not carry payout shares
    BOOST_CHECK_EQUAL(PayoutReject(ProTxVersion::ExtAddr, P2PKHScript(0x01), {{P2PKHScript(0x02), 10000}}),
                      "bad-protx-payout-shares-unexpected");
}

// ---- DIP0026 MakeSignString (external-collateral payloadSig) (P5) ----

BOOST_AUTO_TEST_CASE(makesignstring_v4_format)
{
    CProRegTx p = MakeProReg(ProTxVersion::MultiPayout);
    p.nOperatorReward = 250;
    p.payoutShares = {{P2PKHScript(0xc1), 7000}, {P2SHScript(0xc2), 3000}};

    CTxDestination d0, d1;
    BOOST_REQUIRE(ExtractDestination(p.payoutShares[0].scriptPayout, d0));
    BOOST_REQUIRE(ExtractDestination(p.payoutShares[1].scriptPayout, d1));
    // DIP0026: <addr0>|<reward0>|<addr1>|<reward1>|<operatorReward>|<owner>|<voting>|<hash>
    const std::string expected = EncodeDestination(d0) + "|7000|" + EncodeDestination(d1) + "|3000|"
                                 + "250|" + EncodeDestination(PKHash(p.keyIDOwner)) + "|"
                                 + EncodeDestination(PKHash(p.keyIDVoting)) + "|";

    const std::string s = p.MakeSignString();
    BOOST_CHECK(s.rfind(expected, 0) == 0);          // starts with the expected prefix
    BOOST_CHECK_EQUAL(s.size(), expected.size() + 64); // followed by the 64-hex payload hash
}

BOOST_AUTO_TEST_CASE(makesignstring_v3_unchanged)
{
    CProRegTx p = MakeProReg(ProTxVersion::ExtAddr);
    p.nOperatorReward = 100;
    p.scriptPayout = P2PKHScript(0xc3);

    CTxDestination d;
    BOOST_REQUIRE(ExtractDestination(p.scriptPayout, d));
    const std::string expected = EncodeDestination(d) + "|100|" + EncodeDestination(PKHash(p.keyIDOwner)) + "|"
                                 + EncodeDestination(PKHash(p.keyIDVoting)) + "|";

    const std::string s = p.MakeSignString();
    BOOST_CHECK(s.rfind(expected, 0) == 0);
    BOOST_CHECK_EQUAL(s.size(), expected.size() + 64);
}

// ---- DIP0026 coinbase reward split (P6) ----

static CAmount SumOuts(const std::vector<CTxOut>& outs)
{
    CAmount s = 0;
    for (const auto& o : outs) s += o.nValue;
    return s;
}

BOOST_AUTO_TEST_CASE(split_legacy_single_share)
{
    // A single full share (the GetPayoutShares() view of a pre-v4 MN) must reproduce the legacy
    // single output exactly.
    const auto outs = SplitMasternodeReward(12345, {{P2PKHScript(0x01), PayoutShare::TOTAL_BASIS_POINTS}});
    BOOST_REQUIRE_EQUAL(outs.size(), 1U);
    BOOST_CHECK_EQUAL(outs[0].nValue, 12345);
    BOOST_CHECK(outs[0].scriptPubKey == P2PKHScript(0x01));
}

BOOST_AUTO_TEST_CASE(split_rounding_and_sum_invariant)
{
    // floors 3/3/3 = 9, leftover 1 satoshi handed to the first share -> 4/3/3, sum 10.
    const auto outs = SplitMasternodeReward(
        10, {{P2PKHScript(1), 3334}, {P2PKHScript(2), 3333}, {P2PKHScript(3), 3333}});
    BOOST_REQUIRE_EQUAL(outs.size(), 3U);
    BOOST_CHECK_EQUAL(outs[0].nValue, 4);
    BOOST_CHECK_EQUAL(outs[1].nValue, 3);
    BOOST_CHECK_EQUAL(outs[2].nValue, 3);
    BOOST_CHECK_EQUAL(SumOuts(outs), 10);

    // The outputs always sum to exactly the reward, across a wide range of amounts.
    const std::vector<PayoutShare> shares{{P2PKHScript(1), 1234}, {P2PKHScript(2), 4321}, {P2PKHScript(3), 4445}};
    for (const CAmount r : {CAmount{1}, CAmount{2}, CAmount{3}, CAmount{7}, CAmount{99}, CAmount{100},
                            CAmount{9999}, CAmount{10000}, CAmount{100003}, CAmount{123456789}, CAmount{5000000000}}) {
        BOOST_CHECK_EQUAL(SumOuts(SplitMasternodeReward(r, shares)), r);
    }
}

BOOST_AUTO_TEST_CASE(split_skips_zero_and_edges)
{
    // reward 1, two equal shares: floors 0/0, leftover 1 -> first gets 1, second 0 (omitted).
    const auto outs = SplitMasternodeReward(1, {{P2PKHScript(1), 5000}, {P2PKHScript(2), 5000}});
    BOOST_REQUIRE_EQUAL(outs.size(), 1U);
    BOOST_CHECK_EQUAL(outs[0].nValue, 1);
    BOOST_CHECK(outs[0].scriptPubKey == P2PKHScript(1));

    // zero / negative reward or no shares -> no outputs.
    BOOST_CHECK(SplitMasternodeReward(0, {{P2PKHScript(1), PayoutShare::TOTAL_BASIS_POINTS}}).empty());
    BOOST_CHECK(SplitMasternodeReward(-5, {{P2PKHScript(1), PayoutShare::TOTAL_BASIS_POINTS}}).empty());
    BOOST_CHECK(SplitMasternodeReward(100, {}).empty());

    // determinism: identical inputs yield identical outputs.
    const std::vector<PayoutShare> shares{{P2PKHScript(1), 6000}, {P2PKHScript(2), 4000}};
    BOOST_CHECK(SplitMasternodeReward(777, shares) == SplitMasternodeReward(777, shares));
}

BOOST_AUTO_TEST_SUITE_END()
