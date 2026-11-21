// Copyright (c) 2020-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls/bls.h>
#include <chainlock/chainlock.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <evo/deterministicmns.h>
#include <evo/evodb.h>
#include <evo/providertx.h>
#include <evo/snapshot_config.h>
#include <evo/snapshot_types.h>
#include <governance/governance.h>
#include <governance/object.h>
#include <instantsend/instantsend.h>
#include <instantsend/lock.h>
#include <interfaces/chain.h>
#include <interfaces/node.h>
#include <llmq/commitment.h>
#include <llmq/context.h>
#include <llmq/params.h>
#include <logging.h>
#include <script/standard.h>
#include <spork.h>
#include <streams.h>
#include <sync.h>
#include <test/util/llmq_tests.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <util/std23.h>
#include <util/strencodings.h>
#include <util/translation.h>
#include <validation.h>
#include <version.h>
#include <warnings.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <boost/test/unit_test.hpp>

using interfaces::FoundBlock;

BOOST_FIXTURE_TEST_SUITE(interfaces_tests, TestChain100Setup)

BOOST_AUTO_TEST_CASE(findBlock)
{
    LOCK(Assert(m_node.chainman)->GetMutex());
    auto& chain = m_node.chain;
    const CChain& active = Assert(m_node.chainman)->ActiveChain();

    uint256 hash;
    BOOST_CHECK(chain->findBlock(active[10]->GetBlockHash(), FoundBlock().hash(hash)));
    BOOST_CHECK_EQUAL(hash, active[10]->GetBlockHash());

    int height = -1;
    BOOST_CHECK(chain->findBlock(active[20]->GetBlockHash(), FoundBlock().height(height)));
    BOOST_CHECK_EQUAL(height, active[20]->nHeight);

    CBlock data;
    BOOST_CHECK(chain->findBlock(active[30]->GetBlockHash(), FoundBlock().data(data)));
    BOOST_CHECK_EQUAL(data.GetHash(), active[30]->GetBlockHash());

    int64_t time = -1;
    BOOST_CHECK(chain->findBlock(active[40]->GetBlockHash(), FoundBlock().time(time)));
    BOOST_CHECK_EQUAL(time, active[40]->GetBlockTime());

    int64_t max_time = -1;
    BOOST_CHECK(chain->findBlock(active[50]->GetBlockHash(), FoundBlock().maxTime(max_time)));
    BOOST_CHECK_EQUAL(max_time, active[50]->GetBlockTimeMax());

    int64_t mtp_time = -1;
    BOOST_CHECK(chain->findBlock(active[60]->GetBlockHash(), FoundBlock().mtpTime(mtp_time)));
    BOOST_CHECK_EQUAL(mtp_time, active[60]->GetMedianTimePast());

    bool cur_active{false}, next_active{false};
    uint256 next_hash;
    BOOST_CHECK_EQUAL(active.Height(), 100);
    BOOST_CHECK(chain->findBlock(active[99]->GetBlockHash(), FoundBlock().inActiveChain(cur_active).nextBlock(FoundBlock().inActiveChain(next_active).hash(next_hash))));
    BOOST_CHECK(cur_active);
    BOOST_CHECK(next_active);
    BOOST_CHECK_EQUAL(next_hash, active[100]->GetBlockHash());
    cur_active = next_active = false;
    BOOST_CHECK(chain->findBlock(active[100]->GetBlockHash(), FoundBlock().inActiveChain(cur_active).nextBlock(FoundBlock().inActiveChain(next_active))));
    BOOST_CHECK(cur_active);
    BOOST_CHECK(!next_active);

    BOOST_CHECK(!chain->findBlock({}, FoundBlock()));
}

BOOST_AUTO_TEST_CASE(providerTxCapabilities)
{
    auto node{interfaces::MakeNode(m_node)};
    const auto capabilities{node->evo().getProviderTxCapabilities().value()};
    const ActiveEvoViewScope turn{m_node.evodb.get()};
    const uint16_t expected_version{
        WITH_LOCK(::cs_main, return DeploymentToProtxVersion(Assert(m_node.chainman)->ActiveChain().Tip(),
                                                             *Assert(m_node.chainman)))};
    BOOST_CHECK_EQUAL(capabilities.version, expected_version);
    BOOST_CHECK_EQUAL(capabilities.extended_addresses, expected_version >= ProTxVersion::ExtAddr);
    BOOST_CHECK(!capabilities.ipv4_optional); // Not active on regtest by default
}

struct IPv4OptionalChainSetup : public TestChain100Setup {
    IPv4OptionalChainSetup()
        : TestChain100Setup{ChainType::REGTEST, {"-testactivationheight=ipv4_optional@1"}} {}
};

BOOST_FIXTURE_TEST_CASE(providerTxCapabilitiesIPv4Optional, IPv4OptionalChainSetup)
{
    const auto capabilities{interfaces::MakeNode(m_node)->evo().getProviderTxCapabilities()};
    BOOST_REQUIRE(capabilities);
    BOOST_CHECK(capabilities->ipv4_optional);
}

BOOST_AUTO_TEST_CASE(providerNetInfoValidationDoesNotWaitForChain)
{
    // Dialogs validate endpoints on every keystroke, so that must not wait for cs_main
    auto node{interfaces::MakeNode(m_node)};
    const interfaces::ProviderNetInfo net_info{.core_p2p = {"1.1.1.1:9998"},
                                               .platform_p2p = std::vector<std::string>{"1.1.1.1:22200"},
                                               .platform_https = std::vector<std::string>{"1.1.1.1:443"}};
    std::promise<void> locked;
    std::promise<void> release;
    bool lock_timed_out{false};
    std::thread holder{[&] {
        LOCK(::cs_main);
        locked.set_value();
        lock_timed_out = release.get_future().wait_for(std::chrono::seconds{10}) == std::future_status::timeout;
    }};
    locked.get_future().wait();
    const auto error{node->evo().validateProviderNetInfo(net_info, MnType::Evo, ProTxVersion::ExtAddr,
                                                         /*ipv4_optional=*/false, /*optional=*/false)};
    release.set_value();
    holder.join();
    BOOST_CHECK(!lock_timed_out);
    BOOST_CHECK(!error);
}

// When this node cannot read its Evo state during dual operation, GUI-facing
// operations report it through their own failure channel and the misc
// warning; the error never reaches the caller.
BOOST_AUTO_TEST_CASE(evo_state_unavailable_is_contained)
{
    auto node{interfaces::MakeNode(m_node)};
    CEvoDB& evodb{*Assert(m_node.evodb)};
    const CBlockIndex* tip{WITH_LOCK(::cs_main, return Assert(m_node.chainman)->ActiveChain().Tip())};
    NotifyMNListTip(*Assert(m_node.dmnman), tip);
    auto& consensus{const_cast<Consensus::Params&>(Params().GetConsensus())};
    const auto list_unavailable{
        [](const CBlockIndex*) { throw BlockDataUnavailableError("masternode list unavailable"); }};

    // Without a snapshot, the list can be missing only because block data was
    // pruned, and the warning says so.
    {
        const int dip3_height{std::exchange(consensus.DIP0003Height, 1)};
        Assert(m_node.dmnman)->SetListSnapshotMissHookForTesting(list_unavailable);
        BOOST_CHECK(!node->evo().getListAtChainTip().first);
        Assert(m_node.dmnman)->SetListSnapshotMissHookForTesting(nullptr);
        consensus.DIP0003Height = dip3_height;
        BOOST_CHECK(GetWarnings(/*verbose=*/true).original.find("Block data was pruned; restart with -reindex") !=
                    std::string::npos);
        SetMiscWarning(Untranslated(""));
    }

    BOOST_REQUIRE(evodb.CreateSnapshotGeneration(tip->GetBlockHash(), tip->nHeight));
    BOOST_REQUIRE(
        evodb.PublishSnapshotGeneration(EvoSnapshotConsensusConfig::FromParams(Params()).Fingerprint(), std::nullopt));

    // With DIP3 counted from genesis the tip's masternode list must be read
    // from EvoDB, and that read fails as unavailable.
    const int dip3_height{std::exchange(consensus.DIP0003Height, 1)};
    Assert(m_node.dmnman)->SetListSnapshotMissHookForTesting(list_unavailable);
    interfaces::GOV::GovernanceInfo info;
    BOOST_CHECK_NO_THROW(info = node->gov().getGovernanceInfo());
    BOOST_CHECK(!info.fundingthreshold.has_value());
    BOOST_CHECK_EQUAL(info.superblockcycle, Params().GetConsensus().nSuperblockCycle);
    BOOST_CHECK(!node->gov().getGovernanceInfo().fundingthreshold.has_value());
    BOOST_CHECK(GetWarnings(/*verbose=*/true).original.find("Snapshot state unavailable") != std::string::npos);
    // Polling the same operation again does not raise the warning again.
    SetMiscWarning(Untranslated(""));
    BOOST_CHECK(!node->gov().getGovernanceInfo().fundingthreshold.has_value());
    BOOST_CHECK(GetWarnings(/*verbose=*/true).original.find("Snapshot state unavailable") == std::string::npos);

    std::optional<std::vector<COutPoint>> collaterals{std::vector<COutPoint>{}};
    BOOST_CHECK_NO_THROW(collaterals = m_node.chain->listMNCollaterials({}));
    BOOST_CHECK(!collaterals.has_value());

    // An endpoint the masternode list cannot be checked against is not reported free.
    const interfaces::ProviderNetInfo net_info{.core_p2p = {strprintf("1.2.3.4:%d", Params().GetDefaultPort())}, .platform_p2p = {}, .platform_https = {}};
    const auto find_in_use{[&] {
        return node->evo().findProviderNetInfoInUse(net_info, MnType::Regular, ProTxVersion::BasicBLS,
                                                    /*ipv4_optional=*/false);
    }};
    interfaces::ProviderTxResult<std::optional<std::string>> in_use{std::optional<std::string>{}};
    BOOST_CHECK_NO_THROW(in_use = find_in_use());
    BOOST_CHECK(std::holds_alternative<interfaces::ProviderTxError>(in_use));

    // The current votes on a known object depend on the masternode list too.
    m_node.govman = std::make_unique<CGovernanceManager>(m_args.GetDataDirNet(), *m_node.mn_metaman, *m_node.chainman,
                                                         *m_node.sbman, *m_node.dmnman, *m_node.mn_sync,
                                                         /*collateral=*/nullptr);
    const CGovernanceObject proposal{uint256(), /*nRevisionIn=*/1, GetTime(), GetRandHash(), /*strDataHexIn=*/{}};
    m_node.govman->AddGovernanceObjectForTesting(proposal);
    std::optional<std::vector<CGovernanceVote>> votes{std::vector<CGovernanceVote>{}};
    BOOST_CHECK_NO_THROW(votes = node->gov().getCurrentVotes(proposal.GetHash()));
    BOOST_CHECK(!votes.has_value());

    Assert(m_node.dmnman)->SetListSnapshotMissHookForTesting(nullptr);
    consensus.DIP0003Height = dip3_height;

    // Once the list can be read again, the endpoint is checked, and it is free.
    in_use = find_in_use();
    BOOST_REQUIRE(std::holds_alternative<std::optional<std::string>>(in_use));
    BOOST_CHECK(!std::get<std::optional<std::string>>(in_use).has_value());
    SetMiscWarning(Untranslated(""));
}

// Every interfaces call that reads Evo state binds the view it serves: called
// from a thread that binds nothing, under -checkevobinding, none of them fails
// with, or reports, an unbound access, with one view or with two.
BOOST_AUTO_TEST_CASE(evo_reads_bind_from_an_unbound_thread)
{
    auto node{interfaces::MakeNode(m_node)};
    CEvoDB& evodb{*Assert(m_node.evodb)};
    const CBlockIndex* tip{WITH_LOCK(::cs_main, return Assert(m_node.chainman)->ActiveChain().Tip())};
    NotifyMNListTip(*Assert(m_node.dmnman), tip);
    m_node.govman = std::make_unique<CGovernanceManager>(m_args.GetDataDirNet(), *m_node.mn_metaman, *m_node.chainman,
                                                         *m_node.sbman, *m_node.dmnman, *m_node.mn_sync,
                                                         /*collateral=*/nullptr);
    const CGovernanceObject proposal{uint256(), /*nRevisionIn=*/1, GetTime(), GetRandHash(), /*strDataHexIn=*/{}};
    m_node.govman->AddGovernanceObjectForTesting(proposal);
    const interfaces::ProviderNetInfo net_info{.core_p2p = {strprintf("1.2.3.4:%d", Params().GetDefaultPort())}, .platform_p2p = {}, .platform_https = {}};
    const auto platform_type{Params().GetConsensus().llmqTypePlatform};
    const auto platform_qc{llmq::testutils::CreateValidCommitment(llmq::testutils::GetLLMQParams(platform_type),
                                                                  tip->pprev->GetBlockHash())};
    {
        LOCK(::cs_main);
        auto db_tx{evodb.BeginTransaction()};
        llmq::testutils::WriteMinedCommitment(evodb, platform_qc, tip->GetBlockHash(), /*mined_height=*/tip->nHeight,
                                              /*quorum_height=*/tip->nHeight - 1);
        db_tx->Commit();
    }

    Mutex log_mutex;
    std::vector<std::string> unbound_logged;
    const auto log_callback{LogInstance().PushBackCallback([&](const std::string& line) {
        if (line.find("unbound Evo access") != std::string::npos) WITH_LOCK(log_mutex, unbound_logged.push_back(line));
    })};

    // Returns the calls that threw, and, with `expect_state`, those that
    // reported the Evo state unavailable.
    const auto sweep = [&](bool expect_state) {
        std::vector<std::string> failed;
        std::thread{[&] {
            const auto call = [&](const char* name, auto&& fn) {
                try {
                    if (!fn() && expect_state) failed.emplace_back(strprintf("%s: unavailable", name));
                } catch (const std::exception& e) {
                    failed.emplace_back(strprintf("%s: %s", name, e.what()));
                }
            };
            call("getListAtChainTip", [&] { node->evo().getListAtChainTip(); return true; });
            call("getProviderTxCapabilities", [&] { return node->evo().getProviderTxCapabilities().has_value(); });
            call("findProviderNetInfoInUse", [&] {
                return std::holds_alternative<std::optional<std::string>>(node->evo().findProviderNetInfoInUse(
                    net_info, MnType::Regular, ProTxVersion::BasicBLS, /*ipv4_optional=*/false));
            });
            call("getAllNewerThan", [&] {
                std::vector<CGovernanceObject> objs;
                node->gov().getAllNewerThan(objs, 0);
                return true;
            });
            call("getObjVotes", [&] { return node->gov().getObjVotes(proposal, VOTE_SIGNAL_FUNDING).has_value(); });
            call("getObjUniqueVoters",
                 [&] { return node->gov().getObjUniqueVoters(proposal, VOTE_SIGNAL_FUNDING).has_value(); });
            call("getCurrentVotes", [&] { return node->gov().getCurrentVotes(proposal.GetHash()).has_value(); });
            call("getGovernanceInfo", [&] { return node->gov().getGovernanceInfo().fundingthreshold.has_value(); });
            call("getProposalFundedHeight", [&] { node->gov().getProposalFundedHeight(proposal.GetHash()); return true; });
            call("getFundableProposalHashes", [&] { return node->gov().getFundableProposalHashes().has_value(); });
            call("getBestChainLock", [&] { node->llmq().getBestChainLock(); return true; });
            call("getCreditPoolCounts", [&] { return node->llmq().getCreditPoolCounts().has_value(); });
            call("getInstantSendCounts", [&] { node->llmq().getInstantSendCounts(); return true; });
            call("getPendingAssetUnlocks", [&] { node->llmq().getPendingAssetUnlocks(); return true; });
            call("getQuorumStats", [&] { return node->llmq().getQuorumStats().has_value(); });
            call("getPlatformQuorum", [&] {
                return node->llmq()
                           .getPlatformQuorum(std23::to_underlying(platform_type), platform_qc.quorumHash, tip->nHeight)
                           .status == interfaces::LLMQ::PlatformQuorumStatus::FOUND;
            });
            call("getInstantSendLock", [&] { node->llmq().getInstantSendLock(GetRandHash()); return true; });
            call("node isV24Active", [&] { return node->isV24Active().has_value(); });
            call("masternodeSync", [&] { node->masternodeSync().isSynced(); return true; });
            call("listMNCollaterials", [&] { return m_node.chain->listMNCollaterials({}).has_value(); });
            call("chain isV24Active", [&] { return m_node.chain->isV24Active().has_value(); });
        }}.join();
        return failed;
    };

    for (const auto& failure : sweep(/*expect_state=*/true)) BOOST_ERROR("single view: " + failure);

    BOOST_REQUIRE(evodb.CreateSnapshotGeneration(tip->GetBlockHash(), tip->nHeight));
    BOOST_REQUIRE(
        evodb.PublishSnapshotGeneration(EvoSnapshotConsensusConfig::FromParams(Params()).Fingerprint(), std::nullopt));
    evodb.SetDefaultIdentity(EvoDbIdentity::SNAPSHOT);
    // The snapshot view holds no state, so its reads may report it
    // unavailable; none may fail.
    for (const auto& failure : sweep(/*expect_state=*/false)) BOOST_ERROR("dual views: " + failure);

    LogInstance().DeleteCallback(log_callback);
    for (const auto& line : WITH_LOCK(log_mutex, return unbound_logged)) BOOST_ERROR("unbound access: " + line);
    SetMiscWarning(Untranslated(""));
}

BOOST_AUTO_TEST_CASE(providerNetInfoValidation)
{
    auto node{interfaces::MakeNode(m_node)};
    interfaces::ProviderNetInfo net_info{
        .core_p2p = {"1.1.1.1:9998"},
        .platform_p2p = std::vector<std::string>{"1.1.1.2:22200"},
        .platform_https = std::vector<std::string>{"server.example.com:443"},
    };

    // Unless the caller's capabilities say masternodes may operate without IPv4, Platform must use the Core address
    auto error{node->evo().validateProviderNetInfo(net_info, MnType::Evo, ProTxVersion::ExtAddr,
                                                   /*ipv4_optional=*/false, /*optional=*/false)};
    BOOST_REQUIRE(error);
    BOOST_CHECK_EQUAL(error->reject_reason, "bad-protx-netinfo-addr");

    net_info.platform_p2p = std::vector<std::string>{"1.1.1.1:22200"};
    net_info.platform_https = std::vector<std::string>{"1.1.1.1:443", "server.example.com:443"};
    BOOST_CHECK(!node->evo().validateProviderNetInfo(net_info, MnType::Evo, ProTxVersion::ExtAddr,
                                                     /*ipv4_optional=*/false, /*optional=*/false));

    net_info.platform_p2p = std::vector<std::string>{"server.example.com:22200"};
    error = node->evo().validateProviderNetInfo(net_info, MnType::Evo, ProTxVersion::ExtAddr,
                                                /*ipv4_optional=*/false, /*optional=*/false);
    BOOST_REQUIRE(error);
    BOOST_CHECK_EQUAL(error->code, interfaces::ProviderTxErrorCode::INVALID_PARAMETER);
    BOOST_CHECK_EQUAL(error->message.original,
                      "Error setting platformP2PAddrs[0] to 'server.example.com:22200' (invalid input)");

    net_info.platform_p2p = std::vector<std::string>{"1.1.1.1:9998"};
    error = node->evo().validateProviderNetInfo(net_info, MnType::Evo, ProTxVersion::ExtAddr,
                                                /*ipv4_optional=*/false, /*optional=*/false);
    BOOST_REQUIRE(error);
    BOOST_CHECK_EQUAL(error->message.original, "Error setting platformP2PAddrs[0] to '1.1.1.1:9998' (duplicate)");

    net_info.platform_p2p = std::vector<std::string>{"1.1.1.2:22200", "1.1.1.3:22200", "1.1.1.4:22200", "1.1.1.5:22200",
                                                     "1.1.1.6:22200"};
    error = node->evo().validateProviderNetInfo(net_info, MnType::Evo, ProTxVersion::ExtAddr,
                                                /*ipv4_optional=*/false, /*optional=*/false);
    BOOST_REQUIRE(error);
    BOOST_CHECK_EQUAL(error->message.original,
                      "Error setting platformP2PAddrs[4] to '1.1.1.6:22200' (too many entries)");

    net_info.platform_p2p = uint16_t{22200};
    net_info.platform_https = uint16_t{22201};
    BOOST_CHECK(!node->evo().validateProviderNetInfo(net_info, MnType::Evo, ProTxVersion::BasicBLS,
                                                     /*ipv4_optional=*/false, /*optional=*/false));

    net_info.platform_p2p = std::vector<std::string>{"1.1.1.2:22200"};
    error = node->evo().validateProviderNetInfo(net_info, MnType::Evo, ProTxVersion::BasicBLS,
                                                /*ipv4_optional=*/false, /*optional=*/false);
    BOOST_REQUIRE(error);
    BOOST_CHECK(error->message.original.find("only accepts a bare port number") != std::string::npos);

    net_info = {
        .core_p2p = {},
        .platform_p2p = std::vector<std::string>{"1.1.1.2:22200"},
        .platform_https = std::vector<std::string>{"server.example.com:443"},
    };
    error = node->evo().validateProviderNetInfo(net_info, MnType::Evo, ProTxVersion::ExtAddr,
                                                /*ipv4_optional=*/false, /*optional=*/true);
    BOOST_REQUIRE(error);
    BOOST_CHECK_EQUAL(error->reject_reason, "bad-protx-netinfo-empty");

    net_info = {
        .core_p2p = {"1.1.1.1:9998"},
        .platform_p2p = std::monostate{},
        .platform_https = std::monostate{},
    };
    error = node->evo().validateProviderNetInfo(net_info, MnType::Evo, ProTxVersion::ExtAddr,
                                                /*ipv4_optional=*/false, /*optional=*/true);
    BOOST_REQUIRE(error);
    BOOST_CHECK(error->message.original.find("platformP2PAddrs, cannot be empty if other fields populated") !=
                std::string::npos);
    BOOST_CHECK(!node->evo().validateProviderNetInfo(net_info, MnType::Regular, ProTxVersion::ExtAddr,
                                                     /*ipv4_optional=*/false, /*optional=*/true));

    net_info.platform_p2p = std::vector<std::string>{"1.1.1.2:22200"};
    error = node->evo().validateProviderNetInfo(net_info, MnType::Regular, ProTxVersion::ExtAddr,
                                                /*ipv4_optional=*/false, /*optional=*/true);
    BOOST_REQUIRE(error);
    BOOST_CHECK_EQUAL(error->message.original, "Platform endpoints are only valid for an EvoNode");

    net_info = {
        .core_p2p = {"1.1.1.1:9998"},
        .platform_p2p = std::vector<std::string>{"1.1.1.2:22200"},
        .platform_https = std::monostate{},
    };
    error = node->evo().validateProviderNetInfo(net_info, MnType::Evo, ProTxVersion::ExtAddr,
                                                /*ipv4_optional=*/false, /*optional=*/true);
    BOOST_REQUIRE(error);
    BOOST_CHECK(error->message.original.find("platformHTTPSAddrs, cannot be empty if other fields populated") !=
                std::string::npos);

    net_info = {
        .core_p2p = {"1.1.1.1:9998"},
        .platform_p2p = uint16_t{22200},
        .platform_https = uint16_t{22200},
    };
    error = node->evo().validateProviderNetInfo(net_info, MnType::Evo, ProTxVersion::BasicBLS,
                                                /*ipv4_optional=*/false, /*optional=*/false);
    BOOST_REQUIRE(error);
    BOOST_CHECK_EQUAL(error->reject_reason, "bad-protx-platform-dup-ports");

    net_info.platform_p2p = uint16_t{MainParams().GetDefaultPort()};
    net_info.platform_https = uint16_t{22201};
    error = node->evo().validateProviderNetInfo(net_info, MnType::Evo, ProTxVersion::BasicBLS,
                                                /*ipv4_optional=*/false, /*optional=*/false);
    BOOST_REQUIRE(error);
    BOOST_CHECK_EQUAL(error->reject_reason, "bad-protx-platform-p2p-port");
}

BOOST_AUTO_TEST_CASE(findFirstBlockWithTimeAndHeight)
{
    LOCK(Assert(m_node.chainman)->GetMutex());
    auto& chain = m_node.chain;
    const CChain& active = Assert(m_node.chainman)->ActiveChain();
    uint256 hash;
    int height;
    BOOST_CHECK(chain->findFirstBlockWithTimeAndHeight(/* min_time= */ 0, /* min_height= */ 5, FoundBlock().hash(hash).height(height)));
    BOOST_CHECK_EQUAL(hash, active[5]->GetBlockHash());
    BOOST_CHECK_EQUAL(height, 5);
    BOOST_CHECK(!chain->findFirstBlockWithTimeAndHeight(/* min_time= */ active.Tip()->GetBlockTimeMax() + 1, /* min_height= */ 0));
}

BOOST_AUTO_TEST_CASE(findAncestorByHeight)
{
    LOCK(Assert(m_node.chainman)->GetMutex());
    auto& chain = m_node.chain;
    const CChain& active = Assert(m_node.chainman)->ActiveChain();
    uint256 hash;
    BOOST_CHECK(chain->findAncestorByHeight(active[20]->GetBlockHash(), 10, FoundBlock().hash(hash)));
    BOOST_CHECK_EQUAL(hash, active[10]->GetBlockHash());
    BOOST_CHECK(!chain->findAncestorByHeight(active[10]->GetBlockHash(), 20));
}

BOOST_AUTO_TEST_CASE(findAncestorByHash)
{
    LOCK(Assert(m_node.chainman)->GetMutex());
    auto& chain = m_node.chain;
    const CChain& active = Assert(m_node.chainman)->ActiveChain();
    int height = -1;
    BOOST_CHECK(chain->findAncestorByHash(active[20]->GetBlockHash(), active[10]->GetBlockHash(), FoundBlock().height(height)));
    BOOST_CHECK_EQUAL(height, 10);
    BOOST_CHECK(!chain->findAncestorByHash(active[10]->GetBlockHash(), active[20]->GetBlockHash()));
}

BOOST_AUTO_TEST_CASE(findCommonAncestor)
{
    auto& chain = m_node.chain;
    const CChain& active = *WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return &Assert(m_node.chainman)->ActiveChain());
    auto* orig_tip = active.Tip();
    for (int i = 0; i < 10; ++i) {
        BlockValidationState state;
        m_node.chainman->ActiveChainstate().InvalidateBlock(state, active.Tip());
    }
    BOOST_CHECK_EQUAL(active.Height(), orig_tip->nHeight - 10);
    coinbaseKey.MakeNewKey(true);
    for (int i = 0; i < 20; ++i) {
        CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    }
    BOOST_CHECK_EQUAL(active.Height(), orig_tip->nHeight + 10);
    uint256 fork_hash;
    int fork_height;
    int orig_height;
    BOOST_CHECK(chain->findCommonAncestor(orig_tip->GetBlockHash(), active.Tip()->GetBlockHash(), FoundBlock().height(fork_height).hash(fork_hash), FoundBlock().height(orig_height)));
    BOOST_CHECK_EQUAL(orig_height, orig_tip->nHeight);
    BOOST_CHECK_EQUAL(fork_height, orig_tip->nHeight - 10);
    BOOST_CHECK_EQUAL(fork_hash, active[fork_height]->GetBlockHash());

    uint256 active_hash, orig_hash;
    BOOST_CHECK(!chain->findCommonAncestor(active.Tip()->GetBlockHash(), {}, {}, FoundBlock().hash(active_hash), {}));
    BOOST_CHECK(!chain->findCommonAncestor({}, orig_tip->GetBlockHash(), {}, {}, FoundBlock().hash(orig_hash)));
    BOOST_CHECK_EQUAL(active_hash, active.Tip()->GetBlockHash());
    BOOST_CHECK_EQUAL(orig_hash, orig_tip->GetBlockHash());
}

BOOST_AUTO_TEST_CASE(hasBlocks)
{
    LOCK(::cs_main);
    auto& chain = m_node.chain;
    const CChain& active = Assert(m_node.chainman)->ActiveChain();

    // Test ranges
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 10, 90));
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 10, {}));
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 0, 90));
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 0, {}));
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), -1000, 1000));
    active[5]->nStatus &= ~BLOCK_HAVE_DATA;
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 10, 90));
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 10, {}));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 0, 90));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 0, {}));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), -1000, 1000));
    active[95]->nStatus &= ~BLOCK_HAVE_DATA;
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 10, 90));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 10, {}));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 0, 90));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 0, {}));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), -1000, 1000));
    active[50]->nStatus &= ~BLOCK_HAVE_DATA;
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 10, 90));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 10, {}));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 0, 90));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 0, {}));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), -1000, 1000));

    // Test edge cases
    BOOST_CHECK(chain->hasBlocks(active.Tip()->GetBlockHash(), 6, 49));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 5, 49));
    BOOST_CHECK(!chain->hasBlocks(active.Tip()->GetBlockHash(), 6, 50));
}

namespace {
using PlatformQuorumStatus = interfaces::LLMQ::PlatformQuorumStatus;

const char* StatusName(PlatformQuorumStatus status)
{
    switch (status) {
    case PlatformQuorumStatus::FOUND: return "FOUND";
    case PlatformQuorumStatus::INVALID: return "INVALID";
    case PlatformQuorumStatus::UNAVAILABLE: return "UNAVAILABLE";
    }
    return "?";
}

interfaces::LLMQ::PlatformQuorumResult CheckPlatformQuorum(interfaces::LLMQ& llmq, std::string_view label,
                                                           uint8_t llmq_type, const uint256& quorum_hash,
                                                           uint32_t height, PlatformQuorumStatus expected)
{
    auto result{llmq.getPlatformQuorum(llmq_type, quorum_hash, height)};
    BOOST_CHECK_MESSAGE(result.status == expected, strprintf("%s (height %u): got %s, expected %s", label, height,
                                                             StatusName(result.status), StatusName(expected)));
    BOOST_CHECK_MESSAGE(result.public_key.empty() == (result.status != PlatformQuorumStatus::FOUND),
                        strprintf("%s: only FOUND carries a key", label));
    return result;
}

// Records a ChainLock at `height` on `block`, or on a block this node does not know if null.
void RecordChainLock(chainlock::Chainlocks& chainlocks, int height, const CBlockIndex* block)
{
    const auto clsig{llmq::testutils::CreateChainLock(height, block ? block->GetBlockHash() : InsecureRand256())};
    BOOST_REQUIRE(chainlocks.UpdateBestChainlock(::SerializeHash(clsig), clsig, block));
}

// Adds a header-only block on `prev`, unique and otherwise copied from `like`.
const CBlockIndex* AddHeader(ChainstateManager& chainman, const CBlockIndex& prev, const CBlockIndex& like)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    CBlockHeader header{like.GetBlockHeader()};
    header.hashPrevBlock = prev.GetBlockHash();
    ++header.nNonce;
    return chainman.m_blockman.AddToBlockIndex(header, header.GetHash(), chainman.m_best_header);
}
} // namespace

// getPlatformQuorum answers whether a quorum could have signed Platform state carrying a given Core
// chain-locked height. Mined commitments are installed directly in EvoDB (the same shortcut
// evo_cbtx_tests uses) since a real DKG cannot run in a unit test. After the first checks a
// ChainLock on the tip covers the whole active chain, and the staleness bound equals the tip's.
BOOST_AUTO_TEST_CASE(getPlatformQuorum)
{
    auto node{interfaces::MakeNode(m_node)};
    auto& llmq{node->llmq()};
    const auto platform_type{Params().GetConsensus().llmqTypePlatform};
    BOOST_REQUIRE(platform_type == Consensus::LLMQType::LLMQ_TEST_PLATFORM);
    const auto& params{llmq::testutils::GetLLMQParams(platform_type)};
    const uint8_t type{std23::to_underlying(platform_type)};
    // Four commitments are eligible at any height (signingActiveQuorumCount plus a slack of two) and
    // state older than 4 * 24 = 96 blocks below the ChainLock is stale.
    BOOST_REQUIRE_EQUAL(params.signingActiveQuorumCount, 2);
    BOOST_REQUIRE_EQUAL(params.dkgInterval, 24);
    const int tip{WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Height())};
    BOOST_REQUIRE_EQUAL(tip, 100);
    BOOST_REQUIRE_EQUAL(llmq::PlatformQuorumMinSignedHeight(params, tip), 4);

    // Quorum bases mined one block later: 2 (mined at 3), 10, 20, 30, 40, 50 (mined at 51).
    std::map<int, llmq::CFinalCommitment> qcs;
    llmq::CFinalCommitment other_type_qc;
    {
        LOCK(::cs_main);
        const CChain& active{m_node.chainman->ActiveChain()};
        auto db_tx{m_node.evodb->BeginTransaction()};
        for (const int base : {2, 10, 20, 30, 40, 50}) {
            auto qc{llmq::testutils::CreateValidCommitment(params, active[base]->GetBlockHash())};
            llmq::testutils::WriteMinedCommitment(*m_node.evodb, qc, active[base + 1]->GetBlockHash(),
                                                  /*mined_height=*/base + 1, /*quorum_height=*/base);
            qcs.emplace(base, qc);
        }
        other_type_qc = llmq::testutils::CreateValidCommitment(llmq::testutils::GetLLMQParams(Consensus::LLMQType::LLMQ_TEST),
                                                               active[60]->GetBlockHash());
        llmq::testutils::WriteMinedCommitment(*m_node.evodb, other_type_qc, active[61]->GetBlockHash(),
                                              /*mined_height=*/61, /*quorum_height=*/60);
        db_tx->Commit();
    }
    const auto check = [&](std::string_view label, uint8_t llmq_type, const uint256& hash, uint32_t height,
                           PlatformQuorumStatus expected) {
        return CheckPlatformQuorum(llmq, label, llmq_type, hash, height, expected);
    };
    using enum PlatformQuorumStatus;

    // The newest quorum, with the commitment's 48-byte basic-scheme key. The hash is in Core's
    // internal byte order.
    const auto found{check("newest", type, qcs.at(50).quorumHash, tip, FOUND)};
    BOOST_CHECK(found.public_key == qcs.at(50).quorumPublicKey.ToByteVector());
    BOOST_CHECK_EQUAL(found.public_key.size(), 48U);
    CBLSPublicKey roundtrip;
    roundtrip.SetBytes(found.public_key);
    BOOST_CHECK(roundtrip.IsValid());
    BOOST_CHECK(roundtrip == qcs.at(50).quorumPublicKey);
    // Without a ChainLock this node's chain is not final, so a missing quorum is not evidence yet.
    check("unknown quorum, no ChainLock", type, InsecureRand256(), tip, UNAVAILABLE);

    // Base 2 is eligible at heights 3 and 4; only 4 is within the staleness bound the tip gives.
    // A stale height is no evidence against the quorum, with or without a ChainLock.
    check("at the staleness bound", type, qcs.at(2).quorumHash, 4, FOUND);
    check("below the staleness bound, no ChainLock", type, qcs.at(2).quorumHash, 3, UNAVAILABLE);

    const auto* tip_block{WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip())};
    RecordChainLock(*m_node.chainlocks, tip, tip_block);
    check("below the staleness bound", type, qcs.at(2).quorumHash, 3, UNAVAILABLE);

    uint256 reversed{qcs.at(50).quorumHash};
    std::reverse(reversed.begin(), reversed.end());
    check("byte-reversed hash", type, reversed, tip, INVALID);

    // Window edges at the tip, newest first: 50, 40, 30, 20 are eligible and 10 is fifth.
    check("position 3", type, qcs.at(30).quorumHash, tip, FOUND);
    check("position 4", type, qcs.at(20).quorumHash, tip, FOUND);
    // Base 10 is fourth at height 50. When Platform's Core height jumps from 50 to the tip, its
    // validator set signs the first block carrying the tip's height, so fifth proves nothing.
    check("position 4 before a Core-height jump", type, qcs.at(10).quorumHash, 50, FOUND);
    check("position 5 after a Core-height jump", type, qcs.at(10).quorumHash, tip, UNAVAILABLE);

    // A quorum stays valid for state signed while it was in the window.
    check("historical", type, qcs.at(10).quorumHash, 15, FOUND);
    // Only bases 10 and 2 were mined by height 15, and nothing older exists.
    check("unknown quorum, window shorter than four", type, InsecureRand256(), 15, INVALID);

    check("height above the tip", type, qcs.at(50).quorumHash, tip + 1, UNAVAILABLE);
    check("height beyond int32", type, qcs.at(50).quorumHash, uint32_t{1} << 31, INVALID);
    check("non-Platform type", std23::to_underlying(Consensus::LLMQType::LLMQ_TEST), other_type_qc.quorumHash, tip,
          INVALID);
    check("unknown type", 0xEE, qcs.at(50).quorumHash, tip, INVALID);
    check("unknown quorum", type, InsecureRand256(), tip, INVALID);
    check("mined above the height", type, qcs.at(50).quorumHash, 50, INVALID);
    // A quorum that could sign at a height has its base block on the active chain below it.
    uint256 block_45, block_70, off_chain;
    {
        LOCK(::cs_main);
        const CChain& active{m_node.chainman->ActiveChain()};
        block_45 = active[45]->GetBlockHash();
        block_70 = active[70]->GetBlockHash();
        off_chain = AddHeader(*m_node.chainman, *active[50], *active[51])->GetBlockHash();
    }
    check("block above the height", type, block_70, 50, INVALID);
    check("block off the active chain", type, off_chain, tip, INVALID);
    check("block below the height, outside the window", type, block_45, tip, UNAVAILABLE);

    // ChainLocks stalled at 50 while the tip reached 100: the staleness bound follows the ChainLock,
    // so height 3 is fresh.
    m_node.chainlocks->ResetChainlock();
    const auto* block_50{WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain()[50])};
    RecordChainLock(*m_node.chainlocks, 50, block_50);
    check("stalled ChainLock", type, qcs.at(2).quorumHash, 3, FOUND);
    // Above the ChainLock the active chain may be a competing branch that lacks the newest
    // commitment, so only a quorum found there is decisive.
    check("unknown quorum at the ChainLock", type, InsecureRand256(), 50, INVALID);
    check("unknown quorum above the ChainLock", type, InsecureRand256(), 51, UNAVAILABLE);
    check("unknown quorum far above the ChainLock", type, InsecureRand256(), 86, UNAVAILABLE);
    check("above the ChainLock", type, qcs.at(50).quorumHash, 86, FOUND);
    // A higher ChainLock on a block this node does not know covers nothing yet.
    RecordChainLock(*m_node.chainlocks, 88, nullptr);
    check("unknown quorum below a ChainLock on an unknown block", type, InsecureRand256(), 86, UNAVAILABLE);
    // One on a branch forking from the active chain after block 83 covers the active chain to 83.
    const CBlockIndex* branch_tip;
    {
        LOCK(::cs_main);
        const CChain& active{m_node.chainman->ActiveChain()};
        branch_tip = active[83];
        for (int height{84}; height <= 90; ++height) {
            branch_tip = AddHeader(*m_node.chainman, *branch_tip, *active[height]);
        }
        BOOST_REQUIRE(!active.Contains(branch_tip));
    }
    RecordChainLock(*m_node.chainlocks, 90, branch_tip);
    check("unknown quorum above a competing ChainLock's fork", type, InsecureRand256(), 86, UNAVAILABLE);
    check("unknown quorum below a competing ChainLock's fork", type, InsecureRand256(), 83, INVALID);
    // One on a header built on the tip covers the whole active chain before its block arrives.
    const auto* next_header{WITH_LOCK(::cs_main, return AddHeader(*m_node.chainman, *tip_block, *tip_block))};
    RecordChainLock(*m_node.chainlocks, tip + 1, next_header);
    check("unknown quorum below a ChainLock beyond the tip", type, InsecureRand256(), tip, INVALID);

    // A ChainLock on a block not known yet still raises the staleness bound, but only up to the tip.
    m_node.chainlocks->ResetChainlock();
    RecordChainLock(*m_node.chainlocks, 50, block_50);
    RecordChainLock(*m_node.chainlocks, 1000, nullptr);
    check("ChainLock ahead of the tip, at the bound", type, qcs.at(2).quorumHash, 4, FOUND);
    check("ChainLock ahead of the tip, below the bound", type, qcs.at(2).quorumHash, 3, UNAVAILABLE);

    // An index entry whose commitment record is missing is a local fault, not evidence against the quorum.
    uint256 block_60;
    {
        LOCK(::cs_main);
        block_60 = m_node.chainman->ActiveChain()[60]->GetBlockHash();
        auto db_tx{m_node.evodb->BeginTransaction()};
        m_node.evodb->Write(llmq::testutils::BuildInversedHeightKey(platform_type, 61), 60);
        db_tx->Commit();
    }
    check("commitment record missing", type, block_60, tip, UNAVAILABLE);
}

// In a snapshot view, a quorum missing from the commitments mined above the
// base may still have been mined at or below it. An unmined hash is invalid
// only once the eligible window lies above the base, and only under an
// anchoring ChainLock; a quorum mined below the height never is. A node with
// no Evo state to read cannot tell at all.
BOOST_AUTO_TEST_CASE(getPlatformQuorum_snapshot_view)
{
    auto node{interfaces::MakeNode(m_node)};
    auto& llmq{node->llmq()};
    CEvoDB& evodb{*Assert(m_node.evodb)};
    const auto platform_type{Params().GetConsensus().llmqTypePlatform};
    const auto& params{llmq::testutils::GetLLMQParams(platform_type)};
    const uint8_t type{std23::to_underlying(platform_type)};
    constexpr auto FOUND{PlatformQuorumStatus::FOUND};
    constexpr auto INVALID{PlatformQuorumStatus::INVALID};
    constexpr auto UNAVAILABLE{PlatformQuorumStatus::UNAVAILABLE};
    // With DIP3 active at the base, quorums can have been mined at or below it.
    auto& consensus{const_cast<Consensus::Params&>(Params().GetConsensus())};
    struct Dip3HeightRestore {
        Consensus::Params& params;
        const int height;
        ~Dip3HeightRestore() { params.DIP0003Height = height; }
    } dip3_restore{consensus, std::exchange(consensus.DIP0003Height, 1)};

    // The quorum based at 40 was mined below the snapshot base at 50, where
    // only NORMAL holds it; the snapshot view holds the ones based at 60, 70,
    // 80 and 90, each mined one block later.
    std::map<int, uint256> hashes;
    const auto write_commitment{[&](int base) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        const CChain& active{m_node.chainman->ActiveChain()};
        const auto qc{llmq::testutils::CreateValidCommitment(params, active[base]->GetBlockHash())};
        llmq::testutils::WriteMinedCommitment(evodb, qc, active[base + 1]->GetBlockHash(),
                                              /*mined_height=*/base + 1, /*quorum_height=*/base);
        hashes.emplace(base, qc.quorumHash);
    }};
    {
        LOCK(::cs_main);
        auto db_tx{evodb.BeginTransaction()};
        write_commitment(40);
        db_tx->Commit();
        const CBlockIndex* base{m_node.chainman->ActiveChain()[50]};
        BOOST_REQUIRE(evodb.CreateSnapshotGeneration(base->GetBlockHash(), base->nHeight));
    }
    BOOST_REQUIRE(
        evodb.PublishSnapshotGeneration(EvoSnapshotConsensusConfig::FromParams(Params()).Fingerprint(), std::nullopt));
    evodb.SetDefaultIdentity(EvoDbIdentity::SNAPSHOT);
    {
        LOCK(::cs_main);
        auto db_tx{evodb.BeginTransaction(EvoDbIdentity::SNAPSHOT)};
        for (const int base : {60, 70, 80, 90}) write_commitment(base);
        db_tx->Commit();
    }
    const uint256 unknown{InsecureRand256()};

    CheckPlatformQuorum(llmq, "window above the base", type, hashes.at(90), 100, FOUND);
    CheckPlatformQuorum(llmq, "absent, no ChainLock", type, unknown, 100, UNAVAILABLE);

    RecordChainLock(*m_node.chainlocks, 100, WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip()));
    CheckPlatformQuorum(llmq, "absent, window above the base", type, unknown, 100, INVALID);
    // A window reaching the base cannot be read in full, so not even a quorum
    // in it is answered.
    CheckPlatformQuorum(llmq, "window reaching the base", type, hashes.at(70), 75, UNAVAILABLE);
    CheckPlatformQuorum(llmq, "absent, window reaching the base", type, unknown, 75, UNAVAILABLE);
    CheckPlatformQuorum(llmq, "mined below the base", type, hashes.at(40), 55, UNAVAILABLE);
    CheckPlatformQuorum(llmq, "mined below the base, out of the window", type, hashes.at(40), 100, UNAVAILABLE);
    // A caller bound to a view other than the active chain's gets no answer
    // about the active chain.
    {
        const EvoViewScope normal_view{evodb.ViewFor(EvoDbIdentity::NORMAL)};
        CheckPlatformQuorum(llmq, "bound to another view", type, hashes.at(90), 100, UNAVAILABLE);
    }

    {
        auto evodb_owner{std::move(m_node.evodb)};
        CheckPlatformQuorum(llmq, "no Evo database", type, hashes.at(90), 100, UNAVAILABLE);
        m_node.evodb = std::move(evodb_owner);
    }
    m_node.chainlocks->ResetChainlock();
}

// getInstantSendLock returns the network-serialized islock for a locked txid, and an empty vector
// both for unknown txids and while InstantSend is disabled (spork gate inside the manager).
BOOST_AUTO_TEST_CASE(getInstantSendLock)
{
    constexpr const char* REGTEST_SPORK_PRIVKEY{"cP4EKFyJsHT39LDqgdcB43Y3YXjNyjb5Fuas1GQSeAtjnZWmZEQK"};

    auto node{interfaces::MakeNode(m_node)};
    auto& isman{*Assert(m_node.isman)};

    auto islock{std::make_shared<instantsend::InstantSendLock>()};
    islock->inputs.emplace_back(uint256::ONE, 0);
    islock->txid = uint256::TWO;
    islock->cycleHash = InsecureRand256();
    CBLSSecretKey sk;
    sk.MakeNewKey();
    islock->sig.Set(sk.Sign(islock->GetRequestId()));

    isman.WriteNewISLock(::SerializeHash(*islock), islock, /*minedHeight=*/std::nullopt);

    // SPORK_2 defaults to OFF: the stored lock must not leak through the interface.
    BOOST_CHECK(node->llmq().getInstantSendLock(islock->txid).empty());

    // The fixture builds a bare CSporkManager, so wire up the regtest signer before setting the spork.
    BOOST_REQUIRE(m_node.sporkman->SetSporkAddress(Params().SporkAddress()));
    BOOST_REQUIRE(m_node.sporkman->SetPrivKey(REGTEST_SPORK_PRIVKEY));
    BOOST_REQUIRE(m_node.sporkman->UpdateSpork(SPORK_2_INSTANTSEND_ENABLED, 0).has_value());

    CDataStream expected{SER_NETWORK, PROTOCOL_VERSION};
    expected << *islock;
    const auto ret{node->llmq().getInstantSendLock(islock->txid)};
    BOOST_REQUIRE(!ret.empty());
    BOOST_CHECK_EQUAL(HexStr(ret), HexStr(expected));

    // The exported bytes deserialize back into the same lock.
    CDataStream ds{Span<const uint8_t>{ret}, SER_NETWORK, PROTOCOL_VERSION};
    instantsend::InstantSendLock roundtrip;
    ds >> roundtrip;
    BOOST_CHECK_EQUAL(roundtrip.txid, islock->txid);
    BOOST_CHECK(roundtrip.inputs == islock->inputs);
    BOOST_CHECK_EQUAL(roundtrip.GetRequestId(), islock->GetRequestId());

    // Unknown txid.
    BOOST_CHECK(node->llmq().getInstantSendLock(uint256::ONE).empty());
}

BOOST_AUTO_TEST_SUITE_END()
