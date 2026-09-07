// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/masternodemaintenancetests.h>

#include <bls/bls.h>
#include <chainparams.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <netaddress.h>
#include <netbase.h>
#include <qt/masternodedialogs.h>
#include <qt/masternodemodel.h>
#include <qt/masternodewidgets.h>
#include <qt/qvalidatedlineedit.h>
#include <script/standard.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <univalue.h>
#include <util/translation.h>

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTest>
#include <QTimer>

#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace {

class TestMnEntry final : public interfaces::MnEntry
{
public:
    explicit TestMnEntry(MnType type = MnType::Evo, bool legacy_operator_display = false, uint16_t operator_reward = 0) :
        interfaces::MnEntry(CDeterministicMNCPtr{}),
        m_type{type},
        m_owner{NewKeyID()},
        m_voting{NewKeyID()},
        m_payout{GetScriptForDestination(PKHash{NewKeyID()})},
        m_operator_payout{operator_reward ? GetScriptForDestination(PKHash{NewKeyID()}) : CScript{}},
        m_operator_reward{operator_reward},
        m_core_endpoint{strprintf("127.0.0.1:%d", Params().GetDefaultPort())}
    {
        m_hash.SetHex("01");
        m_primary_service = LookupNumeric("127.0.0.1", 19999);
        setOperatorKey(legacy_operator_display);
    }

    bool isBanned() const override { return m_banned; }
    CService getNetInfoPrimary() const override { return m_primary_service; }
    std::vector<CService> getPlatformHTTPSAddrs() const override { return {}; }
    MnType getType() const override { return m_type; }
    UniValue toJson() const override
    {
        UniValue result(UniValue::VOBJ);
        result.pushKV("collateralHash", uint256::ONE.ToString());
        result.pushKV("collateralIndex", 0);
        UniValue state(UniValue::VOBJ);
        state.pushKV("version", m_operator_version);
        state.pushKV("consecutivePayments", 1);
        state.pushKV("PoSeBanHeight", -1);
        state.pushKV("PoSeRevivedHeight", -1);
        state.pushKV("pubKeyOperator", m_operator_display);
        state.pushKV("platformNodeID", m_platform_node_id);
        if (!m_core_endpoint.empty() || !m_platform_p2p.empty() || !m_platform_https.empty()) {
            UniValue addresses(UniValue::VOBJ);
            addresses.pushKV("core_p2p", StringArray(m_core_endpoint));
            addresses.pushKV("platform_p2p", StringArray(m_platform_p2p));
            addresses.pushKV("platform_https", StringArray(m_platform_https));
            state.pushKV("addresses", addresses);
        }
        result.pushKV("state", state);
        return result;
    }
    const CKeyID& getKeyIdOwner() const override { return m_owner; }
    std::vector<CKeyID> getShareOwnerKeyIds() const override { return {}; }
    std::vector<CScript> getShareRefundScripts() const override { return {}; }
    const CKeyID& getKeyIdVoting() const override { return m_voting; }
    const COutPoint& getCollateralOutpoint() const override { return m_collateral; }
    const CScript& getScriptPayout() const override { return m_payout; }
    std::vector<CScript> getScriptPayouts() const override { return {m_payout}; }
    const CScript& getScriptOperatorPayout() const override { return m_operator_payout; }
    const int32_t& getLastPaidHeight() const override { return m_last_paid; }
    const int32_t& getPoSePenalty() const override { return m_penalty; }
    const int32_t& getRegisteredHeight() const override { return m_registered; }
    const uint16_t& getOperatorReward() const override { return m_operator_reward; }
    const uint256& getProTxHash() const override { return m_hash; }
    const CBLSSecretKey& operatorSecret() const { return m_operator_secret; }

    void mutate()
    {
        m_voting = NewKeyID();
        m_payout = GetScriptForDestination(PKHash{NewKeyID()});
        m_operator_payout = GetScriptForDestination(PKHash{NewKeyID()});
        m_operator_reward = 500;
        m_core_endpoint = "127.0.0.2:20001";
        m_platform_p2p = "127.0.0.2:26657";
        m_platform_https = "api.example.org:8443";
        m_platform_node_id = "2222222222222222222222222222222222222222";
        setOperatorKey(/*legacy_display=*/false);
    }

    void clearEndpoints()
    {
        m_primary_service = {};
        m_core_endpoint.clear();
        m_platform_p2p.clear();
        m_platform_https.clear();
    }

    void setPlatformEndpoints(std::string p2p, std::string https)
    {
        m_platform_p2p = std::move(p2p);
        m_platform_https = std::move(https);
    }

private:
    static CKeyID NewKeyID()
    {
        static unsigned int counter{1};
        CKeyID key_id;
        key_id.SetHex(strprintf("%040u", counter++));
        return key_id;
    }

    static UniValue StringArray(const std::string& value)
    {
        UniValue result(UniValue::VARR);
        if (!value.empty()) result.push_back(value);
        return result;
    }

    void setOperatorKey(bool legacy_display)
    {
        CBLSSecretKey key;
        key.MakeNewKey();
        m_operator_secret = key;
        const auto public_key{key.GetPublicKey()};
        m_operator_display = public_key.ToString(legacy_display);
        m_operator_version = legacy_display ? ProTxVersion::LegacyBLS : ProTxVersion::BasicBLS;
    }

    bool m_banned{false};
    MnType m_type;
    CKeyID m_owner;
    CKeyID m_voting;
    COutPoint m_collateral{uint256::ONE, 0};
    CScript m_payout;
    CScript m_operator_payout;
    int32_t m_last_paid{10};
    int32_t m_penalty{0};
    int32_t m_registered{1};
    uint16_t m_operator_reward{0};
    uint256 m_hash;
    uint16_t m_operator_version{ProTxVersion::BasicBLS};
    CBLSSecretKey m_operator_secret;
    CService m_primary_service;
    std::string m_operator_display;
    std::string m_core_endpoint;
    std::string m_platform_p2p{"127.0.0.1:26656"};
    std::string m_platform_https{"api.example.com:443"};
    std::string m_platform_node_id{"1111111111111111111111111111111111111111"};
};

} // anonymous namespace

void MasternodeMaintenanceTests::automaticFeeSource()
{
    FeeSourcePicker picker;
    picker.setAutomaticOption("Automatic (recommended)");
    QCOMPARE(picker.count(), 1);
    QCOMPARE(picker.currentText(), QString("Automatic (recommended)"));
    QVERIFY(picker.selectedAddress().isEmpty());
}

void MasternodeMaintenanceTests::operatorSecretValidation()
{
    OperatorSecretWidget widget;
    QVERIFY(!widget.isValid());

    CBLSSecretKey key;
    key.MakeNewKey();
    auto* edit{widget.findChild<QLineEdit*>()};
    QVERIFY(edit != nullptr);
    edit->setText(QString::fromStdString(key.ToString(/*specificLegacyScheme=*/false)));
    QVERIFY(widget.isValid());
    const auto parsed{widget.takeSecret()};
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->GetPublicKey().ToByteVector(false), key.GetPublicKey().ToByteVector(false));
    QVERIFY(edit->text().isEmpty());
    QVERIFY(!widget.isValid());

    edit->setText("not-a-secret");
    QVERIFY(!widget.takeSecret().has_value());
    QVERIFY(edit->text().isEmpty());
}

void MasternodeMaintenanceTests::operatorPublicKeyEncoding()
{
    auto source{std::make_shared<TestMnEntry>(MnType::Evo, /*legacy_operator_display=*/true)};
    MasternodeEntry entry{source, "collateral", 50};

    const auto raw{entry.operatorPubKeyBytes()};
    CBLSPublicKey public_key;
    public_key.SetBytes(raw, /*specificLegacyScheme=*/false);
    QVERIFY(public_key.IsValid());
    QCOMPARE(entry.operatorPubKey(/*legacy_scheme=*/false),
             QString::fromStdString(public_key.ToString(/*specificLegacyScheme=*/false)));
    QCOMPARE(entry.operatorPubKey(/*legacy_scheme=*/true),
             QString::fromStdString(public_key.ToString(/*specificLegacyScheme=*/true)));
    QVERIFY(entry.operatorPubKey(/*legacy_scheme=*/false) != entry.operatorPubKey(/*legacy_scheme=*/true));
}

void MasternodeMaintenanceTests::actionRoleGating()
{
    using MasternodeMaintenance::actionAvailability;

    const auto no_wallet{actionAvailability(false, false)};
    QVERIFY(!no_wallet.update_service);
    QVERIFY(!no_wallet.update_registrar);
    QVERIFY(!no_wallet.revoke);

    const auto operator_only{actionAvailability(true, false)};
    QVERIFY(operator_only.update_service);
    QVERIFY(!operator_only.update_registrar);
    QVERIFY(operator_only.revoke);

    const auto owner{actionAvailability(true, true)};
    QVERIFY(owner.update_service);
    QVERIFY(owner.update_registrar);
    QVERIFY(owner.revoke);
}

void MasternodeMaintenanceTests::reconcileMutableFields()
{
    auto initial{std::make_shared<TestMnEntry>()};
    MasternodeEntryList first;
    first.push_back(std::make_shared<MasternodeEntry>(initial, "collateral", 50));

    MasternodeModel model;
    model.reconcile(std::move(first));
    const MasternodeEntry* entry{model.getEntryAt(model.index(0, 0))};
    QVERIFY(entry != nullptr);
    const QString initial_operator{entry->operatorPubKey()};
    const QString initial_voting{entry->votingAddress()};
    const QString initial_payout{entry->payoutAddress()};
    QCOMPARE(entry->coreP2PAddresses(), QString("127.0.0.1:%1").arg(Params().GetDefaultPort()));
    QCOMPARE(entry->platformNodeID(), QString("1111111111111111111111111111111111111111"));

    auto updated{std::make_shared<TestMnEntry>()};
    updated->mutate();
    MasternodeEntryList second;
    second.push_back(std::make_shared<MasternodeEntry>(updated, "collateral", 50));
    model.reconcile(std::move(second));

    entry = model.getEntryAt(model.index(0, 0));
    QVERIFY(entry != nullptr);
    QCOMPARE(entry->coreP2PAddresses(), QString("127.0.0.2:20001"));
    QCOMPARE(entry->platformP2PAddresses(), QString("127.0.0.2:26657"));
    QCOMPARE(entry->platformHTTPSAddresses(), QString("api.example.org:8443"));
    QCOMPARE(entry->platformNodeID(), QString("2222222222222222222222222222222222222222"));
    QVERIFY(entry->operatorPubKey() != initial_operator);
    QVERIFY(entry->votingAddress() != initial_voting);
    QVERIFY(entry->payoutAddress() != initial_payout);
    QVERIFY(!entry->operatorPayoutAddress().isEmpty());
    QCOMPARE(entry->operatorRewardPct(), uint16_t{500});

    auto inactive{std::make_shared<TestMnEntry>()};
    inactive->clearEndpoints();
    const MasternodeEntry inactive_entry{inactive, "collateral", 50};
    QVERIFY(inactive_entry.coreP2PAddresses().isEmpty());
    QVERIFY(inactive_entry.platformP2PAddresses().isEmpty());
    QVERIFY(inactive_entry.platformHTTPSAddresses().isEmpty());
    const QString inactive_html{inactive_entry.toHtml()};
    QVERIFY(!inactive_html.contains("Network Addresses"));
    QVERIFY(!inactive_html.contains("Platform P2P Addresses"));
    QVERIFY(!inactive_html.contains("Platform HTTPS Addresses"));
}

void MasternodeMaintenanceTests::serviceRequestConstruction()
{
    TestChain100Setup test;
    m_node.setContext(&test.m_node);

    const auto check_request = [this](MnType type, interfaces::ProviderTxCapabilities capabilities) {
        auto source{std::make_shared<TestMnEntry>(type, capabilities.version == ProTxVersion::LegacyBLS,
                                                  /*operator_reward=*/500)};
        MasternodeEntry entry{source, "collateral", 50};
        UpdateServiceDialog dialog(m_node, /*wallet_model=*/nullptr, entry, capabilities, /*parent=*/nullptr);
        QString error;
        const auto request{dialog.buildRequest(source->operatorSecret(), error)};
        QVERIFY2(request.has_value(), qPrintable(error));
        QCOMPARE(request->type, type);
        QCOMPARE(request->pro_tx_hash, source->getProTxHash());
        QCOMPARE(request->operator_key.GetPublicKey().ToByteVector(false),
                 source->operatorSecret().GetPublicKey().ToByteVector(false));
        QCOMPARE(request->net_info.core_p2p, std::vector<std::string>{entry.coreP2PAddresses().toStdString()});
        QVERIFY(request->operator_payout.has_value());
        QVERIFY(!request->fee_source.has_value());
        QVERIFY(request->submit);

        if (type == MnType::Regular) {
            QVERIFY(std::holds_alternative<std::monostate>(request->net_info.platform_p2p));
            QVERIFY(std::holds_alternative<std::monostate>(request->net_info.platform_https));
            QVERIFY(!request->platform_node_id.has_value());
        } else if (capabilities.extended_addresses) {
            QCOMPARE(std::get<std::vector<std::string>>(request->net_info.platform_p2p),
                     std::vector<std::string>{"127.0.0.1:26656"});
            QCOMPARE(std::get<std::vector<std::string>>(request->net_info.platform_https),
                     std::vector<std::string>{"api.example.com:443"});
            QVERIFY(request->platform_node_id.has_value());
        } else {
            QCOMPARE(std::get<uint16_t>(request->net_info.platform_p2p), uint16_t{26656});
            QCOMPARE(std::get<uint16_t>(request->net_info.platform_https), uint16_t{443});
            QVERIFY(request->platform_node_id.has_value());
        }

        if (type == MnType::Evo) {
            auto* const secret_edit{dialog.m_operator_key->findChild<QLineEdit*>()};
            QVERIFY(secret_edit != nullptr);
            secret_edit->setText(QString::fromStdString(source->operatorSecret().ToString(/*specificLegacyScheme=*/false)));
            QVERIFY(dialog.m_valid);

            dialog.m_platform_node_id_edit->setText(QString(40, QLatin1Char('0')));
            QVERIFY(!dialog.m_valid);
            error.clear();
            QVERIFY(!dialog.buildRequest(source->operatorSecret(), error).has_value());
            QVERIFY(error.contains("Platform node ID"));
        }
    };

    check_request(MnType::Regular, {ProTxVersion::BasicBLS, false});
    check_request(MnType::Evo, {ProTxVersion::BasicBLS, false});
    check_request(MnType::Evo, {ProTxVersion::ExtAddr, true});

    auto malformed_source{std::make_shared<TestMnEntry>(MnType::Evo)};
    malformed_source->setPlatformEndpoints("missing-port", "api.example.com:not-a-port");
    MasternodeEntry malformed_entry{malformed_source, "collateral", 50};
    UpdateServiceDialog fallback_dialog(m_node, /*wallet_model=*/nullptr, malformed_entry,
                                        interfaces::ProviderTxCapabilities{ProTxVersion::BasicBLS, false},
                                        /*parent=*/nullptr);
    QCOMPARE(fallback_dialog.m_platform_p2p_port_edit->value(), Params().GetDefaultPlatformP2PPort());
    QCOMPARE(fallback_dialog.m_platform_https_port_edit->value(), Params().GetDefaultPlatformHTTPPort());

    QString error;
    const auto fallback_request{fallback_dialog.buildRequest(malformed_source->operatorSecret(), error)};
    QVERIFY2(fallback_request.has_value(), qPrintable(error));
    QCOMPARE(std::get<uint16_t>(fallback_request->net_info.platform_p2p), Params().GetDefaultPlatformP2PPort());
    QCOMPARE(std::get<uint16_t>(fallback_request->net_info.platform_https), Params().GetDefaultPlatformHTTPPort());
}

void MasternodeMaintenanceTests::registrarRequestConstruction()
{
    TestChain100Setup test;
    m_node.setContext(&test.m_node);

    for (const interfaces::ProviderTxCapabilities capabilities :
         {interfaces::ProviderTxCapabilities{ProTxVersion::LegacyBLS, false},
          interfaces::ProviderTxCapabilities{ProTxVersion::ExtAddr, true}}) {
        auto source{std::make_shared<TestMnEntry>(MnType::Regular, capabilities.version == ProTxVersion::LegacyBLS)};
        MasternodeEntry entry{source, "collateral", 50};
        UpdateRegistrarDialog dialog(m_node, /*wallet_model=*/nullptr, entry, capabilities, /*parent=*/nullptr);
        QString error;

        const auto unchanged{dialog.buildRequest(error)};
        QVERIFY2(unchanged.has_value(), qPrintable(error));
        QVERIFY(!unchanged->operator_key.has_value());
        QVERIFY(!unchanged->voting_key.has_value());
        QVERIFY(!unchanged->payouts.has_value());
        QVERIFY(unchanged->submit);

        dialog.m_voting_edit->setText(QString::fromStdString(EncodeDestination(PKHash{uint160{}})));
        QVERIFY(!dialog.m_valid);
        error.clear();
        QVERIFY(!dialog.buildRequest(error).has_value());
        QVERIFY(error.contains("voting address"));

        CBLSSecretKey replacement;
        replacement.MakeNewKey();
        dialog.m_operator_pubkey_edit->setText(
            QString::fromStdString(replacement.GetPublicKey().ToString(capabilities.version == ProTxVersion::LegacyBLS)));
        CKeyID voting;
        voting.SetHex("42");
        dialog.m_voting_edit->setText(QString::fromStdString(EncodeDestination(PKHash{voting})));
        CKeyID payout;
        payout.SetHex("43");
        dialog.m_payout_edit->setText(QString::fromStdString(EncodeDestination(PKHash{payout})));

        error.clear();
        const auto changed{dialog.buildRequest(error)};
        QVERIFY2(changed.has_value(), qPrintable(error));
        QVERIFY(changed->operator_key.has_value());
        QCOMPARE(changed->operator_key->ToByteVector(false), replacement.GetPublicKey().ToByteVector(false));
        QCOMPARE(changed->voting_key, std::optional<CKeyID>{voting});
        QVERIFY(changed->payouts.has_value());
        QCOMPARE(changed->payouts->size(), size_t{1});
        QCOMPARE(changed->payouts->front().destination, CTxDestination{PKHash{payout}});
        QCOMPARE(changed->payouts->front().reward, interfaces::ProviderPayout::MAX_REWARD);
        QCOMPARE(changed->uses_extended_payouts, capabilities.version >= ProTxVersion::ExtAddr);
        QCOMPARE(changed->use_legacy_bls_scheme, capabilities.version == ProTxVersion::LegacyBLS);
    }
}

void MasternodeMaintenanceTests::revokeRequestConstruction()
{
    TestChain100Setup test;
    m_node.setContext(&test.m_node);
    auto source{std::make_shared<TestMnEntry>(MnType::Regular)};
    MasternodeEntry entry{source, "collateral", 50};
    RevokeDialog dialog(m_node, /*wallet_model=*/nullptr, entry, /*parent=*/nullptr);

    for (int reason = 0; reason <= 3; ++reason) {
        dialog.m_reason_combo->setCurrentIndex(reason);
        QString error;
        const auto request{dialog.buildRequest(source->operatorSecret(), error)};
        QVERIFY2(request.has_value(), qPrintable(error));
        QCOMPARE(request->pro_tx_hash, source->getProTxHash());
        QCOMPARE(request->reason, static_cast<uint16_t>(reason));
        QVERIFY(!request->fee_source.has_value());
        QVERIFY(request->submit);
    }
}

void MasternodeMaintenanceTests::dialogFieldGeometry()
{
#if defined(Q_OS_MACOS)
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping dialogFieldGeometry on macOS with the minimal platform due to QTBUG-49686");
        return;
    }
#endif
    TestChain100Setup test;
    m_node.setContext(&test.m_node);
    auto source{std::make_shared<TestMnEntry>(MnType::Regular)};
    MasternodeEntry entry{source, "collateral", 50};

    RevokeDialog revoke(m_node, /*wallet_model=*/nullptr, entry, /*parent=*/nullptr);
    revoke.show();
    QApplication::processEvents();
    const auto field_rect = [](QWidget* field, QWidget* dialog) {
        return QRect{field->mapTo(dialog, QPoint{}), field->size()};
    };
    const QRect reason_rect{field_rect(revoke.m_reason_combo, &revoke)};
    const QRect operator_rect{field_rect(revoke.m_operator_key, &revoke)};
    const QRect fee_rect{field_rect(revoke.m_fee_source, &revoke)};
    const auto fields_align = [](const QRect& expected, const QRect& actual) {
        // Native controls can extend a few pixels outside their layout item.
        return qAbs(expected.left() - actual.left()) <= 12 && qAbs(expected.right() - actual.right()) <= 12;
    };
    QVERIFY(reason_rect.width() > 400);
    QVERIFY(fields_align(reason_rect, operator_rect));
    QVERIFY(fields_align(reason_rect, fee_rect));

    UpdateRegistrarDialog registrar(m_node, /*wallet_model=*/nullptr, entry,
                                    interfaces::ProviderTxCapabilities{ProTxVersion::BasicBLS, false},
                                    /*parent=*/nullptr);
    registrar.show();
    QApplication::processEvents();
    const QRect public_key_rect{field_rect(registrar.m_operator_pubkey_edit, &registrar)};
    for (QWidget* field : {static_cast<QWidget*>(registrar.m_voting_edit), static_cast<QWidget*>(registrar.m_payout_edit),
                           static_cast<QWidget*>(registrar.m_fee_source)}) {
        const QRect rect{field_rect(field, &registrar)};
        QVERIFY(fields_align(public_key_rect, rect));
    }
}

void MasternodeMaintenanceTests::dialogLifecycleAndSubmissionStates()
{
#if defined(Q_OS_MACOS)
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping dialogLifecycleAndSubmissionStates on macOS with the minimal platform due to QTBUG-49686");
        return;
    }
#endif
    TestChain100Setup test;
    m_node.setContext(&test.m_node);
    auto source{std::make_shared<TestMnEntry>(MnType::Regular)};
    MasternodeEntry entry{source, "collateral", 50};
    UpdateRegistrarDialog dialog(m_node, /*wallet_model=*/nullptr, entry,
                                 interfaces::ProviderTxCapabilities{ProTxVersion::BasicBLS, false},
                                 /*parent=*/nullptr);
    QSignalSpy rejected_spy(&dialog, &QDialog::rejected);
    dialog.show();

    dialog.setBusy(true);
    dialog.reject();
    QCOMPARE(rejected_spy.count(), 0);
    QVERIFY(dialog.isVisible());
    dialog.setBusy(false);

    dialog.finishSubmission(interfaces::ProviderTxSubmission{MakeTransactionRef(CMutableTransaction{}), false});
    QVERIFY(dialog.m_status_label->isVisible());
    QVERIFY(dialog.m_status_label->text().contains("not sent", Qt::CaseInsensitive));
    QCOMPARE(rejected_spy.count(), 0);

    dialog.finishSubmission(interfaces::ProviderTxError{interfaces::ProviderTxErrorCode::INTERNAL_ERROR,
                                                        Untranslated("maintenance failed"),
                                                        {},
                                                        std::nullopt});
    QVERIFY(dialog.m_status_label->text().contains("maintenance failed", Qt::CaseInsensitive));

    // shutdown() can synchronously deliver a completed operation from the
    // destructor. It must release busy/unlock state without opening modal UI
    // or changing the dialog result during teardown.
    bool message_box_seen{false};
    QTimer::singleShot(0, [&message_box_seen] {
        for (QWidget* const widget : QApplication::topLevelWidgets()) {
            auto* const message_box{qobject_cast<QMessageBox*>(widget)};
            if (message_box == nullptr) continue;
            message_box_seen = true;
            message_box->accept();
        }
    });
    QSignalSpy accepted_spy(&dialog, &QDialog::accepted);
    dialog.m_destroying = true;
    dialog.setBusy(true);
    dialog.finishSubmission(interfaces::ProviderTxSubmission{MakeTransactionRef(CMutableTransaction{}), /*submitted=*/true});
    QApplication::processEvents();
    QVERIFY(!message_box_seen);
    QCOMPARE(accepted_spy.count(), 0);
    QVERIFY(!dialog.m_busy);
    QVERIFY(QApplication::overrideCursor() == nullptr);
}
