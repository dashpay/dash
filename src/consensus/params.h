// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSENSUS_PARAMS_H
#define BITCOIN_CONSENSUS_PARAMS_H

#include "uint256.h"
#include <map>
#include <string>

namespace Consensus {

enum DeploymentPos
{
    DEPLOYMENT_TESTDUMMY,
    DEPLOYMENT_CSV, // Deployment of BIP68, BIP112, and BIP113.
    DEPLOYMENT_DIP0001, // Deployment of DIP0001 and lower transaction fees.
    DEPLOYMENT_BIP147, // Deployment of BIP147 (NULLDUMMY)
    DEPLOYMENT_DIP0003, // Deployment of DIP0002 and DIP0003 (txv3 and deterministic MN lists)
    DEPLOYMENT_DIP0008, // Deployment of ChainLock enforcement
    // NOTE: Also add new deployments to VersionBitsDeploymentInfo in versionbits.cpp
    MAX_VERSION_BITS_DEPLOYMENTS
};

/**
 * Struct for each individual consensus rule change using BIP9.
 */
struct BIP9Deployment {
    /** Bit position to select the particular bit in nVersion. */
    int bit;
    /** Start MedianTime for version bits miner confirmation. Can be a date in the past */
    int64_t nStartTime;
    /** Timeout/expiry MedianTime for the deployment attempt. */
    int64_t nTimeout;
    /** The number of past blocks (including the block under consideration) to be taken into account for locking in a fork. */
    int64_t nWindowSize;
    /** A number of blocks, in the range of 1..nWindowSize, which must signal for a fork in order to lock it in. */
    int64_t nThreshold;
};

enum LLMQType : uint8_t
{
    LLMQ_NONE = 0xff,

    // Dash LLMQs
    LLMQ_50_60 = 1, // 50 members, 30 (60%) threshold, one per hour
    LLMQ_400_60 = 2, // 400 members, 240 (60%) threshold, one every 12 hours
    LLMQ_400_85 = 3, // 400 members, 340 (85%) threshold, one every 24 hours

    // Alterdot LLMQs
    LLMQ_10_60 = 4, // 10 members, 6 (60%) threshold, one every 2 hours
    //LLMQ_20_60 = 5, // 20 members, 12 (60%) threshold, one every 8 hours
    LLMQ_30_80 = 6, // 30 members, 24 (80%) threshold, one every 16 hours

    // for testing only
    LLMQ_5_60 = 100, // 5 members, 3 (60%) threshold, one every 2 hours
};

// Configures a LLMQ and its DKG
// See https://github.com/dashpay/dips/blob/master/dip-0006.md for more details
struct LLMQParams {
    LLMQType type;

    // not consensus critical, only used in logging, RPC and UI
    std::string name;

    // the size of the quorum, e.g. 50 or 400
    int size;

    // The minimum number of valid members after the DKK. If less members are determined valid, no commitment can be
    // created. Should be higher then the threshold to allow some room for failing nodes, otherwise quorum might end up
    // not being able to ever created a recovered signature if more nodes fail after the DKG
    int minSize;

    // The threshold required to recover a final signature. Should be at least 50%+1 of the quorum size. This value
    // also controls the size of the public key verification vector and has a large influence on the performance of
    // recovery. It also influences the amount of minimum messages that need to be exchanged for a single signing session.
    // This value has the most influence on the security of the quorum. The number of total malicious masternodes
    // required to negatively influence signing sessions highly correlates to the threshold percentage.
    int threshold;

    // The interval in number blocks for DKGs and the creation of LLMQs. If set to 24 for example, a DKG will start
    // every 24 blocks, which is approximately once every hour.
    int dkgInterval;

    // The number of blocks per phase in a DKG session. There are 6 phases plus the mining phase that need to be processed
    // per DKG. Set this value to a number of blocks so that each phase has enough time to propagate all required
    // messages to all members before the next phase starts. If blocks are produced too fast, whole DKG sessions will
    // fail.
    int dkgPhaseBlocks;

    // The starting block inside the DKG interval for when mining of commitments starts. The value is inclusive.
    // Starting from this block, the inclusion of (possibly null) commitments is enforced until the first non-null
    // commitment is mined. The chosen value should be at least 5 * dkgPhaseBlocks so that it starts right after the
    // finalization phase.
    int dkgMiningWindowStart;

    // The ending block inside the DKG interval for when mining of commitments ends. The value is inclusive.
    // Choose a value so that miners have enough time to receive the commitment and mine it. Also take into consideration
    // that miners might omit real commitments and revert to always including null commitments. The mining window should
    // be large enough so that other miners have a chance to produce a block containing a non-null commitment. The window
    // should at the same time not be too large so that not too much space is wasted with null commitments in case a DKG
    // session failed.
    int dkgMiningWindowEnd;

    // In the complaint phase, members will vote on other members being bad (missing valid contribution). If at least
    // dkgBadVotesThreshold have voted for another member to be bad, it will considered to be bad by all other members
    // as well. This serves as a protection against late-comers who send their contribution on the bring of
    // phase-transition, which would otherwise result in inconsistent views of the valid members set
    int dkgBadVotesThreshold;

    // Number of quorums to consider "active" for signing sessions
    int signingActiveQuorumCount;

    // Used for inter-quorum communication. This is the number of quorums for which we should keep old connections. This
    // should be at least one more then the active quorums set.
    int keepOldConnections;
};

/**
 * Parameters that influence chain consensus.
 */
struct Params {
    uint256 hashGenesisBlock;
    uint256 hashDevnetGenesisBlock;
    int nHardForkOne; // block TODO_ADOT_FUTURE refactor hard forks into an array
    int nHardForkTwo; // block
    int nHardForkThree; //block
    int nHardForkFour; //block
    int nHardForkFive; //block
    int nHardForkSix; // block
    int nHardForkSeven; // block, lite/core network mode
    int nHardForkEight; // block, exit core mode, reactivation of MNs and Alterdot-specific functionalities, BIP147
    //int nDetMNRegHeight; // block, start allowing registration of determinisitc MNs, DIP0003Height has taken its place
    int nTempDevFundIncreaseEnd; //block height for temporal Dev fund increase ending
    int nSubsidyHalvingInterval;
    int nMasternodePaymentsStartBlock;
    int nInstantSendConfirmationsRequired; // in blocks
    int nInstantSendKeepLock; // in blocks
    int nInstantSendSigsRequired;
    int nInstantSendSigsTotal;
    int nBudgetPaymentsStartBlock;
    int nBudgetPaymentsCycleBlocks;
    int nBudgetPaymentsWindowBlocks;
    int nSuperblockStartBlock;
    uint256 nSuperblockStartHash;
    int nSuperblockCycle; // in blocks
    int nGovernanceMinQuorum; // Min absolute vote count to trigger an action
    int nGovernanceFilterElements;
    int nOldMasternodeCollateral;
    int nNewMasternodeCollateral; // used for easier switch between collaterals with future hard forks
    int nMasternodeMinimumConfirmations;
    /** Block height and hash at which BIP34 becomes active */
    //int BIP34Height;
    //uint256 BIP34Hash;
    /** Block height at which BIP65 becomes active */
    //int BIP65Height;
    /** Block height at which BIP66 becomes active */
    //int BIP66Height;
    /** Block height at which DIP0001 becomes active */
    int DIP0001Height; // TODO_ADOT_FUTURE not used currently

    int nIntPhaseTotalBlocks;
    int nBlocksPerYear; // expected blocks per year

    // ADOT_COMMENT in Dash certain features were activated in two steps:
    // first came the network signaling which provided the fDIPXXXX context or enablement (e.g. start of registration of Deterministic MNs)
    // second came the spork activation which provides the DIP enforcement (e.g. switch from the old MN system to Det. MNs)
    // in order to maintain consistency we will use this model for major activations and enforcements

    /** Block height at which DIP0003 becomes active */
    int DIP0003Height; // DIP0003Height correponds to nDetMNRegHeight in v1.8 + 1
    /** Block height at which DIP0003 becomes enforced */
    int DIP0003EnforcementHeight; // corresponds to the value set in SPORK_15_DETERMINISTIC_MNS_ENABLED in v1.8 + 1
    uint256 DIP0003EnforcementHash;

    int LLMQSwitchHeight; // height at which the used set of LLMQs changes

    int DIP0006EnforcementHeight; // corresponds to DKG Quorums activation SPORK_17_QUORUM_DKG_ENABLED
    uint256 DIP0006EnforcementHash;

    /** Block height at which DIP0008 becomes active */
    int DIP0008Height; // ChainLocks context enabled
    /** Block height at which DIP0008 becomes enforced */
    int DIP0008EnforcementHeight; // TODO_ADOT_FUTURE ChainLocks and LLMQ-based InstantSend
    uint256 DIP0008EnforcementHash;

    /**
     * Minimum blocks including miner confirmation of the total of nMinerConfirmationWindow blocks in a retargeting period,
     * (nPowTargetTimespan / nPowTargetSpacing) which is also used for BIP9 deployments.
     * Default BIP9Deployment::nThreshold value for deployments where it's not specified and for unknown deployments.
     * Examples: 1916 for 95%, 1512 for testchains.
     */
    uint32_t nRuleChangeActivationThreshold;
    // Default BIP9Deployment::nWindowSize value for deployments where it's not specified and for unknown deployments.
    uint32_t nMinerConfirmationWindow;
    BIP9Deployment vDeployments[MAX_VERSION_BITS_DEPLOYMENTS];
    /** Proof of work parameters */
    uint256 powLimit;
    bool fPowAllowMinDifficultyBlocks;
    bool fPowNoRetargeting;
    int64_t nPowTargetTimespan;
    int64_t nPowTargetSpacing;
    int64_t nDifficultyAdjustmentInterval;
    int64_t nOldPowTargetSpacing;
    int64_t nNewPowTargetSpacing;
    //int nPowDGWHeight;
    uint256 nMinimumChainWork;
    uint256 defaultAssumeValid;

    int64_t GetCurrentPowTargetSpacing(const int& nHeight) const {
        if (nHeight > nHardForkSix)
            return nNewPowTargetSpacing;
        else
            return nOldPowTargetSpacing;
    }

    int GetCurrentMasternodeCollateral(const int& nHeight) const {
        if (nHeight > nHardForkSix)
            return nNewMasternodeCollateral;
        else
            return nOldMasternodeCollateral;
    }

    /** these parameters are only used on devnet and can be configured from the outside */
    int nMinimumDifficultyBlocks{0};
    int nHighSubsidyBlocks{0};
    int nHighSubsidyFactor{1};

    std::map<LLMQType, LLMQParams> llmqs;
    LLMQType llmqChainLocks;
    LLMQType llmqForInstantSend{LLMQ_NONE};
};
} // namespace Consensus

#endif // BITCOIN_CONSENSUS_PARAMS_H
