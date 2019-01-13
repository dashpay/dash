// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Copyright (c) 2014-2017 The Waggox Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "chainparams.h"
#include "consensus/merkle.h"

#include "tinyformat.h"
#include "util.h"
#include "utilstrencodings.h"

#include "arith_uint256.h"

#include <assert.h>

#include <boost/assign/list_of.hpp>

#include "chainparamsseeds.h"

static CBlock CreateGenesisBlock(const char* pszTimestamp, const CScript& genesisOutputScript, uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
        CMutableTransaction txNew;
        txNew.nVersion = 1;
        txNew.vin.resize(1);
        txNew.vout.resize(1);
        txNew.vin[0].scriptSig = CScript() << 486604799 << CScriptNum(4) << std::vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
        txNew.vout[0].nValue = genesisReward;
        txNew.vout[0].scriptPubKey = genesisOutputScript;

        CBlock genesis;
        genesis.nTime    = nTime;
        genesis.nBits    = nBits;
        genesis.nNonce   = nNonce;
        genesis.nVersion = nVersion;
        genesis.vtx.push_back(MakeTransactionRef(std::move(txNew)));
        genesis.hashPrevBlock.SetNull();
        genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
        return genesis;
}

static CBlock CreateDevNetGenesisBlock(const uint256 &prevBlockHash, const std::string& devNetName, uint32_t nTime, uint32_t nNonce, uint32_t nBits, const CAmount& genesisReward)
{
        assert(!devNetName.empty());

        CMutableTransaction txNew;
        txNew.nVersion = 1;
        txNew.vin.resize(1);
        txNew.vout.resize(1);
        // put height (BIP34) and devnet name into coinbase
        txNew.vin[0].scriptSig = CScript() << 1 << std::vector<unsigned char>(devNetName.begin(), devNetName.end());
        txNew.vout[0].nValue = genesisReward;
        txNew.vout[0].scriptPubKey = CScript() << OP_RETURN;

        CBlock genesis;
        genesis.nTime    = nTime;
        genesis.nBits    = nBits;
        genesis.nNonce   = nNonce;
        genesis.nVersion = 4;
        genesis.vtx.push_back(MakeTransactionRef(std::move(txNew)));
        genesis.hashPrevBlock = prevBlockHash;
        genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
        return genesis;
}

/**
 * Build the genesis block. Note that the output of its generation
 * transaction cannot be spent since it did not originally exist in the
 * database.
 *
 * CBlock(hash=00000ffd590b14, ver=1, hashPrevBlock=00000000000000, hashMerkleRoot=e0028e, nTime=1390095618, nBits=1e0ffff0, nNonce=28917698, vtx=1)
 *   CTransaction(hash=e0028e, ver=1, vin.size=1, vout.size=1, nLockTime=0)
 *     CTxIn(COutPoint(000000, -1), coinbase 04ffff001d01044c5957697265642030392f4a616e2f3230313420546865204772616e64204578706572696d656e7420476f6573204c6976653a204f76657273746f636b2e636f6d204973204e6f7720416363657074696e6720426974636f696e73)
 *     CTxOut(nValue=50.00000000, scriptPubKey=0xA9037BAC7050C479B121CF)
 *   vMerkleTree: e0028e
 */
static CBlock CreateGenesisBlock(uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    const char* pszTimestamp = "Waggox coin is create today 20th Decemebr 2018";
    const CScript genesisOutputScript = CScript() << ParseHex("04e60ea9519f2be2ea861c158d46065969bea7482aec8868b25278"
                                                              "8bceaaed6ddd79f681ddba7739085620dd01647174ba701d33248"
                                                              "bb8298395d2e0d491c06904") << OP_CHECKSIG;
        return CreateGenesisBlock(pszTimestamp, genesisOutputScript, nTime, nNonce, nBits, nVersion, genesisReward);
}

static CBlock FindDevNetGenesisBlock(const Consensus::Params& params, const CBlock &prevBlock, const CAmount& reward)
{
        std::string devNetName = GetDevNetName();
        assert(!devNetName.empty());

        CBlock block = CreateDevNetGenesisBlock(prevBlock.GetHash(), devNetName.c_str(), prevBlock.nTime + 1, 0, prevBlock.nBits, reward);

        arith_uint256 bnTarget;
        bnTarget.SetCompact(block.nBits);

        for (uint32_t nNonce = 0; nNonce < UINT32_MAX; nNonce++) {
                block.nNonce = nNonce;

                uint256 hash = block.GetHash();
                if (UintToArith256(hash) <= bnTarget)
                        return block;
        }

        // This is very unlikely to happen as we start the devnet with a very low difficulty. In many cases even the first
        // iteration of the above loop will give a result already
        error("FindDevNetGenesisBlock: could not find devnet genesis block for %s", devNetName);
        assert(false);
}

/**
 * Main network
 */
/**
 * What makes a good checkpoint block?
 * + Is surrounded by blocks with reasonable timestamps
 *   (no blocks before with a timestamp after, none after with
 *    timestamp before)
 * + Contains no strange transactions
 */


class CMainParams : public CChainParams {
public:
    CMainParams() {
            strNetworkID = "main";
                checkpointData = (CCheckpointData) {
            boost::assign::map_list_of
            (  0, uint256S("0x00000a2cb844c2d8286fb7fbc0cff0ddb03baca18346d9102f2545ab0ad47509"))
            (  1, uint256S("0x00000b72e24b316d111663025ab1fc836ff58a7ed6c7b2d9178679d51726536c"))
            (  30, uint256S("0x000001874e3286d5cd4eef2835a14e555e80d493b7b49f57607a0cb929f1e575"))
            (  50, uint256S("0x00000d9947e4ae1d6f20c32e86c5d740120f5def8f0e45dcc470de19548353cd"))
            ( 100, uint256S("0x00000739ae2d975bfb8f0bfcbd8d2d5a8a9f9bab097d262fdf7dfc44d774ca92"))
            ( 111, uint256S("0x000005c50bca55209cb5698ec845f0b1ca32e72dcdcf3f82ca5015422b000601"))  
        };
    chainTxData = ChainTxData{
            1545317100,
            112,    
            0.99107    
        };
             
            consensus.nSubsidyHalvingInterval = 210240; 
            consensus.nMasternodePaymentsStartBlock = 1; 
            consensus.nMasternodePaymentsIncreaseBlock = 0; 
            consensus.nMasternodePaymentsIncreasePeriod = 0; 
            consensus.nInstantSendConfirmationsRequired = 6;
            consensus.nInstantSendKeepLock = 24;
            consensus.nBudgetPaymentsStartBlock = 1; // actual historical value
            consensus.nBudgetPaymentsCycleBlocks = 10; // ~(60*24*30)/2.6, actual number of blocks per month is 200700 / 12 = 16725
            consensus.nBudgetPaymentsWindowBlocks = 100;
            consensus.nSuperblockStartBlock = 1; // The block at which 12.1 goes live (end of final 12.0 budget cycle)
            consensus.nSuperblockCycle = 1; // ~(60*24*30)/2.6, actual number of blocks per month is 200700 / 12 = 16725
            consensus.nSuperblockStartHash = uint256S("00000b72e24b316d111663025ab1fc836ff58a7ed6c7b2d9178679d51726536c");
            consensus.nGovernanceMinQuorum = 10;
            consensus.nGovernanceFilterElements = 20000;
            consensus.nMasternodeMinimumConfirmations = 15;
            consensus.BIP34Height = 1;
            consensus.BIP34Hash = uint256S("0x00000b72e24b316d111663025ab1fc836ff58a7ed6c7b2d9178679d51726536c");
            consensus.BIP65Height = 300;  // 00000000000076d8fcea02ec0963de4abfd01e771fec0863f960c2c64fe6f357
            consensus.BIP66Height = 200; // 00000000000b1fa2dfa312863570e13fae9ca7b5566cb27e55422620b469aefa
            consensus.DIP0001Height = 1000;
            consensus.powLimit = uint256S("00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
            consensus.nPowTargetTimespan = 24 * 60 * 60; // waggox: 1 day
            consensus.nPowTargetSpacing = 2.5 * 60 ; // waggox: 2.5 minutes
            consensus.fPowAllowMinDifficultyBlocks = false;
            consensus.fPowNoRetargeting = false;
            consensus.nPowKGWHeight = 260;
            consensus.nPowDGWHeight = 398;
            consensus.nUpdateDiffAlgoHeight = 0;
            strSporkAddress = "XtjGb5TrRGKSg92tbeuVXcHvuenu29A8a2";
            fMiningRequiresPeers = true;
            fDefaultConsistencyChecks = false;
            fRequireStandard = true;
            fMineBlocksOnDemand = false;
            fAllowMultipleAddressesFromGroup = false;
            fAllowMultiplePorts = false;
            nPoolMaxTransactions = 3;
            nFulfilledRequestExpireTime = 60*60; 
            consensus.nPosTargetSpacing = 2 * 60; // PoSW: 2 minutes
            consensus.nPosTargetTimespan = 60 * 40; // 40 minutes at max for difficulty adjustment 40 mins
            consensus.nStakeMinAge = 60 * 2;
            consensus.nStakeMaxAge = 60 * 60 * 24; // one day
            consensus.nWSTargetDiff = 0x1e0ffff0; // Genesis Difficulty
            consensus.nPoSDiffAdjustRange = 5;
            consensus.nLastPoWBlock = 50000;

            consensus.nRuleChangeActivationThreshold = 1916; // 95% of 2016
            consensus.nMinerConfirmationWindow = 2016; // nPowTargetTimespan / nPowTargetSpacing
            consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
            consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 1199145601; // January 1, 2008
            consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = 1230767999; // December 31, 2008

            // Deployment of BIP68, BIP112, and BIP113.
            consensus.vDeployments[Consensus::DEPLOYMENT_CSV].bit = 0;
            consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nStartTime = 1544157821;
            consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nTimeout = 1575693819;

            // Deployment of DIP0001
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].bit = 1;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].nStartTime = 1544157821;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].nTimeout = 1575693819;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].nWindowSize = 4032;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].nThreshold = 3226; // 80% of 4032

            // Deployment of BIP147
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].bit = 2;
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nStartTime = 1544157821;
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nTimeout = 1575693819;
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nWindowSize = 4032;
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nThreshold = 3226; // 80% of 4032

            // The best chain should have at least this much work.
            consensus.nMinimumChainWork = uint256S("0x0000000000000000000000000000000000000000000000000000000007000700"); //
            // By default assume that the signatures in ancestors of this block are valid.
            consensus.defaultAssumeValid = uint256S("0x000005c50bca55209cb5698ec845f0b1ca32e72dcdcf3f82ca5015422b000601"); // 
            /**
             * The message start string is designed to be unlikely to occur in normal data.
             * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
             * a large 32-bit integer with any alignment.
             */
            pchMessageStart[0] = 0xa6;
            pchMessageStart[1] = 0xa0;
            pchMessageStart[2] = 0x92;
            pchMessageStart[3] = 0x9e;
            vAlertPubKey = ParseHex("04c3b213d0a75f32352f0efaf6c72264b855ae4d09acb5452ae8ed28218fcd7"
                                "3cfb572567780a4588062b6cc93588ff634d4eba06784addc7fe1c78c0ef68"
                                "3c2d7");
            nDefaultPort = 9910;
            nPruneAfterHeight = 100000;

            genesis = CreateGenesisBlock(1545271656, 229965, 0x1e0ffff0, 1, 50 * COIN);
            consensus.hashGenesisBlock = genesis.GetHash();
            assert(consensus.hashGenesisBlock == uint256S("0x00000a2cb844c2d8286fb7fbc0cff0ddb03baca18346d9102f2545ab0ad47509"));
            assert(genesis.hashMerkleRoot == uint256S("0xfcf0f3bdb11ea76705b0dd045a643f3b81f778924e01d7773fc2b93219d4014a"));

            vSeeds.push_back(CDNSSeedData("waggok.tech", "dnsseed1.waggok.tech"));
            vSeeds.push_back(CDNSSeedData("waggok.tech", "dnsseed.waggok.tech"));
            vSeeds.push_back(CDNSSeedData("93.186.254.229", "93.186.254.229"));
            vSeeds.push_back(CDNSSeedData("80.211.1.199", "80.211.1.199"));
            vSeeds.push_back(CDNSSeedData("89.46.65.250", "89.46.65.250"));
            vSeeds.push_back(CDNSSeedData("80.211.7.49", "80.211.7.49"));


            // Waggox addresses start with 'X'
            base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,76);
            // Waggox script addresses start with '7'
            base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,16);
            // Waggox private keys start with '7' or 'X'
            base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,204);
            // Waggox BIP32 pubkeys start with 'xpub' (Bitcoin defaults)
            base58Prefixes[EXT_PUBLIC_KEY] = boost::assign::list_of(0x04)(0x88)(0xB2)(0x1E).convert_to_container<std::vector<unsigned char> >();
            // Waggox BIP32 prvkeys start with 'xprv' (Bitcoin defaults)
            base58Prefixes[EXT_SECRET_KEY] = boost::assign::list_of(0x04)(0x88)(0xAD)(0xE4).convert_to_container<std::vector<unsigned char> >();
            // Waggox BIP44 coin type is '5'
            nExtCoinType = 5;
            vFixedSeeds = std::vector<SeedSpec6>(pnSeed6_main, pnSeed6_main + ARRAYLEN(pnSeed6_main));




    }
};
static CMainParams mainParams;

/**
 * Testnet (v3)
 */
class CTestNetParams : public CChainParams {
public:
    CTestNetParams() {
            strNetworkID = "test";
                    checkpointData = (CCheckpointData) {
            boost::assign::map_list_of
            (    0, uint256S("0x0000059b354e61353e4d0c95c3a2607a2c2cf3225429404d66863d72453c2f19"))

        };
        chainTxData = ChainTxData{        
            1544267395, 
            0,    
                        
            0      
        };
            consensus.nSubsidyHalvingInterval = 210240;
            consensus.nMasternodePaymentsStartBlock = 4010; // not true, but it's ok as long as it's less then nMasternodePaymentsIncreaseBlock
            consensus.nMasternodePaymentsIncreaseBlock = 4030;
            consensus.nMasternodePaymentsIncreasePeriod = 10;
            consensus.nInstantSendConfirmationsRequired = 2;
            consensus.nInstantSendKeepLock = 6;
            consensus.nBudgetPaymentsStartBlock = 46;
            consensus.nBudgetPaymentsCycleBlocks = 24;
            consensus.nBudgetPaymentsWindowBlocks = 10;
            consensus.nSuperblockStartBlock = 3050; // NOTE: Should satisfy nSuperblockStartBlock > nBudgetPaymentsStartBlock
            consensus.nSuperblockCycle = 24; // Superblocks can be issued hourly on testnet
            consensus.nGovernanceMinQuorum = 1;
            consensus.nGovernanceFilterElements = 500;
            consensus.nMasternodeMinimumConfirmations = 1;
            consensus.BIP34Height = 76;
            consensus.BIP34Hash = uint256S("0x000008ebb1db2598e897d17275285767717c6acfeac4c73def49fbea1ddcbcb6");
            consensus.BIP65Height = 2431; // 0000039cf01242c7f921dcb4806a5994bc003b48c1973ae0c89b67809c2bb2ab
            consensus.BIP66Height = 2075; // 0000002acdd29a14583540cb72e1c5cc83783560e38fa7081495d474fe1671f7
            consensus.DIP0001Height = 5500;
            consensus.powLimit = uint256S("00000fffff000000000000000000000000000000000000000000000000000000");
            consensus.nPowTargetTimespan = 60 * 60 * 24; // waggox: 1 day
            consensus.nPowTargetSpacing = 2 * 60; // waggox: 2 minutes
            consensus.fPowAllowMinDifficultyBlocks = true;
            consensus.fPowNoRetargeting = false;
            consensus.nPowKGWHeight = 4001; // nPowKGWHeight >= nPowDGWHeight means "no KGW"
            consensus.nPowDGWHeight = 4001;
            fMiningRequiresPeers = true;
            fDefaultConsistencyChecks = false;
            fRequireStandard = false;
            fMineBlocksOnDemand = false;
            fAllowMultipleAddressesFromGroup = false;
            fAllowMultiplePorts = false;
            nPoolMaxTransactions = 3;
            nFulfilledRequestExpireTime = 5*60; 
            strSporkAddress = "yjPtiKh2uwk3bDutTEA2q9mCtXyiZRWn65";
            // Stake info
            consensus.nPosTargetSpacing = 2 * 60; // PoSW: 2 minutes
            consensus.nPosTargetTimespan = 60 * 40;
            consensus.nStakeMinAge = 60; //one minute
            consensus.nStakeMaxAge = 60 * 60 * 24; // one day
            consensus.nLastPoWBlock = 650;
            consensus.nPoSDiffAdjustRange = 1;
            // highest difficulty | 0x1e0ffff0 (?)
            // smallest difficulty | 0x008000
            consensus.nWSTargetDiff = 0x1e0ffff0; // Genesis Difficulty

            consensus.nRuleChangeActivationThreshold = 1512; // 75% for testchains
            consensus.nMinerConfirmationWindow = 2016; // nPowTargetTimespan / nPowTargetSpacing
            consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
            consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 1199145601; // January 1, 2008
            consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = 1230767999; // December 31, 2008

            // Deployment of BIP68, BIP112, and BIP113.
            consensus.vDeployments[Consensus::DEPLOYMENT_CSV].bit = 0;
            consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nStartTime = 1506556800; // September 28th, 2017
            consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nTimeout = 1538092800; // September 28th, 2018

            // Deployment of DIP0001
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].bit = 1;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].nStartTime = 1505692800; // Sep 18th, 2017
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].nTimeout = 1537228800; // Sep 18th, 2018
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].nWindowSize = 100;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].nThreshold = 50; // 50% of 100

            // Deployment of BIP147
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].bit = 2;
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nStartTime = 1517792400; // Feb 5th, 2018
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nTimeout = 1549328400; // Feb 5th, 2019
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nWindowSize = 100;
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nThreshold = 50; // 50% of 100

            // The best chain should have at least this much work.
            consensus.nMinimumChainWork = uint256S("0x"); // 37900
            // By default assume that the signatures in ancestors of this block are valid.
            consensus.defaultAssumeValid = uint256S("0x"); // 37900


          
        pchMessageStart[0] = 0xe4;
        pchMessageStart[1] = 0xc9;
        pchMessageStart[2] = 0xac;
        pchMessageStart[3] = 0x91;
        vAlertPubKey = ParseHex("04e60ea9519f2be2ea861c158d46065969bea7482aec8868b252788bceaa"
                                "ed6ddd79f681ddba7739085620dd01647174ba701d33248bb8298395d2e" 
                                "0d491c06904");
        nDefaultPort = 9911;
        nPruneAfterHeight = 1000;

        genesis = CreateGenesisBlock(1545277363, 477882, 0x1e0ffff0, 1, 50 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("0x0000059b354e61353e4d0c95c3a2607a2c2cf3225429404d66863d72453c2f19"));
        assert(genesis.hashMerkleRoot == uint256S("0xfcf0f3bdb11ea76705b0dd045a643f3b81f778924e01d7773fc2b93219d4014a"));

        vFixedSeeds.clear();
        vSeeds.clear();
        // nodes with support for servicebits filtering should be at the top
        vSeeds.push_back(CDNSSeedData("testnet1.xyz",  "testnet-seed.website.waggok.tech"));
        vSeeds.push_back(CDNSSeedData("waggoxcoin.xyz.io", "test.dnsseed.waggoxcoin.xyz"));

        // Testnet Waggox addresses start with 'y'
        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,140);
        // Testnet Waggox script addresses start with '8' or '9'
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,19);
        // Testnet private keys start with '9' or 'c' (Bitcoin defaults)
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        // Testnet Waggox BIP32 pubkeys start with 'tpub' (Bitcoin defaults)
        base58Prefixes[EXT_PUBLIC_KEY] = boost::assign::list_of(0x04)(0x35)(0x87)(0xCF).convert_to_container<std::vector<unsigned char> >();
        // Testnet Waggox BIP32 prvkeys start with 'tprv' (Bitcoin defaults)
        base58Prefixes[EXT_SECRET_KEY] = boost::assign::list_of(0x04)(0x35)(0x83)(0x94).convert_to_container<std::vector<unsigned char> >();

        // Testnet Waggox BIP44 coin type is '1' (All coin's testnet default)
        nExtCoinType = 1;

    }
};
static CTestNetParams testNetParams;

/**
 * Devnet
 */
class CDevNetParams : public CChainParams {
public:
    CDevNetParams() {
            strNetworkID = "dev";
                    checkpointData = (CCheckpointData) {
            boost::assign::map_list_of
            (      0, uint256S("0x00000d3afc6c78f97bbff2b80a77c20533c2c37cbddea1300b7c34eae0c81beb"))
            (      1, devnetGenesis.GetHash())
        };

        chainTxData = ChainTxData{
            devnetGenesis.GetBlockTime(), 
            2,                           
            0.01                         
   };
            consensus.nSubsidyHalvingInterval = 210240;
            consensus.nMasternodePaymentsStartBlock = 4010; // not true, but it's ok as long as it's less then nMasternodePaymentsIncreaseBlock
            consensus.nMasternodePaymentsIncreaseBlock = 4030;
            consensus.nMasternodePaymentsIncreasePeriod = 10;
            consensus.nInstantSendConfirmationsRequired = 2;
            consensus.nInstantSendKeepLock = 6;
            consensus.nBudgetPaymentsStartBlock = 4100;
            consensus.nBudgetPaymentsCycleBlocks = 50;
            consensus.nBudgetPaymentsWindowBlocks = 10;
            consensus.nSuperblockStartBlock = 4200; // NOTE: Should satisfy nSuperblockStartBlock > nBudgetPeymentsStartBlock
            consensus.nSuperblockStartHash = uint256(); // do not check this on devnet
            consensus.nSuperblockCycle = 24; // Superblocks can be issued hourly on devnet
            consensus.nGovernanceMinQuorum = 1;
            consensus.nGovernanceFilterElements = 500;
            consensus.nMasternodeMinimumConfirmations = 1;
            consensus.BIP34Height = 1; // BIP34 activated immediately on devnet
            consensus.BIP65Height = 1; // BIP65 activated immediately on devnet
            consensus.BIP66Height = 1; // BIP66 activated immediately on devnet
            consensus.DIP0001Height = 2; // DIP0001 activated immediately on devnet
            consensus.powLimit = uint256S("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"); // ~uint256(0) >> 1
            consensus.nPowTargetTimespan = 24 * 60 * 60; // Waggox: 1 day
            consensus.nPowTargetSpacing = 2.5 * 60; // Waggox: 2.5 minutes
            consensus.fPowAllowMinDifficultyBlocks = true;
            consensus.fPowNoRetargeting = false;
            consensus.nPowKGWHeight = 4001; // nPowKGWHeight >= nPowDGWHeight means "no KGW"
            consensus.nPowDGWHeight = 4001;
            strSporkAddress = "yjPtiKh2uwk3bDutTEA2q9mCtXyiZRWn55";
            fMiningRequiresPeers = true;
            fDefaultConsistencyChecks = false;
            fRequireStandard = false;
            fMineBlocksOnDemand = false;
            fAllowMultipleAddressesFromGroup = true;
            fAllowMultiplePorts = true;
            nPoolMaxTransactions = 3;
            nFulfilledRequestExpireTime = 5*60; 

            // Stake information

            consensus.nPosTargetSpacing = 2 * 60; // PoSW: 1 minutes
            consensus.nPosTargetTimespan = 60 * 40;
            consensus.nStakeMinAge = 60 * 60;
            consensus.nStakeMaxAge = 60 * 60 * 24; // one day
            consensus.nLastPoWBlock = 180675;
            consensus.nRuleChangeActivationThreshold = 1512; // 75% for testchains
            consensus.nMinerConfirmationWindow = 2016; // nPowTargetTimespan / nPowTargetSpacing
            consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
            consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 1199145601; // January 1, 2008
            consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = 1230767999; // December 31, 2008

            // Deployment of BIP68, BIP112, and BIP113.
            consensus.vDeployments[Consensus::DEPLOYMENT_CSV].bit = 0;
            consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nStartTime = 1506556800; // September 28th, 2017
            consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nTimeout = 1538092800; // September 28th, 2018

            // Deployment of DIP0001
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].bit = 1;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].nStartTime = 1505692800; // Sep 18th, 2017
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].nTimeout = 1537228800; // Sep 18th, 2018
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].nWindowSize = 100;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].nThreshold = 50; // 50% of 100

            // Deployment of BIP147
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].bit = 2;
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nStartTime = 1517792400; // Feb 5th, 2018
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nTimeout = 1549328400; // Feb 5th, 2019
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nWindowSize = 100;
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nThreshold = 50; // 50% of 100

            // The best chain should have at least this much work.
            consensus.nMinimumChainWork = uint256S("0x000000000000000000000000000000000000000000000000000000000000000");

            // By default assume that the signatures in ancestors of this block are valid.
            consensus.defaultAssumeValid = uint256S("0x000000000000000000000000000000000000000000000000000000000000000");

            pchMessageStart[0] = 0xbd;
        pchMessageStart[1] = 0x9b;
        pchMessageStart[2] = 0xa2;
        pchMessageStart[3] = 0x9a;
        vAlertPubKey = ParseHex("04e60ea9519f2be2ea861c158d46065969bea7482aec8868b252788bceaaed6"
                                "ddd79f681ddba7739085620dd01647174ba701d33248bb8298395d2e0d491c"
                                "06904");
        nDefaultPort = 9911;
        nPruneAfterHeight = 1000;

        genesis = CreateGenesisBlock(1544267395, 816880, 0x207fffff, 1, 50 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("0x00000d3afc6c78f97bbff2b80a77c20533c2c37cbddea1300b7c34eae0c81beb"));
        assert(genesis.hashMerkleRoot == uint256S("0x0f5fd5f0d3afd9624d349d8abfb40c1fe0c210c0232233ceeaf010af94a88ad2"));

        devnetGenesis = FindDevNetGenesisBlock(consensus, genesis, 50 * COIN);
        consensus.hashDevnetGenesisBlock = devnetGenesis.GetHash();

        vFixedSeeds.clear();
        vSeeds.clear();
        //vSeeds.push_back(CDNSSeedData("Waggoxevo.org",  "devnet-seed.Waggox.xyz"));

        // Testnet Waggox addresses start with 'y'
        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,140);
        // Testnet Waggox script addresses start with '8' or '9'
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,19);
        // Testnet private keys start with '9' or 'c' (Bitcoin defaults)
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        // Testnet Waggox BIP32 pubkeys start with 'tpub' (Bitcoin defaults)
        base58Prefixes[EXT_PUBLIC_KEY] = boost::assign::list_of(0x04)(0x35)(0x87)(0xCF).convert_to_container<std::vector<unsigned char> >();
        // Testnet Waggox BIP32 prvkeys start with 'tprv' (Bitcoin defaults)
        base58Prefixes[EXT_SECRET_KEY] = boost::assign::list_of(0x04)(0x35)(0x83)(0x94).convert_to_container<std::vector<unsigned char> >();

        // Testnet Waggox BIP44 coin type is '1' (All coin's testnet default)
        nExtCoinType = 1;


    }
};
static CDevNetParams *devNetParams;


/**
 * Regression test
 */
class CRegTestParams : public CChainParams {
public:
    CRegTestParams() {
            strNetworkID = "regtest";
        checkpointData = (CCheckpointData){
            boost::assign::map_list_of
            ( 0, uint256S("0x2fc7c5821ba86a701b1b38c19de1b7eb329b84ea3e0d5fc1374e017e96b62e95"))
        };

        chainTxData = ChainTxData{
            0,
            0,
            0
        };
            consensus.nSubsidyHalvingInterval = 150;
            consensus.nMasternodePaymentsStartBlock = 240;
            consensus.nMasternodePaymentsIncreaseBlock = 350;
            consensus.nMasternodePaymentsIncreasePeriod = 10;
            consensus.nInstantSendConfirmationsRequired = 2;
            consensus.nInstantSendKeepLock = 6;
            consensus.nBudgetPaymentsStartBlock = 25;
            consensus.nBudgetPaymentsCycleBlocks = 50;
            consensus.nBudgetPaymentsWindowBlocks = 10;
            consensus.nSuperblockStartBlock = 1500;
            consensus.nSuperblockStartHash = uint256(); // do not check this on regtest
            consensus.nSuperblockCycle = 10;
            consensus.nGovernanceMinQuorum = 1;
            consensus.nGovernanceFilterElements = 100;
            consensus.nMasternodeMinimumConfirmations = 1;
            consensus.BIP34Height = 100000000; // BIP34 has not activated on regtest (far in the future so block v1 are not rejected in tests)
            consensus.BIP34Hash = uint256();
            consensus.BIP65Height = 1351; // BIP65 activated on regtest (Used in rpc activation tests)
            consensus.BIP66Height = 1251; // BIP66 activated on regtest (Used in rpc activation tests)
            consensus.DIP0001Height = 2000;
            consensus.powLimit = uint256S("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
            consensus.nPowTargetTimespan = 24 * 60 * 60; // waggox: 1 day
            consensus.nPowTargetSpacing = 120; // waggox: 2.5 minutes
            consensus.fPowAllowMinDifficultyBlocks = true;
            consensus.fPowNoRetargeting = true;
            consensus.nPowKGWHeight = 15200; // same as mainnet
            consensus.nPowDGWHeight = 34140; // same as mainnet
            consensus.nRuleChangeActivationThreshold = 108; // 75% for testchains
            consensus.nMinerConfirmationWindow = 144; // Faster than normal for regtest (144 instead of 2016)
            consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
            consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 0;
            consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = 999999999999ULL;
            consensus.vDeployments[Consensus::DEPLOYMENT_CSV].bit = 0;
            consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nStartTime = 0;
            consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nTimeout = 999999999999ULL;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].bit = 1;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].nStartTime = 0;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].nTimeout = 999999999999ULL;
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].bit = 2;
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nStartTime = 0;
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nTimeout = 999999999999ULL;
            strSporkAddress = "yj949n1UH6fDhw6HtVE5VMj2iSTaSWBMcW";
            fMiningRequiresPeers = false;
            fDefaultConsistencyChecks = true;
            fRequireStandard = false;
            fMineBlocksOnDemand = true;
            fAllowMultipleAddressesFromGroup = true;
            fAllowMultiplePorts = true;
            nFulfilledRequestExpireTime = 5*60; 
            // Stake info
            consensus.nPosTargetSpacing = 30; // PoSW: 1 minutes
            consensus.nPosTargetTimespan = 60 * 40;
            consensus.nStakeMinAge = 60;
            consensus.nStakeMaxAge = 60 * 60 * 24; // one day
            consensus.nLastPoWBlock = 25;
            // highest difficulty | 0x1e0ffff0 (?)
            // smallest difficulty | 0x008000
            consensus.nWSTargetDiff = 0x1e0ffff0; // Genesis Difficulty

            // The best chain should have at least this much work.
            consensus.nMinimumChainWork = uint256S("0x00");

            // By default assume that the signatures in ancestors of this block are valid.
            consensus.defaultAssumeValid = uint256S("0x00");

            pchMessageStart[0] = 0xa4;
            pchMessageStart[1] = 0xe1;
            pchMessageStart[2] = 0xf8;
            pchMessageStart[3] = 0x8e;
            nDefaultPort = 9912;
            nPruneAfterHeight = 1000;

            genesis = CreateGenesisBlock(1545277713, 1, 0x207fffff, 1, 50 * COIN);
            consensus.hashGenesisBlock = genesis.GetHash();
            assert(consensus.hashGenesisBlock == uint256S("0x2fc7c5821ba86a701b1b38c19de1b7eb329b84ea3e0d5fc1374e017e96b62e95"));
            assert(genesis.hashMerkleRoot == uint256S("0xfcf0f3bdb11ea76705b0dd045a643f3b81f778924e01d7773fc2b93219d4014a"));


            vFixedSeeds.clear(); //!< Regtest mode doesn't have any fixed seeds.
            vSeeds.clear();      //!< Regtest mode doesn't have any DNS seeds.
        // Regtest Waggox addresses start with 'y'
        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,140);
        // Regtest Waggox script addresses start with '8' or '9'
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,19);
        // Regtest private keys start with '9' or 'c' (Bitcoin defaults)
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        // Regtest Waggox BIP32 pubkeys start with 'tpub' (Bitcoin defaults)
        base58Prefixes[EXT_PUBLIC_KEY] = boost::assign::list_of(0x04)(0x35)(0x87)(0xCF).convert_to_container<std::vector<unsigned char> >();
        // Regtest Waggox BIP32 prvkeys start with 'tprv' (Bitcoin defaults)
        base58Prefixes[EXT_SECRET_KEY] = boost::assign::list_of(0x04)(0x35)(0x83)(0x94).convert_to_container<std::vector<unsigned char> >();

        // Regtest Waggox BIP44 coin type is '1' (All coin's testnet default)
        nExtCoinType = 1;

    }

    void UpdateBIP9Parameters(Consensus::DeploymentPos d, int64_t nStartTime, int64_t nTimeout)
    {
            consensus.vDeployments[d].nStartTime = nStartTime;
            consensus.vDeployments[d].nTimeout = nTimeout;
    }
};
static CRegTestParams regTestParams;

static CChainParams *pCurrentParams = 0;

const CChainParams &Params() {
        assert(pCurrentParams);
        return *pCurrentParams;
}

CChainParams& Params(const std::string& chain)
{
        if (chain == CBaseChainParams::MAIN)
                return mainParams;
        else if (chain == CBaseChainParams::TESTNET)
                return testNetParams;
        else if (chain == CBaseChainParams::DEVNET) {
                assert(devNetParams);
                return *devNetParams;
        } else if (chain == CBaseChainParams::REGTEST)
                return regTestParams;
        else
                throw std::runtime_error(strprintf("%s: Unknown chain %s.", __func__, chain));
}

void SelectParams(const std::string& network)
{
        if (network == CBaseChainParams::DEVNET) {
                devNetParams = new CDevNetParams();
        }

        SelectBaseParams(network);
        pCurrentParams = &Params(network);
}

void UpdateRegtestBIP9Parameters(Consensus::DeploymentPos d, int64_t nStartTime, int64_t nTimeout)
{
        regTestParams.UpdateBIP9Parameters(d, nStartTime, nTimeout);
}
