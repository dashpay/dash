// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>

#include <llmq/params.h>
#include <util/std23.h>

#include <arith_uint256.h>
#include <chainparamsseeds.h>
#include <consensus/merkle.h>
#include <deploymentinfo.h>
#include <util/system.h>
#include <versionbits.h>

#include <cassert>
#include <ranges>

static void ReadRegTestActivationArgs(const ArgsManager& args, CChainParams::RegTestOptions& options)
{
    for (const std::string& arg : args.GetArgs("-testactivationheight")) {
        const auto found{arg.find('@')};
        if (found == std::string::npos) {
            throw std::runtime_error(strprintf("Invalid format (%s) for -testactivationheight=name@height.", arg));
        }

        const auto value{arg.substr(found + 1)};
        int32_t height;
        if (!ParseInt32(value, &height) || height < 0 || height >= std::numeric_limits<int>::max()) {
            throw std::runtime_error(strprintf("Invalid height value (%s) for -testactivationheight=name@height.", arg));
        }

        const auto deployment_name{arg.substr(0, found)};
        if (const auto buried_deployment = GetBuriedDeployment(deployment_name)) {
            options.activation_heights[*buried_deployment] = height;
        } else {
            throw std::runtime_error(strprintf("Invalid name (%s) for -testactivationheight=name@height.", arg));
        }
    }

    if (!args.IsArgSet("-vbparams")) return;

    for (const std::string& strDeployment : args.GetArgs("-vbparams")) {
        std::vector<std::string> vDeploymentParams = SplitString(strDeployment, ':');
        if (vDeploymentParams.size() != 3 && vDeploymentParams.size() != 4 && vDeploymentParams.size() != 6 && vDeploymentParams.size() != 9) {
            throw std::runtime_error("Version bits parameters malformed, expecting "
                    "<deployment>:<start>:<end> or "
                    "<deployment>:<start>:<end>:<min_activation_height> or "
                    "<deployment>:<start>:<end>:<min_activation_height>:<window>:<threshold> or "
                    "<deployment>:<start>:<end>:<min_activation_height>:<window>:<thresholdstart>:<thresholdmin>:<falloffcoeff>:<useehf>");
        }
        CChainParams::VersionBitsParameters vbparams{};
        if (!ParseInt64(vDeploymentParams[1], &vbparams.start_time)) {
            throw std::runtime_error(strprintf("Invalid nStartTime (%s)", vDeploymentParams[1]));
        }
        if (!ParseInt64(vDeploymentParams[2], &vbparams.timeout)) {
            throw std::runtime_error(strprintf("Invalid nTimeout (%s)", vDeploymentParams[2]));
        }
        if (vDeploymentParams.size() >= 4) {
            if (!ParseInt32(vDeploymentParams[3], &vbparams.min_activation_height)) {
                throw std::runtime_error(strprintf("Invalid min_activation_height (%s)", vDeploymentParams[3]));
            }
        } else {
            vbparams.min_activation_height = 0;
        }
        if (vDeploymentParams.size() >= 6) {
            if (!ParseInt64(vDeploymentParams[4], &vbparams.window_size)) {
                throw std::runtime_error(strprintf("Invalid nWindowSize (%s)", vDeploymentParams[4]));
            }
            if (!ParseInt64(vDeploymentParams[5], &vbparams.threshold_start)) {
                throw std::runtime_error(strprintf("Invalid nThresholdStart (%s)", vDeploymentParams[5]));
            }
        }
        if (vDeploymentParams.size() == 9) {
            if (!ParseInt64(vDeploymentParams[6], &vbparams.threshold_min)) {
                throw std::runtime_error(strprintf("Invalid nThresholdMin (%s)", vDeploymentParams[6]));
            }
            if (!ParseInt64(vDeploymentParams[7], &vbparams.falloff_coeff)) {
                throw std::runtime_error(strprintf("Invalid nFalloffCoeff (%s)", vDeploymentParams[7]));
            }
            if (!ParseInt64(vDeploymentParams[8], &vbparams.use_ehf)) {
                throw std::runtime_error(strprintf("Invalid nUseEHF (%s)", vDeploymentParams[8]));
            }
        }
        bool found = false;
        for (int j=0; j < (int)Consensus::MAX_VERSION_BITS_DEPLOYMENTS; ++j) {
            if (vDeploymentParams[0] == VersionBitsDeploymentInfo[j].name) {
                // Optional Dash fields left at -1 keep any value set by an earlier -vbparams
                // for the same deployment, as UpdateVersionBitsParameters() did before.
                auto& entry = options.version_bits_parameters[Consensus::DeploymentPos(j)];
                if (vbparams.window_size == -1) vbparams.window_size = entry.window_size;
                if (vbparams.threshold_start == -1) vbparams.threshold_start = entry.threshold_start;
                if (vbparams.threshold_min == -1) vbparams.threshold_min = entry.threshold_min;
                if (vbparams.falloff_coeff == -1) vbparams.falloff_coeff = entry.falloff_coeff;
                if (vbparams.use_ehf == -1) vbparams.use_ehf = entry.use_ehf;
                entry = vbparams;
                found = true;
                LogPrintf("Setting version bits activation parameters for %s to start=%ld, timeout=%ld, min_activation_height=%ld, window=%ld, thresholdstart=%ld, thresholdmin=%ld, falloffcoeff=%ld, useehf=%ld\n",
                          vDeploymentParams[0], vbparams.start_time, vbparams.timeout, vbparams.min_activation_height, vbparams.window_size, vbparams.threshold_start, vbparams.threshold_min, vbparams.falloff_coeff, vbparams.use_ehf);
                break;
            }
        }
        if (!found) {
            throw std::runtime_error(strprintf("Invalid deployment (%s)", vDeploymentParams[0]));
        }
    }
}

static void ReadRegTestDIP3Args(const ArgsManager& args, CChainParams::RegTestOptions& options)
{
    if (!args.IsArgSet("-dip3params")) return;

    std::string strParams = args.GetArg("-dip3params", "");
    std::vector<std::string> vParams = SplitString(strParams, ':');
    if (vParams.size() != 2) {
        throw std::runtime_error("DIP3 parameters malformed, expecting <activation>:<enforcement>");
    }
    int nDIP3ActivationHeight, nDIP3EnforcementHeight;
    if (!ParseInt32(vParams[0], &nDIP3ActivationHeight)) {
        throw std::runtime_error(strprintf("Invalid activation height (%s)", vParams[0]));
    }
    if (!ParseInt32(vParams[1], &nDIP3EnforcementHeight)) {
        throw std::runtime_error(strprintf("Invalid enforcement height (%s)", vParams[1]));
    }
    LogPrintf("Setting DIP3 parameters to activation=%ld, enforcement=%ld\n", nDIP3ActivationHeight, nDIP3EnforcementHeight);
    options.dip3_parameters = CChainParams::DIP3Parameters{nDIP3ActivationHeight, nDIP3EnforcementHeight};
}

static void ReadRegTestBudgetArgs(const ArgsManager& args, CChainParams::RegTestOptions& options)
{
    if (!args.IsArgSet("-budgetparams")) return;

    std::string strParams = args.GetArg("-budgetparams", "");
    std::vector<std::string> vParams = SplitString(strParams, ':');
    if (vParams.size() != 3) {
        throw std::runtime_error("Budget parameters malformed, expecting <masternode>:<budget>:<superblock>");
    }
    int nMasternodePaymentsStartBlock, nBudgetPaymentsStartBlock, nSuperblockStartBlock;
    if (!ParseInt32(vParams[0], &nMasternodePaymentsStartBlock)) {
        throw std::runtime_error(strprintf("Invalid masternode start height (%s)", vParams[0]));
    }
    if (!ParseInt32(vParams[1], &nBudgetPaymentsStartBlock)) {
        throw std::runtime_error(strprintf("Invalid budget start block (%s)", vParams[1]));
    }
    if (!ParseInt32(vParams[2], &nSuperblockStartBlock)) {
        throw std::runtime_error(strprintf("Invalid superblock start height (%s)", vParams[2]));
    }
    LogPrintf("Setting budget parameters to masternode=%ld, budget=%ld, superblock=%ld\n", nMasternodePaymentsStartBlock, nBudgetPaymentsStartBlock, nSuperblockStartBlock);
    options.budget_parameters = CChainParams::BudgetParameters{nMasternodePaymentsStartBlock, nBudgetPaymentsStartBlock, nSuperblockStartBlock};
}

static void ReadRegTestLLMQTestArgs(const ArgsManager& args, CChainParams::RegTestOptions& options, const Consensus::LLMQType llmqType)
{
    assert(llmqType == Consensus::LLMQType::LLMQ_TEST || llmqType == Consensus::LLMQType::LLMQ_TEST_INSTANTSEND || llmqType == Consensus::LLMQType::LLMQ_TEST_PLATFORM);

    std::string cmd_param{"-llmqtestparams"}, llmq_name{"LLMQ_TEST"};
    if (llmqType == Consensus::LLMQType::LLMQ_TEST_INSTANTSEND) {
        cmd_param = "-llmqtestinstantsendparams";
        llmq_name = "LLMQ_TEST_INSTANTSEND";
    }
    if (llmqType == Consensus::LLMQType::LLMQ_TEST_PLATFORM) {
        cmd_param = "-llmqtestplatformparams";
        llmq_name = "LLMQ_TEST_PLATFORM";
    }

    if (!args.IsArgSet(cmd_param)) return;

    std::string strParams = args.GetArg(cmd_param, "");
    std::vector<std::string> vParams = SplitString(strParams, ':');
    if (vParams.size() != 2) {
        throw std::runtime_error(strprintf("%s parameters malformed, expecting <size>:<threshold>", llmq_name));
    }
    int size, threshold;
    if (!ParseInt32(vParams[0], &size)) {
        throw std::runtime_error(strprintf("Invalid %s size (%s)", llmq_name, vParams[0]));
    }
    if (!ParseInt32(vParams[1], &threshold)) {
        throw std::runtime_error(strprintf("Invalid %s threshold (%s)", llmq_name, vParams[1]));
    }
    if (size > Consensus::MAX_LLMQ_SIZE) {
        throw std::runtime_error(strprintf("Invalid %s size (%d), must not exceed the maximum supported quorum size (%d)",
                                           llmq_name, size, Consensus::MAX_LLMQ_SIZE));
    }
    LogPrintf("Setting %s parameters to size=%ld, threshold=%ld\n", llmq_name, size, threshold);
    options.llmq_test_parameters[llmqType] = CChainParams::LLMQParameters{size, threshold};
}

static void ReadRegTestLLMQInstantSendDIP0024Args(const ArgsManager& args, CChainParams::RegTestOptions& options)
{
    if (!args.IsArgSet("-llmqtestinstantsenddip0024")) return;

    options.llmq_dip0024_instantsend = args.GetArg("-llmqtestinstantsenddip0024", "");
}

void ReadRegTestArgs(const ArgsManager& args, CChainParams::RegTestOptions& options)
{
    if (auto value = args.GetBoolArg("-fastprune")) options.fastprune = *value;

    ReadRegTestActivationArgs(args, options);
    ReadRegTestDIP3Args(args, options);
    ReadRegTestBudgetArgs(args, options);
    ReadRegTestLLMQTestArgs(args, options, Consensus::LLMQType::LLMQ_TEST);
    ReadRegTestLLMQTestArgs(args, options, Consensus::LLMQType::LLMQ_TEST_INSTANTSEND);
    ReadRegTestLLMQTestArgs(args, options, Consensus::LLMQType::LLMQ_TEST_PLATFORM);
    ReadRegTestLLMQInstantSendDIP0024Args(args, options);
}

static void ReadDevNetSubsidyAndDiffArgs(const ArgsManager& args, CChainParams::DevNetOptions& options)
{
    if (args.IsArgSet("-minimumdifficultyblocks")) {
        options.minimum_difficulty_blocks = args.GetIntArg("-minimumdifficultyblocks", 0);
        LogPrintf("Setting minimumdifficultyblocks=%ld\n", *options.minimum_difficulty_blocks);
    }
    if (args.IsArgSet("-highsubsidyblocks")) {
        options.high_subsidy_blocks = args.GetIntArg("-highsubsidyblocks", 0);
        LogPrintf("Setting highsubsidyblocks=%ld\n", *options.high_subsidy_blocks);
    }
    if (args.IsArgSet("-highsubsidyfactor")) {
        options.high_subsidy_factor = args.GetIntArg("-highsubsidyfactor", 1);
        LogPrintf("Setting highsubsidyfactor=%ld\n", *options.high_subsidy_factor);
    }
}

static void ReadDevNetPowTargetSpacingArgs(const ArgsManager& args, CChainParams::DevNetOptions& options)
{
    if (!args.IsArgSet("-powtargetspacing")) return;

    std::string strPowTargetSpacing = args.GetArg("-powtargetspacing", "");

    int64_t powTargetSpacing;
    if (!ParseInt64(strPowTargetSpacing, &powTargetSpacing)) {
        throw std::runtime_error(strprintf("Invalid parsing of powTargetSpacing (%s)", strPowTargetSpacing));
    }

    if (powTargetSpacing < 1) {
        throw std::runtime_error(strprintf("Invalid value of powTargetSpacing (%s)", strPowTargetSpacing));
    }

    LogPrintf("Setting powTargetSpacing to %ld\n", powTargetSpacing);
    options.pow_target_spacing = powTargetSpacing;
}

static void ReadDevNetLLMQDevnetParamsArgs(const ArgsManager& args, CChainParams::DevNetOptions& options)
{
    if (!args.IsArgSet("-llmqdevnetparams")) return;

    std::string strParams = args.GetArg("-llmqdevnetparams", "");
    std::vector<std::string> vParams = SplitString(strParams, ':');
    if (vParams.size() != 2) {
        throw std::runtime_error("LLMQ_DEVNET parameters malformed, expecting <size>:<threshold>");
    }
    int size, threshold;
    if (!ParseInt32(vParams[0], &size)) {
        throw std::runtime_error(strprintf("Invalid LLMQ_DEVNET size (%s)", vParams[0]));
    }
    if (!ParseInt32(vParams[1], &threshold)) {
        throw std::runtime_error(strprintf("Invalid LLMQ_DEVNET threshold (%s)", vParams[1]));
    }
    if (size > Consensus::MAX_LLMQ_SIZE) {
        throw std::runtime_error(strprintf("Invalid LLMQ_DEVNET size (%d), must not exceed the maximum supported quorum size (%d)",
                                           size, Consensus::MAX_LLMQ_SIZE));
    }
    LogPrintf("Setting LLMQ_DEVNET parameters to size=%ld, threshold=%ld\n", size, threshold);
    options.llmq_devnet_parameters = CChainParams::LLMQParameters{size, threshold};
}

void ReadDevNetArgs(const ArgsManager& args, CChainParams::DevNetOptions& options)
{
    options.name = args.GetDevNetName();

    ReadDevNetSubsidyAndDiffArgs(args, options);
    if (args.IsArgSet("-llmqchainlocks")) {
        options.llmq_chainlocks = args.GetArg("-llmqchainlocks", "");
    }
    if (args.IsArgSet("-llmqinstantsenddip0024")) {
        options.llmq_dip0024_instantsend = args.GetArg("-llmqinstantsenddip0024", "");
    }
    if (args.IsArgSet("-llmqplatform")) {
        options.llmq_platform = args.GetArg("-llmqplatform", "");
    }
    if (args.IsArgSet("-llmqmnhf")) {
        options.llmq_mnhf = args.GetArg("-llmqmnhf", "");
    }
    ReadDevNetLLMQDevnetParamsArgs(args, options);
    ReadDevNetPowTargetSpacingArgs(args, options);
}

static std::unique_ptr<const CChainParams> globalChainParams;

const CChainParams &Params() {
    assert(globalChainParams);
    return *globalChainParams;
}

std::unique_ptr<const CChainParams> CreateChainParams(const ArgsManager& args, const std::string& chain)
{
    if (chain == CBaseChainParams::MAIN) {
        return CChainParams::Main();
    } else if (chain == CBaseChainParams::TESTNET) {
        return CChainParams::TestNet();
    } else if (chain == CBaseChainParams::DEVNET) {
        auto opts = CChainParams::DevNetOptions{};
        ReadDevNetArgs(args, opts);
        return CChainParams::DevNet(opts);
    } else if (chain == CBaseChainParams::REGTEST) {
        auto opts = CChainParams::RegTestOptions{};
        ReadRegTestArgs(args, opts);
        return CChainParams::RegTest(opts);
    }
    throw std::runtime_error(strprintf("%s: Unknown chain %s.", __func__, chain));
}

void SelectParams(const std::string& network)
{
    SelectBaseParams(network);
    globalChainParams = CreateChainParams(gArgs, network);
}

void SetupChainParamsOptions(ArgsManager& argsman)
{
    SetupChainParamsBaseOptions(argsman);

    argsman.AddArg("-budgetparams=<masternode>:<budget>:<superblock>", "Override masternode, budget and superblock start heights (regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-dip3params=<activation>:<enforcement>", "Override DIP3 activation and enforcement heights (regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-highsubsidyblocks=<n>", "The number of blocks with a higher than normal subsidy to mine at the start of a chain. Block after that height will have fixed subsidy base. (default: 0, devnet-only)", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-highsubsidyfactor=<n>", "The factor to multiply the normal block subsidy by while in the highsubsidyblocks window of a chain (default: 1, devnet-only)", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-llmqchainlocks=<quorum name>", "Override the default LLMQ type used for ChainLocks. Allows using ChainLocks with smaller LLMQs. (default: llmq_devnet, devnet-only)", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-llmqdevnetparams=<size>:<threshold>", "Override the default LLMQ size for the LLMQ_DEVNET quorum (devnet-only)", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-llmqinstantsenddip0024=<quorum name>", "Override the default LLMQ type used for InstantSendDIP0024. (default: llmq_devnet_dip0024, devnet-only)", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-llmqplatform=<quorum name>", "Override the default LLMQ type used for Platform. (default: llmq_devnet_platform, devnet-only)", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-llmqmnhf=<quorum name>", "Override the default LLMQ type used for EHF. (default: llmq_devnet, devnet-only)", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-llmqtestinstantsenddip0024=<quorum name>", "Override the default LLMQ type used for InstantSendDIP0024. Used mainly to test Platform. (default: llmq_test_dip0024, regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-llmqtestinstantsendparams=<size>:<threshold>", "Override the default LLMQ size for the LLMQ_TEST_INSTANTSEND quorums (default: 3:2, regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-llmqtestparams=<size>:<threshold>", "Override the default LLMQ size for the LLMQ_TEST quorum (default: 3:2, regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-llmqtestplatformparams=<size>:<threshold>", "Override the default LLMQ size for the LLMQ_TEST_PLATFORM quorum (default: 3:2, regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-minimumdifficultyblocks=<n>", "The number of blocks that can be mined with the minimum difficulty at the start of a chain (default: 0, devnet-only)", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-powtargetspacing=<n>", "Override the default PowTargetSpacing value in seconds (default: 2.5 minutes, devnet-only)", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-testactivationheight=name@height.", "Set the activation height of 'name' (bip147, bip34, dersig, cltv, csv, brr, dip0001, dip0008, dip0024, v19, v20, mn_rr). (regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-vbparams=<deployment>:<start>:<end>(:min_activation_height(:<window>:<threshold/thresholdstart>(:<thresholdmin>:<falloffcoeff>:<mnactivation>)))",
                 "Use given start/end times and min_activation_height for specified version bits deployment (regtest-only). "
                 "Specifying window, threshold/thresholdstart, thresholdmin, falloffcoeff and mnactivation is optional.", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
}
