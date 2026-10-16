// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/platformtests.h>

#include <qt/test/masternodetestutil.h>

#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <netbase.h>
#include <qt/clientfeeds.h>
#include <qt/platform/platformpage.h>
#include <qt/platform/platformservice.h>
#include <qt/walletmodel.h>
#include <test/util/platform_client.h>
#include <test/util/setup_common.h>
#include <util/settings.h>
#include <util/system.h>
#include <wallet/context.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <QCoreApplication>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using MasternodeTestUtil::GuiModels;
using MasternodeTestUtil::MakeTestWallet;
using MasternodeTestUtil::WalletGuard;
using Gate = PlatformService::Gate;

namespace {
CService Ipv4Evonode() { return LookupNumeric("203.0.113.7", 443); }
CService Ipv4Evonode2() { return LookupNumeric("203.0.113.8", 443); }
CService Ipv6Evonode() { return LookupNumeric("2001:db8::7", 443); }
CService OnionEvonode() { return LookupNumeric("pg6mmjiyjmcrsslvykfwnntlaru7p5svn6y2ymmju6nubxndf4pscryd.onion", 443); }
CService I2pEvonode() { return LookupNumeric("udhdrtrcetjm5sxzskjyr5ztpeszydbh4dpl3pl4utgqqw2v4jna.b32.i2p", 443); }
//! -proxy=127.0.0.1:9050 with -proxyrandomize.
platform::ProxyConfig Tor() { return {LookupNumeric("127.0.0.1", 9050), true}; }

//! One test's node, wallet and models; the wallet has started DashPay unless
//! told otherwise. The node context is reset only after the models are gone.
template <typename ChainSetup>
struct FixtureWithChain {
    ChainSetup chain{CBaseChainParams::REGTEST};
    struct ContextReset {
        interfaces::Node& node;
        ~ContextReset() { node.setContext(nullptr); }
    } reset;
    interfaces::Node& node;
    wallet::WalletContext& context;
    std::shared_ptr<wallet::CWallet> wallet;
    WalletGuard guard;
    GuiModels models;
    WalletModel wallet_model;

    explicit FixtureWithChain(interfaces::Node& node_in, bool consented = true) :
        reset{node_in},
        node(node_in),
        context((node.setContext(&chain.m_node), *node.walletLoader().context())),
        wallet(MakeTestWallet(node, context, "platform")),
        guard(context, wallet),
        models(node),
        wallet_model(interfaces::MakeWallet(context, wallet), models.client)
    {
        if (consented) Assert(wallet_model.wallet().writePlatformData(PlatformService::CONSENT_RECORD, {1}));
    }

    void attach(PlatformPage& page)
    {
        page.setWalletModel(&wallet_model);
        page.setClientModel(&models.client);
    }
    //! The masternode feed read the list again (on masternodeListChanged).
    void listRead() { Q_EMIT models.client.feedMasternode()->dataReady(); }
    //! The test node has no UI interface: the notification is sent by hand.
    void setNetworkActive(bool active)
    {
        node.setNetworkActive(active);
        Q_EMIT models.client.networkActiveChanged(active);
    }
};
//! A synced node: 100 recent blocks.
using Fixture = FixtureWithChain<TestChain100Setup>;
//! A node in initial block download: only the old genesis block.
using SyncingFixture = FixtureWithChain<TestingSetup>;

//! What a service reads instead of the node, and the clients it made.
struct FakeSources {
    PlatformNetworkSettings settings;
    std::vector<CService> evonodes{Ipv4Evonode()};
    bool refuse{false};
    std::vector<platform::ClientConfig> made; //!< one per client asked for
    FakePlatformClient* fake{nullptr};        //!< the latest one

    PlatformService::Sources get()
    {
        return {
            [this](const platform::ClientConfig& config, platform::Executor) -> std::unique_ptr<platform::PlatformClient> {
                made.push_back(config);
                if (refuse) return nullptr;
                auto client{std::make_unique<FakePlatformClient>()};
                fake = client.get();
                return client;
            },
            [this] { return evonodes; },
            [this] { return settings; },
        };
    }
};

//! A read whose callback owns the result: it expires once the client drops
//! the call without calling back.
std::weak_ptr<int> PendingRead(platform::PlatformClient& client)
{
    auto owner{std::make_shared<int>(0)};
    client.getIdentity({}, [owner](platform::Result<platform::Identity>) { Assert(false); });
    return owner;
}

//! A tracked write whose callback owns the result and sets `done` when it
//! runs: it expires if the client drops the call instead.
std::weak_ptr<int> PendingWrite(PlatformService& service, interfaces::Wallet& wallet, bool& done)
{
    auto owner{std::make_shared<int>(0)};
    service.client()->registerName("alice", {}, 0, platform::Signing{wallet, 0, nullptr},
                                   service.trackWrite<platform::Identifier>(
                                       [owner, &done](platform::Result<platform::Identifier>) { done = true; }));
    return owner;
}

QStringList VisibleTexts(const QWidget& page)
{
    QStringList texts;
    for (const auto* label : page.findChildren<QLabel*>()) {
        if (label->isVisibleTo(&page)) texts << label->text();
    }
    return texts;
}

QPushButton* VisibleButton(const QWidget& page, const char* name)
{
    auto* button{page.findChild<QPushButton*>(name)};
    return button && button->isVisibleTo(&page) ? button : nullptr;
}

//! Restores the ArgsManager settings a test forced.
struct SettingsRestore {
    util::Settings saved;
    SettingsRestore()
    {
        gArgs.LockSettings([&](util::Settings& settings) { saved = settings; });
    }
    ~SettingsRestore()
    {
        gArgs.LockSettings([&](util::Settings& settings) { settings = saved; });
    }
};

//! Makes IPv4 and IPv6 unreachable while it lives.
struct NoClearnet {
    NoClearnet()
    {
        g_reachable_nets.Remove(NET_IPV4);
        g_reachable_nets.Remove(NET_IPV6);
    }
    ~NoClearnet()
    {
        g_reachable_nets.Add(NET_IPV4);
        g_reachable_nets.Add(NET_IPV6);
    }
};
} // namespace

//! The route follows Core's rules for its own connections: -proxy (or none)
//! for IPv4 and IPv6, onion evonodes only through that same proxy, the onion
//! proxy alone under -onlynet=onion, never I2P or CJDNS; and the settings
//! are read from the node's reachable networks.
void PlatformTests::routeSelection()
{
    const platform::ProxyConfig other{LookupNumeric("127.0.0.1", 9150), true};
    const platform::ProxyConfig unix_socket{std::string{"/run/tor/socks"}, false};

    struct Case {
        const char* name;
        bool ipv4, ipv6, onion;
        std::optional<platform::ProxyConfig> clearnet_proxy, onion_proxy;
        bool routed;
        std::optional<platform::ProxyConfig> proxy;
        bool allows_ipv4, allows_ipv6, allows_onion;
    };
    const std::vector<Case> cases{
        {"defaults", true, true, true, std::nullopt, std::nullopt, true, std::nullopt, true, true, false},
        {"-proxy", true, true, true, Tor(), Tor(), true, Tor(), true, true, true},
        {"-proxy -onion", true, true, true, Tor(), other, true, Tor(), true, true, false},
        {"-proxy -noonion", true, true, false, Tor(), std::nullopt, true, Tor(), true, true, false},
        {"-proxy=unix:", true, true, true, unix_socket, unix_socket, true, unix_socket, true, true, true},
        {"-onlynet=ipv4", true, false, false, std::nullopt, std::nullopt, true, std::nullopt, true, false, false},
        {"-onlynet=onion -onion", false, false, true, std::nullopt, Tor(), true, Tor(), false, false, true},
        {"-onlynet=onion, no onion proxy yet", false, false, true, std::nullopt, std::nullopt, false, std::nullopt,
         false, false, false},
        {"-onlynet=i2p", false, false, false, std::nullopt, std::nullopt, false, std::nullopt, false, false, false},
    };
    for (const Case& c : cases) {
        PlatformNetworkSettings settings;
        settings.ipv4 = c.ipv4;
        settings.ipv6 = c.ipv6;
        settings.onion = c.onion;
        settings.clearnet_proxy = c.clearnet_proxy;
        settings.onion_proxy = c.onion_proxy;
        const auto route{PlatformRoute::Choose(settings)};
        QVERIFY2(route.has_value() == c.routed, c.name);
        if (!route) continue;
        QVERIFY2(route->proxy == c.proxy, c.name);
        QVERIFY2(route->allows(Ipv4Evonode()) == c.allows_ipv4, c.name);
        QVERIFY2(route->allows(Ipv6Evonode()) == c.allows_ipv6, c.name);
        QVERIFY2(route->allows(OnionEvonode()) == c.allows_onion, c.name);
        QVERIFY2(!route->allows(I2pEvonode()), c.name);
    }

    // The node's reachable networks, not -onlynet itself.
    const PlatformNetworkSettings defaults{PlatformNetworkSettings::Read(m_node)};
    QVERIFY(defaults.ipv4 && defaults.ipv6);
    QVERIFY(!defaults.clearnet_proxy && !defaults.onion_proxy);
    const NoClearnet no_clearnet;
    const PlatformNetworkSettings onion_only{PlatformNetworkSettings::Read(m_node)};
    QVERIFY(!onion_only.ipv4 && !onion_only.ipv6);
}

//! -connect (with a peer, 0 or negated), -maxconnections=0 and
//! -walletbroadcast=0 disable DashPay for a wallet that started it: no
//! client is made, and the tab names the setting. So does a wallet with no
//! HD seed to derive Platform keys from.
void PlatformTests::settingsDisableDashPay()
{
    struct Case {
        const char* name;
        util::SettingsValue value;
        Gate gate;
    };
    const std::vector<Case> cases{
        {"connect", "203.0.113.1", Gate::OUTBOUND_DISABLED}, {"connect", "0", Gate::OUTBOUND_DISABLED},
        {"connect", false, Gate::OUTBOUND_DISABLED},         {"maxconnections", "0", Gate::OUTBOUND_DISABLED},
        {"walletbroadcast", "0", Gate::BROADCAST_DISABLED},
    };
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    for (const Case& c : cases) {
        const SettingsRestore restore;
        gArgs.LockSettings([&](util::Settings& settings) { settings.forced_settings[c.name] = c.value; });
        FakeSources sources;
        PlatformPage page{sources.get()};
        f.attach(page);
        QVERIFY2(sources.made.empty(), c.name);
        QVERIFY2(page.service()->gate() == c.gate, c.name);
        QVERIFY2(VisibleTexts(page).join(' ').contains(QString{"-"} + c.name), c.name);
    }

    // The same wallet with the defaults starts its client.
    FakeSources sources;
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    QCOMPARE(sources.made.size(), size_t{1});

    // A wallet without an HD seed can't derive its Platform keys.
    auto blank{
        std::make_shared<wallet::CWallet>(f.node.context()->chain.get(), "blank", wallet::CreateMockWalletDatabase())};
    blank->LoadWallet();
    blank->SetWalletFlag(wallet::WALLET_FLAG_DESCRIPTORS);
    wallet::AddWallet(f.context, blank);
    WalletGuard blank_guard{f.context, blank};
    WalletModel blank_model{interfaces::MakeWallet(f.context, blank), f.models.client};
    QVERIFY(blank_model.wallet().writePlatformData(PlatformService::CONSENT_RECORD, {1}));
    FakeSources blank_sources;
    PlatformService ineligible{blank_model, f.models.client, blank_sources.get()};
    QVERIFY(blank_sources.made.empty());
    QVERIFY(ineligible.gate() == Gate::WALLET);
    QVERIFY(!ineligible.reason().isEmpty());
}

//! Until the first DashPay click nothing is made that could contact
//! Platform, whatever the node does; the click records the wallet's consent,
//! which later sessions find, and turning DashPay off erases it.
void PlatformTests::nothingBeforeConsent()
{
    Fixture f{m_node, /*consented=*/false};
    QVERIFY(f.models.ok);
    FakeSources sources;
    PlatformPage page{sources.get()};
    f.attach(page);
    f.listRead();
    f.setNetworkActive(false);
    f.setNetworkActive(true);
    QVERIFY(sources.made.empty());
    QVERIFY(page.service()->gate() == Gate::NO_CONSENT);
    QVERIFY(page.findChild<QLabel*>("dashPayNotice")->isVisibleTo(&page));
    QVERIFY(VisibleButton(page, "dashPayStop") == nullptr);

    QPushButton* start{VisibleButton(page, "dashPayStart")};
    QVERIFY(start != nullptr);
    start->click();
    QCOMPARE(sources.made.size(), size_t{1});
    QVERIFY(sources.made.back().endpoints == std::vector<CService>{Ipv4Evonode()});
    QVERIFY(!sources.made.back().proxy);
    QVERIFY(page.service()->client() != nullptr);
    QVERIFY(VisibleButton(page, "dashPayStart") == nullptr);

    // The consent is the wallet's: the next session starts without a click.
    {
        FakeSources next;
        PlatformService service{f.wallet_model, f.models.client, next.get()};
        QCOMPARE(next.made.size(), size_t{1});
    }

    const auto read{PendingRead(*page.service()->client())};
    QPushButton* stop{VisibleButton(page, "dashPayStop")};
    QVERIFY(stop != nullptr);
    stop->click();
    QVERIFY(read.expired());
    QVERIFY(page.service()->client() == nullptr);
    QVERIFY(VisibleButton(page, "dashPayStart") != nullptr);
    FakeSources after;
    PlatformService service{f.wallet_model, f.models.client, after.get()};
    QVERIFY(after.made.empty());
}

//! A node in initial block download has a stale masternode list: no client.
void PlatformTests::syncingNodeStartsNoClient()
{
    SyncingFixture f{m_node};
    QVERIFY(f.models.ok);
    FakeSources sources;
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    f.listRead();
    QVERIFY(sources.made.empty());
    QVERIFY(service.gate() == Gate::SYNCING);
}

//! Turning network activity off destroys the client at once, a write in
//! flight included; turning it on makes a client from the current list.
void PlatformTests::networkOffDestroysClient()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    FakeSources sources;
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    bool written{false};
    const auto write{PendingWrite(service, f.wallet_model.wallet(), written)};

    QSignalSpy changed{&service, &PlatformService::changed};
    f.setNetworkActive(false);
    QVERIFY(write.expired());
    QVERIFY(!written);
    QVERIFY(service.client() == nullptr);
    QVERIFY(service.gate() == Gate::NETWORK_OFF);
    QCOMPARE(changed.size(), 1);

    sources.evonodes = {Ipv4Evonode2()};
    f.setNetworkActive(true);
    QVERIFY(service.client() != nullptr);
    QCOMPARE(sources.made.size(), size_t{2});
    QVERIFY(sources.made.back().endpoints == std::vector<CService>{Ipv4Evonode2()});
}

//! A changed valid evonode set rebuilds an idle client for the new set; an
//! unchanged one keeps it.
void PlatformTests::evonodeChangeRebuildsWhenIdle()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    FakeSources sources;
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    const auto read{PendingRead(*service.client())};

    f.listRead();
    QCOMPARE(sources.made.size(), size_t{1});
    QVERIFY(!read.expired());

    QSignalSpy changed{&service, &PlatformService::changed};
    sources.evonodes = {Ipv4Evonode(), Ipv4Evonode2()};
    f.listRead();
    QCOMPARE(sources.made.size(), size_t{2});
    QVERIFY((sources.made.back().endpoints == std::vector<CService>{Ipv4Evonode(), Ipv4Evonode2()}));
    QVERIFY(read.expired());
    QVERIFY(service.client() != nullptr);
    QCOMPARE(changed.size(), 1);
}

//! A changed set while a write is in flight pauses new calls and lets the
//! write finish on its client; the rebuild comes before any other call. A
//! set that changes back first unpauses the client as it is, and a set with
//! no evonode left waits for the write too.
void PlatformTests::rebuildWaitsForWriteInFlight()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    FakeSources sources;
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    FakePlatformClient* first{sources.fake};
    bool written{false};
    const auto write{PendingWrite(service, f.wallet_model.wallet(), written)};
    QSignalSpy changed{&service, &PlatformService::changed};

    sources.evonodes = {Ipv4Evonode2()};
    f.listRead();
    QVERIFY(service.client() == nullptr);
    QCOMPARE(changed.size(), 1);
    sources.evonodes = {Ipv4Evonode()};
    f.listRead();
    QVERIFY(service.client() == first);
    QCOMPARE(changed.size(), 2);

    sources.evonodes = {Ipv4Evonode2()};
    f.listRead();
    QVERIFY(service.client() == nullptr);
    QVERIFY(!write.expired());
    QCOMPARE(sources.made.size(), size_t{1});
    first->deliverAll();
    QVERIFY(written);
    QCoreApplication::processEvents();
    QCOMPARE(sources.made.size(), size_t{2});
    QVERIFY(sources.made.back().endpoints == std::vector<CService>{Ipv4Evonode2()});
    QVERIFY(service.client() != nullptr);

    FakePlatformClient* second{sources.fake};
    bool second_written{false};
    const auto second_write{PendingWrite(service, f.wallet_model.wallet(), second_written)};
    sources.evonodes.clear();
    f.listRead();
    QVERIFY(service.client() == nullptr);
    QVERIFY(!second_write.expired());
    second->deliverAll();
    QVERIFY(second_written);
    QCoreApplication::processEvents();
    QVERIFY(service.gate() == Gate::NO_EVONODES);
    QCOMPARE(sources.made.size(), size_t{2});
}

//! With -proxy the client connects through it, keeping -proxyrandomize, to
//! the evonodes the proxy reaches: IPv4, IPv6 and onion, never I2P.
void PlatformTests::clientUsesTheProxy()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    FakeSources sources;
    sources.settings.onion = true;
    sources.settings.clearnet_proxy = sources.settings.onion_proxy = Tor();
    sources.evonodes = {I2pEvonode(), OnionEvonode(), Ipv6Evonode(), Ipv4Evonode()};
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    QCOMPARE(sources.made.size(), size_t{1});
    QVERIFY(sources.made.back().proxy == Tor());
    const auto& endpoints{sources.made.back().endpoints};
    QCOMPARE(endpoints.size(), size_t{3});
    QVERIFY(std::find(endpoints.begin(), endpoints.end(), I2pEvonode()) == endpoints.end());
}

//! Under -onlynet=onion the Tor controller sets the onion proxy once it has
//! connected; the next read of the list starts the client through it, to
//! onion evonodes only.
void PlatformTests::onionProxyLaterStartsClient()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    FakeSources sources;
    sources.settings.ipv4 = sources.settings.ipv6 = false;
    sources.settings.onion = true;
    sources.evonodes = {Ipv4Evonode(), OnionEvonode()};
    PlatformPage page{sources.get()};
    f.attach(page);
    QVERIFY(sources.made.empty());
    QVERIFY(page.service()->gate() == Gate::NO_ROUTE);
    QVERIFY(VisibleTexts(page).contains(page.service()->reason()));

    sources.settings.onion_proxy = Tor();
    f.listRead();
    QCOMPARE(sources.made.size(), size_t{1});
    QVERIFY(sources.made.back().proxy == Tor());
    QVERIFY(sources.made.back().endpoints == std::vector<CService>{OnionEvonode()});
}

//! A configuration the SDK refuses is said on the tab and not tried again
//! until the evonode set changes; a set that changes back to it waits for a
//! write in flight like any other change.
void PlatformTests::refusedClientIsShownNotRetried()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    FakeSources sources;
    sources.refuse = true;
    PlatformPage page{sources.get()};
    f.attach(page);
    QVERIFY(page.service()->gate() == Gate::CLIENT_FAILED);
    QVERIFY(VisibleTexts(page).contains(page.service()->reason()));
    f.listRead();
    QCOMPARE(sources.made.size(), size_t{1});

    sources.refuse = false;
    sources.evonodes = {Ipv4Evonode2()};
    f.listRead();
    QCOMPARE(sources.made.size(), size_t{2});
    QVERIFY(page.service()->client() != nullptr);

    bool written{false};
    const auto write{PendingWrite(*page.service(), f.wallet_model.wallet(), written)};
    sources.evonodes = {Ipv4Evonode()};
    f.listRead();
    QVERIFY(!write.expired());
    QVERIFY(page.service()->client() == nullptr);
    sources.fake->deliverAll();
    QVERIFY(written);
    QCoreApplication::processEvents();
    QVERIFY(page.service()->gate() == Gate::CLIENT_FAILED);
    QCOMPARE(sources.made.size(), size_t{2});
}

//! Shutdown detaches the client model before it and the wallet models are
//! deleted, and unloading a wallet deletes its page: either way consumers
//! are told, and the client goes with the service.
void PlatformTests::detachingClientModelStopsClient()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    FakeSources sources;
    std::vector<PlatformService*> told;
    const auto tell = [&told](PlatformService* service) { told.push_back(service); };

    PlatformPage page{sources.get()};
    f.attach(page);
    const auto read{PendingRead(*page.service()->client())};
    QObject::connect(&page, &PlatformPage::serviceChanged, tell);
    page.setClientModel(nullptr);
    QVERIFY(read.expired());
    QVERIFY(page.service() == nullptr);
    QCOMPARE(told, std::vector<PlatformService*>{nullptr});

    told.clear();
    auto unloaded{std::make_unique<PlatformPage>(sources.get())};
    f.attach(*unloaded);
    const auto unloaded_read{PendingRead(*unloaded->service()->client())};
    QObject::connect(unloaded.get(), &PlatformPage::serviceChanged, tell);
    unloaded.reset();
    QVERIFY(unloaded_read.expired());
    QCOMPARE(told, std::vector<PlatformService*>{nullptr});
}
