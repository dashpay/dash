// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/snapshot_config.h>

#include <chainparams.h>
#include <hash.h>

#include <limits>

EvoSnapshotConsensusConfig EvoSnapshotConsensusConfig::FromParams(const CChainParams& params)
{
    const Consensus::Params& consensus{params.GetConsensus()};
    EvoSnapshotConsensusConfig config;
    config.deployments.assign(std::begin(consensus.vDeployments), std::end(consensus.vDeployments));
    config.miner_confirmation_window = consensus.nMinerConfirmationWindow;
    config.rule_change_activation_threshold = consensus.nRuleChangeActivationThreshold;
    config.min_bip9_warning_height = consensus.MinBIP9WarningHeight;
    for (int dep{std::numeric_limits<int16_t>::min()};
         Consensus::ValidDeployment(static_cast<Consensus::BuriedDeployment>(dep)); ++dep) {
        config.buried_heights.push_back(consensus.DeploymentHeight(static_cast<Consensus::BuriedDeployment>(dep)));
    }
    config.dip3_enforcement_height = consensus.DIP0003EnforcementHeight;
    config.dip24_quorums_height = consensus.DIP0024QuorumsHeight;
    config.llmqs = consensus.llmqs;
    for (const Consensus::LLMQType role : {consensus.llmqTypeChainLocks, consensus.llmqTypeDIP0024InstantSend,
                                           consensus.llmqTypePlatform, consensus.llmqTypeMnhf}) {
        config.llmq_roles.push_back(static_cast<uint8_t>(role));
    }
    config.credit_pool_period_blocks = params.CreditPoolPeriodBlocks();
    return config;
}

uint256 EvoSnapshotConsensusConfig::Fingerprint() const
{
    HashWriter writer{};
    writer << *this;
    return writer.GetSHA256();
}
