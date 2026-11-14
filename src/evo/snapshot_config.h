// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_SNAPSHOT_CONFIG_H
#define BITCOIN_EVO_SNAPSHOT_CONFIG_H

#include <consensus/params.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <string>
#include <vector>

class CChainParams;

/** The effective consensus and deployment configuration that stored Evo state
 *  depends on. Deployment and LLMQ parameters decide which raw records (MNHF
 *  signals, commitments, credit-pool windows) were stored, so a snapshot
 *  generation is valid only for the configuration it was published under.
 *
 *  Fingerprint() is SHA256 over this canonical serialization, led by
 *  fingerprint_version. Bump FINGERPRINT_VERSION whenever the covered set
 *  changes. */
struct EvoSnapshotConsensusConfig {
    static constexpr uint16_t FINGERPRINT_VERSION{2};

    uint16_t fingerprint_version{FINGERPRINT_VERSION};
    std::vector<Consensus::BIP9Deployment> deployments;
    uint32_t miner_confirmation_window{0};
    uint32_t rule_change_activation_threshold{0};
    int32_t min_bip9_warning_height{0};
    //! DeploymentHeight() of every BuriedDeployment, in enum order.
    std::vector<int32_t> buried_heights;
    int32_t dip3_enforcement_height{0};
    int32_t dip24_quorums_height{0};
    std::vector<Consensus::LLMQParams> llmqs;
    std::vector<uint8_t> llmq_roles;
    int32_t credit_pool_period_blocks{0};
    //! The block DIP3 enforcement requires at its height.
    uint256 dip3_enforcement_hash;
    int32_t masternode_minimum_confirmations{0};
    //! Inputs of the block subsidy and masternode reward, which the credit
    //! pool balance is checked against.
    int32_t subsidy_halving_interval{0};
    int32_t budget_payments_start_block{0};
    int32_t high_subsidy_blocks{0};
    int32_t high_subsidy_factor{0};
    int32_t superblock_cycle{0};
    int32_t masternode_payments_increase_block{0};
    int32_t masternode_payments_increase_period{0};
    //! Network-specific rules in quorum membership, quorum type enablement and
    //! masternode service validation.
    std::string network_id;
    bool require_routable_external_ip{false};

    static EvoSnapshotConsensusConfig FromParams(const CChainParams& params);
    uint256 Fingerprint() const;

    SERIALIZE_METHODS(EvoSnapshotConsensusConfig, obj)
    {
        READWRITE(obj.fingerprint_version);
        READWRITE(Using<VectorFormatter<DeploymentFormatter>>(obj.deployments));
        READWRITE(obj.miner_confirmation_window, obj.rule_change_activation_threshold, obj.min_bip9_warning_height);
        READWRITE(obj.buried_heights, obj.dip3_enforcement_height, obj.dip24_quorums_height);
        READWRITE(Using<VectorFormatter<LLMQParamsFormatter>>(obj.llmqs));
        READWRITE(obj.llmq_roles, obj.credit_pool_period_blocks);
        READWRITE(obj.dip3_enforcement_hash, obj.masternode_minimum_confirmations);
        READWRITE(obj.subsidy_halving_interval, obj.budget_payments_start_block, obj.high_subsidy_blocks,
                  obj.high_subsidy_factor, obj.superblock_cycle, obj.masternode_payments_increase_block,
                  obj.masternode_payments_increase_period);
        READWRITE(obj.network_id, obj.require_routable_external_ip);
    }

private:
    struct DeploymentFormatter {
        template <typename Stream>
        static void Ser(Stream& s, const Consensus::BIP9Deployment& d)
        {
            s << int32_t{d.bit} << d.nStartTime << d.nTimeout << int32_t{d.min_activation_height} << d.nWindowSize
              << d.nThresholdStart << d.nThresholdMin << d.nFalloffCoeff << d.useEHF;
        }
        template <typename Stream>
        static void Unser(Stream&, Consensus::BIP9Deployment&)
        {
            static_assert(!sizeof(Stream*), "fingerprint input is serialize-only");
        }
    };
    struct LLMQParamsFormatter {
        template <typename Stream>
        static void Ser(Stream& s, const Consensus::LLMQParams& p)
        {
            s << static_cast<uint8_t>(p.type) << std::string{p.name} << p.useRotation << int32_t{p.size}
              << int32_t{p.minSize} << int32_t{p.threshold} << int32_t{p.dkgInterval} << int32_t{p.dkgPhaseBlocks}
              << int32_t{p.dkgMiningWindowStart} << int32_t{p.dkgMiningWindowEnd} << int32_t{p.dkgBadVotesThreshold}
              << int32_t{p.signingActiveQuorumCount} << int32_t{p.keepOldConnections} << int32_t{p.keepOldKeys}
              << int32_t{p.recoveryMembers};
        }
        template <typename Stream>
        static void Unser(Stream&, Consensus::LLMQParams&)
        {
            static_assert(!sizeof(Stream*), "fingerprint input is serialize-only");
        }
    };
};

#endif // BITCOIN_EVO_SNAPSHOT_CONFIG_H
