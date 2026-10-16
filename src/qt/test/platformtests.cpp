// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/platformtests.h>

#include <qt/test/masternodetestutil.h>

#include <crypto/aes.h>
#include <evo/assetlocktx.h>
#include <evo/specialtx.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key.h>
#include <key_io.h>
#include <netbase.h>
#include <qt/clientfeeds.h>
#include <qt/platform/contactflow.h>
#include <qt/platform/identityflow.h>
#include <qt/platform/platformpage.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/usernameflow.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/walletmodel.h>
#include <qt/walletview.h>
#include <script/standard.h>
#include <test/util/platform_client.h>
#include <test/util/setup_common.h>
#include <util/settings.h>
#include <util/system.h>
#include <util/time.h>
#include <wallet/coincontrol.h>
#include <wallet/context.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <QCoreApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

using MasternodeTestUtil::GuiModels;
using MasternodeTestUtil::MakeCoinbaseWallet;
using MasternodeTestUtil::MakeTestWallet;
using MasternodeTestUtil::WalletGuard;
using Gate = PlatformService::Gate;
using Purpose = platform::IdentityPublicKey::Purpose;
using Level = platform::IdentityPublicKey::SecurityLevel;

namespace {
CService Ipv4Evonode() { return LookupNumeric("203.0.113.7", 443); }
CService Ipv4Evonode2() { return LookupNumeric("203.0.113.8", 443); }
CService Ipv6Evonode() { return LookupNumeric("2001:db8::7", 443); }
CService OnionEvonode() { return LookupNumeric("pg6mmjiyjmcrsslvykfwnntlaru7p5svn6y2ymmju6nubxndf4pscryd.onion", 443); }
CService I2pEvonode() { return LookupNumeric("udhdrtrcetjm5sxzskjyr5ztpeszydbh4dpl3pl4utgqqw2v4jna.b32.i2p", 443); }
//! -proxy=127.0.0.1:9050 with -proxyrandomize.
platform::ProxyConfig Tor() { return {LookupNumeric("127.0.0.1", 9050), true}; }

//! 110 blocks: the coinbase key's first coins are mature.
struct MatureChain : TestChain100Setup {
    explicit MatureChain(const std::string& chain_name) :
        TestChain100Setup{chain_name}
    {
        mineBlocks(10);
    }
};

//! On a MatureChain the wallet holds the coinbase key's coins.
template <typename ChainSetup>
std::shared_ptr<wallet::CWallet> MakeFixtureWallet(interfaces::Node& node, wallet::WalletContext& context,
                                                   const ChainSetup& chain)
{
    if constexpr (std::is_same_v<ChainSetup, MatureChain>) {
        return MakeCoinbaseWallet(node, context, chain, "platform", /*encrypt=*/false);
    } else {
        return MakeTestWallet(node, context, "platform");
    }
}

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
        wallet(MakeFixtureWallet(node, context, chain)),
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
//! A synced node and a wallet with coins to fund asset locks with.
using FundedFixture = FixtureWithChain<MatureChain>;

//! What a service reads instead of the node, and the clients it made.
struct FakeSources {
    PlatformNetworkSettings settings;
    std::vector<CService> evonodes{Ipv4Evonode()};
    bool refuse{false};
    //! Run on each new client, to script the answers to the calls it gets first.
    std::function<void(FakePlatformClient&)> script;
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
                if (script) script(*client);
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

platform::Result<platform::Identity> Found(uint8_t id, uint64_t balance)
{
    platform::Result<platform::Identity> result;
    result.status.kind = platform::StatusKind::OK;
    result.value.id.fill(id);
    result.value.balance = balance;
    return result;
}

platform::Result<platform::Identity> NotFound()
{
    platform::Result<platform::Identity> result;
    result.status.kind = platform::StatusKind::NOT_FOUND;
    return result;
}

platform::IdentityPublicKey Key(uint32_t id, Purpose purpose, Level level, uint64_t disabled_at = 0)
{
    platform::IdentityPublicKey key;
    key.id = id;
    key.purpose = purpose;
    key.security_level = level;
    key.disabled_at = disabled_at;
    return key;
}

//! The keys Dash Core registers, by id as the mobile wallets do.
std::vector<platform::IdentityPublicKey> RegisteredKeys()
{
    return {Key(0, Purpose::AUTHENTICATION, Level::MASTER), Key(1, Purpose::AUTHENTICATION, Level::CRITICAL),
            Key(2, Purpose::AUTHENTICATION, Level::HIGH),   Key(3, Purpose::TRANSFER, Level::CRITICAL),
            Key(4, Purpose::ENCRYPTION, Level::MEDIUM),     Key(5, Purpose::DECRYPTION, Level::MEDIUM)};
}

platform::Result<platform::Identifier> Resolved(std::optional<uint8_t> owner)
{
    platform::Result<platform::Identifier> result;
    result.status.kind = owner ? platform::StatusKind::OK : platform::StatusKind::NOT_FOUND;
    if (owner) result.value.fill(*owner);
    return result;
}

platform::Result<platform::Names> Names(const std::vector<std::string>& labels)
{
    platform::Result<platform::Names> result;
    result.status.kind = platform::StatusKind::OK;
    result.value.complete = true;
    for (const std::string& label : labels) {
        result.value.names.push_back({label, label, {}, {}});
    }
    return result;
}

platform::Result<std::vector<platform::ContactRequest>> Requests(std::vector<platform::ContactRequest> requests)
{
    platform::Result<std::vector<platform::ContactRequest>> result;
    result.status.kind = platform::StatusKind::OK;
    result.value = std::move(requests);
    return result;
}

platform::ContactRequest Request(uint8_t from, uint8_t to, uint64_t created_at_ms)
{
    platform::ContactRequest request;
    request.owner.fill(from);
    request.to_user_id.fill(to);
    request.created_at_ms = created_at_ms;
    return request;
}

//! A request from `from` encrypted, as DIP-15 does, from their key
//! `sender_key` (id 1) to the wallet's identity key `our_key`; it carries
//! `pubkey` as the payment chain's key.
platform::ContactRequest IncomingRequest(interfaces::Wallet& wallet, uint8_t from, uint8_t to, const CKey& sender_key,
                                         uint64_t created_at_ms, Span<const uint8_t> pubkey, uint32_t our_key = 5)
{
    auto request{Request(from, to, created_at_ms)};
    request.sender_key_index = 1;
    request.recipient_key_index = our_key;
    const auto secret{wallet.platformECDHSecret({0, our_key}, sender_key.GetPubKey())};
    Assert(secret && secret.value.size() == AES256_KEYSIZE);
    std::vector<uint8_t> compact(4 + 32, 0x11);
    compact.insert(compact.end(), pubkey.begin(), pubkey.end());
    request.encrypted_public_key.assign(AES_BLOCKSIZE, 0x22);
    request.encrypted_public_key.resize(AES_BLOCKSIZE + 80);
    const AES256CBCEncrypt aes{secret.value.data(), request.encrypted_public_key.data(), /*padIn=*/true};
    Assert(aes.Encrypt(compact.data(), compact.size(), request.encrypted_public_key.data() + AES_BLOCKSIZE) == 80);
    return request;
}

//! A counterparty whose key 1 (`key`, of `purpose`) sent their requests;
//! key 2 receives ours.
platform::Result<platform::Identity> Peer(uint8_t id, const CKey& key, Purpose purpose, uint64_t disabled_at = 0)
{
    auto found{Found(id, 0)};
    auto sender{Key(1, purpose, Level::HIGH, disabled_at)};
    const CPubKey pubkey{key.GetPubKey()};
    sender.data.assign(pubkey.begin(), pubkey.end());
    found.value.public_keys = {Key(0, Purpose::AUTHENTICATION, Level::MASTER), sender,
                               Key(2, Purpose::DECRYPTION, Level::MEDIUM)};
    return found;
}

//! The payment chain IncomingRequest sends for `pubkey`.
wallet::FriendshipXpub PaymentChain(const CPubKey& pubkey)
{
    wallet::FriendshipXpub chain;
    chain.pubkey = pubkey;
    std::fill(chain.chaincode.begin(), chain.chaincode.end(), 0x11);
    chain.parent_fingerprint.fill(0x11);
    return chain;
}

CTxDestination PaymentAddress(const wallet::FriendshipXpub& chain, uint32_t index)
{
    CTxDestination address;
    Assert(wallet::DeriveFriendshipPaymentDestination(chain, index, address));
    return address;
}

//! The wallet sends a payment to `address`, as the Send tab does.
void SendTo(interfaces::Wallet& wallet, const CTxDestination& address)
{
    int change_pos{-1};
    CAmount fee;
    const auto tx{wallet.createTransaction({{GetScriptForDestination(address), COIN, false}}, {}, /*sign=*/true,
                                           change_pos, fee)};
    Assert(tx && wallet.commitTransaction(*tx, {}, {}));
}

//! The button in the contacts list row of `id`, or nullptr.
QPushButton* ContactButton(const PlatformPage& page, uint8_t id)
{
    const auto contacts{page.contactFlow()->contacts()};
    const auto* list{page.findChild<QTableWidget*>("dashPayContacts")};
    for (size_t row = 0; row < contacts.size(); ++row) {
        if (contacts[row].id[0] == id) return qobject_cast<QPushButton*>(list->cellWidget(row, 2));
    }
    return nullptr;
}

//! Click Pay on the row of `id`: the address handed to the Send tab, empty
//! if none was.
QString PayContact(PlatformPage& page, FakeSources& sources, uint8_t id)
{
    QSignalSpy paid{&page, &PlatformPage::payTo};
    QPushButton* pay{ContactButton(page, id)};
    if (!pay || pay->objectName() != "dashPayPayContact") return {};
    pay->click();
    sources.fake->deliverAll();
    return paid.size() == 1 ? paid.front().front().toString() : QString{};
}

//! The node has the asset lock's InstantSend lock at once.
std::optional<platform::AssetLockProof> InstantProof(const CTransactionRef& tx)
{
    return platform::InstantAssetLockProof{tx, {1}, 0};
}

//! Confirms, or declines, each asset lock the flow would send, and keeps
//! what it was asked.
struct Confirmations {
    bool accept{true};
    std::vector<std::pair<CAmount, CAmount>> asked; //!< amount, fee

    IdentityFlow::Confirm get()
    {
        return [this](CAmount amount, CAmount fee) {
            asked.emplace_back(amount, fee);
            return accept;
        };
    }
};

uint256 ProofTxid(const platform::AssetLockProof& proof)
{
    return std::get<platform::InstantAssetLockProof>(proof).transaction->GetHash();
}

//! The key the asset lock's credit output pays, as a script.
CScript CreditScript(interfaces::Wallet& wallet, const uint256& txid)
{
    const CTransactionRef tx{wallet.getTx(txid)};
    Assert(tx && tx->nType == TRANSACTION_ASSET_LOCK);
    return Assert(GetTxPayload<CAssetLockPayload>(*tx))->getCreditOutputs().at(0).scriptPubKey;
}

CScript FundingScript(interfaces::Wallet& wallet, const wallet::PlatformKeyRequest& key)
{
    const auto pubkey{wallet.getPlatformPubKey(key)};
    Assert(pubkey);
    return GetScriptForDestination(PKHash{pubkey.value});
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

//! Creating an identity: the user sees the amount and the exact fee first,
//! and declining sends nothing. Accepting commits an asset lock paying the
//! registration funding key and records it; once the node has a lock for it
//! the identity is registered with that proof, and is then the wallet's and
//! the record is gone.
void PlatformTests::registrationFundsAndRegisters()
{
    FundedFixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    FakeSources sources;
    sources.script = [](FakePlatformClient& client) {
        client.identities_by_pubkey_hash = {NotFound()};
        client.registered_identities = {Found(7, 2'999'000'000)};
    };
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    Confirmations confirmations;
    bool locked{false};
    IdentityFlow flow{service, confirmations.get(),
                      [&locked](const CTransactionRef& tx) { return locked ? InstantProof(tx) : std::nullopt; }};
    sources.fake->deliverAll();
    QVERIFY(flow.lookedUp());
    QVERIFY(!flow.identity());

    const size_t txs{wallet.getWalletTxs().size()};
    confirmations.accept = false;
    flow.create();
    QCOMPARE(confirmations.asked.size(), size_t{1});
    QCOMPARE(confirmations.asked.back().first, IdentityFlow::REGISTRATION_AMOUNT);
    QVERIFY(confirmations.asked.back().second > 0);
    QCOMPARE(wallet.getWalletTxs().size(), txs);
    QVERIFY(!flow.funding());
    QCOMPARE(sources.fake->countCalls("registerIdentity"), size_t{0});

    confirmations.accept = true;
    flow.create();
    const auto funding{flow.funding()};
    QVERIFY(funding && !funding->top_up);
    QCOMPARE(funding->amount, IdentityFlow::REGISTRATION_AMOUNT);
    QVERIFY(CreditScript(wallet, funding->txid) == FundingScript(wallet, wallet::RegistrationFundingKey{0}));
    QVERIFY(flow.step() == IdentityFlow::Step::WAITING_FOR_LOCK);
    QCOMPARE(sources.fake->countCalls("registerIdentity"), size_t{0});
    locked = true;
    Q_EMIT f.models.client.instantSendChanged();
    QCOMPARE(sources.fake->countCalls("registerIdentity"), size_t{1});
    QVERIFY(sources.fake->calls.back().identity_index == 0U);
    QVERIFY(ProofTxid(sources.fake->proofs.back()) == funding->txid);

    sources.fake->deliverAll();
    QVERIFY(flow.identity() && flow.identity()->balance == 2'999'000'000U);
    QVERIFY(!flow.funding());
    QVERIFY(flow.error().isEmpty());
}

//! A registration whose write was dropped (network off) or whose answer
//! never came (restart) keeps its record: the next session offers Continue,
//! which sends the same asset lock again without funding another. Platform
//! reporting it consumed means an earlier attempt landed, and the identity
//! is then found by its MASTER key, which ends the record.
void PlatformTests::continueResumesFundingAfterRestart()
{
    FundedFixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    Confirmations confirmations;
    uint256 txid;
    {
        FakeSources sources;
        sources.script = [](FakePlatformClient& client) { client.identities_by_pubkey_hash = {NotFound()}; };
        PlatformService service{f.wallet_model, f.models.client, sources.get()};
        IdentityFlow flow{service, confirmations.get(), InstantProof};
        sources.fake->deliverAll();
        flow.create();
        QCOMPARE(sources.fake->countCalls("registerIdentity"), size_t{1});
        txid = Assert(flow.funding())->txid;

        f.setNetworkActive(false);
        QVERIFY(flow.step() == IdentityFlow::Step::IDLE);
        QVERIFY(flow.funding() && flow.funding()->txid == txid);
        f.setNetworkActive(true);
        sources.fake->deliverAll();
        QVERIFY(flow.lookedUp() && !flow.identity());
        flow.resume();
        QCOMPARE(sources.fake->countCalls("registerIdentity"), size_t{1});
        QVERIFY(ProofTxid(sources.fake->proofs.back()) == txid);
        // Restart before Platform answered.
    }

    const size_t txs{wallet.getWalletTxs().size()};
    FakeSources sources;
    sources.script = [](FakePlatformClient& client) {
        platform::Result<platform::Identity> consumed;
        consumed.status = {platform::StatusKind::CONSENSUS, 10504, "asset lock outpoint already consumed"};
        client.identities_by_pubkey_hash = {NotFound(), Found(9, 1'000)};
        client.registered_identities = {consumed};
    };
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    IdentityFlow flow{service, confirmations.get(), InstantProof};
    sources.fake->deliverAll();
    QVERIFY(flow.funding() && flow.funding()->txid == txid);
    flow.resume();
    QCOMPARE(sources.fake->countCalls("registerIdentity"), size_t{1});
    QVERIFY(ProofTxid(sources.fake->proofs.back()) == txid);
    QCOMPARE(confirmations.asked.size(), size_t{1});
    QCOMPARE(wallet.getWalletTxs().size(), txs);

    sources.fake->deliverAll();
    QCOMPARE(sources.fake->countCalls("getIdentityByPublicKeyHash"), size_t{2});
    QVERIFY(flow.identity() && flow.identity()->id[0] == 9);
    QVERIFY(!flow.funding());
}

//! Each top-up funds an asset lock paying a top-up key not used before,
//! adds it to the identity the wallet found, and shows the new balance.
void PlatformTests::topUpUsesAFreshFundingKey()
{
    FundedFixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    FakeSources sources;
    sources.script = [](FakePlatformClient& client) {
        client.identities_by_pubkey_hash = {Found(5, 100)};
        platform::Result<uint64_t> balance;
        balance.status.kind = platform::StatusKind::OK;
        balance.value = 1'000'000'100;
        client.top_ups = {balance, balance};
    };
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    Confirmations confirmations;
    IdentityFlow flow{service, confirmations.get(), InstantProof};
    sources.fake->deliverAll();
    QVERIFY(flow.identity());

    for (uint32_t index : {0U, 1U}) {
        flow.topUp(IdentityFlow::MIN_TOP_UP);
        const auto funding{flow.funding()};
        QVERIFY(funding && funding->top_up && funding->top_up->funding_index == index);
        QVERIFY(CreditScript(wallet, funding->txid) == FundingScript(wallet, wallet::TopupFundingKey{index}));
        QVERIFY(sources.fake->calls.back().method == "topUpIdentity");
        QVERIFY(sources.fake->calls.back().argument == HexStr(flow.identity()->id));
        QVERIFY(ProofTxid(sources.fake->proofs.back()) == funding->txid);
        sources.fake->deliverAll();
        QVERIFY(!flow.funding());
    }
    QCOMPARE(flow.identity()->balance, uint64_t{1'000'000'100});
    QCOMPARE(confirmations.asked.back().first, IdentityFlow::MIN_TOP_UP);

    // A funding payment abandoned while its write was dropped can't reach
    // Platform: Continue ends its record instead of sending it.
    flow.topUp(IdentityFlow::MIN_TOP_UP);
    const uint256 abandoned{Assert(flow.funding())->txid};
    f.setNetworkActive(false);
    QVERIFY(wallet.abandonTransaction(abandoned));
    f.setNetworkActive(true);
    sources.fake->deliverAll();
    QVERIFY(flow.funding());
    flow.resume();
    QVERIFY(!flow.funding());
    QCOMPARE(sources.fake->countCalls("topUpIdentity"), size_t{0});
    QVERIFY(!flow.error().isEmpty());
}

//! An encrypted wallet derives its MASTER key only unlocked: the tab does
//! not ask for the passphrase on its own but offers the lookup, and once
//! the identity is found it shows its balance and offers a top-up. A client
//! rebuilt while the passphrase dialog is open gets one lookup, shown as
//! running; later clients look it up again without the passphrase.
void PlatformTests::lockedWalletLooksUpOnUnlock()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    QVERIFY(f.wallet->EncryptWallet("test passphrase"));
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);
    int unlocks{0};
    QObject::connect(&f.wallet_model, &WalletModel::requireUnlock, [&] {
        ++unlocks;
        f.setNetworkActive(false);
        f.setNetworkActive(true);
        f.wallet_model.setWalletLocked(/*locked=*/false, "test passphrase");
    });
    FakeSources sources;
    sources.script = [](FakePlatformClient& client) { client.identities_by_pubkey_hash = {Found(3, 4'000'000)}; };
    PlatformPage page{sources.get()};
    f.attach(page);
    QCOMPARE(sources.fake->countCalls("getIdentityByPublicKeyHash"), size_t{0});
    QCOMPARE(unlocks, 0);
    QVERIFY(VisibleButton(page, "dashPayCreateIdentity") == nullptr);
    QVERIFY(!page.findChild<QLabel*>("dashPayIdentity")->isVisibleTo(&page));

    QPushButton* look_up{VisibleButton(page, "dashPayLookUp")};
    QVERIFY(look_up != nullptr);
    look_up->click();
    QCOMPARE(unlocks, 1);
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);
    QCOMPARE(sources.made.size(), size_t{2});
    QCOMPARE(sources.fake->countCalls("getIdentityByPublicKeyHash"), size_t{1});
    QVERIFY(VisibleButton(page, "dashPayLookUp") == nullptr);
    sources.fake->deliverAll();
    QVERIFY(page.findChild<QLabel*>("dashPayIdentity")->isVisibleTo(&page));
    QVERIFY(VisibleButton(page, "dashPayTopUp") != nullptr);
    QVERIFY(VisibleButton(page, "dashPayLookUp") == nullptr);
    QVERIFY(VisibleButton(page, "dashPayCreateIdentity") == nullptr);

    f.setNetworkActive(false);
    f.setNetworkActive(true);
    QCOMPARE(sources.made.size(), size_t{3});
    QCOMPARE(sources.fake->countCalls("getIdentityByPublicKeyHash"), size_t{1});
    QCOMPARE(unlocks, 1);
}

//! A wallet without an identity registers its username in one go, under
//! one passphrase: a free label creates the identity first (funded after the
//! same confirmation) and then the name, signed with the identity's HIGH
//! key. A label DPNS rejects and a premium one are refused before anything
//! is looked up or funded.
void PlatformTests::usernameCreatesTheIdentityFirst()
{
    FundedFixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    QVERIFY(f.wallet->EncryptWallet("test passphrase"));
    int unlocks{0};
    QObject::connect(&f.wallet_model, &WalletModel::requireUnlock, [&] {
        ++unlocks;
        f.wallet_model.setWalletLocked(/*locked=*/false, "test passphrase");
    });
    FakeSources sources;
    sources.script = [](FakePlatformClient& client) {
        client.identities_by_pubkey_hash = {NotFound()};
        client.resolved_names = {Resolved(std::nullopt)};
        auto created{Found(7, 2'999'000'000)};
        created.value.public_keys = RegisteredKeys();
        client.registered_identities = {created};
        client.registered_names = {Resolved(1)};
    };
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    Confirmations confirmations;
    IdentityFlow identity_flow{service, confirmations.get(), InstantProof};
    UsernameFlow flow{service, identity_flow};
    identity_flow.lookUp();
    sources.fake->deliverAll();
    QVERIFY(identity_flow.lookedUp() && !identity_flow.identity());
    QCOMPARE(unlocks, 1);

    const size_t txs{wallet.getWalletTxs().size()};
    for (const char* label : {"a", "-alice2-", "alice"}) {
        flow.registerName(label);
        QVERIFY2(flow.step() == UsernameFlow::Step::IDLE, label);
        QVERIFY2(!flow.error().isEmpty(), label);
    }
    QCOMPARE(sources.fake->countCalls("resolveName"), size_t{0});
    QVERIFY(confirmations.asked.empty());
    QCOMPARE(wallet.getWalletTxs().size(), txs);
    QCOMPARE(unlocks, 1);

    flow.registerName("alice2026");
    QVERIFY(sources.fake->calls.back().method == "resolveName");
    QVERIFY(sources.fake->calls.back().argument == "alice2026");
    sources.fake->deliverAll();
    QCOMPARE(confirmations.asked.size(), size_t{1});
    QCOMPARE(confirmations.asked.back().first, IdentityFlow::REGISTRATION_AMOUNT);
    QCOMPARE(sources.fake->countCalls("registerIdentity"), size_t{1});
    QCOMPARE(sources.fake->countCalls("registerName"), size_t{1});
    QVERIFY(sources.fake->calls.back().argument == "alice2026");
    QVERIFY(sources.fake->calls.back().identity_index == 0U);
    QVERIFY(sources.fake->name_signers.back().first == identity_flow.identity()->id);
    QCOMPARE(sources.fake->name_signers.back().second, uint32_t{2});
    QVERIFY(flow.username() == "alice2026");
    QVERIFY(flow.error().isEmpty());
    QCOMPARE(unlocks, 2);
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);
}

//! An identity without a name registers one directly, with no asset lock:
//! not with a disabled key or a key of another purpose, nor a label
//! someone else holds. A label that resolves to the identity is its own.
//! A rebuild that drops the lookup of a label looks it up again. The tab
//! header shows the name; registering is offered only once the identity's
//! names were read and it has none.
void PlatformTests::usernameForAnExistingIdentity()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    FakeSources sources;
    int clients{0};
    sources.script = [&clients](FakePlatformClient& client) {
        auto found{Found(5, 1'000'000)};
        found.value.public_keys = {Key(0, Purpose::AUTHENTICATION, Level::MASTER),
                                   Key(1, Purpose::AUTHENTICATION, Level::CRITICAL),
                                   Key(2, Purpose::AUTHENTICATION, Level::HIGH, /*disabled_at=*/1),
                                   Key(3, Purpose::ENCRYPTION, Level::HIGH)};
        // The first session's identity has no usable HIGH key; later ones added one.
        if (clients++ == 0) {
            client.resolved_names = {Resolved(std::nullopt), Resolved(5)};
        } else {
            found.value.public_keys.push_back(Key(6, Purpose::AUTHENTICATION, Level::HIGH));
            client.resolved_names = {Resolved(8), Resolved(std::nullopt)};
            client.registered_names = {Resolved(1)};
        }
        client.identities_by_pubkey_hash = {found};
        client.names = {Names({})};
    };
    PlatformPage page{sources.get()};
    f.attach(page);
    const auto* header{page.findChild<QLabel*>("dashPayUsername")};
    auto* input{page.findChild<QLineEdit*>("dashPayUsernameInput")};
    auto* submit{page.findChild<QPushButton*>("dashPayRegisterUsername")};
    sources.fake->deliverNext();
    QVERIFY(page.identityFlow()->identity());
    QVERIFY(!page.usernameFlow()->namesRead());
    QVERIFY(!submit->isVisibleTo(&page));
    sources.fake->deliverAll();
    QVERIFY(!header->isVisibleTo(&page));
    QVERIFY(input->isVisibleTo(&page) && submit->isVisibleTo(&page));
    const size_t txs{wallet.getWalletTxs().size()};

    input->setText("bob-the-builder-9");
    submit->click();
    sources.fake->deliverAll();
    QCOMPARE(sources.fake->countCalls("resolveName"), size_t{1});
    QCOMPARE(sources.fake->countCalls("registerName"), size_t{0});
    QVERIFY(page.findChild<QLabel*>("dashPayUsernameStatus")->isVisibleTo(&page));

    input->setText("mine-already-2");
    submit->click();
    sources.fake->deliverAll();
    QCOMPARE(sources.fake->countCalls("registerName"), size_t{0});
    QCOMPARE(header->text(), QString{"mine-already-2"});
    QVERIFY(header->isVisibleTo(&page) && !submit->isVisibleTo(&page));

    // The next session's identity has a usable key and, as read, no name.
    f.setNetworkActive(false);
    f.setNetworkActive(true);
    sources.fake->deliverAll();
    QVERIFY(submit->isVisibleTo(&page) && !header->isVisibleTo(&page));
    input->setText("taken-name-2");
    submit->click();
    // A rebuild drops the read: the label is resolved again on the new client.
    sources.evonodes = {Ipv4Evonode2()};
    f.listRead();
    QCOMPARE(sources.made.size(), size_t{3});
    sources.fake->deliverAll();
    QCOMPARE(sources.fake->countCalls("resolveName"), size_t{1});
    QVERIFY(std::any_of(sources.fake->calls.begin(), sources.fake->calls.end(), [](const auto& call) {
        return call.method == "resolveName" && call.argument == "taken-name-2";
    }));
    QCOMPARE(sources.fake->countCalls("registerName"), size_t{0});
    QVERIFY(submit->isVisibleTo(&page));

    input->setText("bob-the-builder-9");
    submit->click();
    sources.fake->deliverAll();
    QCOMPARE(sources.fake->countCalls("registerName"), size_t{1});
    QVERIFY(sources.fake->calls.back().argument == "bob-the-builder-9");
    QVERIFY(sources.fake->name_signers.back().first == page.identityFlow()->identity()->id);
    QCOMPARE(sources.fake->name_signers.back().second, uint32_t{6});
    QCOMPARE(wallet.getWalletTxs().size(), txs);
    QVERIFY(header->isVisibleTo(&page));
    QCOMPARE(header->text(), QString{"bob-the-builder-9"});
    QVERIFY(!input->isVisibleTo(&page) && !submit->isVisibleTo(&page));
}

//! One write at a time per wallet, across the three flows: no top-up starts
//! while a username write is in flight, no username is sent while another
//! write is or a contact is being added, and no contact request while a
//! username is being registered. A username write the client dropped is not
//! sent again; the identity's names, read again once the identity is found,
//! show that it landed.
void PlatformTests::droppedUsernameWriteIsReadBack()
{
    FundedFixture f{m_node};
    QVERIFY(f.models.ok);
    FakeSources sources;
    int clients{0};
    sources.script = [&clients](FakePlatformClient& client) {
        auto found{Found(5, 1'000'000'000)};
        found.value.public_keys = RegisteredKeys();
        client.identities_by_pubkey_hash = {found};
        client.names = {Names(clients++ == 0 ? std::vector<std::string>{} : std::vector<std::string>{"carol2026"})};
        client.resolved_names = {Resolved(std::nullopt), Resolved(std::nullopt), Resolved(std::nullopt), Resolved(9)};
        auto contact{Found(9, 0)};
        contact.value.public_keys = {Key(1, Purpose::DECRYPTION, Level::MEDIUM)};
        client.identities = {contact};
        client.sent_requests = {Requests({})};
        client.received_requests = {Requests({})};
    };
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    Confirmations confirmations;
    IdentityFlow identity_flow{service, confirmations.get(), InstantProof};
    UsernameFlow flow{service, identity_flow};
    ContactFlow contacts{service, identity_flow};
    sources.fake->deliverAll();
    QVERIFY(flow.namesRead() && !flow.username());
    const auto names_sent = [&](const std::string& label) {
        const auto& calls{sources.fake->calls};
        return std::count_if(calls.begin(), calls.end(), [&](const FakePlatformClient::Call& call) {
            return call.method == "registerName" && call.argument == label;
        });
    };

    flow.registerName("carol2026");
    sources.fake->deliverNext();
    QVERIFY(flow.step() == UsernameFlow::Step::REGISTERING);
    identity_flow.topUp(IdentityFlow::MIN_TOP_UP);
    QVERIFY(confirmations.asked.empty());
    QVERIFY(!identity_flow.funding());

    f.setNetworkActive(false);
    QVERIFY(flow.step() == UsernameFlow::Step::IDLE);
    f.setNetworkActive(true);
    sources.fake->deliverAll();
    QVERIFY(flow.username() == "carol2026");
    QCOMPARE(names_sent("carol2026"), 0);
    QCOMPARE(sources.fake->countCalls("namesOfIdentity"), size_t{1});

    // Another write began while the label was resolved.
    flow.registerName("dave2026");
    bool written{false};
    PendingWrite(service, f.wallet_model.wallet(), written);
    sources.fake->deliverNext();
    QVERIFY(flow.step() == UsernameFlow::Step::IDLE && !flow.error().isEmpty());
    sources.fake->deliverAll();
    QCOMPARE(names_sent("dave2026"), 0);

    // So did a top-up.
    flow.registerName("erin2026");
    identity_flow.topUp(IdentityFlow::MIN_TOP_UP);
    QVERIFY(identity_flow.step() == IdentityFlow::Step::SENDING);
    sources.fake->deliverNext();
    QVERIFY(flow.step() == UsernameFlow::Step::IDLE && !flow.error().isEmpty());
    sources.fake->deliverAll();
    QCOMPARE(names_sent("erin2026"), 0);

    // A free label resolved while a contact is being added.
    flow.registerName("frank2026");
    contacts.add("grace2026");
    sources.fake->deliverNext();
    QVERIFY(flow.step() == UsernameFlow::Step::IDLE && !flow.error().isEmpty());
    QCOMPARE(names_sent("frank2026"), 0);
    // That contact found while a username is being checked.
    sources.fake->deliverNext();
    QVERIFY(contacts.step() == ContactFlow::Step::FETCHING);
    flow.registerName("heidi2026");
    sources.fake->deliverNext();
    QVERIFY(contacts.step() == ContactFlow::Step::IDLE && !contacts.error().isEmpty());
    QCOMPARE(sources.fake->countCalls("sendContactRequest"), size_t{0});
}

//! Adding a contact by username sends one request from the identity's first
//! enabled ENCRYPTION key, signed with its HIGH key, to the contact's enabled
//! DECRYPTION key, preferred to an ENCRYPTION key. Our receiving chain
//! towards them is imported first, and the accountReference is over that
//! chain. A label DPNS rejects, our own name, a name nobody holds, a contact
//! with no key to encrypt to and one already asked send nothing. A rebuild
//! that drops the lookup of the name or of the contact makes it again. A
//! request the client dropped is not sent again: our requests, read again,
//! show that it landed.
void PlatformTests::contactRequestUsesTheIdentityKeys()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    auto ours{Found(5, 1'000'000)};
    ours.value.public_keys = {Key(0, Purpose::AUTHENTICATION, Level::MASTER), Key(2, Purpose::AUTHENTICATION, Level::HIGH),
                              Key(3, Purpose::ENCRYPTION, Level::MEDIUM, /*disabled_at=*/1),
                              Key(7, Purpose::ENCRYPTION, Level::MEDIUM)};
    auto keyless{Found(8, 0)};
    keyless.value.public_keys = {Key(0, Purpose::AUTHENTICATION, Level::MASTER),
                                 Key(1, Purpose::DECRYPTION, Level::MEDIUM, /*disabled_at=*/1)};
    auto bob{Found(9, 0)};
    bob.value.public_keys = {Key(1, Purpose::ENCRYPTION, Level::MEDIUM),
                             Key(2, Purpose::DECRYPTION, Level::MEDIUM, /*disabled_at=*/1),
                             Key(3, Purpose::DECRYPTION, Level::MEDIUM)};
    auto carol{bob};
    carol.value.id.fill(10);
    FakeSources sources;
    int clients{0};
    sources.script = [&](FakePlatformClient& client) {
        switch (clients++) {
        case 0:
            client.identities_by_pubkey_hash = {ours};
            client.sent_requests = {Requests({}), Requests({Request(5, 9, 1)})};
            client.received_requests = {Requests({})};
            client.resolved_names = {Resolved(5), Resolved(std::nullopt), Resolved(8), Resolved(9), Resolved(9)};
            client.identities = {keyless, bob};
            client.sent_contact_requests = {Resolved(1)};
            break;
        case 1: // rebuilt while carol's name resolved
            client.resolved_names = {Resolved(10)};
            break;
        case 2: // rebuilt while carol's identity was read
            client.identities = {carol};
            break;
        default:
            client.identities_by_pubkey_hash = {ours};
            client.sent_requests = {Requests({Request(5, 9, 1), Request(5, 10, 2)})};
            client.received_requests = {Requests({})};
        }
    };
    PlatformService service{f.wallet_model, f.models.client, sources.get()};
    Confirmations confirmations;
    IdentityFlow identity_flow{service, confirmations.get()};
    ContactFlow flow{service, identity_flow};
    sources.fake->deliverAll();
    QVERIFY(flow.swept() && flow.contacts().empty());

    for (const char* name : {"-x-", "me2026", "nobody2026", "keyless2026"}) {
        flow.add(name);
        sources.fake->deliverAll();
        QVERIFY2(flow.step() == ContactFlow::Step::IDLE && !flow.error().isEmpty(), name);
    }
    QCOMPARE(sources.fake->countCalls("resolveName"), size_t{3});
    QCOMPARE(sources.fake->countCalls("getIdentity"), size_t{1});
    QCOMPARE(sources.fake->countCalls("sendContactRequest"), size_t{0});

    const size_t chains{f.wallet->GetAllScriptPubKeyMans().size()};
    flow.add("bob2026");
    sources.fake->deliverNext();
    sources.fake->deliverNext();
    QCOMPARE(sources.fake->countCalls("sendContactRequest"), size_t{1});
    QVERIFY(sources.fake->calls.back().identity_index == 0U);
    const platform::ContactRequestInput input{sources.fake->contact_inputs.back()};
    QVERIFY(input.sender == ours.value.id);
    QVERIFY(input.recipient == bob.value.id);
    QCOMPARE(input.sender_key_index, uint32_t{7});
    QCOMPARE(input.recipient_key_index, uint32_t{3});
    QCOMPARE(input.signing_key_id, uint32_t{2});
    // The chain the request carries was imported: deriving it again adds none.
    QCOMPARE(f.wallet->GetAllScriptPubKeyMans().size(), chains + 1);
    const auto chain{
        wallet.ensureFriendshipReceivingKeychain({0, uint256{ours.value.id}, uint256{bob.value.id}, GetTime()})};
    QVERIFY(chain);
    QCOMPARE(f.wallet->GetAllScriptPubKeyMans().size(), chains + 1);
    CTxDestination first;
    QVERIFY(wallet::DeriveFriendshipPaymentDestination(chain.value, 0, first));
    QVERIFY(wallet.isSpendable(first));
    wallet::CompactXpub compact;
    QVERIFY(wallet::CompactXpubBytes(chain.value, compact));
    const auto reference{wallet.platformAccountReference({0, 7}, compact)};
    QVERIFY(reference);
    QCOMPARE(input.account_reference, reference.value);

    sources.fake->deliverAll();
    QVERIFY(flow.error().isEmpty());
    QCOMPARE(flow.contacts().size(), size_t{1});
    QVERIFY(flow.contacts().front().id == bob.value.id);
    QVERIFY(flow.contacts().front().state == ContactFlow::State::OUTGOING);

    flow.add("bob2026");
    sources.fake->deliverAll();
    QVERIFY(!flow.error().isEmpty());
    QCOMPARE(sources.fake->countCalls("getIdentity"), size_t{2});

    flow.add("carol2026");
    sources.evonodes = {Ipv4Evonode2()};
    f.listRead();
    QCOMPARE(sources.fake->countCalls("resolveName"), size_t{1});
    QVERIFY(sources.fake->calls.back().argument == "carol2026");
    sources.fake->deliverNext();
    sources.evonodes = {Ipv4Evonode()};
    f.listRead();
    QCOMPARE(sources.made.size(), size_t{3});
    QCOMPARE(sources.fake->countCalls("getIdentity"), size_t{1});
    QVERIFY(sources.fake->calls.back().argument == HexStr(carol.value.id));
    sources.fake->deliverNext();
    QCOMPARE(sources.fake->countCalls("sendContactRequest"), size_t{1});
    QVERIFY(sources.fake->contact_inputs.back().recipient == carol.value.id);
    f.setNetworkActive(false);
    QVERIFY(flow.step() == ContactFlow::Step::IDLE);
    f.setNetworkActive(true);
    sources.fake->deliverAll();
    QCOMPARE(sources.made.size(), size_t{4});
    QCOMPARE(sources.fake->countCalls("sendContactRequest"), size_t{0});
    QCOMPARE(flow.contacts().size(), size_t{2});
    QVERIFY(flow.contacts().back().id == carol.value.id);
}

//! The contacts are what the requests on Platform say, newest per
//! counterparty: a request each way is a contact, theirs alone waits for
//! Accept and ours alone for them. Accepting reads their newest request and
//! takes only one carrying a valid payment key, sent from an enabled
//! ENCRYPTION or AUTHENTICATION key of theirs to one of our keys; our
//! request goes back to their DECRYPTION key, and they are then a contact.
void PlatformTests::contactsAreTheNewestRequests()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    CKey dave_key, erin_key, frank_key, grace_key, heidi_key;
    for (CKey* key : {&dave_key, &erin_key, &frank_key, &grace_key, &heidi_key}) {
        key->MakeNewKey(/*fCompressed=*/true);
    }
    CExtKey chain;
    const std::array<std::byte, 32> seed{};
    chain.SetSeed(seed);
    const CPubKey valid{chain.Neuter().pubkey};
    std::vector<uint8_t> invalid(CPubKey::COMPRESSED_SIZE, 0xff);
    invalid[0] = 0x02;
    auto ours{Found(5, 1'000'000)};
    ours.value.public_keys = RegisteredKeys();
    // Bob (0x0b) answered ours; carol (0x0c) was asked twice and didn't.
    // Dave's newest request carries a valid payment key, erin's doesn't;
    // frank sent his from a key since disabled, grace from an AUTHENTICATION
    // key, and heidi to a key we don't have.
    const auto sent{Requests({Request(5, 0x0b, 1), Request(5, 0x0c, 5), Request(5, 0x0c, 9)})};
    auto with_dave{sent};
    with_dave.value.push_back(Request(5, 0x0d, 10));
    auto with_grace{with_dave};
    with_grace.value.push_back(Request(5, 0x10, 11));
    FakeSources sources;
    sources.script = [&](FakePlatformClient& client) {
        const auto received{Requests(
            {Request(0x0b, 5, 2), IncomingRequest(wallet, 0x0d, 5, dave_key, 2, invalid),
             IncomingRequest(wallet, 0x0d, 5, dave_key, 8, valid), IncomingRequest(wallet, 0x0e, 5, erin_key, 3, valid),
             IncomingRequest(wallet, 0x0e, 5, erin_key, 7, invalid),
             IncomingRequest(wallet, 0x0f, 5, frank_key, 4, valid), IncomingRequest(wallet, 0x10, 5, grace_key, 4, valid),
             IncomingRequest(wallet, 0x11, 5, heidi_key, 4, valid, /*our_key=*/9)})};
        client.identities_by_pubkey_hash = {ours};
        client.names = {Names({"me2026"}), Names({"dave2026"})};
        client.sent_requests = {sent, with_dave, with_grace, with_grace};
        client.received_requests = {received, received};
        client.identities = {Peer(0x0d, dave_key, Purpose::ENCRYPTION), Peer(0x0e, erin_key, Purpose::ENCRYPTION),
                             Peer(0x0f, frank_key, Purpose::ENCRYPTION, /*disabled_at=*/1),
                             Peer(0x10, grace_key, Purpose::AUTHENTICATION), Peer(0x11, heidi_key, Purpose::ENCRYPTION)};
        client.sent_contact_requests = {Resolved(1), Resolved(2)};
    };
    PlatformPage page{sources.get()};
    f.attach(page);
    sources.fake->deliverAll();
    const ContactFlow& flow{*page.contactFlow()};
    using State = ContactFlow::State;
    using Listed = std::vector<std::pair<uint8_t, State>>;
    const auto contacts = [&] {
        Listed contacts;
        for (const auto& contact : flow.contacts()) {
            contacts.emplace_back(contact.id[0], contact.state);
        }
        return contacts;
    };
    QVERIFY(contacts() == (Listed{{0x0d, State::INCOMING},
                                  {0x0e, State::INCOMING},
                                  {0x0f, State::INCOMING},
                                  {0x10, State::INCOMING},
                                  {0x11, State::INCOMING},
                                  {0x0b, State::ESTABLISHED},
                                  {0x0c, State::OUTGOING}}));
    QVERIFY(flow.contacts().front().username == "dave2026");
    QVERIFY(page.findChild<QTableWidget*>("dashPayContacts")->isVisibleTo(&page));
    const auto accept_button = [&](uint8_t id) -> QPushButton* {
        QPushButton* button{ContactButton(page, id)};
        return button && button->objectName() == "dashPayAcceptContact" ? button : nullptr;
    };
    for (uint8_t id : {0x0d, 0x0e, 0x0f, 0x10, 0x11}) {
        QVERIFY(accept_button(id));
    }
    QVERIFY(!accept_button(0x0b) && !accept_button(0x0c));

    const size_t chains{f.wallet->GetAllScriptPubKeyMans().size()};
    for (uint8_t id : {0x0d, 0x0e, 0x0f, 0x10, 0x11}) {
        accept_button(id)->click();
        sources.fake->deliverAll();
        const bool accepted{id == 0x0d || id == 0x10};
        QVERIFY(flow.error().isEmpty() == accepted);
        QVERIFY(page.findChild<QLabel*>("dashPayContactsStatus")->isVisibleTo(&page) != accepted);
    }
    QCOMPARE(sources.fake->countCalls("getIdentity"), size_t{5});
    QCOMPARE(sources.fake->contact_inputs.size(), size_t{2});
    for (const platform::ContactRequestInput& input : sources.fake->contact_inputs) {
        QCOMPARE(input.recipient_key_index, uint32_t{2});
        QCOMPARE(input.sender_key_index, uint32_t{4});
        QCOMPARE(input.signing_key_id, uint32_t{2});
    }
    QCOMPARE(sources.fake->contact_inputs[0].recipient[0], uint8_t{0x0d});
    QCOMPARE(sources.fake->contact_inputs[1].recipient[0], uint8_t{0x10});
    QCOMPARE(f.wallet->GetAllScriptPubKeyMans().size(), chains + 2);
    QVERIFY(contacts() == (Listed{{0x0e, State::INCOMING},
                                  {0x0f, State::INCOMING},
                                  {0x11, State::INCOMING},
                                  {0x0b, State::ESTABLISHED},
                                  {0x0d, State::ESTABLISHED},
                                  {0x10, State::ESTABLISHED},
                                  {0x0c, State::OUTGOING}}));

    VisibleButton(page, "dashPayRefreshContacts")->click();
    sources.fake->deliverAll();
    QCOMPARE(sources.fake->countCalls("receivedRequests"), size_t{2});
    QCOMPARE(flow.contacts().size(), size_t{7});
}

//! Pay on a contact hands the Send tab the next address of the payment chain
//! their newest request carries, counted as used once, in the wallet, before
//! it is shown. Ten addresses past the last one paid, where their wallet
//! stops watching, the lowest one not paid yet is used again. A newer request
//! with another chain starts that chain at its first address.
void PlatformTests::payingAContactUsesEachAddressOnce()
{
    Fixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    CKey bob_key, dave_key;
    bob_key.MakeNewKey(/*fCompressed=*/true);
    dave_key.MakeNewKey(/*fCompressed=*/true);
    CExtKey root, rotated;
    const std::array<std::byte, 32> seed{};
    root.SetSeed(seed);
    QVERIFY(root.Derive(rotated, 1));
    const auto chain{PaymentChain(root.Neuter().pubkey)};
    const auto new_chain{PaymentChain(rotated.Neuter().pubkey)};
    auto ours{Found(5, 1'000'000)};
    ours.value.public_keys = RegisteredKeys();
    FakeSources sources;
    sources.script = [&](FakePlatformClient& client) {
        // Bob (0x0b) and we asked each other, carol (0x0c) hasn't answered
        // and dave (0x0d) waits for us. Read again, bob's newest request
        // carries another chain.
        const auto bob{IncomingRequest(wallet, 0x0b, 5, bob_key, 2, chain.pubkey)};
        const auto dave{IncomingRequest(wallet, 0x0d, 5, dave_key, 2, chain.pubkey)};
        const auto sent{Requests({Request(5, 0x0b, 1), Request(5, 0x0c, 1)})};
        client.identities_by_pubkey_hash = {ours};
        client.names = {Names({"me2026"})};
        client.sent_requests = {sent, sent};
        client.received_requests = {Requests({bob, dave}),
                                    Requests({bob, IncomingRequest(wallet, 0x0b, 5, bob_key, 3, new_chain.pubkey), dave})};
        client.identities.assign(14, Peer(0x0b, bob_key, Purpose::ENCRYPTION));
    };
    const auto at = [](const wallet::FriendshipXpub& payee, uint32_t index) {
        return QString::fromStdString(EncodeDestination(PaymentAddress(payee, index)));
    };

    auto page{std::make_unique<PlatformPage>(sources.get())};
    f.attach(*page);
    sources.fake->deliverAll();
    QVERIFY(ContactButton(*page, 0x0b) && ContactButton(*page, 0x0d));
    QVERIFY(ContactButton(*page, 0x0d)->objectName() != "dashPayPayContact");
    QVERIFY(!ContactButton(*page, 0x0c));
    for (uint8_t id : {0x0c, 0x0d}) {
        platform::Identifier contact;
        contact.fill(id);
        page->contactFlow()->pay(contact);
        QVERIFY(page->contactFlow()->step() == ContactFlow::Step::IDLE);
    }
    QCOMPARE(sources.fake->countCalls("getIdentity"), size_t{0});
    QCOMPARE(PayContact(*page, sources, 0x0b), at(chain, 0));
    QCOMPARE(PayContact(*page, sources, 0x0b), at(chain, 1));
    QCOMPARE(sources.fake->countCalls("getIdentity"), size_t{2});

    // A restart resumes after the last address handed out.
    page = std::make_unique<PlatformPage>(sources.get());
    f.attach(*page);
    sources.fake->deliverAll();
    for (uint32_t index = 2; index < 10; ++index) {
        QCOMPARE(PayContact(*page, sources, 0x0b), at(chain, index));
    }
    QCOMPARE(PayContact(*page, sources, 0x0b), at(chain, 0));
    QCOMPARE(PayContact(*page, sources, 0x0b), at(chain, 0));

    VisibleButton(*page, "dashPayRefreshContacts")->click();
    sources.fake->deliverAll();
    QCOMPARE(PayContact(*page, sources, 0x0b), at(new_chain, 0));
    QCOMPARE(PayContact(*page, sources, 0x0b), at(new_chain, 1));

    // A locked wallet needs its passphrase to decrypt the chain: without it
    // no address is chosen, and none is skipped.
    QVERIFY(f.wallet->EncryptWallet("test passphrase"));
    bool give_passphrase{false};
    QObject::connect(&f.wallet_model, &WalletModel::requireUnlock, [&] {
        if (give_passphrase) f.wallet_model.setWalletLocked(/*locked=*/false, "test passphrase");
    });
    QCOMPARE(PayContact(*page, sources, 0x0b), QString{});
    QVERIFY(!page->contactFlow()->error().isEmpty());
    give_passphrase = true;
    QCOMPARE(PayContact(*page, sources, 0x0b), at(new_chain, 2));
    QCOMPARE(f.wallet_model.getEncryptionStatus(), WalletModel::Locked);
    QCOMPARE(sources.fake->countCalls("getIdentity"), size_t{14});

    // The wallet's view fills the address into its Send tab.
    WalletView view{&f.wallet_model, nullptr};
    QSignalSpy prepared{&view, &WalletView::contactPaymentPrepared};
    Q_EMIT view.findChild<PlatformPage*>()->payTo(at(chain, 3));
    QCOMPARE(prepared.size(), 1);
    view.gotoSendCoinsPage();
    const auto entries{view.findChildren<QValidatedLineEdit*>("payTo")};
    QVERIFY(std::count_if(entries.begin(), entries.end(), [&](const QValidatedLineEdit* entry) {
                return entry->text() == at(chain, 3) && view.currentWidget()->isAncestorOf(entry);
            }) == 1);
}

//! A payment to the contact's chain in the wallet's history, as another
//! wallet on its seed leaves after a rescan, is never paid at again: the next
//! address is the one after the last paid, and the ten-address window starts
//! there. At its end the lowest one not paid yet, even below it, is used
//! again.
void PlatformTests::paymentsFollowTheWalletHistory()
{
    FundedFixture f{m_node};
    QVERIFY(f.models.ok);
    interfaces::Wallet& wallet{f.wallet_model.wallet()};
    CKey bob_key;
    bob_key.MakeNewKey(/*fCompressed=*/true);
    CExtKey root;
    const std::array<std::byte, 32> seed{};
    root.SetSeed(seed);
    const auto chain{PaymentChain(root.Neuter().pubkey)};
    auto ours{Found(5, 1'000'000)};
    ours.value.public_keys = RegisteredKeys();
    FakeSources sources;
    sources.script = [&](FakePlatformClient& client) {
        client.identities_by_pubkey_hash = {ours};
        client.names = {Names({"me2026"})};
        client.sent_requests = {Requests({Request(5, 0x0b, 1)})};
        client.received_requests = {Requests({IncomingRequest(wallet, 0x0b, 5, bob_key, 2, chain.pubkey)})};
        client.identities.assign(14, Peer(0x0b, bob_key, Purpose::ENCRYPTION));
    };
    const auto at = [&](uint32_t index) {
        return QString::fromStdString(EncodeDestination(PaymentAddress(chain, index)));
    };

    SendTo(wallet, PaymentAddress(chain, 5));
    PlatformPage page{sources.get()};
    f.attach(page);
    sources.fake->deliverAll();
    for (uint32_t index = 6; index < 16; ++index) {
        QCOMPARE(PayContact(page, sources, 0x0b), at(index));
    }
    QCOMPARE(PayContact(page, sources, 0x0b), at(0));
    SendTo(wallet, PaymentAddress(chain, 0));
    QCOMPARE(PayContact(page, sources, 0x0b), at(1));
    // Paying the last address handed out moves the window past it.
    SendTo(wallet, PaymentAddress(chain, 15));
    QCOMPARE(PayContact(page, sources, 0x0b), at(16));

    // Another flow started while the contact was read: no address is chosen.
    QSignalSpy paid{&page, &PlatformPage::payTo};
    ContactButton(page, 0x0b)->click();
    page.usernameFlow()->registerName("carol2026");
    sources.fake->deliverNext();
    QVERIFY(page.contactFlow()->step() == ContactFlow::Step::IDLE && !page.contactFlow()->error().isEmpty());
    sources.fake->deliverAll();
    QCOMPARE(sources.fake->countCalls("getIdentity"), size_t{14});
    // A rebuild drops the read; the new client reads the contact again.
    ContactButton(page, 0x0b)->click();
    sources.evonodes = {Ipv4Evonode2()};
    f.listRead();
    sources.fake->deliverAll();
    QCOMPARE(sources.fake->countCalls("getIdentity"), size_t{1});
    QCOMPARE(paid.size(), 1);
    QCOMPARE(paid.front().front().toString(), at(17));
}
