// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/platformtests.h>

#include <qt/test/masternodetestutil.h>

#include <chainlock/chainlock.h>
#include <chainparams.h>
#include <hash.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <netbase.h>
#include <platform/helpers.h>
#include <platform/walletrecords.h>
#include <primitives/transaction.h>
#include <qt/bitcoinunits.h>
#include <qt/optionsmodel.h>
#include <qt/platform/createusernamewizard.h>
#include <qt/platform/dashpayoptionswidget.h>
#include <qt/platform/identityflow.h>
#include <qt/platform/platformoptindialog.h>
#include <qt/platform/platformpage.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/platformui.h>
#include <qt/walletmodel.h>
#include <script/standard.h>
#include <spork.h>
#include <txmempool.h>
#include <test/util/platform_client.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <validation.h>
#include <wallet/context.h>
#include <wallet/platformtypes.h>
#include <wallet/wallet.h>

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTimer>

#include <memory>
#include <optional>
#include <string>

using MasternodeTestUtil::GuiModels;
using MasternodeTestUtil::MakeCoinbaseWallet;
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

//! Deliver every callback the flows re-posted to the GUI thread.
void Drain()
{
    for (int i = 0; i < 20; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents);
}

//! Let (mock) time pass for the flows' confirmation backoff.
void Elapse(int64_t seconds) { SetMockTime(GetTime() + seconds); }

//! Drive the identity flow one step and deliver what it posted.
void Step(IdentityFlow& flow)
{
    flow.advance();
    Drain();
}

//! A verified read showed Platform running `protocol_version`, which the
//! client needs before it builds any transition.
void VerifiedAt(PlatformService& service, uint32_t protocol_version = 14)
{
    service.observeStatus(platform_test::BroadcastStatus(platform::StatusKind::OK), /*verified_read=*/true,
                          protocol_version);
}

platform::Built ScriptedBuild(uint8_t byte)
{
    platform::Built built;
    built.bytes = {byte, byte, byte};
    built.object_id = platform_test::IdentifierFromByte(byte);
    return built;
}

//! An opted-in service over a scripted client, with the record of an
//! identity confirmed on Platform that still has to register `label`.
struct IdentityFixture : Fixture {
    FakePlatformClient* fake{nullptr};
    std::unique_ptr<PlatformService> service;
    const platform::Identifier my_id{platform_test::IdentifierFromByte(0x1D)};

    explicit IdentityFixture(interfaces::Node& node_in, const std::string& label) :
        Fixture(node_in)
    {
        PlatformService::Enable(wallet_model.wallet());
        auto client{std::make_unique<FakePlatformClient>()};
        fake = client.get();
        service = std::make_unique<PlatformService>(wallet_model, models.client, std::move(client));

        platform::IdentityRecord record;
        record.state = platform::IdentityRecord::State::IDENTITY_CONFIRMED;
        record.identity_id = my_id;
        record.auth_key_id = 1;
        record.encryption_key_id = 2;
        record.decryption_key_id = 3;
        record.label = label;
        record.normalized_label = platform::helpers::NormalizeLabel(label);
        record.preorder_salt.fill(0x5A);
        writeRecord(record);
    }

    void writeRecord(const platform::IdentityRecord& record)
    {
        service->writeRecord(platform::records::IDENTITY, platform::SerializeIdentityRecord(record));
        service->identityFlow().reload();
    }

    //! Our identity as Platform would return it, with the four registered keys.
    platform::Identity myIdentity() const
    {
        platform::Identity identity;
        identity.id = my_id;
        for (const auto& spec : IdentityFlow::RegistrationKeys()) {
            const auto pubkey{wallet_model.wallet().getPlatformPubKey(wallet::IdentityAuthKey{0, spec.id})};
            platform::IdentityPublicKey key;
            key.id = spec.id;
            key.purpose = spec.purpose;
            key.security_level = spec.security_level;
            key.data.assign(pubkey.value.begin(), pubkey.value.end());
            identity.public_keys.push_back(key);
        }
        return identity;
    }

    //! Script the proved resolve a registration makes before it preorders:
    //! the name is free.
    void scriptNameCheck() { fake->resolve_name.push_back(platform_test::Absent<platform::DpnsName>()); }

    //! Script the proved reads one document step makes before it builds:
    //! our identity and the DPNS contract nonce.
    void scriptDocumentStep(uint64_t nonce)
    {
        fake->identities.push_back(platform_test::Ok(myIdentity()));
        fake->nonces.push_back(platform_test::Ok<uint64_t>(nonce));
    }
};

//! A ChainLock at the test chain's tip, so every gate of the page is open.
void LockTip(TestChain100Setup& chain)
{
    const auto* tip{WITH_LOCK(chain.m_node.chainman->GetMutex(), return chain.m_node.chainman->ActiveChain().Tip())};
    chain.m_node.sporkman->SetSporkAddress(Params().SporkAddress());
    chain.m_node.sporkman->SetPrivKey("cP4EKFyJsHT39LDqgdcB43Y3YXjNyjb5Fuas1GQSeAtjnZWmZEQK");
    chain.m_node.sporkman->UpdateSpork(SPORK_19_CHAINLOCKS_ENABLED, 0);
    chain.m_node.chainlocks->UpdateBestChainlock(uint256::ONE, chainlock::ChainLockSig{tip->nHeight, tip->GetBlockHash(), CBLSSignature{}},
                                                 tip);
}

//! An opted-in wallet with `record` on a ChainLocked node, and the DashPay
//! page running its own service over a scripted client.
struct PageFixture : Fixture {
    FakePlatformClient* fake{nullptr};
    //! What the page configured its client with.
    std::optional<platform::ClientConfig> config;
    PlatformPage page;

    PageFixture(interfaces::Node& node_in, const platform::IdentityRecord& record) :
        Fixture(node_in),
        page{nullptr, [this](const platform::ClientConfig& client_config) {
                 config = client_config;
                 auto client{std::make_unique<FakePlatformClient>()};
                 fake = client.get();
                 return client;
             }}
    {
        PlatformService::Enable(wallet_model.wallet());
        wallet_model.wallet().writePlatformData(platform::records::IDENTITY, platform::SerializeIdentityRecord(record));
        LockTip(chain);
        page.setWalletModel(&wallet_model);
        page.setClientModel(&models.client);
    }

    PlatformService& service() const { return *page.findChild<PlatformService*>(); }

    //! Every text a user can read on the page: labels, buttons, tooltips.
    QString texts() const
    {
        QStringList out;
        for (const auto* label : page.findChildren<QLabel*>()) {
            out << label->text() << label->toolTip();
        }
        for (const auto* button : page.findChildren<QPushButton*>()) {
            out << button->text() << button->toolTip();
        }
        return out.join('\n');
    }
};

//! The test chain one block on, so its first coinbase is mature to a wallet
//! holding the coinbase key.
struct MatureChain : TestChain100Setup {
    explicit MatureChain(const std::string& chain_name) :
        TestChain100Setup{chain_name}
    {
        CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    }
};

//! An opted-in wallet with coins to fund a new identity, and a service over
//! a scripted client.
struct FundingFixture : FixtureWithChain<MatureChain> {
    std::shared_ptr<wallet::CWallet> funded;
    WalletGuard funded_guard;
    WalletModel funded_model;
    FakePlatformClient* fake{nullptr};
    std::unique_ptr<PlatformService> service;

    explicit FundingFixture(interfaces::Node& node_in) :
        FixtureWithChain(node_in),
        funded(MakeCoinbaseWallet(node, context, chain, "funded", /*encrypt=*/false)),
        funded_guard(context, funded),
        funded_model(interfaces::MakeWallet(context, funded), models.client)
    {
        // What the cost page reads as available.
        funded_model.pollBalanceChanged();
        PlatformService::Enable(funded_model.wallet());
        restartService();
    }

    //! A new service (as after a restart) over a new scripted client.
    void restartService()
    {
        service.reset();
        auto client{std::make_unique<FakePlatformClient>()};
        fake = client.get();
        service = std::make_unique<PlatformService>(funded_model, models.client, std::move(client));
    }

    //! The asset lock transactions the wallet holds.
    size_t assetLocks() const
    {
        size_t count{0};
        for (const auto& wtx : funded_model.wallet().getWalletTxs()) {
            if (wtx.tx->nType == TRANSACTION_ASSET_LOCK) ++count;
        }
        return count;
    }

    //! What an identity lookup by this wallet's identity MASTER key asks for.
    std::string masterKeyHash() const
    {
        const auto pubkey{funded_model.wallet().getPlatformPubKey(wallet::IdentityAuthKey{0, 0})};
        const uint160 hash{Hash160(pubkey.value)};
        return HexStr(hash);
    }
};

//! A registered identity's record (its username "alice").
platform::IdentityRecord RegisteredRecord()
{
    platform::IdentityRecord record;
    record.state = platform::IdentityRecord::State::REGISTERED;
    record.identity_id = platform_test::IdentifierFromByte(0x1D);
    record.auth_key_id = 1;
    record.encryption_key_id = 2;
    record.decryption_key_id = 3;
    record.label = "alice";
    record.normalized_label = "a11ce";
    return record;
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
    QVERIFY(!PlatformService::IsEnabled(f.wallet_model.wallet()));
    QVERIFY(!PlatformService::Availability(f.wallet_model, f.models.client).enabled);

    PlatformPage page;
    page.setWalletModel(&f.wallet_model);
    page.setClientModel(&f.models.client);
    QVERIFY(page.findChild<PlatformService*>() == nullptr);
    QCOMPARE(page.findChild<QStackedWidget*>()->currentIndex(), 0);

    QVERIFY(PlatformService::Enable(f.wallet_model.wallet()));
    QVERIFY(PlatformService::IsEnabled(f.wallet_model.wallet()));

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

//! A service over a scripted client pushes an empty endpoint set while
//! network activity is off, and mints the operations writes sign under.
void PlatformTests::inactiveNetworkAndSigning()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    QVERIFY(PlatformService::Enable(f.wallet_model.wallet()));
    f.node.setNetworkActive(false);

    auto client{std::make_unique<FakePlatformClient>()};
    FakePlatformClient* fake{client.get()};
    PlatformService service{f.wallet_model, f.models.client, std::move(client)};

    QVERIFY(WaitForEndpointUpdates(*fake, 1));
    QVERIFY(fake->endpoint_updates.front().empty());

    // An unencrypted wallet mints an operation without a prompt; it signs
    // only with the keys it was minted for.
    const auto minted{service.beginSigningOperation(platform::OperationKind::PROFILE, {1}, std::nullopt, std::nullopt)};
    QVERIFY(minted.op.has_value());
    QVERIFY(minted.op->allowsKey(1));
    QVERIFY(!minted.op->allowsKey(0));

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
                            StatusKind::UNSUPPORTED_PROTOCOL_VERSION, StatusKind::INTERNAL}) {
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

//! Avatars and badges are drawn from the theme's blue, green and orange:
//! no palette entry is purple or violet in either theme.
void PlatformTests::avatarPaletteHasNoPurple()
{
    const QString saved_theme{QSettings().value("theme").toString()};
    for (const QString theme : {"Light", "Dark"}) {
        QSettings().setValue("theme", theme);
        for (const QColor& color : PlatformUi::avatarPalette()) {
            const int hue{color.hsvHue()};
            QVERIFY2(hue < 250 || hue > 330,
                     qPrintable(QStringLiteral("%1 hue %2 in %3").arg(color.name()).arg(hue).arg(theme)));
        }
    }
    QSettings().setValue("theme", saved_theme);
}

//! The registration steps and what a failure left behind are worded from
//! the record: four steps for a new identity, two when an existing identity
//! only registers a username.
void PlatformTests::registrationStepsAndReassurance()
{
    using State = platform::IdentityRecord::State;
    platform::IdentityRecord record;
    record.funding_amount = 1'000'000;
    record.state = State::FUNDING_LOCKED;
    QCOMPARE(PlatformUi::registrationStepLine(record), QString("Step 2 of 4: Creating your identity"));
    record.state = State::DOMAIN_BROADCAST;
    QCOMPARE(PlatformUi::registrationStepLine(record), QString("Step 4 of 4: Registering your username"));
    record.state = State::NEEDS_UNLOCK;
    record.resume_state = State::PREORDER_BROADCAST;
    QCOMPARE(PlatformUi::registrationStepLine(record), QString("Step 3 of 4: Reserving your username"));

    record.funding_amount = 0;
    record.state = State::PREORDER_BROADCAST;
    QCOMPARE(PlatformUi::registrationStepLine(record), QString("Step 1 of 2: Reserving your username"));
    // The preorder is confirmed: what is left is the domain.
    record.state = State::PREORDER_WAIT;
    QCOMPARE(PlatformUi::registrationStepLine(record), QString("Step 2 of 2: Registering your username"));

    record.state = State::FAILED;
    record.resume_state = State::DOMAIN_BROADCAST;
    QVERIFY(PlatformUi::failureReassurance(record).contains("keeps its balance on Dash Platform"));
    record.resume_state = State::IDENTITY_BROADCAST;
    QVERIFY(PlatformUi::failureReassurance(record).contains("funding payment is safe"));
    // The payment never confirmed, but a release may have paid a fee.
    record.resume_state = State::FUNDING_SENT;
    QVERIFY(PlatformUi::failureReassurance(record).contains("Nothing was paid to Dash Platform"));
    record.resume_state = State::NONE;
    QCOMPARE(PlatformUi::failureReassurance(record), QString("No funds were spent."));
}

//! The identity registers the four keys the mobile wallets expect, and a
//! contested registration is funded from the amount the network's protocol
//! version requires rather than a constant.
void PlatformTests::registrationKeysAndContestedFunding()
{
    using Purpose = platform::IdentityPublicKey::Purpose;
    using Level = platform::IdentityPublicKey::SecurityLevel;
    const auto& keys{IdentityFlow::RegistrationKeys()};
    QCOMPARE(keys.size(), size_t{4});
    QCOMPARE(keys[0].id, 0U);
    QVERIFY(keys[0].purpose == Purpose::AUTHENTICATION && keys[0].security_level == Level::MASTER);
    QCOMPARE(keys[1].id, 1U);
    QVERIFY(keys[1].purpose == Purpose::AUTHENTICATION && keys[1].security_level == Level::HIGH);
    QCOMPARE(keys[2].id, 2U);
    QVERIFY(keys[2].purpose == Purpose::ENCRYPTION && keys[2].security_level == Level::MEDIUM);
    QCOMPARE(keys[3].id, 3U);
    QVERIFY(keys[3].purpose == Purpose::DECRYPTION && keys[3].security_level == Level::MEDIUM);
    QVERIFY(!keys[0].contact_request_bound && !keys[1].contact_request_bound);
    QVERIFY(keys[2].contact_request_bound && keys[3].contact_request_bound);

    IdentityFixture f{m_node, "alice"};
    QVERIFY(f.service->identityFundingAmount(/*contested=*/false).has_value());
    // Before a verified read the SDK does not know the network's version.
    QVERIFY(!f.service->identityFundingAmount(/*contested=*/true).has_value());
    f.fake->contested_fund_credits = 10'000'000'000; // 0.1 DASH in credits
    const auto contested{f.service->identityFundingAmount(/*contested=*/true)};
    QVERIFY(contested.has_value());
    QCOMPARE(*contested, *f.service->identityFundingAmount(/*contested=*/false) + CAmount{10'000'000});
}

//! A new identity is funded only right after a proved lookup showed that
//! none is registered under this wallet's identity MASTER key: a wallet
//! restored from its recovery phrase, or one that turned DashPay off and on
//! again, has no record of the one it may have. An unanswered lookup funds
//! nothing and Register looks again; a found identity funds nothing; a
//! proven absence lets Register go on, by itself, to the funding payment.
void PlatformTests::registrationFundsOnlyWithoutExistingIdentity()
{
    using Existing = IdentityFlow::ExistingIdentity;
    FundingFixture f{m_node};
    QVERIFY(f.models.ok);
    QVERIFY(f.funded_model.getAvailableBalance(nullptr) > 2 * COIN);
    const CAmount funding{COIN};

    // Unanswered: nothing is funded, and Register looks again.
    IdentityFlow* flow{&f.service->identityFlow()};
    QSignalSpy checked(flow, &IdentityFlow::existingIdentityChecked);
    f.fake->identities_by_pubkey_hash.push_back(
        platform_test::Failed<platform::Identity>(platform::StatusKind::UNAVAILABLE));
    QString error;
    QVERIFY(!flow->start("bob2026x", funding, error));
    QVERIFY(flow->existingIdentity() == Existing::CHECKING);
    QVERIFY(error.contains("Checking whether this wallet already has a DashPay identity"));
    Drain();
    QCOMPARE(checked.size(), 1);
    QVERIFY(flow->existingIdentity() == Existing::UNVERIFIED);
    QVERIFY(flow->existingIdentityError().text.contains("so nothing was sent"));
    QCOMPARE(f.fake->countCalls("getIdentityByPublicKeyHash"), size_t{1});
    QCOMPARE(f.fake->calls.back().argument, f.masterKeyHash());
    QCOMPARE(f.assetLocks(), size_t{0});
    QVERIFY(flow->record().state == IdentityFlow::State::NONE);

    // Found: nothing is funded, and it stays found.
    platform::Identity existing;
    existing.id = platform_test::IdentifierFromByte(0x1D);
    f.fake->identities_by_pubkey_hash.push_back(platform_test::Ok(existing));
    QVERIFY(!flow->start("bob2026x", funding, error));
    Drain();
    QCOMPARE(checked.size(), 2);
    QVERIFY(flow->existingIdentity() == Existing::FOUND);
    QVERIFY(!flow->start("bob2026x", funding, error));
    QVERIFY(error.contains("already has a DashPay identity"));
    QCOMPARE(f.fake->countCalls("getIdentityByPublicKeyHash"), size_t{2});
    QCOMPARE(f.assetLocks(), size_t{0});
    QVERIFY(flow->record().state == IdentityFlow::State::NONE);

    // Proven absent: Register goes on to the funding payment by itself.
    f.restartService();
    flow = &f.service->identityFlow();
    CreateUsernameWizard wizard{*f.service, f.funded_model};
    wizard.show();
    auto* input{wizard.findChild<UsernameEntryPage*>()->findChild<QLineEdit*>()};
    input->setText("bob2026x");
    f.fake->resolve_name.push_back(platform_test::Absent<platform::DpnsName>());
    f.service->checkNameAvailability("bob2026x");
    Drain();
    const auto click = [&wizard](const QString& text) {
        for (auto* button : wizard.findChildren<QPushButton*>()) {
            if (button->isVisible() && button->isEnabled() && button->text() == text) {
                button->click();
                return true;
            }
        }
        return false;
    };
    QVERIFY(click("Next"));
    auto* stack{wizard.findChild<QStackedWidget*>()};
    auto* cost{wizard.findChild<UsernameCostPage*>()};
    QCOMPARE(stack->currentWidget(), static_cast<QWidget*>(cost));
    f.fake->identities_by_pubkey_hash.push_back(platform_test::Absent<platform::Identity>());
    QVERIFY(click("Register"));
    QVERIFY(cost->isBusy());
    QVERIFY(!cost->isComplete());
    QCOMPARE(f.assetLocks(), size_t{0});
    Drain();
    QVERIFY(stack->currentWidget() != cost);
    QVERIFY(flow->record().state == IdentityFlow::State::FUNDING_SENT);
    QCOMPARE(f.assetLocks(), size_t{1});
    QCOMPARE(f.fake->countCalls("getIdentityByPublicKeyHash"), size_t{1});
    // The proved answer funded that registration; it does not fund another.
    QVERIFY(flow->existingIdentity() == Existing::UNKNOWN);
}

//! Broadcast outcomes steer the DPNS steps by consensus code and the
//! proved nonce: the preorder and the domain are signed together at the next
//! two nonces; a duplicate salted hash means an earlier preorder was
//! applied; a domain that finds no preorder yet is sent again at each poll
//! and, after the confirmation window, the username goes out again from
//! its preorder; a stale nonce is read again; and the name is confirmed only
//! by a proved resolve, with a spent domain signed again at the next nonce.
void PlatformTests::identityFlowConsensusSteering()
{
    using State = IdentityFlow::State;
    using Kind = platform::OperationKind;
    using platform::StatusKind;
    IdentityFixture f{m_node, "alice"};
    IdentityFlow& flow{f.service->identityFlow()};
    QVERIFY(flow.record().state == State::IDENTITY_CONFIRMED);

    // DPNS nonce 4 on Platform: the preorder is signed at 5, the domain at 6.
    // The saltedDomainHash unique index is taken by our own earlier preorder,
    // so the domain goes out at the nonce Platform shows next.
    f.scriptNameCheck();
    f.scriptDocumentStep(4);
    f.fake->builds.push_back(ScriptedBuild(1));
    f.fake->builds.push_back(ScriptedBuild(2));
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(StatusKind::CONSENSUS, 40105));
    Step(flow);
    QVERIFY(flow.record().state == State::PREORDER_WAIT);
    QCOMPARE(f.fake->build_kinds, (std::vector<Kind>{Kind::DPNS_PREORDER, Kind::DPNS_DOMAIN}));
    QCOMPARE(f.fake->build_key_ids[0], std::vector<uint32_t>{1});
    QCOMPARE(flow.record().signed_preorder.nonce, uint64_t{5});
    QVERIFY(flow.record().signed_domain.empty());

    // Domain at 5: the preorder is not visible to the node yet. The same
    // signed domain goes out again at each poll, never before the backoff.
    f.scriptDocumentStep(4);
    f.fake->builds.push_back(ScriptedBuild(3));
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(StatusKind::CONSENSUS, 40500));
    Step(flow);
    QVERIFY(flow.record().state == State::PREORDER_WAIT);
    QCOMPARE(f.fake->build_kinds.size(), size_t{3});
    QCOMPARE(flow.record().signed_domain.nonce, uint64_t{5});
    const size_t reads{f.fake->countCalls("getIdentity")};
    Step(flow);
    QCOMPARE(f.fake->countCalls("getIdentity"), reads);
    for (int i = 0; i < 3; ++i) {
        Elapse(30);
        f.scriptDocumentStep(4);
        f.fake->broadcasts.push_back(platform_test::BroadcastStatus(StatusKind::CONSENSUS, 40500));
        Step(flow);
        QVERIFY(flow.record().state == State::PREORDER_WAIT);
    }
    QCOMPARE(f.fake->build_kinds.size(), size_t{3});
    QVERIFY(f.fake->broadcast_bytes.back() == ScriptedBuild(3).bytes);
    // Past the confirmation window the username goes out again from its
    // preorder.
    Elapse(181);
    f.scriptDocumentStep(4);
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(StatusKind::CONSENSUS, 40500));
    Step(flow);
    QVERIFY(flow.record().state == State::PREORDER_BROADCAST);
    QVERIFY(flow.record().signed_preorder.empty());

    // Signed again at the nonce Platform shows, sent, and confirmed once
    // Platform has taken the preorder's nonce.
    f.scriptDocumentStep(4);
    f.fake->builds.push_back(ScriptedBuild(4));
    f.fake->builds.push_back(ScriptedBuild(5));
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(StatusKind::OK));
    Step(flow);
    QVERIFY(flow.record().state == State::PREORDER_BROADCAST);
    QCOMPARE(f.fake->build_kinds.size(), size_t{5});
    const size_t sent{f.fake->countCalls("broadcastStateTransition")};
    Elapse(10);
    f.scriptDocumentStep(4);
    Step(flow);
    QVERIFY(flow.record().state == State::PREORDER_BROADCAST);
    QCOMPARE(f.fake->countCalls("broadcastStateTransition"), sent);

    // Taken: the signed domain goes out; a stale nonce answer is read again.
    Elapse(30);
    f.scriptDocumentStep(5);
    f.scriptDocumentStep(5);
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(StatusKind::CONSENSUS, 40204));
    Step(flow);
    QVERIFY(flow.record().state == State::PREORDER_WAIT);
    Elapse(30);
    f.scriptDocumentStep(5);
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(StatusKind::OK));
    Step(flow);
    QVERIFY(flow.record().state == State::DOMAIN_BROADCAST);
    QCOMPARE(f.fake->build_kinds.size(), size_t{5});

    // Proved absences are polled with a backoff; past the window the domain
    // is due again, its nonce is spent by then, and the resolve finding no
    // name has it signed at the next one.
    for (int i = 0; i < 10 && flow.record().state == State::DOMAIN_BROADCAST; ++i) {
        f.fake->resolve_name.push_back(platform_test::Absent<platform::DpnsName>());
        Elapse(30);
        Step(flow);
    }
    QVERIFY(flow.record().state == State::PREORDER_WAIT);
    f.scriptDocumentStep(6);
    f.fake->resolve_name.push_back(platform_test::Absent<platform::DpnsName>());
    f.fake->builds.push_back(ScriptedBuild(6));
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(StatusKind::CONSENSUS, 40105));
    Step(flow);
    QVERIFY(flow.record().state == State::DOMAIN_BROADCAST);
    QCOMPARE(f.fake->build_kinds.size(), size_t{6});
    QVERIFY(f.fake->build_kinds.back() == Kind::DPNS_DOMAIN);
    QCOMPARE(flow.record().signed_domain.nonce, uint64_t{7});

    // Every DPNS build used the persisted salt.
    QCOMPARE(f.fake->build_salts.size(), size_t{6});
    for (const auto& salt : f.fake->build_salts)
        QVERIFY(salt == flow.record().preorder_salt);

    // Confirmation is the proved resolve, owned by our identity. Losing the
    // name to another identity keeps the identity itself: "try again" asks
    // for a new name funded by its credits instead of a new asset lock.
    platform::DpnsName name;
    name.label = "alice";
    name.normalized_label = "a11ce";
    name.identity = platform_test::IdentifierFromByte(0x99);
    f.fake->resolve_name.push_back(platform_test::Ok(name));
    Elapse(30);
    Step(flow);
    QVERIFY(flow.record().state == State::FAILED); // registered by another identity
    flow.reset();
    QVERIFY(flow.record().AwaitsUsername());
    QVERIFY(flow.record().identity_id == f.my_id);
    QVERIFY(f.service->myUsername().isEmpty());
    QVERIFY(f.service->myIdentityId() == std::optional{f.my_id});
    QString error;
    const int tx_count{static_cast<int>(f.wallet_model.wallet().getWalletTxs().size())};
    QVERIFY2(flow.start("bob", 0, error), qPrintable(error));
    QCOMPARE(static_cast<int>(f.wallet_model.wallet().getWalletTxs().size()), tx_count);
    QVERIFY(flow.record().state == State::IDENTITY_CONFIRMED);
    QCOMPARE(QString::fromStdString(flow.record().label), QString("bob"));
}

//! A name that is already registered to our own identity (its confirmation
//! was lost, or another wallet on the seed registered it) is adopted: the
//! wizard says it is ours and completes, and the flow goes to REGISTERED
//! from a proved resolve without spending anything.
void PlatformTests::registrationAdoptsNameAlreadyOurs()
{
    using State = IdentityFlow::State;
    // One node context at a time: the adopted name ends before the name
    // of another identity starts.
    {
        IdentityFixture f{m_node, ""};
        IdentityFlow& flow{f.service->identityFlow()};
        QVERIFY(flow.record().AwaitsUsername());
        platform::DpnsName ours;
        ours.label = "alice";
        ours.normalized_label = "a11ce";
        ours.identity = f.my_id;

        CreateUsernameWizard wizard{*f.service, f.wallet_model};
        auto* entry{wizard.findChild<UsernameEntryPage*>()};
        QVERIFY(entry != nullptr);
        entry->findChild<QLineEdit*>()->setText("alice");
        f.fake->resolve_name.push_back(platform_test::Ok(ours));
        f.service->checkNameAvailability("alice");
        Drain();
        QVERIFY(entry->isComplete());
        QVERIFY(entry->ours());
        bool says_ours{false};
        for (const auto* label : entry->findChildren<QLabel*>()) {
            says_ours |= label->text().contains("already yours");
        }
        QVERIFY(says_ours);

        // Next starts the flow for the name; the proved resolve before any
        // preorder finds it registered to us.
        QVERIFY(entry->validatePage());
        f.fake->resolve_name.push_back(platform_test::Ok(ours));
        flow.advance();
        Drain();
        QVERIFY(flow.record().state == State::REGISTERED);
        QCOMPARE(f.service->myUsername(), QString("alice"));
        QVERIFY(f.fake->build_kinds.empty());
        QCOMPARE(f.fake->countCalls("broadcastStateTransition"), size_t{0});
    }

    // A name registered to anybody else fails before any preorder.
    IdentityFixture g{m_node, "bob"};
    platform::DpnsName theirs;
    theirs.identity = platform_test::IdentifierFromByte(0x99);
    g.fake->resolve_name.push_back(platform_test::Ok(theirs));
    g.service->identityFlow().advance();
    Drain();
    QVERIFY(g.service->identityFlow().record().state == State::FAILED);
    QVERIFY(g.fake->build_kinds.empty());
}

//! A premium name locks the contested vote reserve and pays two transition
//! fees on top: the cost page of an existing identity states the amount
//! against its proved balance and does not go on when the balance cannot
//! cover it, and an identity whose balance does not cover it (the reserve
//! alone does not) fails before its credits are spent on a preorder that
//! could never be followed by a contest.
void PlatformTests::contestedNameNeedsIdentityCredits()
{
    using State = IdentityFlow::State;
    {
        IdentityFixture f{m_node, ""};
        f.fake->contested_fund_credits = 20'000'000'000;
        CreateUsernameWizard wizard{*f.service, f.wallet_model};
        auto* entry{wizard.findChild<UsernameEntryPage*>()};
        auto* cost{wizard.findChild<UsernameCostPage*>()};
        entry->findChild<QLineEdit*>()->setText("Alice");
        f.fake->resolve_name.push_back(platform_test::Absent<platform::DpnsName>());
        f.fake->contested_states.push_back(platform_test::Absent<platform::ContestedNameState>());
        f.service->checkNameAvailability("Alice");
        Drain();
        QVERIFY(entry->contested());
        const auto summary = [cost] {
            QStringList texts;
            for (const auto* label : cost->findChildren<QLabel*>()) {
                texts << label->text();
            }
            return texts.join(' ');
        };

        platform::Identity poor{f.myIdentity()};
        poor.balance = 780'000'000;
        f.fake->identities.push_back(platform_test::Ok(poor));
        cost->initializePage();
        QVERIFY(!cost->isComplete());
        Drain();
        QVERIFY(!cost->isComplete());
        QVERIFY(summary().contains("you have only"));
        QVERIFY(!summary().contains("no new funding transaction"));

        platform::Identity reserve_only{f.myIdentity()};
        reserve_only.balance = 20'000'000'000;
        f.fake->identities.push_back(platform_test::Ok(reserve_only));
        cost->initializePage();
        Drain();
        QVERIFY(!cost->isComplete());
        QVERIFY(summary().contains("you have only"));

        const uint64_t required{*f.service->contestedNameRequiredCredits()};
        QCOMPARE(required, uint64_t{20'000'000'000} + 2 * PlatformService::DOCUMENT_FEE_RESERVE_CREDITS);
        platform::Identity funded{f.myIdentity()};
        funded.balance = required;
        f.fake->identities.push_back(platform_test::Ok(funded));
        cost->initializePage();
        Drain();
        QVERIFY(cost->isComplete());
        QVERIFY(summary().contains("which covers it"));
    }

    IdentityFixture f{m_node, "alice"};
    IdentityFlow& flow{f.service->identityFlow()};
    platform::IdentityRecord record{flow.record()};
    record.contested = true;
    f.writeRecord(record);
    f.fake->contested_fund_credits = 20'000'000'000;
    QCOMPARE(f.service->contestedNameCredits(), std::optional<uint64_t>{20'000'000'000});

    const auto retry_contested = [&] {
        flow.reset();
        record = flow.record();
        record.label = "alice";
        record.normalized_label = "a11ce";
        record.contested = true;
        f.writeRecord(record);
    };
    // The reserve alone leaves nothing for the fees.
    platform::Identity poor{f.myIdentity()};
    poor.balance = 20'000'000'000;
    f.scriptNameCheck();
    f.fake->identities.push_back(platform_test::Ok(poor));
    f.fake->nonces.push_back(platform_test::Ok<uint64_t>(1));
    flow.advance();
    Drain();
    QVERIFY(flow.record().state == State::FAILED);
    QVERIFY(f.fake->build_kinds.empty());
    QVERIFY(flow.record().last_error.find("premium") != std::string::npos);
    // The balance is a Dash amount, never a count of credits.
    const QString error_text{flow.lastErrorText()};
    QVERIFY(error_text.contains(BitcoinUnits::formatWithUnit(f.wallet_model.getOptionsModel()->getDisplayUnit(),
                                                             20'000'000'000 / platform::helpers::CreditsPerDuff())));
    QVERIFY(!error_text.contains("credit"));

    // Exactly enough credits: the preorder and its domain are built.
    retry_contested();
    platform::Identity funded{f.myIdentity()};
    funded.balance = *f.service->contestedNameRequiredCredits();
    f.scriptNameCheck();
    f.fake->identities.push_back(platform_test::Ok(funded));
    f.fake->nonces.push_back(platform_test::Ok<uint64_t>(1));
    f.fake->builds.push_back(ScriptedBuild(1));
    f.fake->builds.push_back(ScriptedBuild(2));
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(platform::StatusKind::OK));
    flow.advance();
    Drain();
    QCOMPARE(f.fake->build_kinds.size(), size_t{2});
    QVERIFY(flow.record().state == State::PREORDER_BROADCAST);
}

//! A node that proves our confirmed identity absent may only lag behind the
//! node that confirmed it: the registration waits instead of failing, and
//! fails only when the absence outlasts the confirmation window.
void PlatformTests::documentStepWaitsForLaggingIdentityRead()
{
    using State = IdentityFlow::State;
    IdentityFixture f{m_node, "alice"};
    IdentityFlow& flow{f.service->identityFlow()};
    platform::IdentityRecord record{flow.record()};
    record.state = State::PREORDER_WAIT;
    f.writeRecord(record);
    f.fake->identities.push_back(platform_test::Absent<platform::Identity>());
    Step(flow);
    QVERIFY(flow.record().state == State::PREORDER_WAIT);
    for (int i = 0; i < 10 && flow.record().state == State::PREORDER_WAIT; ++i) {
        f.fake->identities.push_back(platform_test::Absent<platform::Identity>());
        Elapse(30);
        Step(flow);
    }
    QVERIFY(flow.record().state == State::FAILED);
}

//! An open contest for the name that does not list our identity is not ours:
//! the domain we broadcast may still be lost, so after the confirmation window
//! it is sent again, while a contest we are in is waited out.
void PlatformTests::contestedDomainWaitsOnlyForOurContest()
{
    using State = IdentityFlow::State;
    for (const bool ours : {false, true}) {
        IdentityFixture f{m_node, "alice"};
        IdentityFlow& flow{f.service->identityFlow()};
        platform::IdentityRecord record{flow.record()};
        record.state = State::DOMAIN_BROADCAST;
        record.contested = true;
        f.writeRecord(record);
        platform::ContestedNameState contest;
        contest.contenders.push_back({platform_test::IdentifierFromByte(0x99), 0});
        if (ours) contest.contenders.push_back({f.my_id, 0});
        for (int i = 0; i < 10 && flow.record().state != State::PREORDER_WAIT; ++i) {
            f.fake->resolve_name.push_back(platform_test::Absent<platform::DpnsName>());
            f.fake->contested_states.push_back(platform_test::Ok(contest));
            Elapse(30);
            Step(flow);
        }
        QVERIFY(flow.record().state == (ours ? State::CONTESTED_PENDING : State::PREORDER_WAIT));
    }
}

//! Reads and broadcasts that go unanswered or unverified never fail a
//! registration: the step keeps its state and retries every tick until
//! Platform answers, and a builder error is bounded instead.
void PlatformTests::identityFlowOutageDoesNotFail()
{
    using State = IdentityFlow::State;
    IdentityFixture f{m_node, "alice"};
    IdentityFlow& flow{f.service->identityFlow()};
    QSignalSpy failures(&flow, &IdentityFlow::failed);
    for (int i = 0; i < 20; ++i) {
        flow.advance(); // nothing scripted: every read is UNAVAILABLE
        Drain();
    }
    QVERIFY(flow.record().state == State::IDENTITY_CONFIRMED);
    QCOMPARE(failures.size(), 20);
    QVERIFY(!flow.record().last_error.empty());

    // A broadcast the network could not verify is sent again at the next
    // poll, as signed.
    f.scriptNameCheck();
    f.scriptDocumentStep(1);
    f.fake->builds.push_back(ScriptedBuild(1));
    f.fake->builds.push_back(ScriptedBuild(2));
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(platform::StatusKind::REJECTED));
    Step(flow);
    QVERIFY(flow.record().state == State::PREORDER_BROADCAST);
    Elapse(5);
    f.scriptDocumentStep(1);
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(platform::StatusKind::OK));
    Step(flow);
    QVERIFY(flow.record().state == State::PREORDER_BROADCAST);
    QCOMPARE(f.fake->countCalls("broadcastStateTransition"), size_t{2});
    QVERIFY(f.fake->broadcast_bytes[0] == f.fake->broadcast_bytes[1]);
    QCOMPARE(f.fake->build_kinds.size(), size_t{2});
    Elapse(10);
    f.scriptDocumentStep(2);
    f.scriptDocumentStep(2);
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(platform::StatusKind::OK));
    Step(flow);
    QVERIFY(flow.record().state == State::DOMAIN_BROADCAST);
}

//! A protocol version this build does not know, first seen by a read the
//! flow itself makes, freezes writes: the flow waits where it is with the
//! reason, no builder runs, and the wizard cannot start a registration.
void PlatformTests::identityFlowFreezesOnUnsupportedVersion()
{
    using State = IdentityFlow::State;
    IdentityFixture f{m_node, "alice"};
    IdentityFlow& flow{f.service->identityFlow()};
    QSignalSpy frozen(f.service.get(), &PlatformService::unsupportedProtocolVersion);
    auto ahead{platform_test::Ok(f.myIdentity())};
    ahead.status.kind = platform::StatusKind::UNSUPPORTED_PROTOCOL_VERSION;
    f.scriptNameCheck();
    f.fake->identities.push_back(ahead);
    flow.advance();
    Drain();
    QCOMPARE(frozen.size(), 1);
    QVERIFY(f.service->writesFrozen());
    QVERIFY(flow.record().state == State::IDENTITY_CONFIRMED);
    QVERIFY(f.fake->build_kinds.empty());

    // The next steps do not build, park or fail: they wait for an update.
    f.scriptNameCheck();
    f.scriptDocumentStep(1);
    flow.advance();
    Drain();
    QVERIFY(flow.record().state == State::IDENTITY_CONFIRMED);
    QVERIFY(f.fake->build_kinds.empty());
    QVERIFY(flow.record().last_error.find("upgraded") != std::string::npos);
    QString error;
    QVERIFY(!flow.start("bob", 0, error));
    QVERIFY(!error.isEmpty());
}

//! An identity registration builds the identity-create transition from the
//! locked funding transaction with the four registered keys under one
//! operation, and Platform's transient refusals of the asset lock proof
//! (its core height behind the ChainLock, an InstantSend quorum gone) step
//! back to rebuild instead of failing; a fatal consensus refusal fails but
//! "try again" re-uses the funded asset lock.
void PlatformTests::identityCreateFromFundingLock()
{
    using State = IdentityFlow::State;
    using platform::StatusKind;
    IdentityFixture f{m_node, "alice"};
    IdentityFlow& flow{f.service->identityFlow()};

    // A confirmed, ChainLocked funding transaction in the wallet: the
    // coinbase of the test chain's tip stands in for it.
    const CTransactionRef funding{f.chain.m_coinbase_txns.back()};
    {
        LOCK(f.wallet->cs_wallet);
        const auto* tip{
            WITH_LOCK(f.chain.m_node.chainman->GetMutex(), return f.chain.m_node.chainman->ActiveChain().Tip())};
        f.wallet->AddToWallet(funding, wallet::TxStateConfirmed{tip->GetBlockHash(), tip->nHeight, /*index=*/0});
        f.wallet->SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
    }
    QVERIFY(f.chain.m_node.sporkman->SetSporkAddress(Params().SporkAddress()));
    QVERIFY(f.chain.m_node.sporkman->SetPrivKey("cP4EKFyJsHT39LDqgdcB43Y3YXjNyjb5Fuas1GQSeAtjnZWmZEQK"));
    QVERIFY(f.chain.m_node.sporkman->UpdateSpork(SPORK_19_CHAINLOCKS_ENABLED, 0).has_value());
    {
        const auto* tip{
            WITH_LOCK(f.chain.m_node.chainman->GetMutex(), return f.chain.m_node.chainman->ActiveChain().Tip())};
        const chainlock::ChainLockSig clsig{tip->nHeight, tip->GetBlockHash(), CBLSSignature{}};
        QVERIFY(f.chain.m_node.chainlocks->UpdateBestChainlock(uint256::ONE, clsig, tip));
    }

    platform::IdentityRecord record;
    record.state = State::FUNDING_LOCKED;
    record.funding_txid = funding->GetHash();
    record.funding_amount = 1000000;
    record.auth_key_id = 1;
    record.encryption_key_id = 2;
    record.decryption_key_id = 3;
    record.label = "alice";
    record.normalized_label = "a11ce";
    record.preorder_salt.fill(0x5A);
    f.writeRecord(record);
    VerifiedAt(*f.service);

    // The identity, the username's preorder and domain are signed under one
    // operation each, at DPNS nonces 1 and 2, and persisted before anything
    // is sent. Platform's committed core height trails the ChainLock: not
    // fatal.
    for (uint8_t byte : {0x1D, 0x21, 0x22}) {
        f.fake->builds.push_back(ScriptedBuild(byte));
    }
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(StatusKind::CONSENSUS, 10506));
    Step(flow);
    QVERIFY(flow.record().state == State::IDENTITY_BROADCAST);
    QVERIFY(flow.record().identity_id == platform_test::IdentifierFromByte(0x1D));
    using Kind = platform::OperationKind;
    QCOMPARE(f.fake->build_kinds, (std::vector<Kind>{Kind::IDENTITY_CREATE, Kind::DPNS_PREORDER, Kind::DPNS_DOMAIN}));
    QCOMPARE(f.fake->build_key_ids[0], (std::vector<uint32_t>{0, 1, 2, 3}));
    QCOMPARE(f.fake->build_key_ids[1], std::vector<uint32_t>{1});
    platform::IdentityRecord persisted;
    QVERIFY(platform::DeserializeIdentityRecord(f.service->readRecord(platform::records::IDENTITY), persisted));
    QVERIFY(persisted.signed_identity_create == ScriptedBuild(0x1D).bytes);
    QVERIFY(persisted.signed_preorder.bytes == ScriptedBuild(0x21).bytes);
    QCOMPARE(persisted.signed_preorder.nonce, uint64_t{1});
    QVERIFY(persisted.signed_domain.bytes == ScriptedBuild(0x22).bytes);
    QCOMPARE(persisted.signed_domain.nonce, uint64_t{2});
    QVERIFY(f.fake->last_asset_lock_proof.has_value());
    QVERIFY(!f.fake->last_asset_lock_proof->is_instant);
    QCOMPARE(f.fake->last_asset_lock_proof->core_chain_locked_height, 100U);
    QCOMPARE(f.fake->last_identity_keys.size(), size_t{4});
    for (size_t i = 0; i < 4; ++i) {
        const auto& spec{IdentityFlow::RegistrationKeys()[i]};
        const auto& key{f.fake->last_identity_keys[i]};
        QCOMPARE(key.id, spec.id);
        QVERIFY(key.purpose == spec.purpose && key.security_level == spec.security_level);
        QVERIFY(key.pubkey == f.wallet_model.wallet().getPlatformPubKey(wallet::IdentityAuthKey{0, spec.id}).value);
        QVERIFY((key.contract_bounds.kind == platform::ContractBounds::Kind::SINGLE_CONTRACT_DOCUMENT_TYPE) ==
                spec.contact_request_bound);
    }
    // The same signed transition goes out again at the next poll, and again
    // when the confirmation window runs out; nothing is signed again.
    Elapse(5);
    f.fake->identities.push_back(platform_test::Absent<platform::Identity>());
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(StatusKind::OK));
    Step(flow);
    QCOMPARE(f.fake->countCalls("broadcastStateTransition"), size_t{2});
    for (int i = 0; i < 10 && f.fake->countCalls("broadcastStateTransition") < 3; ++i) {
        f.fake->identities.push_back(platform_test::Absent<platform::Identity>());
        Elapse(30);
        Step(flow);
    }
    QCOMPARE(f.fake->countCalls("broadcastStateTransition"), size_t{3});
    for (const auto& bytes : f.fake->broadcast_bytes)
        QVERIFY(bytes == ScriptedBuild(0x1D).bytes);
    QCOMPARE(f.fake->build_kinds.size(), size_t{3});

    // A fatal refusal (the keys already belong to an identity) fails, and
    // "try again" keeps the funding outpoint for another attempt, signed anew.
    Elapse(30);
    f.fake->identities.push_back(platform_test::Absent<platform::Identity>());
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(StatusKind::CONSENSUS, 40207));
    Step(flow);
    QVERIFY(flow.record().state == State::FAILED);
    flow.reset();
    QVERIFY(flow.record().state == State::FUNDING_LOCKED);
    QVERIFY(flow.record().funding_txid == funding->GetHash());
    QVERIFY(flow.record().identity_id == platform::Identifier{});
    QVERIFY(flow.record().signed_identity_create.empty() && flow.record().signed_preorder.empty());

    // Applied: the proved identity confirms it.
    for (uint8_t byte : {0x1D, 0x21, 0x22}) {
        f.fake->builds.push_back(ScriptedBuild(byte));
    }
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(StatusKind::OK));
    Step(flow);
    QCOMPARE(f.fake->build_kinds.size(), size_t{6});
    f.fake->identities.push_back(platform_test::Ok(f.myIdentity()));
    Elapse(5);
    Step(flow);
    QVERIFY(flow.record().state == State::IDENTITY_CONFIRMED);
    QVERIFY(flow.record().signed_identity_create.empty());
    QVERIFY(!flow.record().signed_preorder.empty());
}

//! A locked encrypted wallet never builds (and so never signs): the flow
//! parks in NEEDS_UNLOCK, ignores ticks, and resumes when the wallet is
//! unlocked, with the operation scoped to the HIGH key.
void PlatformTests::identityFlowNeedsUnlock()
{
    using State = IdentityFlow::State;
    IdentityFixture f{m_node, "alice"};
    // The proved identity carries the wallet's real keys, read while the
    // wallet can still derive them.
    f.scriptNameCheck();
    f.scriptDocumentStep(1);
    f.scriptNameCheck();
    f.scriptDocumentStep(1);
    QVERIFY(f.wallet->EncryptWallet("passphrase"));
    QVERIFY(f.wallet_model.getEncryptionStatus() == WalletModel::Locked);
    IdentityFlow& flow{f.service->identityFlow()};
    f.fake->builds.push_back(ScriptedBuild(1));
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(platform::StatusKind::OK));

    // While the passphrase dialog is open (a nested event loop) a tick must
    // not start the step a second time.
    int ticks_inside{0};
    connect(&f.wallet_model, &WalletModel::requireUnlock, &flow, [&] {
        ++ticks_inside;
        flow.advance();
        Drain();
    });
    flow.advance();
    Drain();
    QCOMPARE(ticks_inside, 1);
    QVERIFY(flow.record().state == State::NEEDS_UNLOCK);
    QVERIFY(flow.record().resume_state == State::IDENTITY_CONFIRMED);
    QVERIFY(f.fake->build_kinds.empty());
    // Ticks do not nag for the passphrase.
    flow.advance();
    Drain();
    QVERIFY(flow.record().state == State::NEEDS_UNLOCK);
    QCOMPARE(f.fake->countCalls("getIdentityContractNonce"), size_t{1});
    // The parked state survives a reload of the record.
    flow.reload();
    QVERIFY(flow.record().state == State::NEEDS_UNLOCK);

    // A record without signed transitions (an existing identity, or one
    // written by an earlier build) signs the preorder and its domain under
    // the one unlock of its first document step.
    f.fake->builds.push_back(ScriptedBuild(2));
    QVERIFY(f.wallet_model.setWalletLocked(false, "passphrase"));
    Drain();
    flow.retryAfterUnlock();
    Drain();
    QVERIFY(flow.record().state == State::PREORDER_BROADCAST);
    QCOMPARE(f.fake->build_kinds.size(), size_t{2});
    QCOMPARE(f.fake->build_key_ids[0], std::vector<uint32_t>{1});
    QCOMPARE(f.fake->build_key_ids[1], std::vector<uint32_t>{1});

    // Locked again: the domain goes out as signed, without asking.
    QVERIFY(f.wallet_model.setWalletLocked(true));
    QSignalSpy asked(&f.wallet_model, &WalletModel::requireUnlock);
    Elapse(5);
    f.scriptDocumentStep(2);
    f.scriptDocumentStep(2);
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(platform::StatusKind::OK));
    Step(flow);
    QVERIFY(flow.record().state == State::DOMAIN_BROADCAST);
    QCOMPARE(asked.size(), 0);
    QCOMPARE(f.fake->build_kinds.size(), size_t{2});
}

//! On a locked encrypted wallet the identity-create step asks for the
//! passphrase before deriving the keys it registers (they come from the
//! seed too), parks when declined, and completes once unlocked.
void PlatformTests::identityCreateNeedsUnlock()
{
    using State = IdentityFlow::State;
    IdentityFixture f{m_node, "alice"};
    const CTransactionRef funding{f.chain.m_coinbase_txns.back()};
    {
        LOCK(f.wallet->cs_wallet);
        const auto* tip{
            WITH_LOCK(f.chain.m_node.chainman->GetMutex(), return f.chain.m_node.chainman->ActiveChain().Tip())};
        f.wallet->AddToWallet(funding, wallet::TxStateConfirmed{tip->GetBlockHash(), tip->nHeight, /*index=*/0});
        f.wallet->SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
    }
    QVERIFY(f.chain.m_node.sporkman->SetSporkAddress(Params().SporkAddress()));
    QVERIFY(f.chain.m_node.sporkman->SetPrivKey("cP4EKFyJsHT39LDqgdcB43Y3YXjNyjb5Fuas1GQSeAtjnZWmZEQK"));
    QVERIFY(f.chain.m_node.sporkman->UpdateSpork(SPORK_19_CHAINLOCKS_ENABLED, 0).has_value());
    {
        const auto* tip{
            WITH_LOCK(f.chain.m_node.chainman->GetMutex(), return f.chain.m_node.chainman->ActiveChain().Tip())};
        const chainlock::ChainLockSig clsig{tip->nHeight, tip->GetBlockHash(), CBLSSignature{}};
        QVERIFY(f.chain.m_node.chainlocks->UpdateBestChainlock(uint256::ONE, clsig, tip));
    }
    platform::IdentityRecord record;
    record.state = State::FUNDING_LOCKED;
    record.funding_txid = funding->GetHash();
    record.funding_amount = 1000000;
    record.label = "alice";
    record.normalized_label = "a11ce";
    f.writeRecord(record);
    VerifiedAt(*f.service);
    QVERIFY(f.wallet->EncryptWallet("passphrase"));
    QVERIFY(f.wallet_model.getEncryptionStatus() == WalletModel::Locked);
    IdentityFlow& flow{f.service->identityFlow()};
    QSignalSpy asked(&f.wallet_model, &WalletModel::requireUnlock);
    for (uint8_t byte : {0x1D, 0x21, 0x22}) {
        f.fake->builds.push_back(ScriptedBuild(byte));
    }
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(platform::StatusKind::OK));

    flow.advance();
    Drain();
    QCOMPARE(asked.size(), 1);
    QVERIFY(flow.record().state == State::NEEDS_UNLOCK);
    QVERIFY(flow.record().resume_state == State::FUNDING_LOCKED);
    QVERIFY(f.fake->build_kinds.empty());

    QVERIFY(f.wallet_model.setWalletLocked(false, "passphrase"));
    Drain();
    flow.retryAfterUnlock();
    Drain();
    QCOMPARE(asked.size(), 1);
    QVERIFY(flow.record().state == State::IDENTITY_BROADCAST);
    QCOMPARE(f.fake->build_kinds.size(), size_t{3});
    QVERIFY(f.fake->build_kinds[0] == platform::OperationKind::IDENTITY_CREATE);
    QCOMPARE(f.fake->build_key_ids[0], (std::vector<uint32_t>{0, 1, 2, 3}));
}

//! A registration record at FUNDING_LOCKED on the fixture's ChainLocked
//! coinbase, for a new identity that chose a display name.
platform::IdentityRecord FundedRecord(IdentityFixture& f)
{
    const CTransactionRef funding{f.chain.m_coinbase_txns.back()};
    {
        LOCK(f.wallet->cs_wallet);
        const auto* tip{
            WITH_LOCK(f.chain.m_node.chainman->GetMutex(), return f.chain.m_node.chainman->ActiveChain().Tip())};
        f.wallet->AddToWallet(funding, wallet::TxStateConfirmed{tip->GetBlockHash(), tip->nHeight, /*index=*/0});
        f.wallet->SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
        const chainlock::ChainLockSig clsig{tip->nHeight, tip->GetBlockHash(), CBLSSignature{}};
        f.chain.m_node.sporkman->SetSporkAddress(Params().SporkAddress());
        f.chain.m_node.sporkman->SetPrivKey("cP4EKFyJsHT39LDqgdcB43Y3YXjNyjb5Fuas1GQSeAtjnZWmZEQK");
        f.chain.m_node.sporkman->UpdateSpork(SPORK_19_CHAINLOCKS_ENABLED, 0);
        f.chain.m_node.chainlocks->UpdateBestChainlock(uint256::ONE, clsig, tip);
    }
    platform::IdentityRecord record;
    record.state = IdentityFlow::State::FUNDING_LOCKED;
    record.funding_txid = funding->GetHash();
    record.funding_amount = 1000000;
    record.auth_key_id = 1;
    record.encryption_key_id = 2;
    record.decryption_key_id = 3;
    record.label = "alice";
    record.normalized_label = "a11ce";
    record.preorder_salt.fill(0x5A);
    record.profile_display_name = "Alice A";
    return record;
}

//! One passphrase per registration: the identity, the username's preorder
//! and domain (DPNS nonces 1 and 2) and the profile (DashPay nonce 1) are
//! signed under one unlock and persisted before the wallet is locked again.
//! After a restart the flow broadcasts what was persisted, in order, each
//! step confirmed by a proved read, without asking for the passphrase.
void PlatformTests::registrationSignsOnceAndResumesAfterRestart()
{
    using State = IdentityFlow::State;
    using Kind = platform::OperationKind;
    using platform::StatusKind;
    IdentityFixture f{m_node, "alice"};
    f.writeRecord(FundedRecord(f));
    VerifiedAt(*f.service);
    platform::Identity mine{f.myIdentity()};
    mine.id = platform_test::IdentifierFromByte(0x1D);
    QVERIFY(f.wallet->EncryptWallet("passphrase"));
    int prompts{0};
    connect(&f.wallet_model, &WalletModel::requireUnlock, &f.wallet_model, [&] {
        ++prompts;
        f.wallet_model.setWalletLocked(false, "passphrase");
    });

    for (uint8_t byte : {0x1D, 0x21, 0x22, 0x23}) {
        f.fake->builds.push_back(ScriptedBuild(byte));
    }
    // Nothing reaches Platform before the process goes away.
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(StatusKind::UNAVAILABLE));
    Step(f.service->identityFlow());
    QCOMPARE(prompts, 1);
    QVERIFY(f.wallet_model.getEncryptionStatus() == WalletModel::Locked);
    QCOMPARE(f.fake->build_kinds,
             (std::vector<Kind>{Kind::IDENTITY_CREATE, Kind::DPNS_PREORDER, Kind::DPNS_DOMAIN, Kind::PROFILE}));
    platform::IdentityRecord persisted;
    QVERIFY(platform::DeserializeIdentityRecord(f.service->readRecord(platform::records::IDENTITY), persisted));
    QVERIFY(persisted.state == State::IDENTITY_BROADCAST);
    QVERIFY(persisted.signed_identity_create == ScriptedBuild(0x1D).bytes);
    QCOMPARE(persisted.signed_preorder.nonce, uint64_t{1});
    QCOMPARE(persisted.signed_domain.nonce, uint64_t{2});
    QVERIFY(persisted.signed_profile.bytes == ScriptedBuild(0x23).bytes);
    QCOMPARE(persisted.signed_profile.nonce, uint64_t{1});

    // Restart: a new service over the same wallet resumes from the record.
    f.service.reset();
    auto client{std::make_unique<FakePlatformClient>()};
    f.fake = client.get();
    FakePlatformClient& fake{*f.fake};
    fake.identities.push_back(platform_test::Absent<platform::Identity>());
    fake.broadcasts.push_back(platform_test::BroadcastStatus(StatusKind::OK));
    f.service = std::make_unique<PlatformService>(f.wallet_model, f.models.client, std::move(client));
    IdentityFlow& flow{f.service->identityFlow()};
    Drain();
    QCOMPARE(fake.broadcast_bytes.size(), size_t{1});
    QVERIFY(fake.broadcast_bytes[0] == ScriptedBuild(0x1D).bytes);

    // Created: the name is free, the persisted preorder goes out.
    Elapse(5);
    fake.identities.push_back(platform_test::Ok(mine));
    Step(flow);
    QVERIFY(flow.record().state == State::IDENTITY_CONFIRMED);
    fake.resolve_name.push_back(platform_test::Absent<platform::DpnsName>());
    fake.identities.push_back(platform_test::Ok(mine));
    fake.nonces.push_back(platform_test::Absent<uint64_t>());
    fake.broadcasts.push_back(platform_test::BroadcastStatus(StatusKind::OK));
    Step(flow);
    QVERIFY(flow.record().state == State::PREORDER_BROADCAST);
    QVERIFY(fake.broadcast_bytes.back() == ScriptedBuild(0x21).bytes);

    // The domain waits until Platform has taken the preorder's nonce.
    Elapse(5);
    fake.identities.push_back(platform_test::Ok(mine));
    fake.nonces.push_back(platform_test::Absent<uint64_t>());
    Step(flow);
    QVERIFY(flow.record().state == State::PREORDER_BROADCAST);
    QCOMPARE(fake.broadcast_bytes.size(), size_t{2});
    Elapse(10);
    for (int i = 0; i < 2; ++i) {
        fake.identities.push_back(platform_test::Ok(mine));
        fake.nonces.push_back(platform_test::Ok<uint64_t>(1));
    }
    fake.broadcasts.push_back(platform_test::BroadcastStatus(StatusKind::OK));
    Step(flow);
    QVERIFY(flow.record().state == State::DOMAIN_BROADCAST);
    QVERIFY(fake.broadcast_bytes.back() == ScriptedBuild(0x22).bytes);

    // Registered: the persisted profile goes out and is confirmed.
    platform::DpnsName ours;
    ours.label = "alice";
    ours.normalized_label = "a11ce";
    ours.identity = mine.id;
    Elapse(5);
    fake.resolve_name.push_back(platform_test::Ok(ours));
    fake.broadcasts.push_back(platform_test::BroadcastStatus(StatusKind::OK));
    Step(flow);
    QVERIFY(flow.record().state == State::REGISTERED);
    QVERIFY(flow.profilePending());
    QVERIFY(fake.broadcast_bytes.back() == ScriptedBuild(0x23).bytes);
    platform::Profile profile;
    profile.owner_id = mine.id;
    profile.display_name = "Alice A";
    profile.revision = 1;
    Elapse(5);
    fake.profiles.push_back(platform_test::Ok(profile));
    Step(flow);
    QVERIFY(!flow.profilePending());
    QVERIFY(flow.profileChosen());
    QVERIFY(flow.record().signed_preorder.empty() && flow.record().signed_domain.empty());

    QCOMPARE(prompts, 1);
    QVERIFY(fake.build_kinds.empty());
    QCOMPARE(fake.broadcast_bytes.size(), size_t{4});
}

//! A funding payment the node keeps refusing to rebroadcast is abandoned to
//! release its coins, but another node may still mine it: the registration
//! keeps its record (and so blocks Disable DashPay) and continues once the
//! payment confirms.
void PlatformTests::abandonedFundingKeepsItsRecord()
{
    using State = IdentityFlow::State;
    IdentityFixture f{m_node, "alice"};
    IdentityFlow& flow{f.service->identityFlow()};
    // Spends a coin the node does not know: the mempool refuses it on every
    // resend.
    CMutableTransaction payment;
    payment.vin.emplace_back(COutPoint{uint256::ONE, 0});
    payment.vout.emplace_back(COIN, CScript() << OP_TRUE);
    const CTransactionRef funding{MakeTransactionRef(payment)};
    {
        LOCK(f.wallet->cs_wallet);
        f.wallet->AddToWallet(funding, wallet::TxStateInactive{});
    }
    platform::IdentityRecord record{FundedRecord(f)};
    record.state = State::FUNDING_SENT;
    record.funding_txid = funding->GetHash();
    f.writeRecord(record);

    Step(flow);
    Elapse(31);
    Step(flow);
    QVERIFY(WITH_LOCK(f.wallet->cs_wallet, return f.wallet->GetWalletTx(funding->GetHash())->isAbandoned()));
    Step(flow);
    QVERIFY(flow.fundingWait() == IdentityFlow::FundingWait::RELEASED);
    // The wait is recorded once, not written to the wallet on every tick.
    const unsigned int writes{f.wallet->GetDatabase().nUpdateCounter};
    QSignalSpy waits(&flow, &IdentityFlow::failed);
    for (int i = 0; i < 10; ++i) {
        Elapse(5);
        Step(flow);
    }
    QCOMPARE(waits.size(), 10);
    QCOMPARE(f.wallet->GetDatabase().nUpdateCounter.load(), writes);
    QVERIFY(flow.record().state == State::FUNDING_SENT);
    QVERIFY(flow.record().funding_txid == funding->GetHash());
    QVERIFY(flow.holdsUnconsumedFunding());
    QVERIFY(PlatformService::HoldsUnconsumedFunding(f.wallet_model.wallet()));

    // Mined after all, in a ChainLocked block: the registration continues.
    {
        LOCK(f.wallet->cs_wallet);
        const auto* tip{
            WITH_LOCK(f.chain.m_node.chainman->GetMutex(), return f.chain.m_node.chainman->ActiveChain().Tip())};
        f.wallet->AddToWallet(funding, wallet::TxStateConfirmed{tip->GetBlockHash(), tip->nHeight, /*index=*/1});
    }
    LockTip(f.chain);
    Step(flow);
    QVERIFY(flow.record().state == State::FUNDING_LOCKED);
    QVERIFY(flow.holdsUnconsumedFunding());
}

//! A mature coinbase coin the wallet can spend, and a funding payment
//! spending it that the network refused and this wallet abandoned.
std::pair<CTransactionRef, CTransactionRef> AbandonedFunding(Fixture& f)
{
    const CTransactionRef coin{f.chain.m_coinbase_txns.front()};
    const auto* block{WITH_LOCK(f.chain.m_node.chainman->GetMutex(), return f.chain.m_node.chainman->ActiveChain()[1])};
    const auto* tip{WITH_LOCK(f.chain.m_node.chainman->GetMutex(), return f.chain.m_node.chainman->ActiveChain().Tip())};
    FlatSigningProvider provider;
    std::string error;
    auto descriptor{Parse("combo(" + EncodeSecret(f.chain.coinbaseKey) + ")", provider, error, /*require_checksum=*/false)};
    assert(descriptor);
    CMutableTransaction payment;
    payment.vin.emplace_back(COutPoint{coin->GetHash(), 0});
    payment.vout.emplace_back(COIN, CScript() << OP_TRUE);
    const CTransactionRef funding{MakeTransactionRef(payment)};
    wallet::WalletDescriptor wallet_descriptor{std::move(descriptor), 0, 0, 1, 1};
    LOCK(f.wallet->cs_wallet);
    const bool added{f.wallet->AddWalletDescriptor(wallet_descriptor, provider, "", false) != nullptr};
    assert(added);
    f.wallet->SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
    f.wallet->AddToWallet(coin, wallet::TxStateConfirmed{block->GetBlockHash(), block->nHeight, /*index=*/0});
    f.wallet->AddToWallet(funding, wallet::TxStateInactive{/*abandoned=*/true});
    return {coin, funding};
}

//! Answer the message boxes the next actions open, in order, with the
//! buttons named `choices`.
void AnswerMessageBoxes(QStringList choices)
{
    auto* timer{new QTimer};
    QObject::connect(timer, &QTimer::timeout, timer, [timer, choices]() mutable {
        for (auto* widget : QApplication::topLevelWidgets()) {
            auto* box{qobject_cast<QMessageBox*>(widget)};
            if (!box || !box->isVisible() || choices.isEmpty()) continue;
            for (auto* button : box->buttons()) {
                if (button->text() == choices.front()) {
                    choices.pop_front();
                    button->click();
                    break;
                }
            }
        }
        if (choices.isEmpty()) timer->deleteLater();
    });
    timer->start(10);
}

//! A spend of the funding payment's coins in a block that is neither
//! ChainLocked nor eight blocks deep may still be reorganized away, and
//! the payment mined: the registration keeps its record until the spend is
//! final. Then it fails, holds nothing, and "try again" starts over.
void PlatformTests::fundingConflictEndsWhenFinal()
{
    using State = IdentityFlow::State;
    IdentityFixture f{m_node, "alice"};
    IdentityFlow& flow{f.service->identityFlow()};
    const auto* tip{WITH_LOCK(f.chain.m_node.chainman->GetMutex(), return f.chain.m_node.chainman->ActiveChain().Tip())};
    // Conflicted by the tip, by a block eight deep, and by the tip again.
    std::vector<CTransactionRef> funding;
    for (const auto* block : {tip, tip->GetAncestor(tip->nHeight - 7), tip}) {
        CMutableTransaction payment;
        payment.vin.emplace_back(COutPoint{uint256::ONE, static_cast<uint32_t>(funding.size())});
        payment.vout.emplace_back(COIN, CScript() << OP_TRUE);
        funding.push_back(MakeTransactionRef(payment));
        LOCK(f.wallet->cs_wallet);
        f.wallet->SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
        f.wallet->AddToWallet(funding.back(), wallet::TxStateConflicted{block->GetBlockHash(), block->nHeight});
    }
    const auto funding_sent = [&](const CTransactionRef& tx) {
        platform::IdentityRecord record{flow.record()};
        record.state = State::FUNDING_SENT;
        record.funding_txid = tx->GetHash();
        record.funding_amount = COIN;
        f.writeRecord(record);
    };
    const auto ended = [&] {
        if (flow.record().state != State::FAILED || flow.record().resume_state != State::FUNDING_SENT) return false;
        if (flow.holdsUnconsumedFunding() || PlatformService::HoldsUnconsumedFunding(f.wallet_model.wallet())) {
            return false;
        }
        flow.reset();
        return flow.record().state == State::NONE && f.service->readRecord(platform::records::IDENTITY).empty();
    };
    platform::IdentityRecord registration{flow.record()};

    funding_sent(funding[0]);
    Step(flow);
    QVERIFY(flow.record().state == State::FUNDING_SENT);
    QVERIFY(flow.fundingWait() == IdentityFlow::FundingWait::ENDING);
    QVERIFY(flow.holdsUnconsumedFunding());
    // The conflicting block was reorganized away and the payment mined in
    // a ChainLocked one: the registration continues.
    {
        LOCK(f.wallet->cs_wallet);
        f.wallet->AddToWallet(funding[0], wallet::TxStateConfirmed{tip->GetBlockHash(), tip->nHeight, /*index=*/1});
    }
    LockTip(f.chain);
    Step(flow);
    QVERIFY(flow.record().state == State::FUNDING_LOCKED);

    f.writeRecord(registration);
    funding_sent(funding[1]);
    Step(flow);
    QVERIFY(ended());

    f.writeRecord(registration);
    funding_sent(funding[2]);
    Step(flow);
    QVERIFY(ended());
}

//! A funding payment the network refused leaves its coins released: the
//! dashboard says so and offers to release the payment, which spends its
//! coins back to the wallet at the current fee, and the registration ends
//! once that spend is final. DashPay can be turned off only then, and the
//! Options dialog says why meanwhile.
void PlatformTests::releasedFundingIsReleasedFromTheCard()
{
    using State = IdentityFlow::State;
    using FundingWait = IdentityFlow::FundingWait;
    PageFixture f{m_node, RegisteredRecord()};
    const auto [coin, funding]{AbandonedFunding(f)};
    platform::IdentityRecord record{RegisteredRecord()};
    record.state = State::FUNDING_SENT;
    record.funding_txid = funding->GetHash();
    record.funding_amount = COIN;
    f.service().writeRecord(platform::records::IDENTITY, platform::SerializeIdentityRecord(record));
    IdentityFlow& flow{f.service().identityFlow()};
    flow.reload();
    Step(flow);
    QVERIFY(flow.record().state == State::FUNDING_SENT);
    QVERIFY(flow.fundingWait() == FundingWait::RELEASED);

    const auto button = [&f](const QString& text) -> QPushButton* {
        for (auto* candidate : f.page.findChildren<QPushButton*>()) {
            if (candidate->text() == text && candidate->isVisibleTo(&f.page)) return candidate;
        }
        return nullptr;
    };
    QVERIFY(f.texts().contains("did not accept the payment that funds your DashPay identity"));
    QVERIFY(!f.texts().contains("few minutes"));
    QPushButton* release{button("Release the funding payment…")};
    QVERIFY(release && release->isEnabled());
    DashPayOptionsWidget section{f.page, f.wallet_model};
    QPushButton* disable{section.findChild<QPushButton*>()};
    const auto reason = [&section] {
        for (const auto* label : section.findChildren<QLabel*>()) {
            if (label->text().startsWith("You can turn DashPay off")) return label->text();
        }
        return QString{};
    };
    QVERIFY(!disable->isEnabled());
    QVERIFY(reason().contains("release that payment on the DashPay tab"));

    // Declined in the confirmation: nothing is sent.
    const auto spends = [&f, funding = funding] {
        return WITH_LOCK(f.wallet->cs_wallet, return f.wallet->GetConflicts(funding->GetHash()));
    };
    AnswerMessageBoxes({"Cancel"});
    release->click();
    QVERIFY(flow.fundingWait() == FundingWait::RELEASED);
    QVERIFY(spends().empty());

    // Not broadcast (the wallet does not broadcast): abandoned, and said so.
    AnswerMessageBoxes({"Release", "OK"});
    release->click();
    QVERIFY(flow.fundingWait() == FundingWait::RELEASED);
    QCOMPARE(spends().size(), size_t{2});
    QVERIFY(button("Release the funding payment…") != nullptr);

    f.wallet->SetBroadcastTransactions(true);
    AnswerMessageBoxes({"Release"});
    button("Release the funding payment…")->click();
    QVERIFY(flow.fundingWait() == FundingWait::ENDING);
    // The funding payment and the released one spend the same coin.
    CTransactionRef spend;
    for (const uint256& txid : spends()) {
        if (txid != funding->GetHash() && f.chain.m_node.mempool->exists(txid)) {
            spend = f.wallet_model.wallet().getTx(txid);
        }
    }
    QVERIFY(spend);
    // Every coin of the payment, back to this wallet less a fee.
    QCOMPARE(spend->vin.size(), size_t{1});
    QVERIFY(spend->vin[0].prevout == funding->vin[0].prevout);
    QCOMPARE(spend->vout.size(), size_t{1});
    QVERIFY(f.wallet_model.wallet().txoutIsMine(spend->vout[0]) != wallet::ISMINE_NO);
    QVERIFY(spend->vout[0].nValue < coin->vout[0].nValue);
    QVERIFY(spend->vout[0].nValue > coin->vout[0].nValue - COIN / 1000);
    QVERIFY(button("Release the funding payment…") == nullptr);
    QVERIFY(f.texts().contains("The registration ends once that payment is final"));
    QVERIFY(f.texts().contains("abandon it on the Transactions tab"));
    QVERIFY(!disable->isEnabled());
    QVERIFY(reason().contains("payment that spent its funding coins is final"));
    QString error;
    QVERIFY(!flow.releaseFunding(error));
    QVERIFY(!error.isEmpty());

    // The spend is mined in the ChainLocked tip: the registration ends.
    const auto* tip{WITH_LOCK(f.chain.m_node.chainman->GetMutex(), return f.chain.m_node.chainman->ActiveChain().Tip())};
    {
        LOCK(f.wallet->cs_wallet);
        f.wallet->AddToWallet(spend, wallet::TxStateConfirmed{tip->GetBlockHash(), tip->nHeight, /*index=*/2});
        f.wallet->AddToWallet(funding, wallet::TxStateConflicted{tip->GetBlockHash(), tip->nHeight});
    }
    Step(flow);
    QVERIFY(flow.record().state == State::FAILED);
    QVERIFY(button("Try again…") != nullptr);
    QVERIFY(disable->isEnabled());
}

//! A registration resumed with its funding locked but nothing signed yet
//! (the process went away before the identity was built) builds nothing
//! until a verified read has shown the protocol version the client builds
//! under; an unanswered read is a wait, not a failure.
void PlatformTests::resumedFundingWaitsForVerifiedVersion()
{
    using State = IdentityFlow::State;
    using Kind = platform::OperationKind;
    IdentityFixture f{m_node, "alice"};
    IdentityFlow& flow{f.service->identityFlow()};
    f.writeRecord(FundedRecord(f));
    for (uint8_t byte : {0x1D, 0x21, 0x22, 0x23}) {
        f.fake->builds.push_back(ScriptedBuild(byte));
    }
    // Unanswered, the read is asked again with the backoff of a wait.
    for (int i = 0; i < 12; ++i) {
        Step(flow);
        Step(flow);
        Elapse(30);
    }
    QVERIFY(f.fake->build_kinds.empty());
    QVERIFY(flow.record().state == State::FUNDING_LOCKED);
    QCOMPARE(f.fake->countCalls("resolveName"), size_t{12});

    auto verified{platform_test::Absent<platform::DpnsName>()};
    verified.metadata.protocol_version = 14;
    f.fake->resolve_name.push_back(verified);
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(platform::StatusKind::OK));
    Step(flow);
    QCOMPARE(f.fake->build_kinds,
             (std::vector<Kind>{Kind::IDENTITY_CREATE, Kind::DPNS_PREORDER, Kind::DPNS_DOMAIN, Kind::PROFILE}));
    QVERIFY(flow.record().state == State::IDENTITY_BROADCAST);
    QCOMPARE(flow.record().signed_preorder.protocol_version, uint32_t{14});
}

//! While writes wait for a Dash Core update, a resumed registration reads
//! nothing for a protocol version no read would raise, and writes its wait
//! to the wallet once.
void PlatformTests::frozenWritesReadNoProtocolVersion()
{
    using State = IdentityFlow::State;
    IdentityFixture f{m_node, "alice"};
    IdentityFlow& flow{f.service->identityFlow()};
    f.writeRecord(FundedRecord(f));
    f.service->observeStatus(platform_test::BroadcastStatus(platform::StatusKind::UNSUPPORTED_PROTOCOL_VERSION),
                             /*verified_read=*/true, /*protocol_version=*/0);
    QVERIFY(f.service->writesFrozen());
    Step(flow);
    const unsigned int writes{f.wallet->GetDatabase().nUpdateCounter};
    for (int i = 0; i < 5; ++i) {
        Elapse(30);
        Step(flow);
    }
    QCOMPARE(f.fake->countCalls("resolveName"), size_t{0});
    QVERIFY(flow.record().state == State::FUNDING_LOCKED);
    QVERIFY(flow.record().last_error.find("upgraded") != std::string::npos);
    QCOMPARE(f.wallet->GetDatabase().nUpdateCounter.load(), writes);
}

//! A pre-signed transition that can no longer apply falls back to signing
//! its step when it gets there, with one prompt: here the domain's nonce was
//! spent (by another wallet of the same seed) while the name is still free.
void PlatformTests::registrationSignsSpentStepAgain()
{
    using State = IdentityFlow::State;
    using platform::StatusKind;
    IdentityFixture f{m_node, "alice"};
    IdentityFlow& flow{f.service->identityFlow()};
    const platform::Identity mine{f.myIdentity()};
    platform::IdentityRecord record{flow.record()};
    record.state = State::PREORDER_WAIT;
    record.signed_preorder = {ScriptedBuild(0x21).bytes, 1};
    record.signed_domain = {ScriptedBuild(0x22).bytes, 2};
    f.writeRecord(record);
    QVERIFY(f.wallet->EncryptWallet("passphrase"));
    int prompts{0};
    connect(&f.wallet_model, &WalletModel::requireUnlock, &f.wallet_model, [&] {
        ++prompts;
        f.wallet_model.setWalletLocked(false, "passphrase");
    });

    f.fake->identities.push_back(platform_test::Ok(mine));
    f.fake->nonces.push_back(platform_test::Ok<uint64_t>(2));
    f.fake->resolve_name.push_back(platform_test::Absent<platform::DpnsName>());
    f.fake->builds.push_back(ScriptedBuild(0x24));
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(StatusKind::OK));
    Step(flow);
    QCOMPARE(prompts, 1);
    QVERIFY(flow.record().state == State::DOMAIN_BROADCAST);
    QCOMPARE(f.fake->build_kinds, std::vector<platform::OperationKind>{platform::OperationKind::DPNS_DOMAIN});
    QCOMPARE(flow.record().signed_domain.nonce, uint64_t{3});
    QVERIFY(f.fake->broadcast_bytes.back() == ScriptedBuild(0x24).bytes);
    QVERIFY(f.wallet_model.getEncryptionStatus() == WalletModel::Locked);

    // Declining the passphrase parks the step, as before.
    record = flow.record();
    record.state = State::PREORDER_WAIT;
    record.signed_domain = {};
    f.writeRecord(record);
    disconnect(&f.wallet_model, &WalletModel::requireUnlock, &f.wallet_model, nullptr);
    f.fake->identities.push_back(platform_test::Ok(mine));
    f.fake->nonces.push_back(platform_test::Ok<uint64_t>(2));
    Step(flow);
    QVERIFY(flow.record().state == State::NEEDS_UNLOCK);
    QVERIFY(flow.record().resume_state == State::PREORDER_WAIT);
}

//! Transitions signed ahead are built under the protocol version Platform
//! ran then. One built under an earlier version is signed again at its step
//! (one prompt) instead of being sent, and one Platform refuses for its
//! document id (protocol version 14 changed the derivation) is signed again
//! at the same nonce rather than failing the registration.
void PlatformTests::registrationSignsAgainAfterUpgrade()
{
    using State = IdentityFlow::State;
    using Kind = platform::OperationKind;
    using platform::StatusKind;
    IdentityFixture f{m_node, "alice"};
    IdentityFlow& flow{f.service->identityFlow()};
    const platform::Identity mine{f.myIdentity()};
    QVERIFY(f.wallet->EncryptWallet("passphrase"));
    int prompts{0};
    connect(&f.wallet_model, &WalletModel::requireUnlock, &f.wallet_model, [&] {
        ++prompts;
        f.wallet_model.setWalletLocked(false, "passphrase");
    });
    const auto verified_at = [&](uint32_t protocol_version) {
        f.service->observeStatus(platform_test::BroadcastStatus(StatusKind::OK), /*verified_read=*/true, protocol_version);
    };

    // Signed at 13, Platform verified at 14: signed again before anything
    // is sent, under one prompt, and stamped with 14.
    verified_at(14);
    platform::IdentityRecord record{flow.record()};
    record.signed_preorder = {ScriptedBuild(0x21).bytes, 1, 13};
    record.signed_domain = {ScriptedBuild(0x22).bytes, 2, 13};
    f.writeRecord(record);
    f.scriptNameCheck();
    f.fake->identities.push_back(platform_test::Ok(mine));
    f.fake->nonces.push_back(platform_test::Absent<uint64_t>());
    f.fake->builds.push_back(ScriptedBuild(0x31));
    f.fake->builds.push_back(ScriptedBuild(0x32));
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(StatusKind::OK));
    Step(flow);
    QCOMPARE(prompts, 1);
    QVERIFY(flow.record().state == State::PREORDER_BROADCAST);
    QCOMPARE(f.fake->build_kinds, (std::vector<Kind>{Kind::DPNS_PREORDER, Kind::DPNS_DOMAIN}));
    QCOMPARE(f.fake->broadcast_bytes, std::vector<std::vector<uint8_t>>{ScriptedBuild(0x31).bytes});
    QCOMPARE(flow.record().signed_preorder.protocol_version, uint32_t{14});
    QCOMPARE(flow.record().signed_domain.protocol_version, uint32_t{14});

    // Signed ahead with no version known on one side: sent, and refused for
    // its document id. Not a failure: signed again at the same nonce.
    record = flow.record();
    record.state = State::IDENTITY_CONFIRMED;
    record.signed_preorder = {ScriptedBuild(0x21).bytes, 1, 0};
    record.signed_domain = {ScriptedBuild(0x22).bytes, 2, 0};
    f.writeRecord(record);
    f.fake->build_kinds.clear();
    f.fake->broadcast_bytes.clear();
    f.scriptNameCheck();
    f.fake->identities.push_back(platform_test::Ok(mine));
    f.fake->nonces.push_back(platform_test::Absent<uint64_t>());
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(StatusKind::CONSENSUS, 10405));
    Step(flow);
    QVERIFY(flow.record().state == State::PREORDER_BROADCAST);
    QVERIFY(flow.record().signed_preorder.empty() && flow.record().signed_domain.empty());
    QVERIFY(f.fake->build_kinds.empty());
    f.fake->identities.push_back(platform_test::Ok(mine));
    f.fake->nonces.push_back(platform_test::Absent<uint64_t>());
    f.fake->builds.push_back(ScriptedBuild(0x33));
    f.fake->builds.push_back(ScriptedBuild(0x34));
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(StatusKind::OK));
    Step(flow);
    QCOMPARE(prompts, 2);
    QVERIFY(flow.record().state == State::PREORDER_BROADCAST);
    QCOMPARE(flow.record().signed_preorder.nonce, uint64_t{1});
    QCOMPARE(f.fake->broadcast_bytes,
             (std::vector<std::vector<uint8_t>>{ScriptedBuild(0x21).bytes, ScriptedBuild(0x33).bytes}));

    // The same for a domain signed ahead: signed again at its nonce.
    record = flow.record();
    record.state = State::PREORDER_WAIT;
    record.signed_domain = {ScriptedBuild(0x22).bytes, 2, 0};
    f.writeRecord(record);
    f.fake->build_kinds.clear();
    f.scriptDocumentStep(1);
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(StatusKind::CONSENSUS, 10405));
    Step(flow);
    QVERIFY(flow.record().state == State::PREORDER_WAIT);
    QVERIFY(flow.record().signed_domain.empty());
    f.scriptDocumentStep(1);
    f.fake->builds.push_back(ScriptedBuild(0x35));
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(StatusKind::OK));
    Step(flow);
    QCOMPARE(prompts, 3);
    QVERIFY(flow.record().state == State::DOMAIN_BROADCAST);
    QCOMPARE(f.fake->build_kinds, std::vector<Kind>{Kind::DPNS_DOMAIN});
    QCOMPARE(flow.record().signed_domain.nonce, uint64_t{2});

    // A profile signed at 13 is not sent at 14: the user adds it instead.
    record = flow.record();
    record.state = State::REGISTERED;
    record.signed_profile = {ScriptedBuild(0x23).bytes, 1, 13};
    record.profile_display_name = "Alice A";
    f.writeRecord(record);
    QVERIFY(flow.profileChosen());
    const size_t sent{f.fake->broadcast_bytes.size()};
    Step(flow);
    QVERIFY(!flow.profilePending());
    QVERIFY(!flow.profileChosen());
    QCOMPARE(f.fake->broadcast_bytes.size(), sent);
}

//! What a failed registration shows is worded when it is shown: a record
//! stored by an earlier build with a raw consensus code reads like a new
//! failure, and a new failure keeps its typed result for "Show details"
//! across a reload.
void PlatformTests::storedFailuresAreWordedWhenShown()
{
    using State = IdentityFlow::State;
    IdentityFixture f{m_node, "qa1shrm"};
    IdentityFlow& flow{f.service->identityFlow()};
    platform::IdentityRecord record{flow.record()};
    record.state = State::FAILED;
    record.resume_state = State::PREORDER_BROADCAST;
    record.last_error = "Platform rejected the request (consensus error 10405).";
    f.writeRecord(record);
    QVERIFY(!flow.lastErrorText().contains("10405"));
    QVERIFY(flow.lastErrorText().contains("not something you did"));

    // A new failure: the preorder refused by consensus.
    flow.reset();
    platform::IdentityRecord retry{flow.record()};
    retry.label = "qa1shrm";
    retry.normalized_label = platform::helpers::NormalizeLabel("qa1shrm");
    f.writeRecord(retry);
    f.scriptNameCheck();
    f.scriptDocumentStep(1);
    f.fake->builds.push_back(ScriptedBuild(1));
    f.fake->builds.push_back(ScriptedBuild(2));
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(platform::StatusKind::CONSENSUS, 10405));
    Step(flow);
    QVERIFY(flow.record().state == State::FAILED);
    flow.reload();
    QVERIFY(flow.lastErrorText().contains("not something you did"));
    QVERIFY(flow.lastErrorDetails().contains("Operation: Reserve username"));
    QVERIFY(flow.lastErrorDetails().contains("InvalidDocumentTransitionIdError"));
}

//! The wizard's entry page only completes on a proved availability answer
//! for what is currently typed.
void PlatformTests::usernameWizardEntry()
{
    IdentityFixture f{m_node, ""};
    CreateUsernameWizard wizard{*f.service, f.wallet_model};
    auto* entry{wizard.findChild<UsernameEntryPage*>()};
    QVERIFY(entry != nullptr);
    QVERIFY(!entry->isComplete());
    auto* input{entry->findChild<QLineEdit*>()};
    QVERIFY(input != nullptr);
    const auto shown = [entry](const QString& text) {
        for (const auto* label : entry->findChildren<QLabel*>()) {
            if (label->isVisibleTo(entry) && label->text().contains(text)) return true;
        }
        return false;
    };
    // A label the DPNS rule refuses (consecutive hyphens) is never looked
    // up; the rule is stated once, by the error, and nothing is "stored".
    input->setText("a--b");
    QTest::qWait(500);
    Drain();
    QVERIFY(!entry->isComplete());
    QVERIFY(f.fake->calls.empty());
    QVERIFY(shown("This can't be a username"));
    QVERIFY(!shown("3 to 63 characters"));
    input->setText("Alice_x");
    QVERIFY(!shown("Stored as"));
    input->clear();
    QVERIFY(shown("3 to 63 characters"));
    input->setText("Alice");
    QVERIFY(!entry->isComplete());

    // A proven absence of the domain and of a contest makes the name available.
    f.fake->resolve_name.push_back(platform_test::Absent<platform::DpnsName>());
    f.fake->contested_states.push_back(platform_test::Absent<platform::ContestedNameState>());
    f.service->checkNameAvailability("Alice");
    Drain();
    QVERIFY(entry->isComplete());
    QVERIFY(entry->contested());
    QCOMPARE(entry->username(), QString("Alice"));
    QVERIFY(shown("Stored as “a11ce”"));
    QCOMPARE(f.fake->calls.front().argument, std::string("a11ce"));
    // Entered again after a failed name step, the page looks the typed name
    // up again rather than keep showing it as available.
    entry->initializePage();
    QVERIFY(!entry->isComplete());
    QVERIFY(!shown("premium username"));
    f.fake->resolve_name.push_back(platform_test::Absent<platform::DpnsName>());
    f.fake->contested_states.push_back(platform_test::Absent<platform::ContestedNameState>());
    f.service->checkNameAvailability("Alice");
    Drain();
    QVERIFY(entry->isComplete());

    // A warning that arrives while the window is open grows it until the
    // warning fits, whole, above the fields below it.
    wizard.resize(wizard.minimumSize());
    wizard.show();
    Drain();
    input->setText("Alic");
    input->setText("Alice");
    f.fake->resolve_name.push_back(platform_test::Absent<platform::DpnsName>());
    f.fake->contested_states.push_back(platform_test::Absent<platform::ContestedNameState>());
    f.service->checkNameAvailability("Alice");
    Drain();
    QVERIFY(entry->contested());
    const QLabel* warning{nullptr};
    for (const auto* label : entry->findChildren<QLabel*>()) {
        if (label->text().contains("premium username")) warning = label;
    }
    QVERIFY(warning != nullptr);
    QVERIFY(warning->height() >= warning->heightForWidth(warning->width()));
    QVERIFY(wizard.height() >= wizard.heightForWidth(wizard.width()));
    wizard.hide();

    // A taken name is not available; a failed read is not "available" either.
    platform::DpnsName taken;
    taken.identity = platform_test::IdentifierFromByte(0x33);
    f.fake->resolve_name.push_back(platform_test::Ok(taken));
    f.service->checkNameAvailability("Alice");
    Drain();
    QVERIFY(!entry->isComplete());
    f.fake->resolve_name.push_back(platform_test::Failed<platform::DpnsName>(platform::StatusKind::UNAVAILABLE));
    f.service->checkNameAvailability("Alice");
    Drain();
    QVERIFY(!entry->isComplete());
}

//! The progress page never claims a step that did not happen: a preorder
//! refused while it is being broadcast fails at "Reserving your username",
//! with a plain sentence (no consensus code) and its reassurance; the
//! window's buttons are Close and Try again…, never Cancel.
void PlatformTests::usernameProgressWording()
{
    IdentityFixture f{m_node, "alice"};
    CreateUsernameWizard wizard{*f.service, f.wallet_model};
    wizard.startAtProgress();
    auto* log{wizard.findChild<QPlainTextEdit*>("registrationLog")};
    QVERIFY(log != nullptr);
    const auto buttons = [&wizard] {
        QStringList shown;
        for (const auto* button : wizard.findChildren<QPushButton*>()) {
            if (button->isVisibleTo(&wizard)) shown << button->text();
        }
        return shown;
    };
    QVERIFY(buttons().contains("Close"));
    QVERIFY(!buttons().contains("Cancel"));

    f.scriptNameCheck();
    f.scriptDocumentStep(1);
    f.fake->builds.push_back(ScriptedBuild(1));
    f.fake->builds.push_back(ScriptedBuild(2));
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(platform::StatusKind::CONSENSUS, 10405));
    f.service->identityFlow().advance();
    Drain();
    QVERIFY(f.service->identityFlow().record().state == IdentityFlow::State::FAILED);
    QVERIFY(log->toPlainText().contains("Reserving your username"));
    QVERIFY(!log->toPlainText().contains("Registering your username"));
    QStringList texts;
    for (const auto* label : wizard.findChildren<QLabel*>()) {
        texts << label->text();
    }
    QVERIFY(texts.contains("Registration did not finish"));
    QVERIFY(texts.join(' ').contains("keeps its balance on Dash Platform"));
    QVERIFY(!texts.join(' ').contains("10405"));
    QVERIFY(buttons().contains("Close"));
    QVERIFY(buttons().contains("Try again…"));
    QVERIFY(!buttons().contains("Cancel"));

    // Registered: a profile is suggested only when no display name was
    // chosen with the username.
    const auto subtitle = [&wizard] {
        for (const auto* label : wizard.findChildren<QLabel*>()) {
            if (label->text().startsWith("You're now")) return label->text();
        }
        return QString{};
    };
    platform::IdentityRecord record{f.service->identityFlow().record()};
    record.state = IdentityFlow::State::REGISTERED;
    record.last_failure.reset();
    f.writeRecord(record);
    QVERIFY(subtitle().contains("add a profile"));
    QVERIFY(buttons().contains("Add profile…"));
    record.profile_display_name = "Alice A";
    record.signed_profile = {ScriptedBuild(0x23).bytes, 1, 0};
    f.writeRecord(record);
    QVERIFY(!subtitle().isEmpty());
    QVERIFY(!subtitle().contains("profile"));
    QVERIFY(!buttons().contains("Add profile…"));
    // Refused, so it is never published: suggested again after all.
    f.fake->broadcasts.push_back(platform_test::BroadcastStatus(platform::StatusKind::CONSENSUS, 40100));
    f.service->identityFlow().advance();
    Drain();
    QVERIFY(!f.service->identityFlow().profilePending());
    QVERIFY(subtitle().contains("add a profile"));
    QVERIFY(buttons().contains("Add profile…"));

    // Waiting for the passphrase: Unlock and continue… is offered only
    // while writes are allowed, like Try again….
    const auto unlock_button = [&wizard]() -> QPushButton* {
        for (auto* button : wizard.findChildren<QPushButton*>()) {
            if (button->text() == "Unlock and continue…" && button->isVisibleTo(&wizard)) return button;
        }
        return nullptr;
    };
    {
        platform::IdentityRecord parked{f.service->identityFlow().record()};
        parked.state = IdentityFlow::State::NEEDS_UNLOCK;
        parked.resume_state = IdentityFlow::State::PREORDER_BROADCAST;
        f.writeRecord(parked);
        QVERIFY(unlock_button() && unlock_button()->isEnabled());
        f.service->observeStatus(platform_test::BroadcastStatus(platform::StatusKind::UNSUPPORTED_PROTOCOL_VERSION),
                                 /*verified_read=*/true, /*protocol_version=*/0);
        f.writeRecord(parked);
        QVERIFY(unlock_button() && !unlock_button()->isEnabled());
        QVERIFY(!unlock_button()->toolTip().isEmpty());
    }
}

//! Disable DashPay wipes the records, the only trace of an asset lock no
//! identity consumed yet: the guard holds from the funding payment until
//! the identity exists, also while that step waits for the passphrase or
//! failed with the lock kept for "try again", and also while a gate keeps
//! the service from starting.
void PlatformTests::unconsumedFundingBlocksDisable()
{
    using State = IdentityFlow::State;
    IdentityFixture f{m_node, "alice"};
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    // The flow's view and the one read from the wallet agree.
    const auto holds = [&] {
        const bool held{f.service->identityFlow().holdsUnconsumedFunding()};
        return PlatformService::HoldsUnconsumedFunding(wallet) == held && held;
    };
    const auto released = [&] {
        return !f.service->identityFlow().holdsUnconsumedFunding() && !PlatformService::HoldsUnconsumedFunding(wallet);
    };
    QVERIFY(released());
    platform::IdentityRecord record{f.service->identityFlow().record()};
    for (const State state : {State::FUNDING_SENT, State::FUNDING_LOCKED, State::IDENTITY_BROADCAST}) {
        record.state = state;
        f.writeRecord(record);
        QVERIFY(holds());
    }
    record.state = State::NEEDS_UNLOCK;
    record.resume_state = State::FUNDING_LOCKED;
    f.writeRecord(record);
    QVERIFY(holds());
    record.resume_state = State::PREORDER_BROADCAST;
    f.writeRecord(record);
    QVERIFY(released());
    record.state = State::FAILED;
    for (const State resume : {State::FUNDING_LOCKED, State::IDENTITY_BROADCAST}) {
        record.resume_state = resume;
        f.writeRecord(record);
        QVERIFY(holds());
    }
    // A failure while funding abandoned the payment; one after the identity
    // exists keeps nothing but the identity.
    for (const State resume : {State::FUNDING_SENT, State::PREORDER_BROADCAST}) {
        record.resume_state = resume;
        f.writeRecord(record);
        QVERIFY(released());
    }

    // The node has no ChainLock, so the page runs no service of its own; the
    // guard is read from the wallet.
    record.resume_state = State::FUNDING_LOCKED;
    f.writeRecord(record);
    PlatformPage page;
    page.setWalletModel(&f.wallet_model);
    page.setClientModel(&f.models.client);
    QVERIFY(page.findChild<PlatformService*>() == nullptr);
    // DashPay is turned off in the Options dialog only, and there not
    // while the funding waits for its identity, which says why in text.
    for (const auto* button : page.findChildren<QPushButton*>()) {
        QVERIFY(button->text() != "Disable DashPay…");
    }
    DashPayOptionsWidget section{page, f.wallet_model};
    QPushButton* disable{section.findChild<QPushButton*>()};
    QCOMPARE(disable->text(), QString("Disable DashPay…"));
    QVERIFY(!disable->isEnabled());
    const auto reason_shown = [&section] {
        for (const auto* label : section.findChildren<QLabel*>()) {
            if (label->text().startsWith("You can turn DashPay off")) return label->isVisibleTo(&section);
        }
        return false;
    };
    QVERIFY(reason_shown());
    record.state = State::REGISTERED;
    f.writeRecord(record);
    page.setClientModel(&f.models.client); // refresh
    QVERIFY(disable->isEnabled());
    QVERIFY(!reason_shown());
}

//! A balance on Dash Platform is a Dash amount in the wallet's display unit,
//! rounded down to a duff, and follows a change of that unit; the word
//! "credit" appears nowhere on the page.
void PlatformTests::balanceShownAsDash()
{
    platform::IdentityRecord record{RegisteredRecord()};
    record.state = platform::IdentityRecord::State::IDENTITY_CONFIRMED;
    record.label.clear();
    record.normalized_label.clear();
    PageFixture f{m_node, record};
    QVERIFY(f.fake != nullptr);
    OptionsModel& options{*f.wallet_model.getOptionsModel()};
    const QVariant unit_before{QVariant::fromValue(options.getDisplayUnit())};
    options.setDisplayUnit(QVariant::fromValue(BitcoinUnit::DASH));
    Q_EMIT f.service().identityBalanceLoaded(722'958'380);
    const auto balance_line = [&f] {
        for (const auto* label : f.page.findChildren<QLabel*>()) {
            if (label->text().startsWith("Balance on Dash Platform")) return label->text();
        }
        return QString{};
    };
    QCOMPARE(balance_line(), "Balance on Dash Platform: " + BitcoinUnits::formatWithUnit(BitcoinUnit::DASH, 722'958));
    // The card asking for a username says what the identity holds, as Dash.
    QVERIFY(f.texts().contains("with " + BitcoinUnits::formatWithUnit(BitcoinUnit::DASH, 722'958) + " on Dash Platform"));
    options.setDisplayUnit(QVariant::fromValue(BitcoinUnit::mDASH));
    QCOMPARE(balance_line(), "Balance on Dash Platform: " + BitcoinUnits::formatWithUnit(BitcoinUnit::mDASH, 722'958));
    QVERIFY(!f.texts().contains("credit", Qt::CaseInsensitive));
    options.setDisplayUnit(unit_before);

    // Neither do the failures a Platform call can end in.
    for (const uint32_t code : {10405u, 30000u, 40210u, 40114u, 40400u}) {
        for (const auto context : {PlatformUi::Context::READ, PlatformUi::Context::NAME_REGISTER}) {
            const auto error{PlatformUi::Describe(platform_test::BroadcastStatus(platform::StatusKind::CONSENSUS, code),
                                                  context, {})};
            QVERIFY(!error.text.contains("credit", Qt::CaseInsensitive));
        }
    }
}

//! With -proxy set DashPay starts as it does without one: the page creates
//! its service, configures the client with that proxy and its per-connection
//! isolation, and pushes the evonodes the proxy reaches.
void PlatformTests::serviceStartsThroughTheProxy()
{
    const NetworkSettingsOverride settings{ProxiedSettings()};
    PageFixture f{m_node, RegisteredRecord()};
    QVERIFY(f.fake != nullptr);
    QVERIFY(f.config.has_value());
    QVERIFY(f.config->proxy == ProxiedSettings().clearnet_proxy);
    QVERIFY(f.config->proxy->randomize_credentials);
    QVERIFY(f.page.availability().available());
    QVERIFY(WaitForEndpointUpdates(*f.fake, 1));
    f.service().setEndpointsForTesting({platform::Endpoint{LookupNumeric("203.0.113.7", 443)}});
    QCOMPARE(f.fake->endpoint_updates.back().size(), size_t{1});
    QVERIFY(f.service().haveEndpoints());
}

//! A client the node's settings cannot start (the SDK refused the proxy)
//! is not a silent blank page: the welcome page says DashPay couldn't
//! start, and it is not tried again on every block.
void PlatformTests::clientFailureIsShown()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    QVERIFY(PlatformService::Enable(f.wallet_model.wallet()));
    LockTip(f.chain);
    int attempts{0};
    PlatformPage page{nullptr, [&attempts](const platform::ClientConfig&) {
                          ++attempts;
                          return std::unique_ptr<platform::PlatformClient>{};
                      }};
    page.setWalletModel(&f.wallet_model);
    page.setClientModel(&f.models.client);
    QVERIFY(page.findChild<PlatformService*>() == nullptr);
    QCOMPARE(attempts, 1);
    QStringList texts;
    for (const auto* label : page.findChildren<QLabel*>()) {
        if (label->isVisibleTo(&page)) texts << label->text();
    }
    QVERIFY(texts.contains("DashPay couldn't start"));
    QMetaObject::invokeMethod(&page, "refresh");
    QCOMPARE(attempts, 1);
}

//! Shutdown detaches the client model before deleting it and the wallet
//! models: the service goes with it, and its consumers are told first.
void PlatformTests::detachingClientModelStopsService()
{
    PageFixture f{m_node, RegisteredRecord()};
    QVERIFY(f.page.findChild<PlatformService*>() != nullptr);
    std::vector<PlatformService*> ready;
    QObject::connect(&f.page, &PlatformPage::platformServiceReady, [&ready](PlatformService* service) {
        ready.push_back(service);
    });
    f.page.setClientModel(nullptr);
    QVERIFY(f.page.findChild<PlatformService*>() == nullptr);
    QCOMPARE(ready, std::vector<PlatformService*>{nullptr});
}

//! A gate that closes after the service started (network activity off, no
//! evonode on the route) is said on the dashboard, with its action.
void PlatformTests::gateClosedAfterStartIsShown()
{
    PageFixture f{m_node, RegisteredRecord()};
    auto* stack{f.page.findChild<QStackedWidget*>()};
    auto* alert{f.page.findChild<QLabel*>("labelAlerts")};
    QVERIFY(alert != nullptr);
    const auto turn_on_shown = [&f] {
        for (const auto* button : f.page.findChildren<QPushButton*>()) {
            if (button->text() == "Turn network on" && button->isVisibleTo(&f.page)) return true;
        }
        return false;
    };
    QCOMPARE(stack->currentIndex(), 1);
    QVERIFY(!alert->isVisibleTo(&f.page));

    // The test node has no UI interface: the notification is sent by hand.
    f.node.setNetworkActive(false);
    Q_EMIT f.models.client.networkActiveChanged(false);
    QCOMPARE(stack->currentIndex(), 1);
    QVERIFY(alert->isVisibleTo(&f.page));
    QVERIFY(alert->text().contains("paused"));
    QVERIFY(turn_on_shown());
    f.node.setNetworkActive(true);
    Q_EMIT f.models.client.networkActiveChanged(true);
    QVERIFY(!alert->isVisibleTo(&f.page));

    // Only an onion evonode, which the direct route does not reach.
    CNetAddr onion;
    QVERIFY(onion.SetSpecial("pg6mmjiyjmcrsslvykfwnntlaru7p5svn6y2ymmju6nubxndf4pscryd.onion"));
    f.service().setEndpointsForTesting({platform::Endpoint{CService{onion, 443}}});
    QVERIFY(f.service().unreachable());
    QCOMPARE(stack->currentIndex(), 1);
    QVERIFY(alert->isVisibleTo(&f.page));
    QVERIFY(alert->text().contains("evonode"));
    QVERIFY(!turn_on_shown());
}

//! Under -onlynet=onion the Tor controller sets the onion proxy only once it
//! has connected; the onion peers that follow it start the service.
void PlatformTests::onionProxyLaterStartsService()
{
    PlatformNetworkSettings onion_only;
    onion_only.ipv4 = onion_only.ipv6 = false;
    onion_only.onion = true;
    const NetworkSettingsOverride settings{onion_only};
    PageFixture f{m_node, RegisteredRecord()};
    QVERIFY(f.page.findChild<PlatformService*>() == nullptr);

    onion_only.onion_proxy = platform::ProxyConfig{LookupNumeric("127.0.0.1", 9050), true};
    PlatformNetworkSettings::SetForTesting(onion_only);
    Q_EMIT f.models.client.numConnectionsChanged(1);
    QVERIFY(f.page.findChild<PlatformService*>() != nullptr);
    QVERIFY(f.config && f.config->proxy == onion_only.onion_proxy);
}

//! Discarding records of another layout keeps a registration record that
//! holds an asset lock no identity consumed yet (seed recovery finds
//! identities, not asset locks), and the registration continues from it;
//! everything else is discarded.
void PlatformTests::layoutChangeKeepsUnconsumedFunding()
{
    using State = IdentityFlow::State;
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    QVERIFY(PlatformService::Enable(wallet));
    wallet.writePlatformData(platform::records::VERSION, {platform::records::CURRENT_VERSION - 1});
    platform::IdentityRecord record;
    record.state = State::FUNDING_LOCKED;
    record.funding_txid = uint256::ONE;
    record.funding_amount = 1'000'000;
    record.label = "alice";
    record.normalized_label = "a11ce";
    wallet.writePlatformData(platform::records::IDENTITY, platform::SerializeIdentityRecord(record));
    wallet.writePlatformData(std::string{platform::records::CONTACT_USERNAME_PREFIX} + "00", {'b', 'o', 'b'});

    PlatformService service{f.wallet_model, f.models.client, std::make_unique<FakePlatformClient>()};
    QVERIFY(service.identityFlow().record().state == State::FUNDING_LOCKED);
    QVERIFY(service.identityFlow().record().funding_txid == uint256::ONE);
    QVERIFY(wallet.getPlatformData("contact/").empty());
    QVERIFY(PlatformService::IsEnabled(wallet));
    QVERIFY(platform::IsRecordSetCurrent(service.readRecord(platform::records::VERSION), /*have_platform_records=*/true));
}
