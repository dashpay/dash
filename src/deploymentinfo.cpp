// Copyright (c) 2016-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <deploymentinfo.h>

#include <consensus/params.h>

#include <string_view>

const struct VBDeploymentInfo VersionBitsDeploymentInfo[Consensus::MAX_VERSION_BITS_DEPLOYMENTS] = {
    {
        /*.name =*/ "testdummy",
        /*.gbt_force =*/ true,
    },
    {
        /*.name =*/"v24",
        /*.gbt_force =*/true,
    },
    {
        /*.name =*/"evo_shares",
        /*.gbt_force =*/true,
    },
};

std::string DeploymentName(Consensus::BuriedDeployment dep)
{
    assert(ValidDeployment(dep));
    switch (dep) {
    case Consensus::DEPLOYMENT_HEIGHTINCB:
        return "bip34";
    case Consensus::DEPLOYMENT_CLTV:
        return "bip65";
    case Consensus::DEPLOYMENT_DERSIG:
        return "bip66";
    case Consensus::DEPLOYMENT_BIP147:
        return "bip147";
    case Consensus::DEPLOYMENT_CSV:
        return "csv";
    case Consensus::DEPLOYMENT_DIP0001:
        return "dip0001";
    case Consensus::DEPLOYMENT_DIP0003:
        return "dip0003";
    case Consensus::DEPLOYMENT_DIP0008:
        return "dip0008";
    case Consensus::DEPLOYMENT_DIP0020:
        return "dip0020";
    case Consensus::DEPLOYMENT_DIP0024:
        return "dip0024";
    case Consensus::DEPLOYMENT_BRR:
        return "realloc";
    case Consensus::DEPLOYMENT_V19:
        return "v19";
    case Consensus::DEPLOYMENT_V20:
        return "v20";
    case Consensus::DEPLOYMENT_MN_RR:
        return "mn_rr";
    case Consensus::DEPLOYMENT_WITHDRAWALS:
        return "withdrawals";
    } // no default case, so the compiler can warn about missing cases
    return "";
}

std::optional<Consensus::BuriedDeployment> GetBuriedDeployment(const std::string_view name)
{
    if (name == "bip147") {
        return Consensus::BuriedDeployment::DEPLOYMENT_BIP147;
    } else if (name == "bip34") {
        return Consensus::BuriedDeployment::DEPLOYMENT_HEIGHTINCB;
    } else if (name == "dersig") {
        return Consensus::BuriedDeployment::DEPLOYMENT_DERSIG;
    } else if (name == "cltv") {
        return Consensus::BuriedDeployment::DEPLOYMENT_CLTV;
    } else if (name == "csv") {
        return Consensus::BuriedDeployment::DEPLOYMENT_CSV;
    } else if (name == "brr") {
        return Consensus::BuriedDeployment::DEPLOYMENT_BRR;
    } else if (name == "dip0001") {
        return Consensus::BuriedDeployment::DEPLOYMENT_DIP0001;
    } else if (name == "dip0008") {
        return Consensus::BuriedDeployment::DEPLOYMENT_DIP0008;
    } else if (name == "dip0024") {
        return Consensus::BuriedDeployment::DEPLOYMENT_DIP0024;
    } else if (name == "v19") {
        return Consensus::BuriedDeployment::DEPLOYMENT_V19;
    } else if (name == "v20") {
        return Consensus::BuriedDeployment::DEPLOYMENT_V20;
    } else if (name == "mn_rr") {
        return Consensus::BuriedDeployment::DEPLOYMENT_MN_RR;
    }
    return std::nullopt;
}
