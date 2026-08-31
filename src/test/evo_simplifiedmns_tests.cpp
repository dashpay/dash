// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <bls/bls.h>
#include <evo/deterministicmns.h>
#include <evo/dmnstate.h>
#include <evo/simplifiedmns.h>
#include <evo/smldiff.h>
#include <netbase.h>
#include <script/standard.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(evo_simplifiedmns_tests, RegTestingSetup)

BOOST_AUTO_TEST_CASE(simplifiedmns_merkleroots)
{
    //TODO: Provide raw data for basic scheme as well
    bls::bls_legacy_scheme.store(true);
    std::vector<std::unique_ptr<CSimplifiedMNListEntry>> entries;
    for (size_t i = 1; i < 16; i++) {
        CSimplifiedMNListEntry smle;
        smle.nVersion = ProTxVersion::GetMax(!bls::bls_legacy_scheme, /*is_extended_addr=*/false);
        smle.netInfo = NetInfoInterface::MakeNetInfo(smle.nVersion);
        smle.proRegTxHash.SetHex(strprintf("%064x", i));
        smle.confirmedHash.SetHex(strprintf("%064x", i));

        BOOST_CHECK_EQUAL(smle.netInfo->AddEntry(NetInfoPurpose::CORE_P2P, strprintf("%d.%d.%d.%d:%d", 0, 0, 0, i, i)),
                          NetInfoStatus::Success);

        std::vector<unsigned char> vecBytes{static_cast<unsigned char>(i)};
        vecBytes.resize(CBLSSecretKey::SerSize);

        smle.pubKeyOperator.Set(CBLSSecretKey(vecBytes).GetPublicKey(), bls::bls_legacy_scheme.load());
        smle.keyIDVoting.SetHex(strprintf("%040x", i));
        smle.isValid = true;

        entries.emplace_back(std::make_unique<CSimplifiedMNListEntry>(smle));
    }

    std::vector<std::string> expectedHashes = {
        "3a1010e28226558560e5296bcee6bf0b9b963b73a1514f5aa2885e270f6b90c1",
        "85d3d93b28689128daf3a41d706ae5002f447b9b6372776f0ca9d53b31146884",
        "8930eee6bd2e7971a7090edfb79f74c00a12280e59adfc2cc99d406a01e368f9",
        "dc2e69caa0ef97e8f5cf40a9530641bd4933dd8c9ad533054537728f7e5f58c2",
        "3e4a0e0a0d2ed397fa27221de3047de21f50d17d0ba43738cbdb9fee96c7cb46",
        "eb18476a1496e1cb912b1d4dd93314b78c6a679d83cae8e144a717b967dc4b8c",
        "6c0d01fa40ac11d7b523facd2bf5632c83f7e4df3f60fd1b364ea90f6c852156",
        "c9e3e69d54e6e95b280ae102593fe114cf4620fa89dd88da1a146ada08815d68",
        "1023f67f735e8e9403d5f083e7a17489619b1790feac4f6b133e9dda15999ae6",
        "5d5fc77944f7c72df236a5baf460c7b9a947144d54d0953521f1494c8a2f7aaa",
        "ac7db66820de3c7506f8c6415fd352e36ac5f27c6adbdfb74de3e109d0d277df",
        "cbc25ca965d0fa69a1fdc1d796b8ee2726a0e2137414e92fb9541630e3189901",
        "ac9934c4049ae952d41fb38e7e9659a558a5ce748bdb7fb613741598d1b16a27",
        "a61177eb14450bb8c56e5f0547035e0f3a70fe46f36901351cc568b2e48e29d0",
        "67798d40a2196f446114c68d86445fe088750515a96eb65392c6bcfac8f3be9b"
    };
    std::vector<std::string> calculatedHashes;

    for (auto& smle : entries) {
        calculatedHashes.emplace_back(smle->CalcHash().ToString());
        //printf("\"%s\",\n", calculatedHashes.back().c_str());
    }

    BOOST_CHECK(expectedHashes == calculatedHashes);

    CSimplifiedMNList sml{std::move(entries)};

    std::string expectedMerkleRoot = "0bae2176078cf42fa3e1fda761d4255d1c1c54777c6a793d0ab2b07c85ed4022";
    std::string calculatedMerkleRoot = sml.CalcMerkleRoot(nullptr).ToString();
    //printf("merkleRoot=\"%s\",\n", calculatedMerkleRoot.c_str());

    BOOST_CHECK(expectedMerkleRoot == calculatedMerkleRoot);
}

BOOST_AUTO_TEST_CASE(simplifiedmns_extended_diff_share_reward)
{
    bls::bls_legacy_scheme.store(false);

    auto make_keyid = [](uint8_t i) {
        CKeyID ret;
        ret.SetHex(strprintf("%040x", i));
        return ret;
    };
    auto make_p2pkh = [&](uint8_t i) { return GetScriptForDestination(PKHash(make_keyid(i))); };

    auto state = std::make_shared<CDeterministicMNState>();
    state->nVersion = ProTxVersion::ExtAddr;
    state->confirmedHash.SetHex(strprintf("%064x", 1));
    state->netInfo = NetInfoInterface::MakeNetInfo(state->nVersion);
    BOOST_CHECK_EQUAL(state->netInfo->AddEntry(NetInfoPurpose::CORE_P2P, "1.1.1.1:9999"), NetInfoStatus::Success);
    std::vector<unsigned char> vecBytes{1};
    vecBytes.resize(CBLSSecretKey::SerSize);
    state->pubKeyOperator.Set(CBLSSecretKey(vecBytes).GetPublicKey(), bls::bls_legacy_scheme.load());
    state->keyIDVoting = make_keyid(2);
    state->shares = CollateralShares{CCollateralShare(600 * COIN, make_p2pkh(3), CScript(), make_keyid(4)),
                                     CCollateralShare(400 * COIN, make_p2pkh(5), CScript(), make_keyid(6))};

    auto dmn = std::make_shared<CDeterministicMN>(/*_internalId=*/1, MnType::Regular);
    dmn->proTxHash.SetHex(strprintf("%064x", 7));
    dmn->collateralOutpoint = COutPoint(uint256::ONE, 0);
    dmn->pdmnState = state;

    CDeterministicMNList from(uint256::ONE, /*_height=*/1, /*_totalRegisteredCount=*/1);
    from.AddMN(dmn);

    // Same list except one share's reward script changed, as a ProUpShareTx would do
    auto newState = std::make_shared<CDeterministicMNState>(*state);
    newState->shares[1].scriptReward = make_p2pkh(8);
    CDeterministicMNList to(from);
    to.UpdateMN(dmn->proTxHash, newState);

    // The consensus SML leaf must not be affected by share data
    BOOST_CHECK(from.GetMN(dmn->proTxHash)->to_sml_entry().CalcHash() ==
                to.GetMN(dmn->proTxHash)->to_sml_entry().CalcHash());

    // A non-extended diff sees no change, but an extended diff must carry the updated entry
    BOOST_CHECK(BuildSimplifiedDiff(from, to, /*extended=*/false).mnList.empty());
    const auto extDiff = BuildSimplifiedDiff(from, to, /*extended=*/true);
    BOOST_REQUIRE_EQUAL(extDiff.mnList.size(), 1U);
    BOOST_CHECK(extDiff.mnList[0].proRegTxHash == dmn->proTxHash);
    BOOST_CHECK(extDiff.mnList[0].shares == newState->shares);
}
BOOST_AUTO_TEST_SUITE_END()
