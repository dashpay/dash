// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/platformtests.h>

#include <qt/test/masternodetestutil.h>

#include <chainparams.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <netbase.h>
#include <platform/walletrecords.h>
#include <qt/platform/dashpayoptionswidget.h>
#include <qt/platform/platformoptindialog.h>
#include <qt/platform/platformpage.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/platformui.h>
#include <qt/walletmodel.h>
#include <test/util/platform_client.h>
#include <test/util/setup_common.h>
#include <util/system.h>
#include <wallet/context.h>
#include <wallet/wallet.h>

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTimer>

#include <memory>
#include <optional>
#include <string>

using MasternodeTestUtil::GuiModels;
using MasternodeTestUtil::MakeTestWallet;
using MasternodeTestUtil::WalletGuard;
using wallet::WalletContext;

namespace {
//! One test's node, wallet and models. The node context is reset only
//! after the models (and the client feed thread they run) are gone.
template <typename ChainSetup>
struct FixtureWithChain {
    ChainSetup chain{CBaseChainParams::REGTEST};
    struct ContextReset {
        interfaces::Node& node;
        ~ContextReset() { node.setContext(nullptr); }
    } reset;
    interfaces::Node& node;
    WalletContext& context;
    std::shared_ptr<wallet::CWallet> wallet;
    WalletGuard guard;
    GuiModels models;
    WalletModel wallet_model;

    explicit FixtureWithChain(interfaces::Node& node_in) :
        reset{node_in},
        node(node_in),
        context((node.setContext(&chain.m_node), *node.walletLoader().context())),
        wallet(MakeTestWallet(node, context, "platform")),
        guard(context, wallet),
        models(node),
        wallet_model(interfaces::MakeWallet(context, wallet), models.client)
    {
    }
};
//! A synced node: 100 recent blocks.
using Fixture = FixtureWithChain<TestChain100Setup>;
//! A node still in initial block download: only the (old) genesis block.
using SyncingFixture = FixtureWithChain<TestingSetup>;

//! Replaces the node's network settings DashPay reads while it lives: a
//! proxy set on the node could not be unset for the tests after it.
struct NetworkSettingsOverride {
    explicit NetworkSettingsOverride(const PlatformNetworkSettings& settings)
    {
        PlatformNetworkSettings::SetForTesting(settings);
    }
    ~NetworkSettingsOverride() { PlatformNetworkSettings::SetForTesting(std::nullopt); }
};

//! -proxy=127.0.0.1:9050 with -proxyrandomize: the same proxy for IPv4,
//! IPv6 and onion.
PlatformNetworkSettings ProxiedSettings()
{
    PlatformNetworkSettings settings;
    settings.onion = true;
    settings.clearnet_proxy = settings.onion_proxy = platform::ProxyConfig{LookupNumeric("127.0.0.1", 9050), true};
    return settings;
}

//! Wait for the endpoint set the service collects on its worker thread
//! after construction (or a node event) to reach the client.
bool WaitForEndpointUpdates(const FakePlatformClient& fake, size_t count)
{
    for (int i = 0; i < 200 && fake.endpoint_updates.size() < count; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QTest::qWait(5);
    }
    return fake.endpoint_updates.size() == count;
}
} // namespace

//! Without the per-wallet opt-in the page never constructs a service, so no
//! client and no connection exist; with the opt-in but network settings that
//! leave DashPay no network (-onlynet=onion without an onion proxy) the
//! service is refused with a visible reason. A proxy is not a reason.
void PlatformTests::optInGating()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    // The test chain is regtest, which has no Platform chain id; the GUI
    // override supplies one as it would on a devnet.
    gArgs.ForceSetArg("-platformchainid", "dash-devnet-test");
    QCOMPARE(QString::fromStdString(PlatformService::EffectiveChainId()), QString("dash-devnet-test"));

    QVERIFY(!PlatformService::IsEnabled(f.wallet_model.wallet()));
    QVERIFY(!PlatformService::Availability(f.wallet_model, f.models.client).enabled);

    PlatformPage page;
    page.setWalletModel(&f.wallet_model);
    page.setClientModel(&f.models.client);
    QVERIFY(page.findChild<PlatformService*>() == nullptr);
    QCOMPARE(page.findChild<QStackedWidget*>()->currentIndex(), 0);

    // Opting in records no chain id: only a verified response stamps one.
    QVERIFY(PlatformService::Enable(f.wallet_model.wallet()));
    QVERIFY(PlatformService::IsEnabled(f.wallet_model.wallet()));
    QVERIFY(f.wallet_model.wallet().getPlatformData(platform::records::CHAIN_ID).empty());

    // No network DashPay can use: enabled, but refused with a reason and no
    // service.
    {
        PlatformNetworkSettings onion_only;
        onion_only.ipv4 = onion_only.ipv6 = false;
        onion_only.onion = true;
        const NetworkSettingsOverride settings{onion_only};
        const PlatformAvailability excluded{PlatformService::Availability(f.wallet_model, f.models.client)};
        QVERIFY(excluded.enabled);
        QVERIFY(excluded.gate == PlatformAvailability::Gate::NETWORK_SETTINGS);
        QVERIFY(!excluded.reason.isEmpty());
        QVERIFY(!excluded.available());
        page.setClientModel(&f.models.client);
        QVERIFY(page.findChild<PlatformService*>() == nullptr);
    }

    // A proxy is no gate: the only remaining one on this node is the
    // ChainLock, which the test chain never has.
    const NetworkSettingsOverride settings{ProxiedSettings()};
    const PlatformAvailability no_chainlock{PlatformService::Availability(f.wallet_model, f.models.client)};
    QVERIFY(no_chainlock.enabled);
    QVERIFY(no_chainlock.gate == PlatformAvailability::Gate::NO_CHAINLOCK);
    QVERIFY(page.findChild<PlatformService*>() == nullptr);

    // Opting out wipes every Platform record.
    QVERIFY(PlatformService::WipeRecords(f.wallet_model.wallet()));
    QVERIFY(!PlatformService::IsEnabled(f.wallet_model.wallet()));
    QVERIFY(f.wallet_model.wallet().getPlatformData("platform/").empty());
}

//! The route follows Core's rules for its own connections: -proxy (or none)
//! for IPv4 and IPv6, onion evonodes only through that same proxy, the onion
//! proxy alone under -onlynet=onion, never I2P or CJDNS; and the settings
//! are read from the node's reachable networks.
void PlatformTests::routeSelection()
{
    const CService ipv4{LookupNumeric("203.0.113.7", 443)};
    const CService ipv6{LookupNumeric("2001:db8::7", 443)};
    CNetAddr onion_addr;
    QVERIFY(onion_addr.SetSpecial("pg6mmjiyjmcrsslvykfwnntlaru7p5svn6y2ymmju6nubxndf4pscryd.onion"));
    const CService onion{onion_addr, 443};
    CNetAddr i2p_addr;
    QVERIFY(i2p_addr.SetSpecial("udhdrtrcetjm5sxzskjyr5ztpeszydbh4dpl3pl4utgqqw2v4jna.b32.i2p"));
    const CService i2p{i2p_addr, 443};
    const platform::ProxyConfig tor{LookupNumeric("127.0.0.1", 9050), true};
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
        {"-proxy", true, true, true, tor, tor, true, tor, true, true, true},
        {"-proxy -onion", true, true, true, tor, other, true, tor, true, true, false},
        {"-proxy -noonion", true, true, false, tor, std::nullopt, true, tor, true, true, false},
        {"-proxy=unix:", true, true, true, unix_socket, unix_socket, true, unix_socket, true, true, true},
        {"-onlynet=ipv4", true, false, false, std::nullopt, std::nullopt, true, std::nullopt, true, false, false},
        {"-onlynet=onion -onion", false, false, true, std::nullopt, tor, true, tor, false, false, true},
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
        QVERIFY2(route->allows(ipv4) == c.allows_ipv4, c.name);
        QVERIFY2(route->allows(ipv6) == c.allows_ipv6, c.name);
        QVERIFY2(route->allows(onion) == c.allows_onion, c.name);
        QVERIFY2(!route->allows(i2p), c.name);
    }
    // -proxyrandomize=0 reaches the client as it is.
    PlatformNetworkSettings shared;
    shared.clearnet_proxy = platform::ProxyConfig{LookupNumeric("127.0.0.1", 9050), false};
    QVERIFY(!PlatformRoute::Choose(shared)->proxy->randomize_credentials);

    // The node's reachable networks, not -onlynet itself.
    const PlatformNetworkSettings defaults{PlatformNetworkSettings::Read(m_node)};
    QVERIFY(defaults.ipv4 && defaults.ipv6);
    QVERIFY(!defaults.clearnet_proxy && !defaults.onion_proxy);
    g_reachable_nets.Remove(NET_IPV4);
    g_reachable_nets.Remove(NET_IPV6);
    const PlatformNetworkSettings no_clearnet{PlatformNetworkSettings::Read(m_node)};
    g_reachable_nets.Add(NET_IPV4);
    g_reachable_nets.Add(NET_IPV6);
    QVERIFY(!no_clearnet.ipv4 && !no_clearnet.ipv6);
}

//! A service whose route reaches none of the evonodes (only IPv4 ones under
//! -onlynet=onion) pushes no endpoint, so nothing leaves the machine, and
//! says why; an evonode on the route is pushed and clears it.
void PlatformTests::endpointsOffTheRouteAreNotPushed()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    gArgs.ForceSetArg("-platformchainid", "dash-devnet-test");
    QVERIFY(PlatformService::Enable(f.wallet_model.wallet()));
    PlatformRoute onion_only;
    onion_only.proxy = platform::ProxyConfig{LookupNumeric("127.0.0.1", 9050), true};
    onion_only.ipv4 = onion_only.ipv6 = false;
    onion_only.onion = true;
    auto client{std::make_unique<FakePlatformClient>()};
    FakePlatformClient* fake{client.get()};
    PlatformService service{f.wallet_model, f.models.client, std::move(client), onion_only};
    QSignalSpy reachability(&service, &PlatformService::reachabilityChanged);
    QVERIFY(WaitForEndpointUpdates(*fake, 1));
    QVERIFY(!service.unreachable());

    service.setEndpointsForTesting({platform::Endpoint{LookupNumeric("203.0.113.7", 443)}});
    QCOMPARE(fake->endpoint_updates.size(), size_t{2});
    QVERIFY(fake->endpoint_updates.back().empty());
    QVERIFY(!service.haveEndpoints());
    QVERIFY(service.unreachable());
    QCOMPARE(reachability.size(), 1);
    const PlatformAvailability unreachable{PlatformService::Availability(f.wallet_model, f.models.client, &service)};
    QVERIFY(unreachable.gate == PlatformAvailability::Gate::UNREACHABLE);
    QVERIFY(unreachable.reason.contains("evonode"));

    CNetAddr onion;
    QVERIFY(onion.SetSpecial("pg6mmjiyjmcrsslvykfwnntlaru7p5svn6y2ymmju6nubxndf4pscryd.onion"));
    service.setEndpointsForTesting(
        {platform::Endpoint{LookupNumeric("203.0.113.7", 443)}, platform::Endpoint{CService{onion, 443}}});
    QCOMPARE(fake->endpoint_updates.back().size(), size_t{1});
    QVERIFY(fake->endpoint_updates.back().front().service.IsTor());
    QVERIFY(service.haveEndpoints());
    QVERIFY(!service.unreachable());
    QCOMPARE(reachability.size(), 2);
    QVERIFY(PlatformService::Availability(f.wallet_model, f.models.client, &service).gate !=
            PlatformAvailability::Gate::UNREACHABLE);
}

//! The opt-in says the wallet connects through evonodes and what the
//! answering evonode sees, without calling their operators strangers.
void PlatformTests::optInDisclosureCopy()
{
    PlatformOptInDialog dialog{"qa-carol"};
    QStringList texts;
    for (const auto* label : dialog.findChildren<QLabel*>()) {
        texts << label->text();
    }
    const QString text{texts.join(' ')};
    QVERIFY(text.contains("qa-carol"));
    QVERIFY(text.contains("evonodes"));
    QVERIFY(text.contains("proxy"));
    for (const char* gone : {"other people", "operator", "not applied", "stays off"}) {
        QVERIFY2(!text.contains(gone), gone);
    }
}

namespace {
//! Answer the next modal dialog DashPay opens once it shows: tick the opt-in
//! and enable, or press the confirmation's button labelled `button`.
void AnswerNextDialog(const QString& button)
{
    QTimer::singleShot(0, [button] {
        for (QWidget* dialog : QApplication::topLevelWidgets()) {
            if (!dialog->isVisible() || !dialog->isModal()) continue;
            if (auto* acknowledge{dialog->findChild<QCheckBox*>()}) acknowledge->setChecked(true);
            for (auto* candidate : dialog->findChildren<QPushButton*>()) {
                if (candidate->text() == button) candidate->click();
            }
        }
    });
}
} // namespace

//! DashPay is turned on and off in the Options dialog's Wallet page, for
//! the wallet shown, at once and through the opt-in or its confirmation.
void PlatformTests::dashPayOptionsSection()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    gArgs.ForceSetArg("-platformchainid", "dash-devnet-test");
    PlatformPage page;
    page.setWalletModel(&f.wallet_model);
    page.setClientModel(&f.models.client);
    DashPayOptionsWidget section{page, f.wallet_model};
    const auto state = [&section] { return section.findChildren<QLabel*>().first()->text(); };
    QPushButton* button{section.findChild<QPushButton*>()};
    QVERIFY(button != nullptr);
    QVERIFY(state().contains("off for wallet"));
    QCOMPARE(button->text(), QString("Enable DashPay…"));

    AnswerNextDialog("Enable DashPay");
    button->click();
    QVERIFY(PlatformService::IsEnabled(f.wallet_model.wallet()));
    QVERIFY(state().contains("on for wallet"));
    QCOMPARE(button->text(), QString("Disable DashPay…"));

    // Cancel is the confirmation's default: nothing changes.
    AnswerNextDialog("Cancel");
    button->click();
    QVERIFY(PlatformService::IsEnabled(f.wallet_model.wallet()));
    AnswerNextDialog("Disable DashPay");
    button->click();
    QVERIFY(!PlatformService::IsEnabled(f.wallet_model.wallet()));
    QVERIFY(f.wallet_model.wallet().getPlatformData("platform/").empty());
    QVERIFY(state().contains("off for wallet"));
    QCOMPARE(page.findChild<QStackedWidget*>()->currentIndex(), 0);

    // The DashPay page itself has no Disable button.
    for (const auto* candidate : page.findChildren<QPushButton*>()) {
        QVERIFY(!candidate->text().startsWith("Disable"));
    }
}

//! -platformchainid is a testing aid: on mainnet params the CChainParams
//! value always wins, whatever the argument says.
void PlatformTests::mainnetChainIdNotOverridable()
{
    {
        BasicTestingSetup mainnet{CBaseChainParams::MAIN};
        gArgs.ForceSetArg("-platformchainid", "evo-fake");
        QCOMPARE(QString::fromStdString(PlatformService::EffectiveChainId()), QString("evo1"));
    }
    {
        BasicTestingSetup testnet{CBaseChainParams::TESTNET};
        gArgs.ForceSetArg("-platformchainid", "evo-fake");
        QCOMPARE(QString::fromStdString(PlatformService::EffectiveChainId()), QString("evo-fake"));
        gArgs.ForceSetArg("-platformchainid", "");
        QCOMPARE(QString::fromStdString(PlatformService::EffectiveChainId()), QString("dash-testnet-51"));
    }
}

//! Write the chain id a wallet's records were stamped with.
void StampChainId(interfaces::Wallet& wallet, const std::string& chain_id)
{
    wallet.writePlatformData(platform::records::CHAIN_ID, {chain_id.begin(), chain_id.end()});
}

std::string RecordedChainId(interfaces::Wallet& wallet)
{
    const auto records{wallet.getPlatformData(platform::records::CHAIN_ID)};
    const auto it{records.find(platform::records::CHAIN_ID)};
    return it == records.end() ? std::string{} : std::string{it->second.begin(), it->second.end()};
}

//! A service over a scripted client: records for another chain enter the
//! network-changed state, in which nothing signs until the state is
//! discarded; while network activity is off the endpoint set pushed to the
//! client is empty.
void PlatformTests::networkChangedAndInactive()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    gArgs.ForceSetArg("-platformchainid", "dash-devnet-test");
    QVERIFY(PlatformService::Enable(f.wallet_model.wallet()));
    StampChainId(f.wallet_model.wallet(), "dash-devnet-old");
    f.node.setNetworkActive(false);

    auto client{std::make_unique<FakePlatformClient>()};
    FakePlatformClient* fake{client.get()};
    PlatformService service{f.wallet_model, f.models.client, std::move(client)};

    QVERIFY(service.networkChanged());
    QVERIFY(!service.beginSigningOperation(platform::OperationKind::PROFILE, {1}, std::nullopt, std::nullopt));
    QVERIFY(WaitForEndpointUpdates(*fake, 1));
    QVERIFY(fake->endpoint_updates.front().empty());

    service.discardStateAfterNetworkChange();
    QVERIFY(!service.networkChanged());
    QVERIFY(PlatformService::IsEnabled(f.wallet_model.wallet()));
    // Discarding leaves the chain id to the next verified response: a
    // wallet never stamped with it is not "changed" under any chain id.
    QVERIFY(RecordedChainId(f.wallet_model.wallet()).empty());
    gArgs.ForceSetArg("-platformchainid", "dash-devnet-other");
    QVERIFY(!PlatformService(f.wallet_model, f.models.client, std::make_unique<FakePlatformClient>()).networkChanged());
    gArgs.ForceSetArg("-platformchainid", "dash-devnet-test");

    // An unencrypted wallet mints an operation without a prompt; it signs
    // only with the keys it was minted for.
    const auto op{service.beginSigningOperation(platform::OperationKind::PROFILE, {1}, std::nullopt, std::nullopt)};
    QVERIFY(op.has_value());
    QVERIFY(op->allowsKey(1));
    QVERIFY(!op->allowsKey(0));

    f.node.setNetworkActive(true);
}

//! A node that has not finished its initial block download is refused by
//! the availability gate, and a service over it pushes no endpoints: the
//! masternode list and quorums at a stale tip are not the network's.
void PlatformTests::syncingNodePushesNoEndpoints()
{
    SyncingFixture f{m_node};
    QVERIFY(f.models.ok);
    QVERIFY(f.node.isInitialBlockDownload());
    gArgs.ForceSetArg("-platformchainid", "dash-devnet-test");
    QVERIFY(PlatformService::Enable(f.wallet_model.wallet()));
    QVERIFY(f.node.getNetworkActive());

    const PlatformAvailability syncing{PlatformService::Availability(f.wallet_model, f.models.client)};
    QVERIFY(syncing.enabled);
    QVERIFY(!syncing.available());
    QVERIFY(syncing.reason.contains("syncing"));

    auto client{std::make_unique<FakePlatformClient>()};
    FakePlatformClient* fake{client.get()};
    PlatformService service{f.wallet_model, f.models.client, std::move(client)};
    QVERIFY(WaitForEndpointUpdates(*fake, 1));
    QVERIFY(fake->endpoint_updates.front().empty());
}

//! Network activity off empties the client's endpoints; turning it on
//! collects and pushes them again at once (the dashboard reads again when
//! they arrive, endpointsAvailable()), and consumers hear of both changes.
void PlatformTests::networkResumePushesEndpointsAgain()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    gArgs.ForceSetArg("-platformchainid", "dash-devnet-test");
    QVERIFY(PlatformService::Enable(f.wallet_model.wallet()));
    auto client{std::make_unique<FakePlatformClient>()};
    FakePlatformClient* fake{client.get()};
    PlatformService service{f.wallet_model, f.models.client, std::move(client)};
    QSignalSpy active(&service, &PlatformService::networkActiveChanged);
    QVERIFY(WaitForEndpointUpdates(*fake, 1));
    // The test node has no UI interface: the notification the GUI would
    // get is sent by hand.
    f.node.setNetworkActive(false);
    Q_EMIT f.models.client.networkActiveChanged(false);
    QVERIFY(WaitForEndpointUpdates(*fake, 2));
    QVERIFY(fake->endpoint_updates.back().empty());
    QVERIFY(!service.networkActive());
    QVERIFY(!service.haveEndpoints());
    f.node.setNetworkActive(true);
    Q_EMIT f.models.client.networkActiveChanged(true);
    QVERIFY(WaitForEndpointUpdates(*fake, 3));
    QVERIFY(service.networkActive());
    QCOMPARE(active.size(), 2);
    QVERIFY(!active.at(0).at(0).toBool() && active.at(1).at(0).toBool());

    // Turned off and on again before the collection for "off" landed: the
    // endpoints are collected once more for "on" rather than at the next
    // timer tick, and only that collection is pushed.
    f.node.setNetworkActive(false);
    Q_EMIT f.models.client.networkActiveChanged(false);
    f.node.setNetworkActive(true);
    Q_EMIT f.models.client.networkActiveChanged(true);
    QVERIFY(WaitForEndpointUpdates(*fake, 4));

    // Turned on and off again while a collection runs: what it collected
    // under "on" is never pushed, only the set collected for "off".
    f.node.setNetworkActive(true);
    Q_EMIT f.models.client.networkActiveChanged(true);
    f.node.setNetworkActive(false);
    Q_EMIT f.models.client.networkActiveChanged(false);
    QVERIFY(WaitForEndpointUpdates(*fake, 5));
    QTest::qWait(200);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    QCOMPARE(fake->endpoint_updates.size(), size_t{5});
    QVERIFY(fake->endpoint_updates.back().empty());
    QVERIFY(!service.haveEndpoints());
}

//! Broadcast decisions are typed: success is OK or ALREADY_EXISTS, which
//! carry no user-visible error, and every failure kind has a plain sentence;
//! the raw result (code, internal message) is only in the details.
void PlatformTests::statusDescriptions()
{
    using platform::StatusKind;
    using PlatformUi::Context;
    QVERIFY(PlatformUi::Describe(platform_test::BroadcastStatus(StatusKind::OK), Context::READ, {}).text.isEmpty());
    QVERIFY(
        PlatformUi::Describe(platform_test::BroadcastStatus(StatusKind::ALREADY_EXISTS), Context::READ, {}).text.isEmpty());
    for (const auto kind : {StatusKind::CONSENSUS, StatusKind::UNAVAILABLE, StatusKind::REJECTED,
                            StatusKind::CHAIN_ID_MISMATCH, StatusKind::UNSUPPORTED_PROTOCOL_VERSION, StatusKind::INTERNAL}) {
        platform::Status status{platform_test::BroadcastStatus(kind, 12345)};
        status.message = "internal detail 0xdeadbeef";
        const auto error{PlatformUi::Describe(status, Context::PROFILE, "Save profile")};
        QVERIFY(!error.text.isEmpty());
        QVERIFY(error.text.at(0).isUpper());
        QVERIFY(!error.text.contains("12345"));
        QVERIFY(!error.text.contains("deadbeef"));
        QVERIFY(error.details.contains("Operation: Save profile"));
        QVERIFY(error.details.contains("Message: internal detail 0xdeadbeef"));
    }
    // A consensus code means what it means in its context, with the name in
    // the details.
    const auto taken{PlatformUi::Describe(platform_test::BroadcastStatus(StatusKind::CONSENSUS, 40105),
                                          Context::NAME_REGISTER, "Register username", "alice")};
    QVERIFY(taken.text.contains("alice"));
    QVERIFY(taken.details.contains("40105 DuplicateUniqueIndexError"));
    const auto sent{PlatformUi::Describe(platform_test::BroadcastStatus(StatusKind::CONSENSUS, 40105),
                                         Context::CONTACT_REQUEST, "Send contact request", "bob")};
    QVERIFY(sent.text.contains("already sent a contact request to bob"));
    QVERIFY(PlatformUi::Describe(platform_test::BroadcastStatus(StatusKind::CONSENSUS, 10405), Context::PROFILE, {})
                .text.contains("Your funds are safe"));
    // Only the registration steps retry on their own.
    QVERIFY(PlatformUi::Describe(platform_test::BroadcastStatus(StatusKind::UNAVAILABLE), Context::NAME_REGISTER, {})
                .text.contains("keep trying"));
    QVERIFY(!PlatformUi::Describe(platform_test::BroadcastStatus(StatusKind::UNAVAILABLE), Context::SEARCH, {})
                 .text.contains("keep trying"));
    // -platformchainid is named only where it was set: mainnet refuses it.
    const platform::Status mismatch{platform_test::BroadcastStatus(StatusKind::CHAIN_ID_MISMATCH)};
    gArgs.ForceSetArg("-platformchainid", "dash-devnet-test");
    QVERIFY(PlatformUi::Describe(mismatch, Context::READ, {}).text.contains("-platformchainid"));
    gArgs.LockSettings([](util::Settings& settings) { settings.forced_settings.erase("platformchainid"); });
    QVERIFY(!PlatformUi::Describe(mismatch, Context::READ, {}).text.contains("-platformchainid"));
}

//! A REJECTED read whose block time trails the local clock (the SDK's
//! StaleNodeError::Time) says Platform stopped producing blocks, and when,
//! instead of promising that a retry in a minute will help.
void PlatformTests::staleBlockTimeDescription()
{
    using platform::StatusKind;
    using PlatformUi::Context;
    const int64_t block_ms{1'790'000'000'000};
    const std::string stale_text{"received invalid time: expected 1790014400000ms, received 1790000000000 ms, "
                                 "tolerance 600000 ms; try another server"};
    platform::Status stale{platform_test::BroadcastStatus(StatusKind::REJECTED)};
    stale.message = stale_text;
    QCOMPARE(PlatformUi::StaleBlockTimeMs(stale), std::optional<int64_t>{block_ms});
    const auto error{PlatformUi::Describe(stale, Context::READ, "Load identity")};
    QVERIFY(error.text.contains("hasn't produced a new block since"));
    QVERIFY(error.text.contains(GUIUtil::dateTimeStr(QDateTime::fromMSecsSinceEpoch(block_ms))));
    QVERIFY(!error.text.contains("Try again in a minute"));
    QVERIFY(error.details.contains(QString::fromStdString("Message: " + stale_text)));
    // Wrapped by the SDK's retry exhaustion: still the same halt.
    stale.message = "no available addresses to retry, last error: " + stale_text;
    QCOMPARE(PlatformUi::StaleBlockTimeMs(stale), std::optional<int64_t>{block_ms});

    // A block time ahead of the local clock is this computer's clock, not a
    // halted chain; other rejections and other kinds keep their texts.
    platform::Status ahead{platform_test::BroadcastStatus(StatusKind::REJECTED)};
    ahead.message = "received invalid time: expected 1790000000000ms, received 1790014400000 ms, tolerance 600000 ms; "
                    "try another server";
    QVERIFY(!PlatformUi::StaleBlockTimeMs(ahead));
    QVERIFY(PlatformUi::Describe(ahead, Context::READ, {}).text.contains("Try again in a minute"));
    platform::Status height{platform_test::BroadcastStatus(StatusKind::REJECTED)};
    height.message = "received height is outdated: expected 100, received 10, tolerance 1; try another server";
    QVERIFY(!PlatformUi::StaleBlockTimeMs(height));
    QVERIFY(PlatformUi::Describe(height, Context::READ, {}).text.contains("Try again in a minute"));
    platform::Status unavailable{platform_test::BroadcastStatus(StatusKind::UNAVAILABLE)};
    unavailable.message = stale_text;
    QVERIFY(!PlatformUi::StaleBlockTimeMs(unavailable));
}
