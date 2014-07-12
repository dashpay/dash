//
// Unit tests for alert system
//

#include <boost/foreach.hpp>
#include <boost/test/unit_test.hpp>
#include <fstream>
#include <iostream>
#include <stdio.h>

#include "alert.h"
#include "base58.h"
#include "key.h"
#include "serialize.h"
#include "util.h"

using namespace std;
namespace fs = boost::filesystem;

bool SignAndSave(CAlert &alert, std::vector<CAlert> &alerts)
{
    CDataStream ds(SER_DISK, PROTOCOL_VERSION);
    ds << alert.nVersion
       << alert.nRelayUntil
       << alert.nExpiration
       << alert.nID
       << alert.nCancel
       << alert.setCancel
       << alert.nMinVer
       << alert.nMaxVer
       << alert.setSubVer
       << alert.nPriority
       << alert.strComment
       << alert.strStatusBar
       << alert.strReserved;

    alert.vchMsg.assign(ds.begin(),ds.end());
    uint256 hash = alert.GetHash();
    std::vector<unsigned char> sig;
    CBitcoinSecret secret;
    if (!secret.SetString("7rDMuTnMxWdqRsvk5fYfwkaZoguWJMoDucyZvKmURVukdAkGiVb"))
    {
        cout << "Error Setting Private Key" << endl;
        return false;
    }
    CKey key = secret.GetKey();

    if (!key.Sign(hash, sig))
    {
        cout << "Could Not Sign Message" << endl;
        return false;
    }
    alert.vchSig = sig;

    try
    {
        alerts.push_back(alert);
    }

    catch (std::exception &e)
    {
        cout << "Exception caught " << e.what() << endl;
    }
    return true;

}

struct SetUpAlerts
{
    SetUpAlerts()
    {

        CAlert alert;
        alert.nRelayUntil = 60;
        alert.nExpiration = 24 * 60 * 60;
        alert.nID = 1;
        alert.nCancel = 0;   // cancels previous messages up to this ID number
        alert.nMinVer = 0;  // These versions are protocol versions
        alert.nMaxVer = 70001;
        alert.nPriority = 1;
        alert.strComment = "Alert comment";
        alert.strStatusBar = "Alert 1";

        if (!SignAndSave(alert, alerts))
        {
            return;
        }

        CAlert alert2(alert);
        alert2.setSubVer.insert(std::string("/Satoshi:0.1.0/"));
        alert2.strStatusBar = "Alert 1 for Satoshi 0.1.0";
        if (!SignAndSave(alert2, alerts))
        {
            return;
        }

        CAlert alert3(alert2);
        alert3.setSubVer.insert(std::string("/Satoshi:0.2.0/"));
        alert3.strStatusBar = "Alert 1 for Satoshi 0.1.0, 0.2.0";
        if (!SignAndSave(alert3, alerts))
        {
            return;
        }

        CAlert alert4(alert3);
        alert4.setSubVer.clear();
        ++alert4.nID;
        alert4.nCancel = 1;
        alert4.nPriority = 100;
        alert4.strStatusBar = "Alert 2, cancels 1";
        if (!SignAndSave(alert4, alerts))
        {
            return;
        }

        CAlert alert5(alert4);
        alert5.nExpiration += 60;
        ++alert5.nID;
        if (!SignAndSave(alert5, alerts))
        {
            return;
        }

        CAlert alert6(alert5);
        ++alert6.nID;
        alert6.nMinVer = 11;
        alert6.nMaxVer = 22;
        if (!SignAndSave(alert6, alerts))
        {
            return;
        }

        CAlert alert7(alert6);
        ++alert7.nID;
        alert7.strStatusBar = "Alert 2 for Satoshi 0.1.0";
        alert7.setSubVer.insert(std::string("/Satoshi:0.1.0/"));
        if (!SignAndSave(alert7, alerts))
        {
            return;
        }

        CAlert alert8(alert7);
        ++alert8.nID;
        alert8.nMinVer = 0;
        alert8.nMaxVer = 999999;
        alert8.strStatusBar = "Evil Alert'; /bin/ls; echo '";
        alert8.setSubVer.clear();
        if (!SignAndSave(alert8, alerts))
        {
            return;
        }

        return;

    }
    ~SetUpAlerts()
    {
    }

    static std::vector<std::string> read_lines(boost::filesystem::path filepath)
    {
        std::vector<std::string> result;

        std::ifstream f(filepath.string().c_str());
        std::string line;
        while (std::getline(f, line))
            result.push_back(line);

        return result;
    }

    std::vector<CAlert> alerts;
};

BOOST_FIXTURE_TEST_SUITE(Alert_tests, SetUpAlerts)

BOOST_AUTO_TEST_CASE(AlertApplies)
{
    SetMockTime(11);

    BOOST_FOREACH(const CAlert& alert, alerts)
    {
        BOOST_CHECK(alert.CheckSignature());
    }
    // Matches:
    BOOST_CHECK(alerts[0].AppliesTo(1, ""));
    BOOST_CHECK(alerts[0].AppliesTo(70001, ""));
    BOOST_CHECK(alerts[0].AppliesTo(1, "/Satoshi:11.11.11/"));

    BOOST_CHECK(alerts[1].AppliesTo(1, "/Satoshi:0.1.0/"));
    BOOST_CHECK(alerts[1].AppliesTo(70001, "/Satoshi:0.1.0/"));

    BOOST_CHECK(alerts[2].AppliesTo(1, "/Satoshi:0.1.0/"));
    BOOST_CHECK(alerts[2].AppliesTo(1, "/Satoshi:0.2.0/"));

    // Don't match:
    BOOST_CHECK(!alerts[0].AppliesTo(-1, ""));
    BOOST_CHECK(!alerts[0].AppliesTo(70002, ""));

    BOOST_CHECK(!alerts[1].AppliesTo(1, ""));
    BOOST_CHECK(!alerts[1].AppliesTo(1, "Satoshi:0.1.0"));
    BOOST_CHECK(!alerts[1].AppliesTo(1, "/Satoshi:0.1.0"));
    BOOST_CHECK(!alerts[1].AppliesTo(1, "Satoshi:0.1.0/"));
    BOOST_CHECK(!alerts[1].AppliesTo(-1, "/Satoshi:0.1.0/"));
    BOOST_CHECK(!alerts[1].AppliesTo(70002, "/Satoshi:0.1.0/"));
    BOOST_CHECK(!alerts[1].AppliesTo(1, "/Satoshi:0.2.0/"));

    BOOST_CHECK(!alerts[2].AppliesTo(1, "/Satoshi:0.3.0/"));

    SetMockTime(0);
}

// This uses sh 'echo' to test the -alertnotify function, writing to a
// /tmp file. So skip it on Windows:
#ifndef WIN32
BOOST_AUTO_TEST_CASE(AlertNotify)
{
    SetMockTime(11);

    boost::filesystem::path temp = GetTempPath() / "alertnotify.txt";
    boost::filesystem::remove(temp);

    mapArgs["-alertnotify"] = std::string("echo %s >> ") + temp.string();

    BOOST_FOREACH(CAlert alert, alerts)
    alert.ProcessAlert(false);

    std::vector<std::string> r = read_lines(temp);
    //
    // Only want to run these tests if the "alertnotify.txt" has been read OK and has at least one record
    // in it.
    //
    if (r.size() > 0)
    {
        BOOST_CHECK_EQUAL(r.size(), 1u);
        BOOST_CHECK_EQUAL(r[0], "Evil Alert; /bin/ls; echo "); // single-quotes should be removed
    }
    boost::filesystem::remove(temp);

    SetMockTime(0);
}
#endif

BOOST_AUTO_TEST_SUITE_END()
