// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <governance/object.h>
#include <util/strencodings.h>

#include <test/data/proposals_invalid.json.h>
#include <test/data/proposals_valid.json.h>

#include <test/util/json.h>
#include <test/util/setup_common.h>

#include <string>

#include <boost/test/unit_test.hpp>

#include <univalue.h>

BOOST_FIXTURE_TEST_SUITE(governance_validators_tests, BasicTestingSetup)

static std::string CreateEncodedProposalObject(const UniValue& objJSON)
{
    UniValue innerArray(UniValue::VARR);
    innerArray.push_back(UniValue("proposal"));
    innerArray.push_back(objJSON);

    UniValue outerArray(UniValue::VARR);
    outerArray.push_back(innerArray);

    std::string strData = outerArray.write();
    std::string strHex = HexStr(strData);
    return strHex;
}

BOOST_AUTO_TEST_CASE(valid_proposals_test)
{
    // all proposals are valid but expired
    UniValue tests = read_json(std::string(json_tests::proposals_valid, json_tests::proposals_valid + sizeof(json_tests::proposals_valid)));

    BOOST_CHECK_MESSAGE(tests.size(), "Empty `tests`");
    for(size_t i = 0; i < tests.size(); ++i) {
        const UniValue& objProposal = tests[i][0];
        bool fAllowScript = tests[i][1].get_bool();

        // legacy format
        std::string strHexData1 = CreateEncodedProposalObject(objProposal);
        std::string strErrorMessages;
        BOOST_CHECK(!governance::ValidateProposal(strHexData1, strErrorMessages, /*fCheckExpiration=*/true, fAllowScript));
        BOOST_CHECK_EQUAL(strErrorMessages, "Proposal must be a JSON object;JSON parsing error;");

        // current format
        std::string strHexData2 = HexStr(objProposal.write());
        std::string strErrorMessages2;
        BOOST_CHECK_MESSAGE(governance::ValidateProposal(strHexData2, strErrorMessages2, /*fCheckExpiration=*/false,
                                                         fAllowScript),
                            strErrorMessages2);
        std::string strErrorMessages3;
        BOOST_CHECK_MESSAGE(!governance::ValidateProposal(strHexData2, strErrorMessages3, /*fCheckExpiration=*/true,
                                                          fAllowScript),
                            strErrorMessages3);
    }
}

BOOST_AUTO_TEST_CASE(invalid_proposals_test)
{
    // all proposals are invalid regardless of being expired or not
    // (i.e. we don't even check for expiration here)
    UniValue tests = read_json(std::string(json_tests::proposals_invalid, json_tests::proposals_invalid + sizeof(json_tests::proposals_invalid)));

    BOOST_CHECK_MESSAGE(tests.size(), "Empty `tests`");
    for(size_t i = 0; i < tests.size(); ++i) {
        const UniValue& objProposal = tests[i];

        // legacy format
        std::string strHexData1 = CreateEncodedProposalObject(objProposal);
        std::string strErrorMessages1;
        BOOST_CHECK(!governance::ValidateProposal(strHexData1, strErrorMessages1, /*fCheckExpiration=*/true,
                                                  /*fAllowScript=*/false));
        std::string strErrorMessages1b;
        BOOST_CHECK_MESSAGE(!governance::ValidateProposal(strHexData1, strErrorMessages1b, /*fCheckExpiration=*/false,
                                                          /*fAllowScript=*/false),
                            strErrorMessages1b);

        // current format
        std::string strHexData2 = HexStr(objProposal.write());
        std::string strErrorMessages2;
        BOOST_CHECK_MESSAGE(!governance::ValidateProposal(strHexData2, strErrorMessages2, /*fCheckExpiration=*/false,
                                                          /*fAllowScript=*/false),
                            strErrorMessages2);
    }
}

BOOST_AUTO_TEST_SUITE_END()
