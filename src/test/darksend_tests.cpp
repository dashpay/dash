#include <boost/algorithm/string.hpp>
#include <boost/foreach.hpp>
#include <boost/test/unit_test.hpp>

#include "base58.h"
#include "util.h"
#include "main.h"
#include "core.h"
#include "darksend.h"
#include "masternode.h"

using namespace std;

BOOST_AUTO_TEST_SUITE(darksend_tests)

BOOST_AUTO_TEST_CASE(darksend_payment_amount)
{
    int64 blockValue = 50*COIN;

    int64 val_2p5p = (blockValue / 40);
    int64 val_5p = (blockValue / 20);
    int64 val_10p = (blockValue / 10);
    int64 val_20p = (blockValue / 5);

    BOOST_CHECK(GetMasternodePayment(10000, blockValue) == val_20p);
    BOOST_CHECK(GetMasternodePayment(158005, blockValue) != val_20p);
    BOOST_CHECK(GetMasternodePayment(158005, blockValue) == val_20p+val_5p);
    BOOST_CHECK(GetMasternodePayment(158005+((576*30)*3), blockValue) == val_20p+val_10p+val_5p+val_2p5p);
    BOOST_CHECK(GetMasternodePayment(158005+((576*30)*8), blockValue) == val_20p+val_20p+val_5p+val_2p5p);
    BOOST_CHECK(GetMasternodePayment(158005+((576*30)*33), blockValue) == (blockValue / 5)+(blockValue / 5)+(blockValue / 5));
}


BOOST_AUTO_TEST_CASE(darksend_payments)
{
    darkSendPool.unitTest = true;
    darkSendMasterNodes.clear();

    CService addr;
    std::vector<unsigned char> vchSig;
    CMasternodePayments mnp;

    uint256 n1; n1.SetHex("5c4573335c56fd5c9e1851bb5c0616de96d35a41b4d2971094f2380e84be8d32");
    uint256 n2; n2.SetHex("477e4441c36f0707de97f1d4505d093f5d0e63cb29b02c39a53bd9641bd5ac74");
    uint256 n3; n3.SetHex("2044a4b6796a3508103536075f77dad810a8fd1c6ce943e755c33f2e611f5509");

    CTxIn t1 = CTxIn(n1, 0);
    CTxIn t2 = CTxIn(n2, 0);
    CTxIn t3 = CTxIn(n3, 0);

    CMasterNode mn1(addr, t1, CPubKey(), vchSig, 0, CPubKey(), PROTOCOL_VERSION);
    darkSendMasterNodes.push_back(mn1);
    CMasterNode mn2(addr, t2, CPubKey(), vchSig, 0, CPubKey(), PROTOCOL_VERSION);
    darkSendMasterNodes.push_back(mn2);
    CMasterNode mn3(addr, t3, CPubKey(), vchSig, 0, CPubKey(), PROTOCOL_VERSION);
    darkSendMasterNodes.push_back(mn3);

    CMasternodePaymentWinner w1; w1.nBlockHeight = 100000; w1.vin = t1;
    CMasternodePaymentWinner w2; w2.nBlockHeight = 100000; w2.vin = t2;
    CMasternodePaymentWinner w3; w3.nBlockHeight = 100000; w3.vin = t3;

    CTxIn out;
    BOOST_CHECK( mnp.GetWinningMasternode(100000, out) == false);

    BOOST_CHECK( mnp.AddWinningMasternode(w1) == true);
    BOOST_CHECK( mnp.AddWinningMasternode(w1) == false); //shouldn't insert again

    // should be here now
    BOOST_CHECK( mnp.GetWinningMasternode(100000, out) == true);
    BOOST_CHECK( mnp.CalculateScore(100000, t1) == mnp.CalculateScore(100000, out)); // should match

    BOOST_CHECK( mnp.GetWinningMasternode(100001, out) == false);

    BOOST_CHECK( mnp.AddWinningMasternode(w2) == true); //wins
    BOOST_CHECK( mnp.AddWinningMasternode(w2) == false); 

    BOOST_CHECK( mnp.AddWinningMasternode(w3) == false); //loses
    BOOST_CHECK( mnp.AddWinningMasternode(w3) == false); 
}

BOOST_AUTO_TEST_CASE(darksend_sign)
{

    std::string errorMessage = "";
    std::string strBase64;
    CKey key;
    CPubKey pubkey;
    std::vector<unsigned char> vchSig;

    CDarkSendSigner dss;
    dss.SetKey("XDPugk3QgxVpQ4BubgzKaXhQudtaBnjuos9w6ZTojYx68EipNnt7", errorMessage, key, pubkey);
    BOOST_CHECK(dss.SignMessage("hello", errorMessage, vchSig, key) == true);
    BOOST_CHECK(dss.VerifyMessage(pubkey, vchSig, "hello", errorMessage) == true);
    BOOST_CHECK(dss.VerifyMessage(pubkey, vchSig, "hello2", errorMessage) == false);
}

BOOST_AUTO_TEST_CASE(darksend_vote)
{
    CPubKey key;
    CMasterNodeVote mnv;
    mnv.Set(key, 1);
    mnv.Vote();
    BOOST_CHECK(mnv.GetVotes() == 2);
    mnv.Vote();
    BOOST_CHECK(mnv.GetVotes() == 3);
}

BOOST_AUTO_TEST_CASE(set_collateral_address_bad)
{
	CDarkSendPool * dsp_ptr = new CDarkSendPool();

	string crappy = "badaddress";

	BOOST_CHECK( dsp_ptr->SetCollateralAddress(crappy) == false );
	delete dsp_ptr;
}

BOOST_AUTO_TEST_CASE(set_collateral_address_production)
{
	CDarkSendPool * dsp_ptr = new CDarkSendPool();

	string prod = "Xq19GqFvajRrEdDHYRKGYjTsQfpV5jyipF";

	BOOST_CHECK( dsp_ptr->SetCollateralAddress(prod) == true );
	delete dsp_ptr;
}

BOOST_AUTO_TEST_CASE(set_collateral_address_testnet)
{
	CDarkSendPool * dsp_ptr = new CDarkSendPool();

	string testnet = "mxE2Rp3oYpSEFdsN5TdHWhZvEHm3PJQQVm";

	BOOST_CHECK( dsp_ptr->SetCollateralAddress(testnet) == true );
	delete dsp_ptr;
}

BOOST_AUTO_TEST_CASE(darksend_denom)
{

    darkSendDenominations.push_back( (500   * COIN)+1 );
    darkSendDenominations.push_back( (100   * COIN)+1 );
    darkSendDenominations.push_back( (10    * COIN)+1 );
    darkSendDenominations.push_back( (1     * COIN)+1 );

    std::vector<CTxOut> vout1;
    std::vector<CTxOut> vout2;

    CTxOut d1 = CTxOut(); d1.nValue = (500   * COIN)+1;
    CTxOut d2 = CTxOut(); d2.nValue = (100   * COIN)+1;
    CTxOut d3 = CTxOut(); d3.nValue = (10   * COIN)+1;
    CTxOut d4 = CTxOut(); d4.nValue = (1   * COIN)+1;
    CTxOut d6 = CTxOut(); d6.nValue = 12.3 * COIN; //non-denom

    vout1.push_back(d1);
    vout1.push_back(d3);

    vout2.push_back(d1);
    vout2.push_back(d2);
    vout2.push_back(d3);

    BOOST_CHECK(darkSendPool.GetDenominations(vout1) != darkSendPool.GetDenominations(vout2));

    vout1.push_back(d2);

    BOOST_CHECK(darkSendPool.GetDenominations(vout1) == darkSendPool.GetDenominations(vout2));

    vout1.push_back(d2);

    BOOST_CHECK(darkSendPool.GetDenominations(vout1) == darkSendPool.GetDenominations(vout2));

    vout1.push_back(d4);

    BOOST_CHECK(darkSendPool.GetDenominations(vout1) != darkSendPool.GetDenominations(vout2));

    // check 2

    vout1.clear();
    vout2.clear();

    vout1.push_back(d1);
    vout1.push_back(d3);
    vout1.push_back(d2);

    vout2.push_back(d1);
    vout2.push_back(d2);
    vout2.push_back(d3);

    BOOST_CHECK(darkSendPool.GetDenominations(vout1) == darkSendPool.GetDenominations(vout2));

    // check 3

    vout1.clear();
    vout2.clear();

    vout1.push_back(d1);
    vout1.push_back(d3);
    vout1.push_back(d2);

    vout2.push_back(d3);
    vout2.push_back(d2);
    vout2.push_back(d1);

    BOOST_CHECK(darkSendPool.GetDenominations(vout1) == darkSendPool.GetDenominations(vout2));

    // check 4

    vout1.clear();
    vout2.clear();

    vout1.push_back(d1);
    vout1.push_back(d3);
    vout1.push_back(d2);

    vout2.push_back(d3);
    vout2.push_back(d2);
    vout2.push_back(d2);
    vout2.push_back(d2);
    vout2.push_back(d1);

    BOOST_CHECK(darkSendPool.GetDenominations(vout1) == darkSendPool.GetDenominations(vout2));

    vout2.push_back(d4);

    BOOST_CHECK(darkSendPool.GetDenominations(vout1) != darkSendPool.GetDenominations(vout2));


    // check 5

    vout1.clear();
    vout2.clear();

    vout1.push_back(d1);
    vout1.push_back(d3);
    vout1.push_back(d2);

    vout2.push_back(d3);
    vout2.push_back(d2);
    vout2.push_back(d2);
    vout2.push_back(d2);
    vout2.push_back(d1);

    BOOST_CHECK(darkSendPool.GetDenominations(vout1) == darkSendPool.GetDenominations(vout2));

    vout1.push_back(d6); //add a non-denom, should be different

    BOOST_CHECK(darkSendPool.GetDenominations(vout1) != darkSendPool.GetDenominations(vout2));

    vout2.push_back(d6); //should be the same now

    BOOST_CHECK(darkSendPool.GetDenominations(vout1) == darkSendPool.GetDenominations(vout2));

    vout2.push_back(d6); //multiples shouldn't affect this
    vout2.push_back(d6); 
    vout2.push_back(d6); 

    BOOST_CHECK(darkSendPool.GetDenominations(vout1) == darkSendPool.GetDenominations(vout2));
}

BOOST_AUTO_TEST_CASE(darksend_session)
{
    darkSendPool.unitTest = true;
    
    std::string strReason = "";
    BOOST_CHECK(darkSendPool.IsCompatibleWithSession(1, CTransaction(), strReason));
    BOOST_CHECK(darkSendPool.IsCompatibleWithSession(2, CTransaction(), strReason) == false);
    BOOST_CHECK(darkSendPool.IsCompatibleWithSession(3, CTransaction(), strReason) == false);
    BOOST_CHECK(darkSendPool.IsCompatibleWithSession(4, CTransaction(), strReason) == false);
    BOOST_CHECK(darkSendPool.IsCompatibleWithSession(5, CTransaction(), strReason) == false);
    BOOST_CHECK(darkSendPool.IsCompatibleWithSession(1, CTransaction(), strReason));
    if(darkSendPool.GetMaxPoolTransactions() >= 3) BOOST_CHECK(darkSendPool.IsCompatibleWithSession(1, CTransaction(), strReason));
    if(darkSendPool.GetMaxPoolTransactions() >= 4) BOOST_CHECK(darkSendPool.IsCompatibleWithSession(1, CTransaction(), strReason));
    if(darkSendPool.GetMaxPoolTransactions() >= 5) BOOST_CHECK(darkSendPool.IsCompatibleWithSession(1, CTransaction(), strReason));
    BOOST_CHECK(darkSendPool.IsCompatibleWithSession(531, CTransaction(), strReason) == false);

    darkSendPool.SetNull();

    BOOST_CHECK(darkSendPool.IsCompatibleWithSession(12, CTransaction(), strReason));
    BOOST_CHECK(darkSendPool.IsCompatibleWithSession(131, CTransaction(), strReason) == false);
    BOOST_CHECK(darkSendPool.IsCompatibleWithSession(151, CTransaction(), strReason) == false);
    BOOST_CHECK(darkSendPool.IsCompatibleWithSession(751, CTransaction(), strReason) == false);
    BOOST_CHECK(darkSendPool.IsCompatibleWithSession(12, CTransaction(), strReason));
    if(darkSendPool.GetMaxPoolTransactions() >= 3) BOOST_CHECK(darkSendPool.IsCompatibleWithSession(12, CTransaction(), strReason));
    if(darkSendPool.GetMaxPoolTransactions() >= 4) BOOST_CHECK(darkSendPool.IsCompatibleWithSession(12, CTransaction(), strReason));
    if(darkSendPool.GetMaxPoolTransactions() >= 5) BOOST_CHECK(darkSendPool.IsCompatibleWithSession(12, CTransaction(), strReason));
    BOOST_CHECK(darkSendPool.IsCompatibleWithSession(33, CTransaction(), strReason) == false);

}

BOOST_AUTO_TEST_CASE(darksend_masternode_search_by_vin)
{
    darkSendMasterNodes.clear();

    uint256 n1 = 10000;
    uint256 n2 = 10001;
    uint256 n3 = 99999;
    CTxIn testVin1 = CTxIn(n1, 0);
    CTxIn testVin2 = CTxIn(n2, 0);
    CTxIn testVinNotFound = CTxIn(n3, 0);
    CService addr;
    std::vector<unsigned char> vchSig;

    //setup a couple fake masternodes
    CMasterNode mn1(addr, testVin1, CPubKey(), vchSig, 0, CPubKey(), PROTOCOL_VERSION);
    darkSendMasterNodes.push_back(mn1);

    CMasterNode mn2(addr, testVin2, CPubKey(), vchSig, 0, CPubKey(), PROTOCOL_VERSION);
    darkSendMasterNodes.push_back(mn2);

    BOOST_CHECK(GetMasternodeByVin(testVinNotFound) == -1);
    BOOST_CHECK(GetMasternodeByVin(testVin1) == 0);
    BOOST_CHECK(GetMasternodeByVin(testVin2) == 1);
}

BOOST_AUTO_TEST_CASE(darksend_add_entry)
{
    std::vector<CTxIn> vin;
    vin.push_back( CTxIn(1000, 0) );
    vin.push_back( CTxIn(1001, 0) );

    std::vector<CTxOut> vout;
    vout.push_back( CTxOut(1, CScript()) );
    vout.push_back( CTxOut(1, CScript()) );

    //try added entries
    CDarkSendEntry e;
    BOOST_CHECK(e.sev.size() == 0);
    BOOST_CHECK(e.Add(vin, 1, CTransaction(), vout) == true);
    BOOST_CHECK(e.sev.size() == 2);
    BOOST_CHECK(e.Add(vin, 1, CTransaction(), vout) == false);
    BOOST_CHECK(e.sev.size() == 2);

    //sign one of the vin
    BOOST_CHECK(e.AddSig(CTxIn(1001, 0)) == true);
    BOOST_CHECK(e.AddSig(CTxIn(1001, 0)) == false);

    //sign non-existant
    BOOST_CHECK(e.AddSig(CTxIn(9999001, 0)) == false);

}

BOOST_AUTO_TEST_CASE(darksend_masternode_class)
{
    std::string strPubKey = "XpAy7r5RVdGLnnjWNKuB9EUDiJ5Tje9GZ8";
    CPubKey pubkey(ParseHex(strPubKey));

    std::vector<unsigned char> newSig;
    int64 newNow = GetTimeMicros();

    CMasterNode mn(CService("10.10.10.10:9999"), CTxIn(1000, 0), pubkey, newSig, newNow, pubkey, PROTOCOL_VERSION);
    mn.unitTest = true;
    mn.UpdateLastSeen();
    mn.Check();
    BOOST_CHECK(mn.enabled == 1); // ok
    mn.lastTimeSeen -= MASTERNODE_EXPIRATION_SECONDS;
    mn.Check();
    BOOST_CHECK(mn.enabled == 2); // hasn't pinged
    mn.lastTimeSeen -= MASTERNODE_EXPIRATION_SECONDS;
    mn.Check();
    BOOST_CHECK(mn.enabled == 4); // expired
}

BOOST_AUTO_TEST_CASE(darksend_pool_add_entry)
{
    std::vector<CTxIn> vin;
    vin.push_back( CTxIn(1000, 0) );
    vin.push_back( CTxIn(1001, 0) );

    std::vector<CTxOut> vout;
    vout.push_back( CTxOut(1, CScript()) );
    vout.push_back( CTxOut(1, CScript()) );

    //set to entries mode
    darkSendPool.state = POOL_STATUS_ACCEPTING_ENTRIES;

    //try added entries
    CDarkSendEntry e;
    BOOST_CHECK(e.Add(vin, 1, CTransaction(), vout) == true);

    // add first entry
    darkSendPool.entries.push_back(e);
    BOOST_CHECK(darkSendPool.state == POOL_STATUS_ACCEPTING_ENTRIES);
    BOOST_CHECK(darkSendPool.entries.size() == 1);
    darkSendPool.Check();
    BOOST_CHECK(darkSendPool.state == POOL_STATUS_ACCEPTING_ENTRIES);
    BOOST_CHECK(darkSendPool.entries.size() == 1);
    
    // add second entry
    darkSendPool.entries.push_back(e);
    BOOST_CHECK(darkSendPool.state == POOL_STATUS_ACCEPTING_ENTRIES);
    BOOST_CHECK(darkSendPool.entries.size() == 2);
    darkSendPool.Check();
    BOOST_CHECK(darkSendPool.state == POOL_STATUS_ACCEPTING_ENTRIES);
    BOOST_CHECK(darkSendPool.entries.size() == 2);

    // add third entry
    darkSendPool.entries.push_back(e);
    BOOST_CHECK(darkSendPool.state == POOL_STATUS_ACCEPTING_ENTRIES);
    BOOST_CHECK(darkSendPool.entries.size() == 3);
    darkSendPool.Check();
    BOOST_CHECK(darkSendPool.state == POOL_STATUS_SIGNING);
    BOOST_CHECK(darkSendPool.entries.size() == 3);

}

BOOST_AUTO_TEST_SUITE_END()
