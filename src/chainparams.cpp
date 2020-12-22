// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Copyright (c) 2014-2017 The Polis Core developers
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

#define NEVER32 0xFFFFFFFF

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
        const char* pszTimestamp = "Polis, from people to people";
        const CScript genesisOutputScript = CScript() << ParseHex("04678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4cef38c4f35504e51ec112de5c384df7ba0b8d578a4c702b6bf11d5f") << OP_CHECKSIG;
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

// this one is for testing only
static Consensus::LLMQParams llmq5_60 = {
        .type = Consensus::LLMQ_5_60,
        .name = "llmq_5_60",
        .size = 5,
        .minSize = 2,
        .threshold = 2,

        .dkgInterval = 24, // one DKG per hour
        .dkgPhaseBlocks = 2,
        .dkgMiningWindowStart = 10, // dkgPhaseBlocks * 5 = after finalization
        .dkgMiningWindowEnd = 18,
        .dkgBadVotesThreshold = 8,

        .signingActiveQuorumCount = 2, // just a few ones to allow easier testing

        .keepOldConnections = 3,
};

static Consensus::LLMQParams llmq50_60 = {
        .type = Consensus::LLMQ_50_60,
        .name = "llmq_50_60",
        .size = 50,
        .minSize = 40,
        .threshold = 30,

        .dkgInterval = 24, // one DKG per hour
        .dkgPhaseBlocks = 2,
        .dkgMiningWindowStart = 10, // dkgPhaseBlocks * 5 = after finalization
        .dkgMiningWindowEnd = 18,
        .dkgBadVotesThreshold = 40,

        .signingActiveQuorumCount = 24, // a full day worth of LLMQs

        .keepOldConnections = 25,
};

static Consensus::LLMQParams llmq400_60 = {
        .type = Consensus::LLMQ_400_60,
        .name = "llmq_400_60",
        .size = 400,
        .minSize = 300,
        .threshold = 240,

        .dkgInterval = 24 * 12, // one DKG every 12 hours
        .dkgPhaseBlocks = 4,
        .dkgMiningWindowStart = 20, // dkgPhaseBlocks * 5 = after finalization
        .dkgMiningWindowEnd = 28,
        .dkgBadVotesThreshold = 300,

        .signingActiveQuorumCount = 4, // two days worth of LLMQs

        .keepOldConnections = 5,
};

// Used for deployment and min-proto-version signalling, so it needs a higher threshold
static Consensus::LLMQParams llmq400_85 = {
        .type = Consensus::LLMQ_400_85,
        .name = "llmq_400_85",
        .size = 400,
        .minSize = 350,
        .threshold = 340,

        .dkgInterval = 24 * 24, // one DKG every 24 hours
        .dkgPhaseBlocks = 4,
        .dkgMiningWindowStart = 20, // dkgPhaseBlocks * 5 = after finalization
        .dkgMiningWindowEnd = 48, // give it a larger mining window to make sure it is mined
        .dkgBadVotesThreshold = 300,

        .signingActiveQuorumCount = 4, // two days worth of LLMQs

        .keepOldConnections = 5,
};


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

            consensus.nSubsidyHalvingInterval = 262800; // Note: actual number of blocks per calendar year with DGW v3 is ~200700 (for example 449750 - 249050)
            consensus.nMasternodePaymentsStartBlock = 15; // not true, but it's ok as long as it's less then nMasternodePaymentsIncreaseBlock
            // consensus.nMasternodePaymentsIncreaseBlock = 1569325056; // actual historical value
            // consensus.nMasternodePaymentsIncreasePeriod = 1569325056; // 17280 - actual historical value
            consensus.nInstantSendConfirmationsRequired = 6;
            consensus.nInstantSendKeepLock = 24;
            consensus.nBudgetPaymentsStartBlock = 32800; // actual historical value
            consensus.nBudgetPaymentsCycleBlocks = 16616; // ~(60*24*30)/2.6, actual number of blocks per month is 200700 / 12 = 16725
            consensus.nBudgetPaymentsWindowBlocks = 100;
            consensus.nSuperblockStartBlock = 66464; // The block at which 12.1 goes live (end of final 12.0 budget cycle)
            consensus.nSuperblockCycle = 16616; // ~(60*24*30)/2.6, actual number of blocks per month is 200700 / 12 = 16725
            consensus.nSuperblockStartHash = uint256S("0000000000000a7d13390bb29ac6d4466afb16358aeb3cd0656215392ed2468d");
            consensus.nGovernanceMinQuorum = 10;
            consensus.nGovernanceFilterElements = 20000;
            consensus.nMasternodeMinimumConfirmations = 15;
            consensus.BIP34Height = 951;
            consensus.BIP34Hash = uint256S("0x000001f35e70f7c5705f64c6c5cc3dea9449e74d5b5c7cf74dad1bcca14a8012");
            consensus.BIP65Height = 84672;  // 00000000000076d8fcea02ec0963de4abfd01e771fec0863f960c2c64fe6f357
            consensus.BIP66Height = 245817; // 00000000000b1fa2dfa312863570e13fae9ca7b5566cb27e55422620b469aefa
            consensus.DIP0001Height = 12096;
            consensus.DIP0003Height = 453000;
            consensus.DIP0003EnforcementHeight = 527920; // est 28/01/2020
            consensus.DIP0003EnforcementHash = uint256();
            consensus.powLimit = uint256S("00000fffff000000000000000000000000000000000000000000000000000000");
            consensus.nPowTargetTimespan = 24 * 60 * 60; // polis: 1 day
            consensus.nPowTargetSpacing = 120 ; // polis: 2 minutes
            consensus.fPowAllowMinDifficultyBlocks = false;
            consensus.fPowNoRetargeting = false;
            consensus.nPowKGWHeight = 551;
            consensus.nPowDGWHeight = 551;
            consensus.nMaxBlockSpacingFixDeploymentHeight = 381587;
            consensus.nStakeMinAgeSwitchTime = 1561734000;
            consensus.nPosMitigationSwitchTime = 1570665600;

            // Stake information
            consensus.nPosTargetSpacing = 2 * 60; // PoSW: 2 minutes
            consensus.nPosTargetTimespan = 60 * 40; // 40 minutes at max for difficulty adjustment 40 mins
            consensus.nStakeMaxAge = 60 * 60 * 24; // one day
            consensus.nWSTargetDiff = 0x1e0ffff0; // Genesis Difficulty
            consensus.nPoSDiffAdjustRange = 5;
            consensus.nMinStakeHistory = 120; // Minimum depth in chain required for valid stake
            consensus.nMinStakeHistoryHeight = 598000; // Activation height of MinDepth rule

            // POS hard fork date
            consensus.nLastPoWBlock = 209720;

            // hard coded payment Block
            consensus.nHardCodedPayment = 686805;
            consensus.nHardCodedPaymentAddress = "PGxr2egr3K7YUVpeR33d5EDG1Qi1LGiJx2";

            consensus.nRuleChangeActivationThreshold = 1916; // 95% of 2016
            consensus.nMinerConfirmationWindow = 2016; // nPowTargetTimespan / nPowTargetSpacing
    
            consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
            consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 1199145601; // January 1, 2008
            consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = 1230767999; // December 31, 2008

            // Deployment of BIP68, BIP112, and BIP113.
            consensus.vDeployments[Consensus::DEPLOYMENT_CSV].bit = 0;
            consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nStartTime = 1486252800; // Feb 5th, 2017
            consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nTimeout = 1517788800; // Feb 5th, 2018

            // Deployment of DIP0001
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].bit = 1;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].nStartTime = 1508025600; // Oct 15th, 2017
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].nTimeout = 1539561600; // Oct 15th, 2018
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].nWindowSize = 4032;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].nThreshold = 3226; // 80% of 4032

            // Deployment of BIP147
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].bit = 2;
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nStartTime = 1524477600; // Apr 23th, 2018
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nTimeout = 1556013600; // Apr 23th, 2019
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nWindowSize = 4032;
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nThreshold = 3226; // 80% of 4032

            // Deployment of DIP0003
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0003].bit = 3;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0003].nStartTime = 1570665600; // 10/10/2019
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0003].nTimeout = 1578549600;   // + 3months
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0003].nWindowSize = 500;       // 500 hosts
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0003].nThreshold = 200;        // 40% of 500 hosts
    
            // Deployment of DIP0008
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0008].bit = 4;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0008].nStartTime = 1576947018; // 21/12/2019
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0008].nTimeout = 1581465600;   // 02/12/2020
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0008].nWindowSize = 500;       // 500 hosts
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0008].nThreshold = 200;        // 40% of 500 hosts

            // The best chain should have at least this much work.
            consensus.nMinimumChainWork = uint256S("0x0"); // 134622
            // By default assume that the signatures in ancestors of this block are valid.
            consensus.defaultAssumeValid = uint256S("0x00000000000002115266e3f36778feb4fe9488fccabcd004e13c13ab46ddfaeb"); // 134622
            /**
             * The message start string is designed to be unlikely to occur in normal data.
             * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
             * a large 32-bit integer with any alignment.
             */
            pchMessageStart[0] = 0xbf;
            pchMessageStart[1] = 0x0c;
            pchMessageStart[2] = 0x6b;
            pchMessageStart[3] = 0xbd;
            vAlertPubKey = ParseHex("04678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4cef38c4f35504e51ec112de5c384df7ba0b8d578a4c702b6bf11d5f");
            nDefaultPort = 24126;
            nPruneAfterHeight = 100000;

            genesis = CreateGenesisBlock(1513862955, 327643, 0x1e0ffff0, 1, 50 * COIN);
            consensus.hashGenesisBlock = genesis.GetHash();
            assert(consensus.hashGenesisBlock == uint256S("0x000009701eb781a8113b1af1d814e2f060f6408a2c990db291bc5108a1345c1e"));
            assert(genesis.hashMerkleRoot == uint256S("0x5dc9bcf5d1e4802dad0045a88849e3ad97d07a5b8aaee1114ed5ae03b98c4bfc"));

            vSeeds.push_back(CDNSSeedData("167.172.143.204","167.172.143.204"));
            vSeeds.push_back(CDNSSeedData("165.22.82.200", "165.22.82.200"));
            vSeeds.push_back(CDNSSeedData("46.101.145.29", "46.101.145.29"));
            vSeeds.push_back(CDNSSeedData("178.128.7.220", "178.128.7.220"));
            vSeeds.push_back(CDNSSeedData("165.227.140.247", "165.227.140.247"));
            vSeeds.push_back(CDNSSeedData("45.32.184.252", "45.32.184.252"));
            vSeeds.push_back(CDNSSeedData("95.111.237.31", "95.111.237.31"));
            vSeeds.push_back(CDNSSeedData("161.97.112.31", "161.97.112.31"));
            vSeeds.push_back(CDNSSeedData("167.71.3.168", "167.71.3.168"));
            vSeeds.push_back(CDNSSeedData("64.227.44.5", "64.227.44.5"));

            // polis addresses start with 'P'
            base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,55);
            // polis script addresses start with '3'
            base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,56);
            // polis private keys start with '3' or 'p'
            base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,60);
            // polis BIP32 pubkeys start with 'ppub' (Polis Prefix)
            base58Prefixes[EXT_PUBLIC_KEY] = boost::assign::list_of(0x03)(0xE2)(0x5D)(0x7E).convert_to_container<std::vector<unsigned char> >();
            // polis BIP32 prvkeys start with 'pprv' (Polis Prefix)
            base58Prefixes[EXT_SECRET_KEY] = boost::assign::list_of(0x03)(0xE2)(0x59)(0x45).convert_to_container<std::vector<unsigned char> >();

            // polis BIP44 coin type is '5'
            nExtCoinType = 1997;

            vFixedSeeds = std::vector<SeedSpec6>(pnSeed6_main, pnSeed6_main + ARRAYLEN(pnSeed6_main));
    
            // long living quorum params
            consensus.llmqs[Consensus::LLMQ_50_60] = llmq50_60;
            consensus.llmqs[Consensus::LLMQ_400_60] = llmq400_60;
            consensus.llmqs[Consensus::LLMQ_400_85] = llmq400_85;
            consensus.llmqChainLocks = Consensus::LLMQ_400_60;
            consensus.llmqForInstantSend = Consensus::LLMQ_50_60;

            fMiningRequiresPeers = true;
            fDefaultConsistencyChecks = false;
            fRequireStandard = true;
            fMineBlocksOnDemand = false;
            fAllowMultipleAddressesFromGroup = false;
            fAllowMultiplePorts = false;

            nPoolMinParticipants = 3;
            nPoolMaxParticipants = 5;
            nFulfilledRequestExpireTime = 60*60; // fulfilled requests expire in 1 hour
    
            vSporkAddresses = {"PAvya6xSBRb755Uhe2aXBFsjuyi68hTP8u"};
            nMinSporkKeys = 1;
            fBIP9CheckMasternodesUpgraded = true;
            consensus.fLLMQAllowDummyCommitments = false;

            checkpointData = (CCheckpointData) {
                boost::assign::map_list_of
                        ( 0, uint256S("0x000009701eb781a8113b1af1d814e2f060f6408a2c990db291bc5108a1345c1e"))
                        ( 10, uint256S("0x000006f686844f1873f4fcf77516eaa0a11f5762b797314c5875438c97fe4562"))
                        ( 50, uint256S("0x00000547ca650f3be6ddf31871dfd5f76faafcd12b6083706e3d0a88c9a0cdbe"))
                        ( 100, uint256S("0x000001c12d74ca8120d2751bb2846a5af44892045099750efe73a0cbb1eca860"))
                        ( 200, uint256S("0x000000053db45327e7fcce9b8baf9060622c01456ac677121a6f8dcf15a98d7b"))
                        ( 300, uint256S("0x00000000027d5a77e5c38e366465be2f21292fa3eb4d290efb6316eebcc5c0ca"))
                        ( 400, uint256S("0x0000000000004bcce3cea51dd54da213272b7ef1aa35dca5269e92a56ec7974b"))
                        ( 500, uint256S("0x000000000000157f78c69400b85824588e381ca4d11384e172394468a02bea32"))
                        ( 1000, uint256S("0x00000000000061cd1733fc8b84017169be61d6a2406f21f8dca0dd9a57c7de96"))
                        ( 2000, uint256S("0x000000000000129a0f9540f12a27a6b13242fa2d9b90f52083d860fa5ef5ec3e"))
                        ( 3000, uint256S("0x0000000000000b5968c7af1b6e76b6958aa6058bf2f0a7aa66e67cbf4d47b690"))
                        ( 4000, uint256S("0x0000000000000b443f7faeaaf85178b5364664ee7374259cf0f7b03e9531a108"))
                        ( 5000, uint256S("0x00000000000004779b06d324ea780378ad8e41ddc9056129444030243faf5ad4"))
                        ( 6000, uint256S("0x00000000000027ffb241efa3fafa43a3121216d70e79b28bc2afb8457d84cbfe"))
                        ( 7000, uint256S("0x0000000000003a79e1b54fe39070e0d53799cdca901db726eb1c847ce8d0c890"))
                        ( 8000, uint256S("0x000000000000045991733d2bc793d17a6fcafd7bfa35adb68b4036813b64fbf1"))
                        ( 70000, uint256S("0x00000000000006fd8cdcc16c6432c8934e8f4b598f447db295851107437647a7"))
                        ( 80000, uint256S("0x0000000000000eaf98251863568f95a865bba9476e060d93dfd7f02f123b0748"))
                        ( 100000, uint256S("0x0000000000000818820ee2aecc291c7b6076984cf7c0c152869de128cf30262f"))
                        ( 111111, uint256S("0x0000000000000bcea9747fd93566d7c37d7e07afb50066f332e17925ef776b3b"))
                        ( 226729, uint256S("0x679fcb43f8318a3bdf42908c5290b6871426de008321589b72dcca907eeba0ea"))
                        ( 229186, uint256S("0x79eb91ffb16cf095b4bc730ea2bd158df1aa81ee3246f64b85535b18d91593d6"))
                        ( 262800, uint256S("0xc7ba46c8521735e42a84f9bb84b5bcc3fb6b57a2f32aee5d5c03ab52e0cf0e57"))
                        ( 270300, uint256S("0xe31ddef7188566d6bbd8b1b74ba31b1ade883673bec95c6b4a41d5c21ab7f691"))
                        ( 280600, uint256S("0xf2911e19b667f05b091d8217098813055aa26e1eca6c8eba5b84ff63df414208"))
                        ( 324353, uint256S("0x852675a3957d48e0f1198fdda33e496211317d89bab71ff39c28faaf3d4e61af"))
                        ( 336284, uint256S("0xd3dbdedff4c5b1f6e47ab43d2b0dc1f9a5a011035b4b936284a5b8725d405a78"))
                        ( 379624, uint256S("0x58b38dde8562a110d53bae6cc5326678615a9f461095bcc189b6de6d0f7e65cc"))
                        ( 453700, uint256S("0xaebf0dde0fde68ffba89b515f3e7d8f64e15ccf309f8fe60302e4df482c34555"))
                        ( 750669, uint256S("0x8db94d662b88ef32cab70c2b2e117c885defbd30da68fcde5fa64f6aaa89c292"))
            };
        chainTxData = ChainTxData{
                1608655984, // * UNIX timestamp of last checkpoint block
                2388905,    // * total number of transactions between genesis and last checkpoint
                //   (the tx=... number in the SetBestChain debug.log lines)
                0.1        // * estimated number of transactions per day after checkpoint

        };
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
            consensus.nSubsidyHalvingInterval = NEVER32;
            consensus.nMasternodePaymentsStartBlock = 15; // not true, but it's ok as long as it's less then nMasternodePaymentsIncreaseBlock
            consensus.nMasternodePaymentsIncreaseBlock = NEVER32;
            consensus.nMasternodePaymentsIncreasePeriod = NEVER32;
            consensus.nInstantSendConfirmationsRequired = 2;
            consensus.nInstantSendKeepLock = 6;
            consensus.nBudgetPaymentsStartBlock = 46;
            consensus.nBudgetPaymentsCycleBlocks = 24;
            consensus.nBudgetPaymentsWindowBlocks = 10;
            consensus.nSuperblockStartBlock = 3050; // NOTE: Should satisfy nSuperblockStartBlock > nBudgetPaymentsStartBlock
            // consensus.nSuperblockStartHash = uint256S("000001af046f4ed575a48b919ed28be8a40c6a78df8d7830fbbfd07ec17a1fee");
            consensus.nSuperblockCycle = 24; // Superblocks can be issued hourly on testnet
            consensus.nGovernanceMinQuorum = 1;
            consensus.nGovernanceFilterElements = 500;
            consensus.nMasternodeMinimumConfirmations = 1;
            consensus.BIP34Height = 76;
            consensus.BIP34Hash = uint256S("0x000008ebb1db2598e897d17275285767717c6acfeac4c73def49fbea1ddcbcb6");
            consensus.BIP65Height = 2431; // 0000039cf01242c7f921dcb4806a5994bc003b48c1973ae0c89b67809c2bb2ab
            consensus.BIP66Height = 2075; // 0000002acdd29a14583540cb72e1c5cc83783560e38fa7081495d474fe1671f7
            consensus.DIP0001Height = 50;
            consensus.DIP0003Height = 9465;
            consensus.DIP0003EnforcementHeight = 9465;
            consensus.DIP0003EnforcementHash = uint256();
            consensus.powLimit = uint256S("0000fffff0000000000000000000000000000000000000000000000000000000");
            consensus.nPowTargetTimespan = 60 * 60 * 24; // polis: 1 day
            consensus.nPowTargetSpacing = 2 * 60; // polis: 2 minutes
            consensus.fPowAllowMinDifficultyBlocks = true;
            consensus.fPowNoRetargeting = false;
            consensus.nPowKGWHeight = 4001; // nPowKGWHeight >= nPowDGWHeight means "no KGW"
            consensus.nPowDGWHeight = 4001;

            // Stake info
            consensus.nPosTargetSpacing = 2 * 60;
            consensus.nPosTargetTimespan = 60 * 40;
            consensus.nStakeMaxAge = 60 * 60 * 24 * 30;
            consensus.nLastPoWBlock = 150;
            consensus.nPoSDiffAdjustRange = 1;
            consensus.nWSTargetDiff = 0x1f00ffff; // Genesis Difficulty
            consensus.nMaxBlockSpacingFixDeploymentHeight = -1;
            consensus.nStakeMinAgeSwitchTime = -1;

            consensus.nRuleChangeActivationThreshold = 1512; // 75% for testchains
            consensus.nMinerConfirmationWindow = 2016; // nPowTargetTimespan / nPowTargetSpacing
            consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
            consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 1199145601; // January 1, 2008
            consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = 1230767999; // December 31, 2008

            // hard coded payment Block
            consensus.nHardCodedPayment = 9999999999;
            consensus.nHardCodedPaymentAddress = "PGxr2egr3K7YUVpeR33d5EDG1Qi1LGiJx2";

            // Deployment of BIP68, BIP112, and BIP113.
            consensus.vDeployments[Consensus::DEPLOYMENT_CSV].bit = 0;
            consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nStartTime = Consensus::BIP9Deployment::ALWAYS_ACTIVE;
            consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;

            // Deployment of DIP0001
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].bit = 1;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].nStartTime = Consensus::BIP9Deployment::ALWAYS_ACTIVE;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].nWindowSize = 50;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0001].nThreshold = 40;

            // Deployment of BIP147
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].bit = 2;
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nStartTime = Consensus::BIP9Deployment::ALWAYS_ACTIVE;
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nWindowSize = 50;
            consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nThreshold = 40;

            // Deployment of DIP0003
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0003].bit = 3;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0003].nStartTime = Consensus::BIP9Deployment::ALWAYS_ACTIVE;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0003].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0003].nWindowSize = 50;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0003].nThreshold = 40;

            // Deployment of DIP0008
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0008].bit = 4;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0008].nStartTime = Consensus::BIP9Deployment::ALWAYS_ACTIVE;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0008].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0008].nWindowSize = 50;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0008].nThreshold = 40;

            // The best chain should have at least this much work.
            consensus.nMinimumChainWork = uint256S("0000000000000000000000000000000000000000000000000000000000000000");
            // By default assume that the signatures in ancestors of this block are valid.
            consensus.defaultAssumeValid = uint256S("0000000000000000000000000000000000000000000000000000000000000000");

            pchMessageStart[0] = 0xce;
            pchMessageStart[1] = 0xe2;
            pchMessageStart[2] = 0xca;
            pchMessageStart[3] = 0xff;
            vAlertPubKey = ParseHex("04517d8a699cb43d3938d7b24faaff7cda448ca4ea267723ba614784de661949bf632d6304316b244646dea079735b9a6fc4af804efb4752075b9fe2245e14e412");
            nDefaultPort = 21430;
            nPruneAfterHeight = 1000;

            uint32_t nTime = 1569000000;
            uint32_t nNonce = 0;

            if (!nNonce) {
                while (UintToArith256(genesis.GetHash()) >
                       UintToArith256(consensus.powLimit))
                {
                    nNonce++;
                    genesis = CreateGenesisBlock(nTime, nNonce, 0x1f00ffff, 1, 50 * COIN);
                }
            }

            genesis = CreateGenesisBlock(nTime, nNonce, 0x1f00ffff, 1, 50 * COIN);
            consensus.hashGenesisBlock = genesis.GetHash();

            vSeeds.push_back(CDNSSeedData("testnetseed.poliscentral.org", "testnetseed.poliscentral.org"));
            vSeeds.push_back(CDNSSeedData("testnetseed2.poliscentral.org", "testnetseed2.poliscentral.org"));

            // Testnet polis addresses start with 'y'
            base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,140);
            // Testnet polis script addresses start with '8' or '9'
            base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,19);
            // Testnet private keys start with '9' or 'c' (Bitcoin defaults)
            base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
            // Testnet polis BIP32 pubkeys start with 'tpub' (Bitcoin defaults)
            base58Prefixes[EXT_PUBLIC_KEY] = boost::assign::list_of(0x04)(0x35)(0x87)(0xCF).convert_to_container<std::vector<unsigned char> >();
            // Testnet polis BIP32 prvkeys start with 'tprv' (Bitcoin defaults)
            base58Prefixes[EXT_SECRET_KEY] = boost::assign::list_of(0x04)(0x35)(0x83)(0x94).convert_to_container<std::vector<unsigned char> >();

            // Testnet polis BIP44 coin type is '1' (All coin's testnet default)
            nExtCoinType = 1;
    
            // long living quorum params
            consensus.llmqs[Consensus::LLMQ_50_60] = llmq5_60;
            consensus.llmqs[Consensus::LLMQ_400_60] = llmq400_60;
            consensus.llmqs[Consensus::LLMQ_400_85] = llmq400_85;
            consensus.llmqChainLocks = Consensus::LLMQ_50_60;
            consensus.llmqForInstantSend = Consensus::LLMQ_50_60;

            fMiningRequiresPeers = true;
            fDefaultConsistencyChecks = false;
            fRequireStandard = false;
            fMineBlocksOnDemand = false;
            fAllowMultipleAddressesFromGroup = false;
            fAllowMultiplePorts = false;

            nPoolMaxParticipants = 3;
            nFulfilledRequestExpireTime = 5*60; // fulfilled requests expire in 5 minutes
    
            vSporkAddresses = {"yRiUNgbfBYp3SYXEqYLNepyVqPC3u3BxJW"};
            nMinSporkKeys = 1;
            fBIP9CheckMasternodesUpgraded = true;
            consensus.fLLMQAllowDummyCommitments = true;

            checkpointData = (CCheckpointData) {
                    boost::assign::map_list_of
                            (   0, uint256S("0x"))


            };
            chainTxData = ChainTxData{
                    0, // * UNIX timestamp of last checkpoint block
                    0,       // * total number of transactions between genesis and last checkpoint
                    //   (the tx=... number in the SetBestChain debug.log lines)
                    0         // * estimated number of transactions per day after checkpoint

            };

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
            consensus.nPowTargetTimespan = 24 * 60 * 60; // Polis: 1 day
            consensus.nPowTargetSpacing = 2.5 * 60; // Polis: 2.5 minutes
            consensus.fPowAllowMinDifficultyBlocks = true;
            consensus.fPowNoRetargeting = false;
            consensus.nPowKGWHeight = 4001; // nPowKGWHeight >= nPowDGWHeight means "no KGW"
            consensus.nPowDGWHeight = 4001;
            consensus.nMaxBlockSpacingFixDeploymentHeight = 700;
            consensus.nStakeMinAgeSwitchTime = 1561734000;


            consensus.nPosTargetSpacing = 2 * 60; // PoSW: 1 minutes
            consensus.nPosTargetTimespan = 60 * 40;
            consensus.nStakeMaxAge = 60 * 60 * 24; // one day
            consensus.nLastPoWBlock = 180675;

            // hard coded payment Block
            consensus.nHardCodedPayment = 9999999999;
            consensus.nHardCodedPaymentAddress = "PGxr2egr3K7YUVpeR33d5EDG1Qi1LGiJx2";

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
    
            // Deployment of DIP0003
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0003].bit = 3;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0003].nStartTime = 1535752800; // Sep 1st, 2018
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0003].nTimeout = 1567288800; // Sep 1st, 2019
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0003].nWindowSize = 100;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0003].nThreshold = 50; // 50% of 100
    
            // Deployment of DIP0008
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0008].bit = 4;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0008].nStartTime = 1553126400; // Mar 21st, 2019
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0008].nTimeout = 1584748800; // Mar 21st, 2020
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0008].nWindowSize = 100;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0008].nThreshold = 50; // 50% of 100

            // The best chain should have at least this much work.
            consensus.nMinimumChainWork = uint256S("0x000000000000000000000000000000000000000000000000000000000000000");

            // By default assume that the signatures in ancestors of this block are valid.
            consensus.defaultAssumeValid = uint256S("0x000000000000000000000000000000000000000000000000000000000000000");

            pchMessageStart[0] = 0xe2;
            pchMessageStart[1] = 0xca;
            pchMessageStart[2] = 0xff;
            pchMessageStart[3] = 0xce;
            vAlertPubKey = ParseHex("04517d8a699cb43d3938d7b24faaff7cda448ca4ea267723ba614784de661949bf632d6304316b244646dea079735b9a6fc4af804efb4752075b9fe2245e14e412");
            nDefaultPort = 19999;
            nPruneAfterHeight = 1000;

            genesis = CreateGenesisBlock(1417713337, 1096447, 0x207fffff, 1, 50 * COIN);
            consensus.hashGenesisBlock = genesis.GetHash();
            assert(consensus.hashGenesisBlock == uint256S("0x000008ca1832a4baf228eb1553c03d3a2c8e02399550dd6ea8d65cec3ef23d2e"));
            assert(genesis.hashMerkleRoot == uint256S("0xe0028eb9648db56b1ac77cf090b99048a8007e2bb64b68f092c03c7f56a662c7"));

            devnetGenesis = FindDevNetGenesisBlock(consensus, genesis, 50 * COIN);
            consensus.hashDevnetGenesisBlock = devnetGenesis.GetHash();

            vFixedSeeds.clear();
            vSeeds.clear();
            //vSeeds.push_back(CDNSSeedData("polisevo.org",  "devnet-seed.polisevo.org"));

            // Testnet Polis addresses start with 'y'
            base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,140);
            // Testnet Polis script addresses start with '8' or '9'
            base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,19);
            // Testnet private keys start with '9' or 'c' (Bitcoin defaults)
            base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
            // Testnet Polis BIP32 pubkeys start with 'tpub' (Bitcoin defaults)
            base58Prefixes[EXT_PUBLIC_KEY] = boost::assign::list_of(0x04)(0x35)(0x87)(0xCF).convert_to_container<std::vector<unsigned char> >();
            // Testnet Polis BIP32 prvkeys start with 'tprv' (Bitcoin defaults)
            base58Prefixes[EXT_SECRET_KEY] = boost::assign::list_of(0x04)(0x35)(0x83)(0x94).convert_to_container<std::vector<unsigned char> >();

            // Testnet Dash BIP44 coin type is '1' (All coin's testnet default)
            nExtCoinType = 1;
    
            // long living quorum params
            consensus.llmqs[Consensus::LLMQ_50_60] = llmq50_60;
            consensus.llmqs[Consensus::LLMQ_400_60] = llmq400_60;
            consensus.llmqs[Consensus::LLMQ_400_85] = llmq400_85;
            consensus.llmqChainLocks = Consensus::LLMQ_50_60;
            consensus.llmqForInstantSend = Consensus::LLMQ_50_60;

            fMiningRequiresPeers = true;
            fDefaultConsistencyChecks = false;
            fRequireStandard = false;
            fMineBlocksOnDemand = false;
            fAllowMultipleAddressesFromGroup = true;
            fAllowMultiplePorts = true;

            nPoolMaxParticipants = 3;
            nFulfilledRequestExpireTime = 5*60; // fulfilled requests expire in 5 minutes
    
            vSporkAddresses = {"yjPtiKh2uwk3bDutTEA2q9mCtXyiZRWn55"};
            nMinSporkKeys = 1;
            // devnets are started with no blocks and no MN, so we can't check for upgraded MN (as there are none)
            fBIP9CheckMasternodesUpgraded = false;
            consensus.fLLMQAllowDummyCommitments = true;

            checkpointData = (CCheckpointData) {
                    boost::assign::map_list_of
                            (      0, uint256S("0x000008ca1832a4baf228eb1553c03d3a2c8e02399550dd6ea8d65cec3ef23d2e"))
                            (      1, devnetGenesis.GetHash())
            };

            chainTxData = ChainTxData{
                    devnetGenesis.GetBlockTime(), // * UNIX timestamp of devnet genesis block
                    2,                            // * we only have 2 coinbase transactions when a devnet is started up
                    0.01                          // * estimated number of transactions per second
            };
    }

    void UpdateSubsidyAndDiffParams(int nMinimumDifficultyBlocks, int nHighSubsidyBlocks, int nHighSubsidyFactor)
    {
        consensus.nMinimumDifficultyBlocks = nMinimumDifficultyBlocks;
        consensus.nHighSubsidyBlocks = nHighSubsidyBlocks;
        consensus.nHighSubsidyFactor = nHighSubsidyFactor;
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
            consensus.nPowTargetTimespan = 24 * 60 * 60; // polis: 1 day
            consensus.nPowTargetSpacing = 120; // polis: 2.5 minutes
            consensus.fPowAllowMinDifficultyBlocks = true;
            consensus.fPowNoRetargeting = true;
            consensus.nPowKGWHeight = 15200; // same as mainnet
            consensus.nPowDGWHeight = 34140; // same as mainnet
            consensus.nMaxBlockSpacingFixDeploymentHeight = 700;
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
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0003].bit = 3;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0003].nStartTime = 0;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0003].nTimeout = 999999999999ULL;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0008].bit = 4;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0008].nStartTime = 0;
            consensus.vDeployments[Consensus::DEPLOYMENT_DIP0008].nTimeout = 999999999999ULL;

            // Stake info
            consensus.nPosTargetSpacing = 30; // PoSW: 1 minutes
            consensus.nPosTargetTimespan = 60 * 40;
            consensus.nStakeMaxAge = 60 * 60 * 24; // one day
            consensus.nLastPoWBlock = 25;
            // highest difficulty | 0x1e0ffff0 (?)
            // smallest difficulty | 0x008000
            consensus.nWSTargetDiff = 0x1e0ffff0; // Genesis Difficulty
            consensus.nStakeMinAgeSwitchTime = 1561734000;

            // hard coded payment Block
            consensus.nHardCodedPayment = 9999999999;
            consensus.nHardCodedPaymentAddress = "PGxr2egr3K7YUVpeR33d5EDG1Qi1LGiJx2";

            // The best chain should have at least this much work.
            consensus.nMinimumChainWork = uint256S("0x00");

            // By default assume that the signatures in ancestors of this block are valid.
            consensus.defaultAssumeValid = uint256S("0x00");

            pchMessageStart[0] = 0xfc;
            pchMessageStart[1] = 0xc1;
            pchMessageStart[2] = 0xb7;
            pchMessageStart[3] = 0xdc;
            nDefaultPort = 19994;
            nPruneAfterHeight = 1000;

            genesis = CreateGenesisBlock(1513831322, 887612, 0x1e0ffff0, 1, 50 * COIN);
            consensus.hashGenesisBlock = genesis.GetHash();
            assert(consensus.hashGenesisBlock == uint256S("0x00000f3b4ec57f851b3706c056eda3120875896c9970495ae2efd49b46472a05"));
            assert(genesis.hashMerkleRoot == uint256S("0x5dc9bcf5d1e4802dad0045a88849e3ad97d07a5b8aaee1114ed5ae03b98c4bfc"));


            vFixedSeeds.clear(); //!< Regtest mode doesn't have any fixed seeds.
            vSeeds.clear();      //!< Regtest mode doesn't have any DNS seeds.

            fMiningRequiresPeers = false;
            fDefaultConsistencyChecks = true;
            fRequireStandard = false;
            fMineBlocksOnDemand = true;
            fAllowMultipleAddressesFromGroup = true;
            fAllowMultiplePorts = true;

            nFulfilledRequestExpireTime = 5*60; // fulfilled requests expire in 5 minutes
    
            // privKey: cP4EKFyJsHT39LDqgdcB43Y3YXjNyjb5Fuas1GQSeAtjnZWmZEQK
            vSporkAddresses = {"yj949n1UH6fDhw6HtVE5VMj2iSTaSWBMcW"};
            nMinSporkKeys = 1;
            // regtest usually has no masternodes in most tests, so don't check for upgraged MNs
            fBIP9CheckMasternodesUpgraded = false;
            consensus.fLLMQAllowDummyCommitments = true;

            checkpointData = (CCheckpointData){
                    boost::assign::map_list_of
                            ( 0, uint256S("0x000008ca1832a4baf228eb1553c03d3a2c8e02399550dd6ea8d65cec3ef23d2e"))
            };

            chainTxData = ChainTxData{
                    0,
                    0,
                    0
            };

            base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,140);
            // Regtest polis script addresses start with '8' or '9'
            base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,19);
            // Regtest private keys start with '9' or 'c' (Bitcoin defaults)
            base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
            // Regtest polis BIP32 pubkeys start with 'tpub' (Bitcoin defaults)
            base58Prefixes[EXT_PUBLIC_KEY] = boost::assign::list_of(0x04)(0x35)(0x87)(0xCF).convert_to_container<std::vector<unsigned char> >();
            // Regtest polis BIP32 prvkeys start with 'tprv' (Bitcoin defaults)
            base58Prefixes[EXT_SECRET_KEY] = boost::assign::list_of(0x04)(0x35)(0x83)(0x94).convert_to_container<std::vector<unsigned char> >();

            // Regtest Dash BIP44 coin type is '1' (All coin's testnet default)
            nExtCoinType = 1;
    
            // long living quorum params
            consensus.llmqs[Consensus::LLMQ_50_60] = llmq50_60;
            consensus.llmqs[Consensus::LLMQ_50_60] = llmq50_60;
            consensus.llmqChainLocks = Consensus::LLMQ_5_60;
            consensus.llmqForInstantSend = Consensus::LLMQ_5_60;
    }

    void UpdateBIP9Parameters(Consensus::DeploymentPos d, int64_t nStartTime, int64_t nTimeout, int64_t nWindowSize, int64_t nThreshold)
    {
        consensus.vDeployments[d].nStartTime = nStartTime;
        consensus.vDeployments[d].nTimeout = nTimeout;
        if (nWindowSize != -1) {
            consensus.vDeployments[d].nWindowSize = nWindowSize;
        }
        if (nThreshold != -1) {
            consensus.vDeployments[d].nThreshold = nThreshold;
        }
    }

    void UpdateDIP3Parameters(int nActivationHeight, int nEnforcementHeight)
    {
        consensus.DIP0003Height = nActivationHeight;
        consensus.DIP0003EnforcementHeight = nEnforcementHeight;
    }

    void UpdateBudgetParameters(int nMasternodePaymentsStartBlock, int nBudgetPaymentsStartBlock, int nSuperblockStartBlock)
    {
        consensus.nMasternodePaymentsStartBlock = nMasternodePaymentsStartBlock;
        consensus.nBudgetPaymentsStartBlock = nBudgetPaymentsStartBlock;
        consensus.nSuperblockStartBlock = nSuperblockStartBlock;
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

void UpdateRegtestBIP9Parameters(Consensus::DeploymentPos d, int64_t nStartTime, int64_t nTimeout, int64_t nWindowSize, int64_t nThreshold)
{
    regTestParams.UpdateBIP9Parameters(d, nStartTime, nTimeout, nWindowSize, nThreshold);
}

void UpdateRegtestDIP3Parameters(int nActivationHeight, int nEnforcementHeight)
{
    regTestParams.UpdateDIP3Parameters(nActivationHeight, nEnforcementHeight);
}

void UpdateRegtestBudgetParameters(int nMasternodePaymentsStartBlock, int nBudgetPaymentsStartBlock, int nSuperblockStartBlock)
{
    regTestParams.UpdateBudgetParameters(nMasternodePaymentsStartBlock, nBudgetPaymentsStartBlock, nSuperblockStartBlock);
}

void UpdateDevnetSubsidyAndDiffParams(int nMinimumDifficultyBlocks, int nHighSubsidyBlocks, int nHighSubsidyFactor)
{
    assert(devNetParams);
    devNetParams->UpdateSubsidyAndDiffParams(nMinimumDifficultyBlocks, nHighSubsidyBlocks, nHighSubsidyFactor);
}
