// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "chainparams.h"

#include "random.h"
#include "util.h"
#include "utilstrencodings.h"

#include <assert.h>

#include <boost/assign/list_of.hpp>

using namespace std;
using namespace boost::assign;

struct SeedSpec6 {
    uint8_t addr[16];
    uint16_t port;
};

#include "chainparamsseeds.h"

/**
 * Main network
 */

//! Convert the pnSeeds6 array into usable address objects.
static void convertSeed6(std::vector<CAddress> &vSeedsOut, const SeedSpec6 *data, unsigned int count)
{
    // It'll only connect to one or two seed nodes because once it connects,
    // it'll get a pile of addresses with newer timestamps.
    // Seed nodes are given a random 'last seen time' of between one and two
    // weeks ago.
    const int64_t nOneWeek = 7*24*60*60;
    for (unsigned int i = 0; i < count; i++)
    {
        struct in6_addr ip;
        memcpy(&ip, data[i].addr, sizeof(ip));
        CAddress addr(CService(ip, data[i].port));
        addr.nTime = GetTime() - GetRand(nOneWeek) - nOneWeek;
        vSeedsOut.push_back(addr);
    }
}

/**
 * What makes a good checkpoint block?
 * + Is surrounded by blocks with reasonable timestamps
 *   (no blocks before with a timestamp after, none after with
 *    timestamp before)
 * + Contains no strange transactions
 */

static Checkpoints::MapCheckpoints mapCheckpoints =
        boost::assign::map_list_of
        (      0,	uint256("0x00000449ae58462bbad8d26c7eb0270d332948872cfe97b7d5c42c154bfa5523"))
        (   9950,	uint256("0x2bc193c75c0f967825ea78f1e92e57880040f011fa9343d321b34742563039d4"))
        (  25000,	uint256("0x2c3628b858553f96771ef8771bfc85e6738442e8e995927abe1287f4f45a53f8"))
        (  56000,	uint256("0x317c2f300b998e885a914e551270ec199aef14ade90d94c454685225b50d50d5"))
        (  76000,	uint256("0x5ae7590572ff76223dffaa04ef87c24e824bb3ea614a9f23b1f89c9acf259c08"))
        (  90001,	uint256("0x5c89cf3a408121b7850bfbbe01bd0b3cacf2927b3803b2e502bec0329f43af57"))
        ( 100014,	uint256("0x70050e7917796b8fe4974b226b8e5fbdb386174bb4904daf3465a94de5876443"))
        ( 120010,	uint256("0x74f3a0917a0ca006445df6753f1a80de6c05659b6e1ae51105659ce3b446f7ac"))
        ( 130542,	uint256("0x902fe4e1e0a51ccbf806bb79b582b90b415a7c3bca272b763488633fb04187d4"))
        ;
//TODO: Change params below:
static const Checkpoints::CCheckpointData data = {
        &mapCheckpoints,
        0,     // * UNIX timestamp of last checkpoint block
        0,     // * total number of transactions between genesis and last checkpoint
               //   (the tx=... number in the SetBestChain debug.log lines)
        0      // * estimated number of transactions per day after checkpoint
    };

static Checkpoints::MapCheckpoints mapCheckpointsTestnet =
        boost::assign::map_list_of
        (      0,	uint256("0x0"))
        ;
static const Checkpoints::CCheckpointData dataTestnet = {
        &mapCheckpointsTestnet,
        0,
        0,
        0
        };

static Checkpoints::MapCheckpoints mapCheckpointsRegtest =
        boost::assign::map_list_of
        (      0,	uint256("0x0"))
        ;
static const Checkpoints::CCheckpointData dataRegtest = {
        &mapCheckpointsRegtest,
        0,
        0,
        0
    };

class CMainParams : public CChainParams {
public:
    CMainParams() {
        networkID = CBaseChainParams::MAIN;
        strNetworkID = "main";
        /** 
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 4-byte int at any alignment.
         */
        pchMessageStart[0] = 0xb1;
        pchMessageStart[1] = 0xb5;
        pchMessageStart[2] = 0xa2;
        pchMessageStart[3] = 0xa4;
        //Need a new alert pub key.
        vAlertPubKey = ParseHex("0469983e0cc246fb426e7358f2aa29e09e4033c841455dc296f8f2dc99bb41fafda903f80f617ce9414aecb895d501cb5fb73c0bef8bb30ddab8e4a78f504dfd83");
        nDefaultPort = 28280;
        bnProofOfWorkLimit = ~uint256(0) >> 20;  // BTX starting difficulty is 1 / 2^12
        //TODO: What are these????????
        nSubsidyHalvingInterval = 210000;
        nEnforceBlockUpgradeMajority = 750;
        nRejectBlockOutdatedMajority = 950;
        nToCheckBlockUpgradeMajority = 1000;
        nMinerThreads = 0;
        nTargetTimespan = 24 * 60 * 60; // BTX: 1 day
        nTargetSpacing = 2.5 * 60; // BTX: 2.5 minutes
        //TODO: What are these????????
        /**
         * Build the genesis block. Note that the output of the genesis coinbase cannot
         * be spent as it did not originally exist in the database.
         * 
         * CBlock(hash=00000ffd590b14, ver=1, hashPrevBlock=00000000000000, hashMerkleRoot=e0028e, nTime=1390095618, nBits=1e0ffff0, nNonce=28917698, vtx=1)
         *   CTransaction(hash=e0028e, ver=1, vin.size=1, vout.size=1, nLockTime=0)
         *     CTxIn(COutPoint(000000, -1), coinbase 04ffff001d01044c5957697265642030392f4a616e2f3230313420546865204772616e64204578706572696d656e7420476f6573204c6976653a204f76657273746f636b2e636f6d204973204e6f7720416363657074696e6720426974636f696e73)
         *     CTxOut(nValue=50.00000000, scriptPubKey=0xA9037BAC7050C479B121CF)
         *   vMerkleTree: e0028e
         */
        const char* pszTimestamp = "BT, currency for people (and robots...and animals, maybe)";
        CMutableTransaction txNew;
        //txNew.nTime = 1430222400;
        txNew.vin.resize(1);
        txNew.vout.resize(1);
        txNew.vin[0].scriptSig = CScript() << 0 << 42 << vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
        txNew.vout[0].nValue = NULL;
        genesis.vtx.push_back(txNew);
        genesis.hashPrevBlock = 0;
        genesis.hashMerkleRoot = genesis.BuildMerkleTree();
        genesis.nVersion = 1;
        genesis.nTime    = 1430222400;
        genesis.nBits    = 0x1e0fffff;
        genesis.nNonce   = 171310;

        hashGenesisBlock = genesis.GetHash();
        assert(hashGenesisBlock == uint256("0x00000449ae58462bbad8d26c7eb0270d332948872cfe97b7d5c42c154bfa5523"));
        assert(genesis.hashMerkleRoot == uint256("0x543528ec7c8617e4816916806d3536b6a22f18907deae54c13c30d725b7d908f"));

        vSeeds.push_back(CDNSSeedData("btxcoin.com", "dnsseed.btxcoin.com"));
        vSeeds.push_back(CDNSSeedData("btxcoin.net", "dnsseed.btxcoin.net"));
        vSeeds.push_back(CDNSSeedData("bitcointx.io", "dnsseed.bitcointx.io"));
        vSeeds.push_back(CDNSSeedData("bitcointx.info", "dnsseed.bitcointx.info"));

        base58Prefixes[PUBKEY_ADDRESS] = list_of(35);
        base58Prefixes[SCRIPT_ADDRESS] = list_of(85);
        base58Prefixes[SECRET_KEY] =     list_of(153);
        base58Prefixes[EXT_PUBLIC_KEY] = list_of(0x04)(0x88)(0xB2)(0x1E);
        base58Prefixes[EXT_SECRET_KEY] = list_of(0x04)(0x88)(0xAD)(0xE4);      // BTX BIP44 coin type is '5'

        convertSeed6(vFixedSeeds, pnSeed6_main, ARRAYLEN(pnSeed6_main));
        //TODO: Trace this variable in original code.
        nLastPOWBlock = 2000;

        fRequireRPCPassword = true;
        fMiningRequiresPeers = true;
        fAllowMinDifficultyBlocks = false;
        fDefaultConsistencyChecks = false;
        fRequireStandard = true;
        fMineBlocksOnDemand = false;
        fSkipProofOfWorkCheck = false;
        fTestnetToBeDeprecatedFieldRPC = false;

        nPoolMaxTransactions = 3;
        strSporkKey = "0";
        strMasternodePaymentsPubKey = "0";
        strDarksendPoolDummyAddress = "New BTX address";
        nStartMasternodePayments = 1403728576; //Wed, 25 Jun 2014 20:36:16 GMT
    }

    const Checkpoints::CCheckpointData& Checkpoints() const 
    {
        return data;
    }
};
static CMainParams mainParams;

/**
 * Testnet (v3)
 */
class CTestNetParams : public CMainParams {
public:
    CTestNetParams() {
        networkID = CBaseChainParams::TESTNET;
        strNetworkID = "test";
        pchMessageStart[0] = 0xb2;
        pchMessageStart[1] = 0xa0;
        pchMessageStart[2] = 0xa2;
        pchMessageStart[3] = 0xa3;
        bnProofOfWorkLimit = ~uint256(0) >> 12;
        vAlertPubKey = ParseHex("04bce55edfa485b7f0efb41f56be9356cc597c570d2b9479b9ee0f1eb4bcd271ca8dc7b12289fa3b366d1868453544e369abfeb00b646aff8d61cfc8111e122c5e");
        nDefaultPort = 28281;
        nEnforceBlockUpgradeMajority = 51;
        nRejectBlockOutdatedMajority = 75;
        nToCheckBlockUpgradeMajority = 100;
        nMinerThreads = 0;
        nTargetTimespan = 24 * 60 * 60; // BTX: 1 day
        nTargetSpacing = 2.5 * 60; // BTX: 2.5 minutes

        //! Modify the testnet genesis block so the timestamp is valid for a later start.
        genesis.nTime = 1390666206;
        genesis.nNonce = 3861367235;
        genesis.nBits  = bnProofOfWorkLimit.GetCompact();
        genesis.nNonce = 219671;
        hashGenesisBlock = genesis.GetHash();
        assert(hashGenesisBlock == uint256("0xd3aa2697d4c3d92664895cac715a3f2529e377a36bdb22cff4d28ea11ec85796"));

        vFixedSeeds.clear();
        vSeeds.clear();
        /*vSeeds.push_back(CDNSSeedData("btxpay.io", "testnet-seed.btxpay.io"));
        vSeeds.push_back(CDNSSeedData("btx.qa", "testnet-seed.btx.qa"));
        *///legacy seeders

        base58Prefixes[PUBKEY_ADDRESS] = list_of(125);
        base58Prefixes[SCRIPT_ADDRESS] = list_of(196);
        base58Prefixes[SECRET_KEY]     = list_of(239);
        base58Prefixes[EXT_PUBLIC_KEY] = list_of(0x04)(0x35)(0x87)(0xCF);
        base58Prefixes[EXT_SECRET_KEY] = list_of(0x04)(0x35)(0x83)(0x94);

        // Testnet btx BIP44 coin type is '5' (All coin's testnet default)
        convertSeed6(vFixedSeeds, pnSeed6_test, ARRAYLEN(pnSeed6_test));

        fRequireRPCPassword = true;
        fMiningRequiresPeers = true;
        fAllowMinDifficultyBlocks = true;
        fDefaultConsistencyChecks = false;
        fRequireStandard = false;
        fMineBlocksOnDemand = false;
        fTestnetToBeDeprecatedFieldRPC = true;

        nPoolMaxTransactions = 2;
        strSporkKey = "0";
        strMasternodePaymentsPubKey = "0";
        strDarksendPoolDummyAddress = "new";
        nStartMasternodePayments = 1420837558; //Fri, 09 Jan 2015 21:05:58 GMT
    }
    const Checkpoints::CCheckpointData& Checkpoints() const 
    {
        return dataTestnet;
    }
};
static CTestNetParams testNetParams;

/**
 * Regression test
 */
class CRegTestParams : public CTestNetParams {
public:
    CRegTestParams() {
        networkID = CBaseChainParams::REGTEST;
        strNetworkID = "regtest";
        pchMessageStart[0] = 0xcc;
        pchMessageStart[1] = 0xcc;
        pchMessageStart[2] = 0xcc;
        pchMessageStart[3] = 0xcc;
        nSubsidyHalvingInterval = 150;
        nEnforceBlockUpgradeMajority = 750;
        nRejectBlockOutdatedMajority = 950;
        nToCheckBlockUpgradeMajority = 1000;
        nMinerThreads = 1;
        nTargetTimespan = 24 * 60 * 60; // BTX: 1 day
        nTargetSpacing = 2.5 * 60; // BTX: 2.5 minutes
        bnProofOfWorkLimit = ~uint256(0) >> 1;
        genesis.nTime = 1417713337;
        genesis.nBits = 0x207fffff;
        genesis.nNonce = 1096447;
        hashGenesisBlock = genesis.GetHash();
        nDefaultPort = 19994;
        assert(hashGenesisBlock == uint256("0x000008ca1832a4baf228eb1553c03d3a2c8e02399550dd6ea8d65cec3ef23d2e"));

        vFixedSeeds.clear(); //! Regtest mode doesn't have any fixed seeds.
        vSeeds.clear();  //! Regtest mode doesn't have any DNS seeds.

        fRequireRPCPassword = false;
        fMiningRequiresPeers = false;
        fAllowMinDifficultyBlocks = true;
        fDefaultConsistencyChecks = true;
        fRequireStandard = false;
        fMineBlocksOnDemand = true;
        fTestnetToBeDeprecatedFieldRPC = false;
    }
    const Checkpoints::CCheckpointData& Checkpoints() const 
    {
        return dataRegtest;
    }
};
static CRegTestParams regTestParams;

/**
 * Unit test
 */
class CUnitTestParams : public CMainParams, public CModifiableParams {
public:
    CUnitTestParams() {
        networkID = CBaseChainParams::UNITTEST;
        strNetworkID = "unittest";
        nDefaultPort = 28445;
        vFixedSeeds.clear(); //! Unit test mode doesn't have any fixed seeds.
        vSeeds.clear();  //! Unit test mode doesn't have any DNS seeds.

        fRequireRPCPassword = false;
        fMiningRequiresPeers = false;
        fDefaultConsistencyChecks = true;
        fAllowMinDifficultyBlocks = false;
        fMineBlocksOnDemand = true;
    }

    const Checkpoints::CCheckpointData& Checkpoints() const 
    {
        // UnitTest share the same checkpoints as MAIN
        return data;
    }

    //! Published setters to allow changing values in unit test cases
    virtual void setSubsidyHalvingInterval(int anSubsidyHalvingInterval)  { nSubsidyHalvingInterval=anSubsidyHalvingInterval; }
    virtual void setEnforceBlockUpgradeMajority(int anEnforceBlockUpgradeMajority)  { nEnforceBlockUpgradeMajority=anEnforceBlockUpgradeMajority; }
    virtual void setRejectBlockOutdatedMajority(int anRejectBlockOutdatedMajority)  { nRejectBlockOutdatedMajority=anRejectBlockOutdatedMajority; }
    virtual void setToCheckBlockUpgradeMajority(int anToCheckBlockUpgradeMajority)  { nToCheckBlockUpgradeMajority=anToCheckBlockUpgradeMajority; }
    virtual void setDefaultConsistencyChecks(bool afDefaultConsistencyChecks)  { fDefaultConsistencyChecks=afDefaultConsistencyChecks; }
    virtual void setAllowMinDifficultyBlocks(bool afAllowMinDifficultyBlocks) {  fAllowMinDifficultyBlocks=afAllowMinDifficultyBlocks; }
    virtual void setSkipProofOfWorkCheck(bool afSkipProofOfWorkCheck) { fSkipProofOfWorkCheck = afSkipProofOfWorkCheck; }
};
static CUnitTestParams unitTestParams;


static CChainParams *pCurrentParams = 0;

CModifiableParams *ModifiableParams()
{
   assert(pCurrentParams);
   assert(pCurrentParams==&unitTestParams);
   return (CModifiableParams*)&unitTestParams;
}

const CChainParams &Params() {
    assert(pCurrentParams);
    return *pCurrentParams;
}

CChainParams &Params(CBaseChainParams::Network network) {
    switch (network) {
        case CBaseChainParams::MAIN:
            return mainParams;
        case CBaseChainParams::TESTNET:
            return testNetParams;
        case CBaseChainParams::REGTEST:
            return regTestParams;
        case CBaseChainParams::UNITTEST:
            return unitTestParams;
        default:
            assert(false && "Unimplemented network");
            return mainParams;
    }
}

void SelectParams(CBaseChainParams::Network network) {
    SelectBaseParams(network);
    pCurrentParams = &Params(network);
}

bool SelectParamsFromCommandLine()
{
    CBaseChainParams::Network network = NetworkIdFromCommandLine();
    if (network == CBaseChainParams::MAX_NETWORK_TYPES)
        return false;

    SelectParams(network);
    return true;
}
