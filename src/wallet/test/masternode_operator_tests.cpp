// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls/bls.h>
#include <chain.h>
#include <clientversion.h>
#include <evo/netinfo.h>
#include <evo/providertx.h>
#include <evo/specialtx.h>
#include <evo/types.h>
#include <interfaces/wallet.h>
#include <streams.h>
#include <support/cleanse.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>
#include <wallet/bip39.h>
#include <wallet/hdchain.h>
#include <wallet/masternode_operator.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wallet {
namespace {

// The known-answer vectors are cross-checked against DashSync: the seed phrase
// and the expected legacy-serialized operator public key at m/9'/1'/3'/3'/0
// appear in the hardcoded ProRegTx payloads of
// dashsync-iOS/Example/Tests/DSProviderTransactionsTests.m
// (testCollateralProviderRegistrationTransaction and
// testNoCollateralProviderRegistrationTransaction).
const SecureString DASHSYNC_MNEMONIC{
    "enemy check owner stumble unaware debris suffer peanut good fabric bleak outside"};
const SecureString SECOND_MNEMONIC{
    "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"};
const SecureString MNEMONIC_PASSPHRASE{"Dash operator key test"};
const std::string DASHSYNC_OPERATOR_SECRET{"344e7bf67fddf1c2d2f629f2392ce2b4af99393cd8a4c70d4bbaef7aaf4c364b"};
const std::string DASHSYNC_MAINNET_OPERATOR_SECRET{"0461b8370ac28e7d7c453ab769d6edc97e9a79e400ae100bfbbf7ea9d834fd2c"};
const std::string DASHSYNC_OPERATOR_PUBLIC_KEY_LEGACY{
    "157b10706659e25eb362b5d902d809f9160b1688e201ee6e94b40f9b5062d7074683ef05a2d5efb7793c47059c878dfa"};

void SetupMnemonicWallet(CWallet& wallet, const SecureString& mnemonic = DASHSYNC_MNEMONIC,
                         const SecureString& passphrase = {})
{
    LOCK(wallet.cs_wallet);
    wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
    wallet.SetupDescriptorScriptPubKeyMans(mnemonic, passphrase);
}

void SetupLegacyMnemonicWallet(CWallet& wallet)
{
    CHDChain chain;
    BOOST_REQUIRE(chain.SetMnemonic(DASHSYNC_MNEMONIC, {}, /*fUpdateID=*/true));
    chain.AddAccount();
    LOCK(wallet.cs_wallet);
    BOOST_REQUIRE(wallet.GetOrCreateLegacyScriptPubKeyMan()->AddHDChainSingle(chain));
}

//! Independently derive the first `count` operator public keys the DashSync way.
std::vector<std::vector<unsigned char>> DashSyncPublicKeys(uint32_t count)
{
    SecureVector seed;
    CMnemonic::ToSeed(DASHSYNC_MNEMONIC, {}, seed);
    constexpr uint32_t hardened{0x80000000};
    const bls::ExtendedPrivateKey account{bls::ExtendedPrivateKey::FromSeed(bls::Bytes{seed.data(), seed.size()})
                                              .PrivateChild(hardened | MASTERNODE_OPERATOR_PURPOSE, /*fLegacy=*/true)
                                              .PrivateChild(hardened | 1, /*fLegacy=*/true)
                                              .PrivateChild(hardened | MASTERNODE_OPERATOR_PROVIDER_FEATURE,
                                                            /*fLegacy=*/true)
                                              .PrivateChild(hardened | MASTERNODE_OPERATOR_SUBFEATURE,
                                                            /*fLegacy=*/true)};
    memory_cleanse(seed.data(), seed.size());

    std::vector<std::vector<unsigned char>> public_keys;
    public_keys.reserve(count);
    for (uint32_t index{0}; index < count; ++index) {
        auto secret_bytes{account.PrivateChild(index, /*fLegacy=*/true).GetPrivateKey().SerializeToArray()};
        const CBLSSecretKey secret{Span<const unsigned char>{secret_bytes.data(), secret_bytes.size()}};
        memory_cleanse(secret_bytes.data(), secret_bytes.size());
        BOOST_REQUIRE(secret.IsValid());
        public_keys.emplace_back(secret.GetPublicKey().ToByteVector(/*specificLegacyScheme=*/false));
    }
    return public_keys;
}

using interfaces::MasternodeOperatorKeyStatus;

std::optional<MasternodeOperatorWatermark> ReadWatermarkRecord(CWallet& wallet)
{
    MasternodeOperatorWatermark watermark;
    WalletBatch batch{wallet.GetDatabase()};
    if (!batch.ReadMasternodeOperatorWatermark(watermark)) return std::nullopt;
    return watermark;
}

std::optional<MasternodeOperatorLookahead> ReadLookaheadRecord(CWallet& wallet)
{
    MasternodeOperatorLookahead lookahead;
    WalletBatch batch{wallet.GetDatabase()};
    if (!batch.ReadMasternodeOperatorLookahead(lookahead)) return std::nullopt;
    return lookahead;
}

MasternodeOperatorKeyStatus TopUpLookahead(CWallet& wallet)
{
    LOCK(wallet.cs_wallet);
    return wallet.TopUpMasternodeOperatorLookahead();
}

std::vector<unsigned char> RandomOperatorKeyBytes()
{
    CBLSSecretKey secret;
    secret.MakeNewKey();
    return secret.GetPublicKey().ToByteVector(/*specificLegacyScheme=*/false);
}

CBLSPublicKey ParseBasic(const std::vector<unsigned char>& bytes)
{
    CBLSPublicKey key;
    key.SetBytes(bytes, /*specificLegacyScheme=*/false);
    BOOST_REQUIRE(key.IsValid());
    return key;
}

CTransactionRef BuildProRegTx(const CBLSPublicKey& operator_key, bool legacy_encoding)
{
    CProRegTx payload;
    payload.nVersion = legacy_encoding ? ProTxVersion::LegacyBLS : ProTxVersion::BasicBLS;
    payload.netInfo = NetInfoInterface::MakeNetInfo(payload.nVersion);
    payload.pubKeyOperator.Set(operator_key, legacy_encoding);

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_PROVIDER_REGISTER;
    SetTxPayload(tx, payload);
    return MakeTransactionRef(tx);
}

CTransactionRef BuildProUpRegTx(const CBLSPublicKey& operator_key, bool legacy_encoding)
{
    CProUpRegTx payload;
    payload.nVersion = legacy_encoding ? ProTxVersion::LegacyBLS : ProTxVersion::BasicBLS;
    payload.pubKeyOperator.Set(operator_key, legacy_encoding);

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_PROVIDER_UPDATE_REGISTRAR;
    SetTxPayload(tx, payload);
    return MakeTransactionRef(tx);
}

struct MasternodeOperatorTestingSetup : public WalletTestingSetup {
    MasternodeOperatorTestingSetup() :
        WalletTestingSetup(CBaseChainParams::REGTEST)
    {
        gArgs.ForceSetArg("-keypool", "1");
    }

    ~MasternodeOperatorTestingSetup() { gArgs.ForceRemoveArg("keypool"); }

    std::shared_ptr<CWallet> MakeWallet(std::unique_ptr<WalletDatabase> database = CreateMockWalletDatabase())
    {
        auto wallet{std::make_shared<CWallet>(m_node.chain.get(), m_coinjoin_loader.get(), "", m_args,
                                              std::move(database))};
        BOOST_REQUIRE(wallet->LoadWallet() == DBErrors::LOAD_OK);
        return wallet;
    }

    std::shared_ptr<CWallet> MakeNamedWallet(const std::string& name, bool existing)
    {
        DatabaseOptions options;
        options.require_create = !existing;
        options.require_existing = existing;
        DatabaseStatus status;
        bilingual_str error;
        auto database{MakeWalletDatabase(name, options, status, error)};
        BOOST_REQUIRE_MESSAGE(database, error.original);
        auto wallet{
            std::make_shared<CWallet>(m_node.chain.get(), m_coinjoin_loader.get(), name, m_args, std::move(database))};
        BOOST_REQUIRE(wallet->LoadWallet() == DBErrors::LOAD_OK);
        return wallet;
    }

    std::unique_ptr<interfaces::Wallet> MakeWalletInterface(const std::shared_ptr<CWallet>& wallet)
    {
        return interfaces::MakeWallet(*Assert(m_wallet_loader->context()), wallet);
    }
};

//! Database whose writes can be toggled to fail, for fail-closed checks.
class ToggleFailBatch final : public DatabaseBatch
{
private:
    bool& m_write_success;
    bool ReadKey(CDataStream&&, CDataStream&) override { return false; }
    bool WriteKey(CDataStream&&, CDataStream&&, bool) override { return m_write_success; }
    bool EraseKey(CDataStream&&) override { return m_write_success; }
    bool HasKey(CDataStream&&) override { return false; }
    bool ErasePrefix(Span<const std::byte>) override { return m_write_success; }

public:
    explicit ToggleFailBatch(bool& write_success) : m_write_success(write_success) {}
    void Flush() override {}
    void Close() override {}
    bool StartCursor() override { return true; }
    bool ReadAtCursor(CDataStream&, CDataStream&, bool& complete) override
    {
        complete = true;
        return true;
    }
    void CloseCursor() override {}
    bool TxnBegin() override { return true; }
    bool TxnCommit() override { return true; }
    bool TxnAbort() override { return true; }
};

class ToggleFailDatabase final : public WalletDatabase
{
public:
    bool write_success{true};

    void Open() override {}
    void AddRef() override {}
    void RemoveRef() override {}
    bool Rewrite(const char*) override { return true; }
    bool Backup(const std::string&) const override { return true; }
    void Flush() override {}
    void Close() override {}
    bool PeriodicFlush() override { return true; }
    void IncrementUpdateCounter() override { ++nUpdateCounter; }
    void ReloadDbEnv() override {}
    std::string Filename() override { return "toggle-fail"; }
    std::string Format() override { return "toggle-fail"; }
    std::unique_ptr<DatabaseBatch> MakeBatch(bool) override { return std::make_unique<ToggleFailBatch>(write_success); }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(masternode_operator_tests, MasternodeOperatorTestingSetup)

BOOST_AUTO_TEST_CASE(watermark_known_answer)
{
    const auto expected{DashSyncPublicKeys(3)};
    auto wallet{MakeWallet()};
    SetupMnemonicWallet(*wallet);
    auto interface{MakeWalletInterface(wallet)};
    BOOST_REQUIRE(interface->hasMasternodeOperatorKeySource());

    auto first{interface->getNewMasternodeOperatorKey({})};
    BOOST_REQUIRE(first.status == MasternodeOperatorKeyStatus::SUCCESS);
    BOOST_CHECK_EQUAL(first.key.path, "m/9'/1'/3'/3'/0");
    BOOST_CHECK_EQUAL(HexStr(first.key.secret_key), DASHSYNC_OPERATOR_SECRET);
    BOOST_CHECK(first.key.public_key == expected[0]);
    const CBLSSecretKey first_secret{
        Span<const unsigned char>{first.key.secret_key.data(), first.key.secret_key.size()}};
    BOOST_REQUIRE(first_secret.IsValid());
    BOOST_CHECK_EQUAL(HexStr(first_secret.GetPublicKey().ToByteVector(/*specificLegacyScheme=*/true)),
                      DASHSYNC_OPERATOR_PUBLIC_KEY_LEGACY);

    // The consumption watermark was persisted, and the recognition
    // lookahead follows it: gap-limit keys starting at the watermark.
    const auto watermark{ReadWatermarkRecord(*wallet)};
    BOOST_REQUIRE(watermark.has_value());
    BOOST_CHECK_EQUAL(watermark->next_index, 1U);
    const auto lookahead{ReadLookaheadRecord(*wallet)};
    BOOST_REQUIRE(lookahead.has_value());
    BOOST_CHECK_EQUAL(lookahead->base_index, 1U);
    BOOST_CHECK_EQUAL(lookahead->public_keys.size(), MASTERNODE_OPERATOR_GAP_LIMIT);
    BOOST_CHECK(lookahead->public_keys[0] == expected[1]);
    BOOST_CHECK(lookahead->source_id == watermark->source_id);

    // Consumption is permanent: the next key is the next index.
    auto second{interface->getNewMasternodeOperatorKey({})};
    BOOST_REQUIRE(second.status == MasternodeOperatorKeyStatus::SUCCESS);
    BOOST_CHECK_EQUAL(second.key.path, "m/9'/1'/3'/3'/1");
    BOOST_CHECK(second.key.public_key == expected[1]);
    BOOST_CHECK_EQUAL(ReadWatermarkRecord(*wallet)->next_index, 2U);
}

BOOST_AUTO_TEST_CASE(mainnet_path_and_mnemonic_passphrase)
{
    auto wallet{MakeWallet()};
    SetupMnemonicWallet(*wallet);
    CBLSSecretKey mainnet_secret;
    {
        LOCK(wallet->cs_wallet);
        bool derived{false};
        for (ScriptPubKeyMan* spk_man : wallet->GetAllScriptPubKeyMans()) {
            std::vector<unsigned char> source_id;
            if (spk_man->GetMasternodeOperatorKeySource(source_id) !=
                MasternodeOperatorKeySourceStatus::AVAILABLE) {
                continue;
            }
            BOOST_REQUIRE(spk_man->DeriveMasternodeOperatorKey(/*coin_type=*/5, /*index=*/0, mainnet_secret) ==
                          MasternodeOperatorKeyStatus::SUCCESS);
            derived = true;
            break;
        }
        BOOST_REQUIRE(derived);
    }
    BOOST_CHECK_EQUAL(HexStr(mainnet_secret.ToByteVector(/*specificLegacyScheme=*/false)),
                      DASHSYNC_MAINNET_OPERATOR_SECRET);
    BOOST_CHECK_EQUAL(MasternodeOperatorKeyPath(/*coin_type=*/5, /*index=*/0), "m/9'/5'/3'/3'/0");

    auto protected_wallet{MakeWallet()};
    SetupMnemonicWallet(*protected_wallet, DASHSYNC_MNEMONIC, MNEMONIC_PASSPHRASE);
    auto interface{MakeWalletInterface(protected_wallet)};
    auto protected_key{interface->getNewMasternodeOperatorKey({})};
    BOOST_REQUIRE(protected_key.status == MasternodeOperatorKeyStatus::SUCCESS);
    // A BIP39 passphrase changes the seed, and with it every derived key.
    BOOST_CHECK(HexStr(protected_key.key.secret_key) != DASHSYNC_OPERATOR_SECRET);
}

BOOST_AUTO_TEST_CASE(exact_recovery_returns_consumed_keys_only)
{
    auto wallet{MakeWallet()};
    SetupMnemonicWallet(*wallet);
    auto interface{MakeWalletInterface(wallet)};

    const auto expected{DashSyncPublicKeys(6)};
    auto first{interface->getNewMasternodeOperatorKey({})};
    BOOST_REQUIRE(first.status == MasternodeOperatorKeyStatus::SUCCESS);
    auto second{interface->getNewMasternodeOperatorKey({})};
    BOOST_REQUIRE(second.status == MasternodeOperatorKeyStatus::SUCCESS);

    // Recovery of consumed keys re-derives the same secrets, repeatably.
    auto recovered{interface->getMasternodeOperatorKey(expected[0])};
    BOOST_REQUIRE(recovered.status == MasternodeOperatorKeyStatus::SUCCESS);
    BOOST_CHECK_EQUAL(recovered.key.path, "m/9'/1'/3'/3'/0");
    BOOST_CHECK(recovered.key.secret_key == first.key.secret_key);
    auto again{interface->getMasternodeOperatorKey(expected[0])};
    BOOST_REQUIRE(again.status == MasternodeOperatorKeyStatus::SUCCESS);
    BOOST_CHECK(again.key.secret_key == first.key.secret_key);

    // Exposure equals consumption: a derivable key above the watermark has
    // never been exposed, so it is not addressable.
    BOOST_CHECK(interface->getMasternodeOperatorKey(expected[5]).status == MasternodeOperatorKeyStatus::NOT_FOUND);
    // Recovery is read-only: the watermark did not move.
    BOOST_CHECK_EQUAL(ReadWatermarkRecord(*wallet)->next_index, 2U);

    BOOST_CHECK(interface->getMasternodeOperatorKey(RandomOperatorKeyBytes()).status ==
                MasternodeOperatorKeyStatus::NOT_FOUND);
    BOOST_CHECK(interface->getMasternodeOperatorKey({0x01, 0x02, 0x03}).status ==
                MasternodeOperatorKeyStatus::INVALID_KEY);
}

BOOST_AUTO_TEST_CASE(sync_hook_advances_watermark)
{
    // A restored wallet: same mnemonic, empty database. Feeding provider
    // transactions through the sync path must advance the watermark past the
    // matched indexes, regardless of the wire encoding of the operator key.
    auto wallet{MakeWallet()};
    SetupMnemonicWallet(*wallet);
    const auto expected{DashSyncPublicKeys(4)};

    // Without a lookahead nothing can be matched: recognition is best-effort.
    wallet->transactionAddedToMempool(BuildProRegTx(ParseBasic(expected[0]), /*legacy_encoding=*/true), GetTime());
    BOOST_CHECK(!ReadWatermarkRecord(*wallet).has_value());

    BOOST_REQUIRE(TopUpLookahead(*wallet) == MasternodeOperatorKeyStatus::SUCCESS);
    wallet->transactionAddedToMempool(BuildProRegTx(ParseBasic(expected[0]), /*legacy_encoding=*/true), GetTime());
    BOOST_CHECK_EQUAL(ReadWatermarkRecord(*wallet)->next_index, 1U);

    // A hit above the watermark consumes every index below it too: issuance
    // is lowest-first, so index 1 must have been handed out before index 2.
    wallet->transactionAddedToMempool(BuildProUpRegTx(ParseBasic(expected[2]), /*legacy_encoding=*/false), GetTime());
    BOOST_CHECK_EQUAL(ReadWatermarkRecord(*wallet)->next_index, 3U);

    // Foreign operator keys and keys below the watermark change nothing.
    wallet->transactionAddedToMempool(BuildProRegTx(ParseBasic(RandomOperatorKeyBytes()), /*legacy_encoding=*/false),
                                      GetTime());
    wallet->transactionAddedToMempool(BuildProRegTx(ParseBasic(expected[0]), /*legacy_encoding=*/false), GetTime());
    BOOST_CHECK_EQUAL(ReadWatermarkRecord(*wallet)->next_index, 3U);

    auto interface{MakeWalletInterface(wallet)};
    auto next{interface->getNewMasternodeOperatorKey({})};
    BOOST_REQUIRE(next.status == MasternodeOperatorKeyStatus::SUCCESS);
    BOOST_CHECK_EQUAL(next.key.path, "m/9'/1'/3'/3'/3");
    BOOST_CHECK(next.key.public_key == expected[3]);
}

BOOST_AUTO_TEST_CASE(predicate_scans_past_active_keys)
{
    auto wallet{MakeWallet()};
    SetupMnemonicWallet(*wallet);
    auto interface{MakeWalletInterface(wallet)};

    const auto expected{DashSyncPublicKeys(3)};
    std::vector<std::vector<unsigned char>> queried;
    auto result{interface->getNewMasternodeOperatorKey([&](const CBLSPublicKey& candidate) {
        const auto bytes{candidate.ToByteVector(/*specificLegacyScheme=*/false)};
        queried.push_back(bytes);
        return bytes == expected[0];
    })};
    BOOST_REQUIRE(result.status == MasternodeOperatorKeyStatus::SUCCESS);
    BOOST_CHECK_EQUAL(result.key.path, "m/9'/1'/3'/3'/1");
    // The scan starts at the watermark, visits candidates in index order and
    // stops after a full gap of not-in-use indexes above the last hit.
    BOOST_REQUIRE_EQUAL(queried.size(), MASTERNODE_OPERATOR_GAP_LIMIT + 1);
    BOOST_CHECK(queried[0] == expected[0]);
    BOOST_CHECK(queried[1] == expected[1]);

    // The in-use key was consumed by the scan, not skipped: issuing below a
    // demonstrably active index is what this exists to prevent.
    BOOST_CHECK_EQUAL(ReadWatermarkRecord(*wallet)->next_index, 2U);
    auto next{interface->getNewMasternodeOperatorKey({})};
    BOOST_REQUIRE(next.status == MasternodeOperatorKeyStatus::SUCCESS);
    BOOST_CHECK_EQUAL(next.key.path, "m/9'/1'/3'/3'/2");
    BOOST_CHECK(next.key.public_key == expected[2]);
}

BOOST_AUTO_TEST_CASE(encrypted_wallets_lock_unlock)
{
    auto wallet{MakeWallet()};
    SetupMnemonicWallet(*wallet);
    BOOST_REQUIRE(wallet->EncryptWallet("passphrase"));
    auto interface{MakeWalletInterface(wallet)};
    BOOST_CHECK(interface->hasMasternodeOperatorKeySource());

    auto locked{interface->getNewMasternodeOperatorKey({})};
    BOOST_CHECK(locked.status == MasternodeOperatorKeyStatus::WALLET_LOCKED);
    BOOST_CHECK(locked.key.secret_key.empty());

    // No operator-key records exist until the first operation.
    BOOST_REQUIRE(wallet->Unlock("passphrase"));
    BOOST_CHECK(!ReadWatermarkRecord(*wallet).has_value());
    BOOST_CHECK(!ReadLookaheadRecord(*wallet).has_value());

    auto unlocked{interface->getNewMasternodeOperatorKey({})};
    BOOST_REQUIRE(unlocked.status == MasternodeOperatorKeyStatus::SUCCESS);
    BOOST_CHECK_EQUAL(HexStr(unlocked.key.secret_key), DASHSYNC_OPERATOR_SECRET);
    BOOST_CHECK_EQUAL(ReadWatermarkRecord(*wallet)->next_index, 1U);

    // A lock landing while the predicate runs (without cs_wallet held)
    // must not durably burn an index it can no longer derive.
    auto raced{interface->getNewMasternodeOperatorKey([&](const CBLSPublicKey&) {
        BOOST_REQUIRE(wallet->Lock());
        return false;
    })};
    BOOST_CHECK(raced.status == MasternodeOperatorKeyStatus::WALLET_LOCKED);
    BOOST_CHECK(raced.key.secret_key.empty());
    BOOST_CHECK_EQUAL(ReadWatermarkRecord(*wallet)->next_index, 1U);
    BOOST_REQUIRE(wallet->Unlock("passphrase"));
    auto after_race{interface->getNewMasternodeOperatorKey({})};
    BOOST_REQUIRE(after_race.status == MasternodeOperatorKeyStatus::SUCCESS);
    BOOST_CHECK_EQUAL(after_race.key.path, "m/9'/1'/3'/3'/1");

    BOOST_REQUIRE(wallet->Lock());
    BOOST_CHECK(interface->getMasternodeOperatorKey(unlocked.key.public_key).status ==
                MasternodeOperatorKeyStatus::WALLET_LOCKED);
}

BOOST_AUTO_TEST_CASE(fresh_wallets_have_no_records_until_first_use)
{
    auto wallet{MakeWallet()};
    SetupMnemonicWallet(*wallet);
    auto interface{MakeWalletInterface(wallet)};
    BOOST_CHECK(interface->hasMasternodeOperatorKeySource());
    BOOST_CHECK(!ReadWatermarkRecord(*wallet).has_value());
    BOOST_CHECK(!ReadLookaheadRecord(*wallet).has_value());

    auto first{interface->getNewMasternodeOperatorKey({})};
    BOOST_REQUIRE(first.status == MasternodeOperatorKeyStatus::SUCCESS);
    BOOST_CHECK(ReadWatermarkRecord(*wallet).has_value());
    BOOST_CHECK(ReadLookaheadRecord(*wallet).has_value());
}

BOOST_AUTO_TEST_CASE(database_write_failure_withholds_the_key)
{
    auto database{std::make_unique<ToggleFailDatabase>()};
    auto* database_ptr{database.get()};
    auto wallet{MakeWallet(std::move(database))};
    SetupMnemonicWallet(*wallet);
    auto interface{MakeWalletInterface(wallet)};
    BOOST_REQUIRE(TopUpLookahead(*wallet) == MasternodeOperatorKeyStatus::SUCCESS);

    // If the watermark cannot be persisted, no secret is returned and the
    // index remains available.
    database_ptr->write_success = false;
    auto failed{interface->getNewMasternodeOperatorKey({})};
    BOOST_CHECK(failed.status == MasternodeOperatorKeyStatus::DATABASE_ERROR);
    BOOST_CHECK(failed.key.secret_key.empty());

    // The sync hook must not mutate the in-memory watermark either when its
    // write fails: index 0 must still be handed out below.
    const auto expected{DashSyncPublicKeys(1)};
    wallet->transactionAddedToMempool(BuildProRegTx(ParseBasic(expected[0]), /*legacy_encoding=*/true), GetTime());

    database_ptr->write_success = true;
    auto succeeded{interface->getNewMasternodeOperatorKey({})};
    BOOST_REQUIRE(succeeded.status == MasternodeOperatorKeyStatus::SUCCESS);
    BOOST_CHECK_EQUAL(succeeded.key.path, "m/9'/1'/3'/3'/0");
    BOOST_CHECK(succeeded.key.public_key == expected[0]);
}

BOOST_AUTO_TEST_CASE(unsupported_sources_fail_closed)
{
    auto no_source{MakeWallet()};
    auto no_source_interface{MakeWalletInterface(no_source)};
    BOOST_CHECK(!no_source_interface->hasMasternodeOperatorKeySource());
    BOOST_CHECK(no_source_interface->getNewMasternodeOperatorKey({}).status ==
                MasternodeOperatorKeyStatus::NOT_SUPPORTED);

    // Legacy wallets are unsupported even with a genuine mnemonic: operator
    // keys are a descriptor-wallet feature.
    auto legacy{MakeWallet()};
    SetupLegacyMnemonicWallet(*legacy);
    auto legacy_interface{MakeWalletInterface(legacy)};
    BOOST_CHECK(!legacy_interface->hasMasternodeOperatorKeySource());
    BOOST_CHECK(legacy_interface->getNewMasternodeOperatorKey({}).status == MasternodeOperatorKeyStatus::NOT_SUPPORTED);

    auto descriptor_without_mnemonic{MakeWallet()};
    {
        std::array<unsigned char, 32> bytes{};
        bytes.back() = 1;
        CExtKey master;
        master.SetSeed(MakeByteSpan(bytes));
        LOCK(descriptor_without_mnemonic->cs_wallet);
        descriptor_without_mnemonic->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        descriptor_without_mnemonic->SetupDescriptorScriptPubKeyMans(master, {}, {});
    }
    auto no_mnemonic_interface{MakeWalletInterface(descriptor_without_mnemonic)};
    BOOST_CHECK(!no_mnemonic_interface->hasMasternodeOperatorKeySource());

    auto ambiguous{MakeWallet()};
    SetupMnemonicWallet(*ambiguous, DASHSYNC_MNEMONIC);
    SetupMnemonicWallet(*ambiguous, SECOND_MNEMONIC);
    auto ambiguous_interface{MakeWalletInterface(ambiguous)};
    BOOST_CHECK(!ambiguous_interface->hasMasternodeOperatorKeySource());

    for (const auto flag : {WALLET_FLAG_DISABLE_PRIVATE_KEYS, WALLET_FLAG_EXTERNAL_SIGNER}) {
        auto restricted{MakeWallet()};
        SetupMnemonicWallet(*restricted);
        restricted->SetWalletFlag(flag);
        auto restricted_interface{MakeWalletInterface(restricted)};
        BOOST_CHECK(!restricted_interface->hasMasternodeOperatorKeySource());
        BOOST_CHECK(restricted_interface->getNewMasternodeOperatorKey({}).status ==
                    MasternodeOperatorKeyStatus::NOT_SUPPORTED);
    }
}

BOOST_AUTO_TEST_CASE(named_database_persists_consumption_across_reload)
{
    const std::string name{"mn-operator"};
    interfaces::MasternodeOperatorKey consumed;
    {
        auto wallet{MakeNamedWallet(name, /*existing=*/false)};
        SetupMnemonicWallet(*wallet);
        auto interface{MakeWalletInterface(wallet)};
        auto result{interface->getNewMasternodeOperatorKey({})};
        BOOST_REQUIRE(result.status == MasternodeOperatorKeyStatus::SUCCESS);
        consumed = result.key;
    }

    auto reloaded{MakeNamedWallet(name, /*existing=*/true)};
    auto interface{MakeWalletInterface(reloaded)};
    BOOST_REQUIRE(interface->hasMasternodeOperatorKeySource());
    auto recovered{interface->getMasternodeOperatorKey(consumed.public_key)};
    BOOST_REQUIRE(recovered.status == MasternodeOperatorKeyStatus::SUCCESS);
    BOOST_CHECK(recovered.key.secret_key == consumed.secret_key);
    BOOST_CHECK_EQUAL(recovered.key.path, consumed.path);

    auto next{interface->getNewMasternodeOperatorKey({})};
    BOOST_REQUIRE(next.status == MasternodeOperatorKeyStatus::SUCCESS);
    BOOST_CHECK_EQUAL(next.key.path, "m/9'/1'/3'/3'/1");
}

BOOST_AUTO_TEST_CASE(stale_and_malformed_records_are_ignored)
{
    // Structurally broken records must not fail the load, and well-formed
    // records left behind by a different seed must not be trusted.
    {
        const std::string name{"mn-operator-malformed"};
        {
            auto wallet{MakeNamedWallet(name, /*existing=*/false)};
            SetupMnemonicWallet(*wallet);
            // Empty source id fails validation at load.
            WalletBatch batch{wallet->GetDatabase()};
            BOOST_REQUIRE(batch.WriteMasternodeOperatorWatermark({{}, 5}));
            // Not a serialized lookahead at all.
            std::unique_ptr<DatabaseBatch> raw{wallet->GetDatabase().MakeBatch()};
            BOOST_REQUIRE(raw->Write(DBKeys::MASTERNODE_OPERATOR_LOOKAHEAD, uint8_t{7}));
        }
        auto reloaded{MakeNamedWallet(name, /*existing=*/true)};
        auto interface{MakeWalletInterface(reloaded)};
        auto result{interface->getNewMasternodeOperatorKey({})};
        BOOST_REQUIRE(result.status == MasternodeOperatorKeyStatus::SUCCESS);
        BOOST_CHECK_EQUAL(HexStr(result.key.secret_key), DASHSYNC_OPERATOR_SECRET);
    }

    {
        const std::string name{"mn-operator-stale"};
        const std::vector<unsigned char> foreign_source(21, 0xAA);
        const std::vector<std::vector<unsigned char>> foreign_keys{RandomOperatorKeyBytes(), RandomOperatorKeyBytes()};
        {
            auto wallet{MakeNamedWallet(name, /*existing=*/false)};
            SetupMnemonicWallet(*wallet);
            WalletBatch batch{wallet->GetDatabase()};
            BOOST_REQUIRE(batch.WriteMasternodeOperatorWatermark({foreign_source, 400}));
            BOOST_REQUIRE(batch.WriteMasternodeOperatorLookahead({foreign_source, 0, foreign_keys}));
        }
        auto reloaded{MakeNamedWallet(name, /*existing=*/true)};
        // A provider transaction matching the foreign lookahead must not
        // advance anything: the lookahead does not belong to the active seed.
        reloaded->transactionAddedToMempool(BuildProRegTx(ParseBasic(foreign_keys[0]), /*legacy_encoding=*/false),
                                            GetTime());
        BOOST_CHECK_EQUAL(ReadWatermarkRecord(*reloaded)->next_index, 400U);
        BOOST_CHECK(ReadWatermarkRecord(*reloaded)->source_id == foreign_source);

        // Issuance ignores the foreign watermark and starts at index 0.
        auto interface{MakeWalletInterface(reloaded)};
        auto result{interface->getNewMasternodeOperatorKey({})};
        BOOST_REQUIRE(result.status == MasternodeOperatorKeyStatus::SUCCESS);
        BOOST_CHECK_EQUAL(HexStr(result.key.secret_key), DASHSYNC_OPERATOR_SECRET);
        BOOST_CHECK_EQUAL(result.key.path, "m/9'/1'/3'/3'/0");
        // The foreign records were replaced by ones for the active seed.
        BOOST_CHECK_EQUAL(ReadWatermarkRecord(*reloaded)->next_index, 1U);
        BOOST_CHECK(ReadWatermarkRecord(*reloaded)->source_id != foreign_source);
    }
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
