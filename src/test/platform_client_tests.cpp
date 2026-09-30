// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/client.h>
#include <platform/helpers.h>
#include <platform/marshal.h>
#include <platform/signer.h>
#include <platform/types.h>
#include <platform/walletrecords.h>

#include <dash/platform/ffi.h>

#include <hash.h>
#include <interfaces/wallet.h>
#include <key.h>
#include <netbase.h>
#include <pubkey.h>
#include <test/util/platform_client.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <wallet/context.h>
#include <wallet/platformtypes.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <memory>
#include <string>
#include <vector>

using namespace platform;
using wallet::CreateMockWalletDatabase;
using wallet::CWallet;
using wallet::IdentityAuthKey;
using wallet::RegistrationFundingKey;
using wallet::WALLET_FLAG_DESCRIPTORS;

namespace platform {
//! Mints operations for the tests, as PlatformService does for the GUI.
struct SigningOperationTestAccess {
    static SigningOperation Make(interfaces::Wallet& wallet, OperationKind kind, std::vector<uint32_t> key_ids,
                                 std::optional<IdentityPublicKey> document_key,
                                 std::optional<RegistrationFundingKey> funding_key)
    {
        return SigningOperation(wallet, kind, std::move(key_ids), std::move(document_key), funding_key, nullptr);
    }
};
} // namespace platform

BOOST_FIXTURE_TEST_SUITE(platform_client_tests, TestingSetup)

namespace {

const SecureString MNEMONIC{"birth kingdom trash renew flavor utility donkey gasp regular alert pave layer"};

//! A seeded descriptor wallet behind interfaces::Wallet, so signing goes
//! through the real seams (cs_wallet, DIP-13 derivation, SignCompact).
struct SeededWallet {
    wallet::WalletContext m_context;
    std::shared_ptr<CWallet> m_wallet;
    std::unique_ptr<interfaces::Wallet> m_iface;

    explicit SeededWallet(const TestingSetup& setup)
    {
        m_context.args = &const_cast<TestingSetup&>(setup).m_args;
        m_context.chain = setup.m_node.chain.get();
        m_context.coinjoin_loader = setup.m_node.coinjoin_loader.get();
        m_wallet = std::make_shared<CWallet>(setup.m_node.chain.get(), setup.m_node.coinjoin_loader.get(), "",
                                             *m_context.args, CreateMockWalletDatabase());
        m_wallet->LoadWallet();
        {
            LOCK(m_wallet->cs_wallet);
            m_wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
            m_wallet->SetupDescriptorScriptPubKeyMans(MNEMONIC, "");
        }
        m_iface = interfaces::MakeWallet(m_context, m_wallet);
    }
};

std::vector<uint8_t> Preimage(uint8_t variant, size_t size = 40)
{
    std::vector<uint8_t> out(size, 0x77);
    out[0] = variant;
    return out;
}

uint256 DoubleSha(const std::vector<uint8_t>& data)
{
    uint256 out;
    CHash256().Write(data).Finalize(out);
    return out;
}

IdentityPublicKey HighKey()
{
    IdentityPublicKey key;
    key.id = 1;
    key.purpose = IdentityPublicKey::Purpose::AUTHENTICATION;
    key.security_level = IdentityPublicKey::SecurityLevel::HIGH;
    return key;
}

platform_ffi::Status FfiStatus(platform_ffi::StatusKind kind, uint32_t code = 0)
{
    platform_ffi::Status status;
    status.kind = kind;
    status.consensus_code = code;
    status.message = "scripted";
    return status;
}

platform_ffi::Meta FfiMeta()
{
    platform_ffi::Meta meta;
    meta.height = 1234;
    meta.core_chain_locked_height = 56;
    meta.time_ms = 7890;
    meta.protocol_version = 14;
    meta.chain_id = "dash-testnet-51";
    return meta;
}

} // namespace

//! Every bridge status kind maps onto the C++ enumerator with the same
//! value, and the value of a verified read is only present when the bridge
//! says it is meaningful (OK and UNSUPPORTED_PROTOCOL_VERSION).
BOOST_AUTO_TEST_CASE(status_kinds_and_value_presence)
{
    const std::vector<std::pair<platform_ffi::StatusKind, StatusKind>> kinds{
        {platform_ffi::StatusKind::Ok, StatusKind::OK},
        {platform_ffi::StatusKind::ProvenAbsent, StatusKind::PROVEN_ABSENT},
        {platform_ffi::StatusKind::AlreadyExists, StatusKind::ALREADY_EXISTS},
        {platform_ffi::StatusKind::Consensus, StatusKind::CONSENSUS},
        {platform_ffi::StatusKind::Unavailable, StatusKind::UNAVAILABLE},
        {platform_ffi::StatusKind::Rejected, StatusKind::REJECTED},
        {platform_ffi::StatusKind::ChainIdMismatch, StatusKind::CHAIN_ID_MISMATCH},
        {platform_ffi::StatusKind::UnsupportedProtocolVersion, StatusKind::UNSUPPORTED_PROTOCOL_VERSION},
        {platform_ffi::StatusKind::Internal, StatusKind::INTERNAL},
    };
    for (const auto& [ffi_kind, kind] : kinds) {
        platform_ffi::VerifiedU64 verified;
        verified.status = FfiStatus(ffi_kind, 40105);
        verified.meta = FfiMeta();
        verified.value = 9;
        const Result<uint64_t> result{marshal::FromFfi(verified)};
        BOOST_CHECK(result.status.kind == kind);
        BOOST_CHECK_EQUAL(result.status.consensus_code, 40105U);
        BOOST_CHECK_EQUAL(result.status.message, "scripted");
        BOOST_CHECK_EQUAL(result.metadata.height, 1234U);
        BOOST_CHECK_EQUAL(result.metadata.core_chain_locked_height, 56U);
        BOOST_CHECK_EQUAL(result.metadata.protocol_version, 14U);
        BOOST_CHECK_EQUAL(result.metadata.chain_id, "dash-testnet-51");
        const bool value_expected{kind == StatusKind::OK || kind == StatusKind::UNSUPPORTED_PROTOCOL_VERSION};
        BOOST_CHECK_EQUAL(result.value.has_value(), value_expected);
        BOOST_CHECK_EQUAL(result.ok(), kind == StatusKind::OK);
        BOOST_CHECK_EQUAL(result.provenAbsent(), kind == StatusKind::PROVEN_ABSENT);
    }
}

BOOST_AUTO_TEST_CASE(identity_and_documents_marshal)
{
    platform_ffi::VerifiedIdentity verified;
    verified.status = FfiStatus(platform_ffi::StatusKind::Ok);
    verified.meta = FfiMeta();
    verified.value.id.fill(0xAB);
    verified.value.balance = 5000;
    verified.value.revision = 3;
    platform_ffi::IdentityKey key;
    key.id = 2;
    key.purpose = 1;
    key.security_level = 3;
    key.key_type = 0;
    key.read_only = false;
    for (int i = 0; i < 33; ++i)
        key.data.push_back(static_cast<uint8_t>(i));
    key.disabled_at = 0;
    key.bounds.kind = platform_ffi::BoundsKind::SingleContractDocumentType;
    key.bounds.contract_id.fill(0xCD);
    key.bounds.document_type = "contactRequest";
    verified.value.keys.push_back(key);
    key.id = 3;
    key.disabled_at = 42;
    key.bounds = platform_ffi::ContractBounds{};
    key.bounds.kind = platform_ffi::BoundsKind::NoBounds;
    verified.value.keys.push_back(key);

    const Result<Identity> result{marshal::FromFfi(verified)};
    BOOST_REQUIRE(result.ok());
    const Identity& identity{*result.value};
    BOOST_CHECK(identity.id == platform_test::IdentifierFromByte(0xAB));
    BOOST_CHECK_EQUAL(identity.balance, 5000U);
    BOOST_CHECK_EQUAL(identity.revision, 3U);
    BOOST_REQUIRE_EQUAL(identity.public_keys.size(), 2U);
    BOOST_CHECK(identity.public_keys[0].purpose == IdentityPublicKey::Purpose::ENCRYPTION);
    BOOST_CHECK(identity.public_keys[0].security_level == IdentityPublicKey::SecurityLevel::MEDIUM);
    BOOST_CHECK(!identity.public_keys[0].disabled_at);
    BOOST_CHECK(identity.public_keys[0].contract_bounds.kind == ContractBounds::Kind::SINGLE_CONTRACT_DOCUMENT_TYPE);
    BOOST_CHECK_EQUAL(identity.public_keys[0].contract_bounds.document_type, "contactRequest");
    BOOST_CHECK_EQUAL(identity.public_keys[0].data.size(), 33U);
    BOOST_CHECK(identity.public_keys[1].disabled_at == std::optional<uint64_t>{42});
    BOOST_CHECK(identity.public_keys[1].contract_bounds.kind == ContractBounds::Kind::NONE);

    // The round trip back to the bridge form keeps everything the builders
    // need (a disabled key stays disabled, bounds stay bound).
    const platform_ffi::Identity back{marshal::ToFfi(identity)};
    BOOST_REQUIRE_EQUAL(back.keys.size(), 2U);
    BOOST_CHECK_EQUAL(back.keys[0].bounds.document_type, std::string("contactRequest"));
    BOOST_CHECK_EQUAL(back.keys[1].disabled_at, 42U);
    BOOST_CHECK_EQUAL(back.keys[0].data.size(), 33U);

    platform_ffi::VerifiedContested contested;
    contested.status = FfiStatus(platform_ffi::StatusKind::Ok);
    contested.meta = FfiMeta();
    platform_ffi::Contender contender;
    contender.identity.fill(0x11);
    contender.votes = 7;
    contender.has_votes = true;
    contested.value.contenders.push_back(contender);
    contender.has_votes = false;
    contender.votes = 99;
    contested.value.contenders.push_back(contender);
    contested.value.abstain = 2;
    contested.value.lock = 3;
    contested.value.winner_kind = platform_ffi::WinnerKind::WonByIdentity;
    contested.value.winner.fill(0x11);
    contested.value.ends_at = 77;
    const Result<ContestedNameState> state{marshal::FromFfi(contested)};
    BOOST_REQUIRE(state.ok());
    BOOST_CHECK(state.value->outcome == ContestedNameState::Outcome::WON);
    BOOST_CHECK(state.value->winner == platform_test::IdentifierFromByte(0x11));
    BOOST_REQUIRE_EQUAL(state.value->contenders.size(), 2U);
    BOOST_CHECK_EQUAL(state.value->contenders[0].votes, 7U);
    BOOST_CHECK_EQUAL(state.value->contenders[1].votes, 0U);
    BOOST_CHECK_EQUAL(state.value->abstain_votes, 2U);
    BOOST_CHECK_EQUAL(state.value->lock_votes, 3U);
    BOOST_CHECK_EQUAL(state.value->ends_at, 77U);
}

//! A page is one bridge call: the cursor and the has_more flag come back as
//! the bridge reported them, and the caller continues on a later call.
BOOST_AUTO_TEST_CASE(one_page_per_call)
{
    platform_ffi::VerifiedDpnsNames verified;
    verified.status = FfiStatus(platform_ffi::StatusKind::Ok);
    verified.meta = FfiMeta();
    for (int i = 0; i < 3; ++i) {
        platform_ffi::DpnsName name;
        name.label = "name" + ToString(i);
        name.normalized_label = "name" + ToString(i);
        name.parent = "dash";
        name.identity.fill(static_cast<uint8_t>(i));
        name.document_id.fill(static_cast<uint8_t>(0x10 + i));
        name.owner.fill(static_cast<uint8_t>(i));
        verified.items.push_back(name);
    }
    verified.page.next_start_after.fill(0x12);
    verified.page.has_more = true;
    const Result<Paged<DpnsName>> page{marshal::FromFfi(verified)};
    BOOST_REQUIRE(page.ok());
    BOOST_REQUIRE_EQUAL(page.value->items.size(), 3U);
    BOOST_CHECK_EQUAL(page.value->items[2].label, "name2");
    BOOST_CHECK_EQUAL(page.value->items[2].parent_domain, "dash");
    BOOST_CHECK(page.value->next_start_after == platform_test::IdentifierFromByte(0x12));
    BOOST_CHECK(page.value->has_more);

    // The fake records the cursor each call passes, so a flow test can
    // assert it continued from the previous page rather than restarting.
    FakePlatformClient fake;
    fake.names_of_identity.push_back(page);
    fake.names_of_identity.push_back(platform_test::Ok(Paged<DpnsName>{}));
    Identifier cursor{};
    fake.namesOfIdentity(platform_test::IdentifierFromByte(1), cursor,
                         [&](Result<Paged<DpnsName>> res) { cursor = res.value->next_start_after; });
    fake.namesOfIdentity(platform_test::IdentifierFromByte(1), cursor, [](Result<Paged<DpnsName>>) {});
    BOOST_REQUIRE_EQUAL(fake.calls.size(), 2U);
    BOOST_CHECK(fake.calls[0].start_after == Identifier{});
    BOOST_CHECK(fake.calls[1].start_after == platform_test::IdentifierFromByte(0x12));
}

//! The proxy reaches the SDK as Core configured it: a numeric address or a
//! Unix socket path, with -proxyrandomize as per-connection isolation, and
//! none as a direct connection. A proxy the SDK cannot use gives no client
//! rather than one that connects directly.
BOOST_AUTO_TEST_CASE(proxy_config_reaches_the_sdk)
{
    const platform_ffi::Proxy direct{marshal::ToFfi(std::nullopt)};
    BOOST_CHECK_EQUAL(direct.kind, 0U);
    BOOST_CHECK(direct.address.empty());
    BOOST_CHECK(!direct.isolate);

    const platform_ffi::Proxy tcp{marshal::ToFfi(ProxyConfig{LookupNumeric("127.0.0.1", 9050), true})};
    BOOST_CHECK_EQUAL(tcp.kind, 1U);
    BOOST_CHECK_EQUAL(std::string{tcp.address}, "127.0.0.1:9050");
    BOOST_CHECK(tcp.isolate);

    const platform_ffi::Proxy tcp6{marshal::ToFfi(ProxyConfig{LookupNumeric("::1", 9050), false})};
    BOOST_CHECK_EQUAL(tcp6.kind, 1U);
    BOOST_CHECK_EQUAL(std::string{tcp6.address}, "[::1]:9050");
    BOOST_CHECK(!tcp6.isolate);

    const platform_ffi::Proxy unix_socket{marshal::ToFfi(ProxyConfig{std::string{"/run/tor/socks"}, true})};
    BOOST_CHECK_EQUAL(unix_socket.kind, 2U);
    BOOST_CHECK_EQUAL(std::string{unix_socket.address}, "/run/tor/socks");
    BOOST_CHECK(unix_socket.isolate);

    ClientConfig config;
    config.network = ClientConfig::Network::TESTNET;
    config.tenderdash_chain_id = "dash-testnet-51";
    config.platform_llmq_type = 6;
    config.proxy = ProxyConfig{LookupNumeric("127.0.0.1", 9050), true};
    auto client{MakeSdkPlatformClient(config)};
    BOOST_REQUIRE(client);
    client->shutdown();
    // The SDK takes no Unix socket path it cannot connect to (an empty one).
    config.proxy = ProxyConfig{std::string{}, true};
    BOOST_CHECK(!MakeSdkPlatformClient(config));
}

//! The signer hashes the preimage itself, only signs with a key inside the
//! operation and only a preimage of the operation's kind, and answers with
//! the wallet's compact signature over sha256d of the bytes.
BOOST_AUTO_TEST_CASE(wallet_signer_scopes_keys_and_kinds)
{
    SeededWallet seeded{*this};
    interfaces::Wallet& wallet{*seeded.m_iface};
    const SigningOperation op{
        SigningOperationTestAccess::Make(wallet, OperationKind::DPNS_DOMAIN, {1}, HighKey(), std::nullopt)};
    const WalletSigner signer{op};

    const auto preimage{Preimage(STATE_TRANSITION_VARIANT_BATCH)};
    std::vector<uint8_t> signature;
    BOOST_REQUIRE(signer.signForKey(1, preimage, signature));
    BOOST_REQUIRE_EQUAL(signature.size(), 65U);
    CPubKey recovered;
    BOOST_REQUIRE(recovered.RecoverCompact(DoubleSha(preimage), signature));
    const auto expected{wallet.getPlatformPubKey(IdentityAuthKey{0, 1})};
    BOOST_REQUIRE(expected);
    BOOST_CHECK(recovered == expected.value);

    // Key outside the operation.
    BOOST_CHECK(!signer.signForKey(0, preimage, signature));
    BOOST_CHECK(!signer.signForKey(2, preimage, signature));
    // Wrong variant byte for a document operation, and an empty preimage.
    BOOST_CHECK(!signer.signForKey(1, Preimage(STATE_TRANSITION_VARIANT_IDENTITY_CREATE), signature));
    BOOST_CHECK(!signer.signForKey(1, std::vector<uint8_t>{}, signature));
    // A document operation has no asset lock to sign.
    std::array<uint8_t, 32> sighash{};
    BOOST_CHECK(!signer.signAssetLockSighash(sighash, signature));
}

//! An identity registration signs with every key it registers and with the
//! funding key exactly once.
BOOST_AUTO_TEST_CASE(wallet_signer_asset_lock_once)
{
    SeededWallet seeded{*this};
    interfaces::Wallet& wallet{*seeded.m_iface};
    const SigningOperation op{SigningOperationTestAccess::Make(wallet, OperationKind::IDENTITY_CREATE, {0, 1, 2, 3},
                                                               std::nullopt, RegistrationFundingKey{0})};
    const WalletSigner signer{op};

    const auto preimage{Preimage(STATE_TRANSITION_VARIANT_IDENTITY_CREATE)};
    std::vector<uint8_t> signature;
    for (const uint32_t key_id : {0U, 1U, 2U, 3U}) {
        BOOST_CHECK(signer.signForKey(key_id, preimage, signature));
    }
    BOOST_CHECK(!signer.signForKey(4, preimage, signature));
    BOOST_CHECK(!signer.signForKey(1, Preimage(STATE_TRANSITION_VARIANT_BATCH), signature));

    std::array<uint8_t, 32> sighash{};
    sighash.fill(0x42);
    BOOST_REQUIRE(signer.signAssetLockSighash(sighash, signature));
    BOOST_REQUIRE_EQUAL(signature.size(), 65U);
    CPubKey recovered;
    BOOST_REQUIRE(recovered.RecoverCompact(uint256{sighash}, signature));
    const auto funding{wallet.getPlatformPubKey(RegistrationFundingKey{0})};
    BOOST_REQUIRE(funding);
    BOOST_CHECK(recovered == funding.value);
    // The second asset-lock signature of the same operation is refused.
    BOOST_CHECK(!signer.signAssetLockSighash(sighash, signature));
}

//! A locked wallet never signs: the flow parks in NEEDS_UNLOCK instead.
BOOST_AUTO_TEST_CASE(wallet_signer_refuses_locked_wallet)
{
    SeededWallet seeded{*this};
    BOOST_REQUIRE(seeded.m_wallet->EncryptWallet("passphrase"));
    BOOST_REQUIRE(seeded.m_wallet->Lock());
    const SigningOperation op{
        SigningOperationTestAccess::Make(*seeded.m_iface, OperationKind::PROFILE, {1}, HighKey(), std::nullopt)};
    const WalletSigner signer{op};
    std::vector<uint8_t> signature;
    BOOST_CHECK(!signer.signForKey(1, Preimage(STATE_TRANSITION_VARIANT_BATCH), signature));
    BOOST_CHECK(signature.empty());
}

//! The pure helpers are the SDK's: normalization folds confusables, the
//! contested rule and the validity rule are the DPNS contract's, and the
//! DIP-15 masking round-trips.
BOOST_AUTO_TEST_CASE(pure_helpers)
{
    BOOST_CHECK_EQUAL(helpers::NormalizeLabel("Alice"), "a11ce");
    BOOST_CHECK_EQUAL(helpers::NormalizeLabel("B0b-Oli"), "b0b-011");
    BOOST_CHECK(helpers::IsValidUsername("alice"));
    BOOST_CHECK(!helpers::IsValidUsername("-alice"));
    BOOST_CHECK(!helpers::IsValidUsername("al"));
    BOOST_CHECK(helpers::IsContestedUsername("alice"));
    BOOST_CHECK(!helpers::IsContestedUsername("alice-wonderland-2026"));
    BOOST_CHECK(helpers::SystemContractId(helpers::SystemContract::DPNS) !=
                helpers::SystemContractId(helpers::SystemContract::DASHPAY));
    BOOST_CHECK_EQUAL(helpers::CreditsPerDuff(), 1000U);

    std::array<uint8_t, 32> mac{};
    for (size_t i = 0; i < mac.size(); ++i)
        mac[i] = static_cast<uint8_t>(i * 7);
    const uint32_t reference{helpers::Dip15AccountReferenceFromMac(mac, 5, 3)};
    const auto unmasked{helpers::Dip15UnmaskAccountReference(mac, reference)};
    BOOST_CHECK_EQUAL(unmasked.version, 3U);
    BOOST_CHECK_EQUAL(unmasked.account_index, 5U);

    // Receive-side policy: never ECDH with the MASTER key, and only the
    // purposes the SDK accepts.
    BOOST_CHECK(!helpers::Dip15ReceiveKeysAcceptable(IdentityPublicKey::Purpose::ENCRYPTION,
                                                     IdentityPublicKey::Purpose::DECRYPTION, 0));
    BOOST_CHECK(helpers::Dip15ReceiveKeysAcceptable(IdentityPublicKey::Purpose::ENCRYPTION,
                                                    IdentityPublicKey::Purpose::DECRYPTION, 3));

    Identity recipient;
    IdentityPublicKey enc{HighKey()};
    enc.id = 2;
    enc.purpose = IdentityPublicKey::Purpose::ENCRYPTION;
    enc.security_level = IdentityPublicKey::SecurityLevel::MEDIUM;
    enc.data.assign(33, 2);
    IdentityPublicKey dec{enc};
    dec.id = 3;
    dec.purpose = IdentityPublicKey::Purpose::DECRYPTION;
    recipient.public_keys = {HighKey(), enc, dec};
    BOOST_CHECK(helpers::Dip15SelectRecipientKey(recipient) == std::optional<uint32_t>{3});
    recipient.public_keys = {HighKey()};
    BOOST_CHECK(!helpers::Dip15SelectRecipientKey(recipient));

    const std::array<uint8_t, 32> secret{};
    BOOST_CHECK(!helpers::Dip15DecryptXpub(secret, std::vector<uint8_t>(10, 0)));
}

//! Records are versioned as a set: any layout but the current one wipes.
BOOST_AUTO_TEST_CASE(wallet_records_round_trip_and_version_rule)
{
    IdentityRecord record;
    record.state = IdentityRecord::State::NEEDS_UNLOCK;
    record.resume_state = IdentityRecord::State::PREORDER_WAIT;
    record.funding_txid = uint256::ONE;
    record.funding_key_index = 0;
    record.funding_amount = 1000000;
    record.identity_id = platform_test::IdentifierFromByte(0x33);
    record.auth_key_id = 2;
    record.encryption_key_id = 4;
    record.decryption_key_id = 5;
    record.label = "Alice";
    record.normalized_label = "a11ce";
    record.preorder_salt.fill(0x55);
    record.contested = true;
    record.last_error = "none";
    record.started_at = 1700000000;
    const auto bytes{SerializeIdentityRecord(record)};
    IdentityRecord parsed;
    BOOST_REQUIRE(DeserializeIdentityRecord(bytes, parsed));
    BOOST_CHECK(parsed.state == IdentityRecord::State::NEEDS_UNLOCK);
    BOOST_CHECK(parsed.resume_state == IdentityRecord::State::PREORDER_WAIT);
    BOOST_CHECK(parsed.funding_txid == uint256::ONE);
    BOOST_CHECK_EQUAL(parsed.funding_amount, 1000000);
    BOOST_CHECK(parsed.identity_id == record.identity_id);
    BOOST_CHECK_EQUAL(parsed.auth_key_id, 2U);
    BOOST_CHECK_EQUAL(parsed.encryption_key_id, 4U);
    BOOST_CHECK_EQUAL(parsed.decryption_key_id, 5U);
    BOOST_CHECK_EQUAL(parsed.label, "Alice");
    BOOST_CHECK_EQUAL(parsed.normalized_label, "a11ce");
    BOOST_CHECK(parsed.preorder_salt == record.preorder_salt);
    BOOST_CHECK(parsed.contested);
    BOOST_CHECK_EQUAL(parsed.last_error, "none");
    BOOST_CHECK_EQUAL(parsed.started_at, 1700000000);

    // Another layout version, truncation and trailing bytes are refused.
    auto other_version{bytes};
    other_version[0] = 1;
    BOOST_CHECK(!DeserializeIdentityRecord(other_version, parsed));
    auto truncated{bytes};
    truncated.pop_back();
    BOOST_CHECK(!DeserializeIdentityRecord(truncated, parsed));
    auto trailing{bytes};
    trailing.push_back(0);
    BOOST_CHECK(!DeserializeIdentityRecord(trailing, parsed));
    BOOST_CHECK(!DeserializeIdentityRecord({}, parsed));
    // Only canonical values are accepted, so an accepted record always
    // serializes back to the same bytes: a bool byte other than 0/1 and a
    // state byte naming no enumerator are refused, not mapped.
    // version, state, txid, key index, amount, identity id, three key ids,
    // two 5-char labels with their length bytes, salt.
    constexpr size_t contested_offset{1 + 1 + 32 + 4 + 8 + 32 + 12 + 6 + 6 + 32};
    BOOST_REQUIRE_EQUAL(bytes[contested_offset], 1);
    auto bool_two{bytes};
    bool_two[contested_offset] = 2;
    BOOST_CHECK(!DeserializeIdentityRecord(bool_two, parsed));
    auto unknown_state{bytes};
    unknown_state[1] = 77;
    BOOST_CHECK(!DeserializeIdentityRecord(unknown_state, parsed));
    auto unknown_resume{bytes};
    unknown_resume[contested_offset + 1] = 77;
    BOOST_CHECK(!DeserializeIdentityRecord(unknown_resume, parsed));

    // A record without the signed transitions and the failure ends after
    // started_at (as records written before they existed do); with them it
    // round-trips them all.
    IdentityRecord signed_record{record};
    signed_record.signed_identity_create = {3, 3, 3};
    signed_record.signed_preorder = {{2, 1}, 1, 13};
    signed_record.signed_domain = {{2, 2}, 2, 14};
    signed_record.signed_profile = {{5}, 1};
    signed_record.profile_display_name = "Alice A";
    signed_record.last_failure = IdentityRecord::Failure{"Reserve username", 1700000001,
                                                         Status{StatusKind::CONSENSUS, 10405, "raw"}};
    const auto signed_bytes{SerializeIdentityRecord(signed_record)};
    BOOST_CHECK_GT(signed_bytes.size(), bytes.size());
    BOOST_CHECK(std::equal(bytes.begin(), bytes.end(), signed_bytes.begin()));
    BOOST_REQUIRE(DeserializeIdentityRecord(signed_bytes, parsed));
    BOOST_CHECK(parsed.signed_identity_create == signed_record.signed_identity_create);
    BOOST_CHECK(parsed.signed_preorder.bytes == signed_record.signed_preorder.bytes);
    BOOST_CHECK_EQUAL(parsed.signed_domain.nonce, 2U);
    BOOST_CHECK_EQUAL(parsed.signed_profile.nonce, 1U);
    BOOST_CHECK_EQUAL(parsed.signed_preorder.protocol_version, 13U);
    BOOST_CHECK_EQUAL(parsed.signed_domain.protocol_version, 14U);
    BOOST_CHECK_EQUAL(parsed.signed_profile.protocol_version, 0U);
    BOOST_CHECK_EQUAL(parsed.profile_display_name, "Alice A");
    BOOST_REQUIRE(parsed.last_failure && parsed.last_failure->status);
    BOOST_CHECK_EQUAL(parsed.last_failure->operation, "Reserve username");
    BOOST_CHECK_EQUAL(parsed.last_failure->status->consensus_code, 10405U);
    BOOST_CHECK(SerializeIdentityRecord(parsed) == signed_bytes);
    // A nonce or protocol version without its transition is not what this
    // build writes.
    IdentityRecord stray_nonce{record};
    stray_nonce.signed_domain.nonce = 2;
    stray_nonce.profile_display_name = "x";
    BOOST_CHECK(!DeserializeIdentityRecord(SerializeIdentityRecord(stray_nonce), parsed));
    IdentityRecord stray_version{record};
    stray_version.signed_profile.protocol_version = 14;
    stray_version.profile_display_name = "x";
    BOOST_CHECK(!DeserializeIdentityRecord(SerializeIdentityRecord(stray_version), parsed));

    BOOST_CHECK(IsRecordSetCurrent({}, /*have_platform_records=*/false));
    BOOST_CHECK(!IsRecordSetCurrent({}, /*have_platform_records=*/true));
    BOOST_CHECK(IsRecordSetCurrent(EncodeRecordVersion(), /*have_platform_records=*/true));
    BOOST_CHECK(!IsRecordSetCurrent({1}, /*have_platform_records=*/true));

    BOOST_CHECK_EQUAL(DecodePaymentCursor(EncodePaymentCursor(0x01020304)), 0x01020304U);
    BOOST_CHECK_EQUAL(DecodePaymentCursor({1, 2, 3}), 0U);
    BOOST_CHECK(DecodeContactOutRecord(EncodeContactOutRecord(1700000000)) == std::optional<int64_t>{1700000000});
    BOOST_CHECK(!DecodeContactOutRecord({1}));
}

BOOST_AUTO_TEST_SUITE_END()
