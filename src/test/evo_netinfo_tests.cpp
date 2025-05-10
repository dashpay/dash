// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <chainparams.h>
#include <evo/netinfo.h>
#include <netbase.h>
#include <streams.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(evo_netinfo_tests, BasicTestingSetup)

const std::vector<std::pair</*input=*/std::string, /*expected_ret=*/NetInfoStatus>> vals{
    // Address and port specified
    {"1.1.1.1:9999", NetInfoStatus::Success},
    // Address specified, port should default to default P2P core
    {"1.1.1.1", NetInfoStatus::Success},
    // Non-mainnet port on mainnet
    {"1.1.1.1:9998", NetInfoStatus::BadPort},
    // Internal addresses not allowed on mainnet
    {"127.0.0.1:9999", NetInfoStatus::NotRoutable},
    // Valid IPv4 formatting but invalid IPv4 address
    {"0.0.0.0:9999", NetInfoStatus::BadAddress},
    // Port greater than uint16_t max
    {"1.1.1.1:99999", NetInfoStatus::BadInput},
    // Only IPv4 allowed
    {"[2606:4700:4700::1111]:9999", NetInfoStatus::BadInput},
    // Domains are not allowed
    {"example.com:9999", NetInfoStatus::BadInput},
    // Incorrect IPv4 address
    {"1.1.1.256:9999", NetInfoStatus::BadInput},
    // Missing address
    {":9999", NetInfoStatus::BadInput},
};

void ValidateGetEntries(const NetInfoList& entries, const size_t expected_size)
{
    BOOST_CHECK_EQUAL(entries.size(), expected_size);
    for (const NetInfoEntry& entry : entries) {
        BOOST_CHECK(entry.IsTriviallyValid());
    }
}

BOOST_AUTO_TEST_CASE(mnnetinfo_rules)
{
    // Validate AddEntry() rules enforcement
    for (const auto& [input, expected_ret] : vals) {
        MnNetInfo netInfo;
        BOOST_CHECK_EQUAL(netInfo.AddEntry(input), expected_ret);
        if (expected_ret != NetInfoStatus::Success) {
            // An empty MnNetInfo is considered malformed
            BOOST_CHECK_EQUAL(netInfo.Validate(), NetInfoStatus::Malformed);
            BOOST_CHECK(netInfo.GetEntries().empty());
        } else {
            BOOST_CHECK_EQUAL(netInfo.Validate(), NetInfoStatus::Success);
            ValidateGetEntries(netInfo.GetEntries(), /*expected_size=*/1);
        }
    }

    {
        // MnNetInfo only stores one value, overwriting prohibited
        MnNetInfo netInfo;
        BOOST_CHECK_EQUAL(netInfo.AddEntry("1.1.1.1:9999"), NetInfoStatus::Success);
        BOOST_CHECK_EQUAL(netInfo.AddEntry("1.1.1.2:9999"), NetInfoStatus::MaxLimit);
        ValidateGetEntries(netInfo.GetEntries(), /*expected_size=*/1);
    }
}

bool CheckIfSerSame(const CService& lhs, const MnNetInfo& rhs)
{
    CHashWriter ss_lhs(SER_GETHASH, 0), ss_rhs(SER_GETHASH, 0);
    ss_lhs << lhs;
    ss_rhs << rhs;
    return ss_lhs.GetSHA256() == ss_rhs.GetSHA256();
}

BOOST_AUTO_TEST_CASE(cservice_compatible)
{
    // Empty values should be the same
    CService service;
    MnNetInfo netInfo;
    BOOST_CHECK(CheckIfSerSame(service, netInfo));

    // Valid IPv4 address, valid port
    service = LookupNumeric("1.1.1.1", 9999);
    netInfo.Clear();
    BOOST_CHECK_EQUAL(netInfo.AddEntry("1.1.1.1:9999"), NetInfoStatus::Success);
    BOOST_CHECK(CheckIfSerSame(service, netInfo));

    // Valid IPv4 address, default P2P port implied
    service = LookupNumeric("1.1.1.1", Params().GetDefaultPort());
    netInfo.Clear();
    BOOST_CHECK_EQUAL(netInfo.AddEntry("1.1.1.1"), NetInfoStatus::Success);
    BOOST_CHECK(CheckIfSerSame(service, netInfo));

    // Lookup() failure (domains not allowed), MnNetInfo should remain empty if Lookup() failed
    service = CService();
    netInfo.Clear();
    BOOST_CHECK_EQUAL(netInfo.AddEntry("example.com"), NetInfoStatus::BadInput);
    BOOST_CHECK(CheckIfSerSame(service, netInfo));

    // Validation failure (non-IPv4 not allowed), MnNetInfo should remain empty if ValidateService() failed
    service = CService();
    netInfo.Clear();
    BOOST_CHECK_EQUAL(netInfo.AddEntry("[2606:4700:4700::1111]:9999"), NetInfoStatus::BadInput);
    BOOST_CHECK(CheckIfSerSame(service, netInfo));
}

BOOST_AUTO_TEST_SUITE_END()
