// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <interfaces/coinjoin.h>
#include <interfaces/wallet.h>
#include <key.h>
#include <key_io.h>
#include <script/descriptor.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <wallet/bip39.h>
#include <wallet/coincontrol.h>
#include <wallet/context.h>
#include <wallet/platformkeys.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <optional>
#include <set>
#include <vector>

using namespace wallet;
using namespace wallet::platformkeys;

BOOST_FIXTURE_TEST_SUITE(platformkeys_tests, BasicTestingSetup)

//! Seed shared by all DIP-14 test vectors (dashpay/dips dip-0014.md), from
//! mnemonic "birth kingdom trash renew flavor utility donkey gasp regular
//! alert pave layer".
static SecureVector Dip14Seed()
{
    const std::vector<unsigned char> seed{ParseHex(
        "b16d3782e714da7c55a397d5f19104cfed7ffa8036ac514509bbb50807f8ac59"
        "8eeb26f0797bd8cc221a6cbff2168d90a5e9ee025a5bd977977b9eccd97894bb")};
    return SecureVector{seed.begin(), seed.end()};
}

static CExtKey Dip14Master()
{
    const SecureVector seed{Dip14Seed()};
    CExtKey master;
    master.SetSeed(MakeByteSpan(seed));
    return master;
}

static std::array<uint8_t, 32> Arr32(const std::string& hex)
{
    const std::vector<unsigned char> v{ParseHex(hex)};
    BOOST_REQUIRE_EQUAL(v.size(), 32U);
    std::array<uint8_t, 32> out;
    std::copy(v.begin(), v.end(), out.begin());
    return out;
}

static std::string DerivedKeyHex(const Path& path)
{
    ExtKey256 out;
    BOOST_REQUIRE(DeriveExtKey(Dip14Master(), path, out));
    return HexStr(Span{out.key.begin(), out.key.size()});
}

// DIP-14 test vector 1: m/<id1>/<id2>'/<id3>/0
BOOST_AUTO_TEST_CASE(dip14_vector_1)
{
    const Path path{
        PathElement::Normal256(Arr32("775d3854c910b7dee436869c4724bed2fe0784e198b8a39f02bbb49d8ebcfc3b")),
        PathElement{Arr32("f537439f36d04a15474ff7423e4b904a14373fafb37a41db74c84f1dbb5c89a6"), true},
        PathElement::Normal256(Arr32("4c4592ca670c983fc43397dfd21a6f427fac9b4ac53cb4dcdc6522ec51e81e79")),
        PathElement::Normal(0),
    };
    BOOST_CHECK_EQUAL(DerivedKeyHex(path), "e8781fdef72862968cd9a4d2df34edaf9dcc5b17629ec505f0d2d1a8ed6f9f09");
}

// DIP-14 test vector 2: m/9'/5'/15'/0'/<idA>'/<idB>'/0 (DIP-15 shape)
BOOST_AUTO_TEST_CASE(dip14_vector_2)
{
    const Path path{
        PathElement::Hardened(9),
        PathElement::Hardened(5),
        PathElement::Hardened(15),
        PathElement::Hardened(0),
        PathElement{Arr32("555d3854c910b7dee436869c4724bed2fe0784e198b8a39f02bbb49d8ebcfc3a"), true},
        PathElement{Arr32("a137439f36d04a15474ff7423e4b904a14373fafb37a41db74c84f1dbb5c89b5"), true},
        PathElement::Normal(0),
    };
    BOOST_CHECK_EQUAL(DerivedKeyHex(path), "fac40790776d171ee1db90899b5eb2df2f7d2aaf35ad56f07ffb8ed2c57f8e60");
}

// DIP-14 test vector 3: m/<id> (single 256-bit non-hardened step)
BOOST_AUTO_TEST_CASE(dip14_vector_3)
{
    const Path path{
        PathElement::Normal256(Arr32("775d3854c910b7dee436869c4724bed2fe0784e198b8a39f02bbb49d8ebcfc3b")),
    };
    BOOST_CHECK_EQUAL(DerivedKeyHex(path), "f6a95ae75ea8362d9478932f71b262b3d981918fe030316686a475dea4889938");
}

// DIP-14 test vector 4: m/<id1>/<id2>'
BOOST_AUTO_TEST_CASE(dip14_vector_4)
{
    const Path path{
        PathElement::Normal256(Arr32("775d3854c910b7dee436869c4724bed2fe0784e198b8a39f02bbb49d8ebcfc3b")),
        PathElement{Arr32("f537439f36d04a15474ff7423e4b904a14373fafb37a41db74c84f1dbb5c89a6"), true},
    };
    BOOST_CHECK_EQUAL(DerivedKeyHex(path), "b898ad92d3a0698bc3117d3777d82676673816ce52f4fc2f1263a2f676825f90");
}

//! Non-hardened 256-bit public derivation must match private derivation
//! (this is what lets a contact derive our friendship addresses from an
//! exported xpub).
BOOST_AUTO_TEST_CASE(dip14_public_derivation_matches)
{
    const Path parent_path{
        PathElement::Hardened(9),
        PathElement::Hardened(1),
        PathElement::Hardened(15),
        PathElement::Hardened(0),
    };
    ExtKey256 parent;
    BOOST_REQUIRE(DeriveExtKey(Dip14Master(), parent_path, parent));

    const auto id_a{Arr32("555d3854c910b7dee436869c4724bed2fe0784e198b8a39f02bbb49d8ebcfc3a")};
    const auto id_b{Arr32("a137439f36d04a15474ff7423e4b904a14373fafb37a41db74c84f1dbb5c89b5")};

    // Private side: parent/idA/idB
    Path leaf_path{parent_path};
    leaf_path.push_back(PathElement::Normal256(id_a));
    leaf_path.push_back(PathElement::Normal256(id_b));
    ExtKey256 leaf;
    BOOST_REQUIRE(DeriveExtKey(Dip14Master(), leaf_path, leaf));

    // Public side: neuter parent, then derive idA/idB
    ExtPubKey256 pub_parent{parent.key.GetPubKey(), parent.chaincode};
    ExtPubKey256 pub_mid, pub_leaf;
    BOOST_REQUIRE(DerivePubKey(pub_parent, PathElement::Normal256(id_a), pub_mid));
    BOOST_REQUIRE(DerivePubKey(pub_mid, PathElement::Normal256(id_b), pub_leaf));

    BOOST_CHECK(pub_leaf.pubkey == leaf.key.GetPubKey());
    BOOST_CHECK(pub_leaf.chaincode == leaf.chaincode);

    // Hardened steps must be rejected on the public side.
    ExtPubKey256 unused;
    BOOST_CHECK(!DerivePubKey(pub_parent, PathElement{id_a, true}, unused));
    BOOST_CHECK(!DerivePubKey(pub_parent, PathElement::Hardened(0), unused));

    // Invalid or uncompressed parents (contact xpubs are externally supplied)
    // must fail instead of tripping CPubKey::Derive() asserts.
    const ExtPubKey256 invalid_parent{CPubKey{}, parent.chaincode};
    BOOST_CHECK(!DerivePubKey(invalid_parent, PathElement::Normal(0), unused));
    CKey uncompressed_key;
    uncompressed_key.MakeNewKey(/*fCompressed=*/false);
    const ExtPubKey256 uncompressed_parent{uncompressed_key.GetPubKey(), parent.chaincode};
    BOOST_CHECK(!DerivePubKey(uncompressed_parent, PathElement::Normal(0), unused));
    BOOST_CHECK(!DerivePubKey(uncompressed_parent, PathElement::Normal256(id_a), unused));
}

//! Cross-implementation vector: the same friendship path derived by
//! rust-dashcore's key-wallet crate (`key_wallet::bip32` with
//! `ChildNumber::Normal256` — the derivation backend behind
//! dashwallet-ios/android via rs-platform-wallet) from this seed and these
//! identity ids yields exactly these key bytes. The identity ids are
//! asymmetric byte sequences, so a byte-order reversal anywhere between the
//! raw Platform identifier bytes and the DIP-14 256-bit index would change
//! the result; symmetric fixtures cannot catch that. Identifier bytes enter
//! the path exactly as Platform serves them (big-endian number, the same
//! bytes base58-encoded in identity ids) — callers must not round-trip them
//! through uint256S() of a natural-order hex string, which reverses.
//! Vector generated with key-wallet at rust-dashcore 36b49cb7f9c0:
//!   seed = Dip14Seed(), path m/9'/1'/15'/0'/(id_a)/(id_b).
BOOST_AUTO_TEST_CASE(friendship_derivation_matches_key_wallet)
{
    std::array<uint8_t, 32> id_a, id_b;
    for (size_t i{0}; i < 32; ++i) {
        id_a[i] = static_cast<uint8_t>(i);          // 000102..1f
        id_b[i] = static_cast<uint8_t>(0xff - i);   // fffefd..e0
    }
    const auto path{FriendshipPath(/*coin_type=*/1, /*account=*/0, id_a, id_b)};
    ExtKey256 out;
    BOOST_REQUIRE(DeriveExtKey(Dip14Master(), path, out));
    BOOST_CHECK_EQUAL(HexStr(Span{out.key.begin(), out.key.size()}),
                      "815adcfab00d029d50a044a255cebc510fe6800c46082cf679dae8c46ca5e1f5");
    BOOST_CHECK_EQUAL(HexStr(out.chaincode),
                      "1ed00e776865c0308fddae8d22fb3ae47ffd51ce409b298d1987aafdec6bef6b");
    BOOST_CHECK_EQUAL(HexStr(out.key.GetPubKey()),
                      "025b4ff1b10b9e1990df46dfa2f986300a7e6eb4a85289f4419581950edd3760bf");
}

//! ECDH secrets must be symmetric and match the libsecp256k1 KDF
//! (SHA256 of compressed shared point), as used by DashPay contact requests.
BOOST_AUTO_TEST_CASE(ecdh_symmetry)
{
    CKey a, b;
    a.MakeNewKey(/*fCompressed=*/true);
    b.MakeNewKey(/*fCompressed=*/true);

    SecureVector s_ab, s_ba;
    BOOST_REQUIRE(ComputeECDHSecret(a, b.GetPubKey(), s_ab));
    BOOST_REQUIRE(ComputeECDHSecret(b, a.GetPubKey(), s_ba));
    BOOST_CHECK_EQUAL(s_ab.size(), 32U);
    BOOST_CHECK(s_ab == s_ba);
}

//! Mnemonic behind Dip14Seed() above.
static constexpr const char* DIP14_MNEMONIC{
    "birth kingdom trash renew flavor utility donkey gasp regular alert pave layer"};

//! Descriptor wallet with a BIP39 mnemonic, wrapped in the interfaces::Wallet
//! through which the Platform key provider is served.
struct FriendshipWalletSetup : public TestChain100Setup {
    explicit FriendshipWalletSetup(const SecureString& mnemonic = "")
    {
        m_context.args = &m_args;
        m_context.chain = m_node.chain.get();
        m_context.coinjoin_loader = m_node.coinjoin_loader.get();
        std::tie(m_wallet, m_iface) = MakeSeededWallet(mnemonic);
    }

    //! An independent wallet instance (own mock database) set up from the
    //! given mnemonic — a fresh restore of that recovery phrase.
    std::pair<std::shared_ptr<CWallet>, std::unique_ptr<interfaces::Wallet>>
    MakeSeededWallet(const SecureString& mnemonic)
    {
        auto wallet = std::make_shared<CWallet>(m_node.chain.get(), m_node.coinjoin_loader.get(), "", m_args,
                                                CreateMockWalletDatabase());
        wallet->LoadWallet();
        {
            LOCK(wallet->cs_wallet);
            wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
            wallet->SetupDescriptorScriptPubKeyMans(mnemonic, "");
        }
        auto iface = interfaces::MakeWallet(m_context, wallet);
        return {std::move(wallet), std::move(iface)};
    }

    std::pair<std::shared_ptr<CWallet>, std::unique_ptr<interfaces::Wallet>>
    MakeXprvWallet(const CExtKey& root)
    {
        auto wallet = std::make_shared<CWallet>(m_node.chain.get(), m_node.coinjoin_loader.get(), "", m_args,
                                                CreateMockWalletDatabase());
        wallet->LoadWallet();

        FlatSigningProvider provider;
        std::string error;
        auto parsed{Parse("pkh(" + EncodeExtKey(root) + "/*)", provider, error, /*require_checksum=*/false)};
        BOOST_REQUIRE_MESSAGE(parsed, error);
        WalletDescriptor descriptor(std::move(parsed), /*creation_time=*/0, /*range_start=*/0,
                                    /*range_end=*/10, /*next_index=*/0);
        {
            LOCK(wallet->cs_wallet);
            wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
            auto* spk_man{wallet->AddWalletDescriptor(descriptor, provider, "", /*internal=*/false)};
            BOOST_REQUIRE(spk_man);
            wallet->AddActiveScriptPubKeyMan(spk_man->GetID(), /*internal=*/false);
        }

        auto iface{interfaces::MakeWallet(m_context, wallet)};
        return {std::move(wallet), std::move(iface)};
    }

    static isminetype IsMineIn(CWallet& wallet, const CTxDestination& destination)
    {
        LOCK(wallet.cs_wallet);
        return wallet.IsMine(GetScriptForDestination(destination));
    }

    isminetype IsMine(const CTxDestination& destination) { return IsMineIn(*m_wallet, destination); }

    //! Payment address `index` of the friendship chain rooted at (user_a, user_b),
    //! taken through the same xpub export the contact would receive.
    static CTxDestination PaymentDestinationVia(interfaces::Wallet& iface, const uint256& user_a,
                                                const uint256& user_b, uint32_t index)
    {
        const auto keychain{iface.ensureFriendshipReceivingKeychain(
            FriendshipKeychainRequest{ACCOUNT, user_a, user_b, /*birth_time=*/0})};
        BOOST_REQUIRE(keychain);
        CTxDestination destination;
        BOOST_REQUIRE(DeriveFriendshipPaymentDestination(keychain.value, index, destination));
        return destination;
    }

    CTxDestination PaymentDestination(const uint256& user_a, const uint256& user_b, uint32_t index)
    {
        return PaymentDestinationVia(*m_iface, user_a, user_b, index);
    }

    static constexpr uint32_t ACCOUNT{0};
    const uint256 m_my_id{uint256S("1111111111111111111111111111111111111111111111111111111111111111")};
    const uint256 m_their_id{uint256S("2222222222222222222222222222222222222222222222222222222222222222")};

    WalletContext m_context;
    std::shared_ptr<CWallet> m_wallet;
    std::unique_ptr<interfaces::Wallet> m_iface;
};

//! Our own receiving chain for a friendship is imported as a private
//! descriptor: its addresses must be spendable, not watch-only, and coins paid
//! to them must count towards the spendable balance.
BOOST_FIXTURE_TEST_CASE(friendship_own_chain_is_spendable, FriendshipWalletSetup)
{
    const auto keychain{m_iface->ensureFriendshipReceivingKeychain(
        FriendshipKeychainRequest{ACCOUNT, m_my_id, m_their_id, /*birth_time=*/0})};
    BOOST_REQUIRE(keychain);

    // The public chain returned after import derives the addresses made ours.
    for (uint32_t index = 0; index < 5; ++index) {
        CTxDestination destination;
        BOOST_REQUIRE(DeriveFriendshipPaymentDestination(keychain.value, index, destination));
        BOOST_CHECK_EQUAL(IsMine(destination), ISMINE_SPENDABLE);
    }

    const CScript script{GetScriptForDestination(PaymentDestination(m_my_id, m_their_id, 0))};
    CMutableTransaction payment;
    payment.vin.emplace_back(g_insecure_rand_ctx.rand256(), 0);
    payment.vout.emplace_back(COIN, script);

    LOCK(m_wallet->cs_wallet);
    m_wallet->AddToWallet(MakeTransactionRef(payment), TxStateInMempool{});

    CCoinControl coin_control;
    coin_control.m_include_unsafe_inputs = true;
    BOOST_CHECK_EQUAL(AvailableCoins(*m_wallet, &coin_control).Size(), 1U);
}

//! The contact's receiving chain must never become ours. If it did, payments to
//! the contact would be classified as payments-to-self and their outputs would
//! be counted as our own coins.
BOOST_FIXTURE_TEST_CASE(friendship_contact_chain_is_not_ours, FriendshipWalletSetup)
{
    const auto keychain{m_iface->ensureFriendshipReceivingKeychain(
        FriendshipKeychainRequest{ACCOUNT, m_my_id, m_their_id, /*birth_time=*/0})};
    BOOST_REQUIRE(keychain);

    // A real contact derives that chain from their own seed, which our wallet
    // never sees; those destinations must be foreign too.
    CExtKey contact_key;
    const std::array<std::byte, 32> contact_seed{std::byte{7}};
    contact_key.SetSeed(contact_seed);
    const CExtPubKey contact_xpub{contact_key.Neuter()};
    const FriendshipXpub friendship_xpub{contact_xpub.pubkey, contact_xpub.chaincode};
    for (uint32_t index = 0; index < 5; ++index) {
        CTxDestination destination;
        BOOST_REQUIRE(DeriveFriendshipPaymentDestination(friendship_xpub, index, destination));
        BOOST_CHECK_EQUAL(IsMine(destination), ISMINE_NO);
    }
}

struct Dip14WalletSetup : public FriendshipWalletSetup {
    Dip14WalletSetup() : FriendshipWalletSetup(SecureString{DIP14_MNEMONIC}) {}
};

//! All active descriptor managers may share one root extended key, but the
//! provider must reject a wallet whose active managers disagree on the source.
BOOST_FIXTURE_TEST_CASE(platform_key_source_must_be_unambiguous, Dip14WalletSetup)
{
    const auto before{m_iface->getPlatformPubKey(IdentityAuthKey{0, 0})};
    BOOST_REQUIRE(before);

    // The internal hook must not permit an empty path to expose the BIP32
    // master key, even though the typed public requests cannot form one.
    {
        LOCK(m_wallet->cs_wallet);
        bool checked{false};
        for (const auto* spk_man : m_wallet->GetActiveScriptPubKeyMans()) {
            std::vector<unsigned char> source;
            if (spk_man->GetPlatformKeySource(source) != PlatformKeyStatus::SUCCESS) continue;
            ExtKey256 out;
            BOOST_CHECK(spk_man->DerivePlatformKey({}, out) == PlatformKeyStatus::INVALID_ARGUMENT);
            checked = true;
            break;
        }
        BOOST_REQUIRE(checked);
    }

    // A second setup replaces the active managers. Re-activating one original
    // manager leaves the wallet with active managers from two mnemonic roots.
    std::set<uint256> first_ids;
    {
        LOCK(m_wallet->cs_wallet);
        for (auto* spk_man : m_wallet->GetActiveScriptPubKeyMans()) first_ids.insert(spk_man->GetID());
        m_wallet->SetupDescriptorScriptPubKeyMans(
            "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about", "");
        m_wallet->AddActiveScriptPubKeyMan(*first_ids.begin(), /*internal=*/true);
    }

    const auto ambiguous{m_iface->getPlatformPubKey(IdentityAuthKey{0, 0})};
    BOOST_CHECK(ambiguous.status == PlatformKeyStatus::AMBIGUOUS_SOURCE);
}

//! An imported root xprv is sufficient for Platform derivation. The mnemonic
//! and BIP39 seed are only one way to create that extended private key.
BOOST_FIXTURE_TEST_CASE(xprv_only_wallet_derives_platform_keys, FriendshipWalletSetup)
{
    const CExtKey master{Dip14Master()};
    const auto [wallet, iface]{MakeXprvWallet(master)};
    const auto result{iface->getPlatformPubKey(IdentityAuthKey{0, 0})};
    BOOST_REQUIRE(result);

    ExtKey256 expected;
    BOOST_REQUIRE(DeriveExtKey(master, IdentityAuthKeyPath(static_cast<uint32_t>(Params().ExtCoinType()), 0, 0), expected));
    BOOST_CHECK(result.value == expected.key.GetPubKey());
}

//! The standardized Platform paths are absolute paths from m. An imported
//! child xprv cannot be treated as m because hardened ancestors are not
//! recoverable from it.
BOOST_FIXTURE_TEST_CASE(child_xprv_has_no_platform_keys, FriendshipWalletSetup)
{
    CExtKey child;
    BOOST_REQUIRE(Dip14Master().Derive(child, 0));
    const auto [wallet, iface]{MakeXprvWallet(child)};
    const auto result{iface->getPlatformPubKey(IdentityAuthKey{0, 0})};
    BOOST_CHECK(result.status == PlatformKeyStatus::NOT_SUPPORTED);
}

//! A root source is the complete extended public key, including its chain
//! code. The same private scalar paired with another chain code defines a
//! different Platform key tree and must be rejected as ambiguous.
BOOST_FIXTURE_TEST_CASE(platform_key_source_includes_chaincode, Dip14WalletSetup)
{
    CExtKey alternate{Dip14Master()};
    *alternate.chaincode.begin() ^= 1;

    FlatSigningProvider provider;
    std::string error;
    auto parsed{Parse("pkh(" + EncodeExtKey(alternate) + "/*)", provider, error, /*require_checksum=*/false)};
    BOOST_REQUIRE_MESSAGE(parsed, error);
    WalletDescriptor descriptor(std::move(parsed), /*creation_time=*/0, /*range_start=*/0,
                                /*range_end=*/10, /*next_index=*/0);
    {
        LOCK(m_wallet->cs_wallet);
        auto* spk_man{m_wallet->AddWalletDescriptor(descriptor, provider, "", /*internal=*/true)};
        BOOST_REQUIRE(spk_man);
        m_wallet->AddActiveScriptPubKeyMan(spk_man->GetID(), /*internal=*/true);
    }

    const auto result{m_iface->getPlatformPubKey(IdentityAuthKey{0, 0})};
    BOOST_CHECK(result.status == PlatformKeyStatus::AMBIGUOUS_SOURCE);
}

//! Platform derivation uses the descriptor root xprv, so damage to the
//! optional mnemonic metadata cannot redirect it to a different key tree.
BOOST_FIXTURE_TEST_CASE(invalid_mnemonic_does_not_change_platform_keys, FriendshipWalletSetup)
{
    const SecureString bad_mnemonic{
        "birth kingdom trash renew flavor utility donkey gasp regular alert pave kingdom"};
    BOOST_REQUIRE(!CMnemonic::Check(bad_mnemonic));

    auto wallet = std::make_shared<CWallet>(m_node.chain.get(), m_node.coinjoin_loader.get(), "", m_args,
                                            CreateMockWalletDatabase());
    wallet->LoadWallet();

    // The master key a corrupt record would pair with: derived from the very
    // string that fails validation (AddKey() asserts the pairing).
    SecureVector seed;
    CMnemonic::ToSeed(bad_mnemonic, "", seed);
    CExtKey master;
    master.SetSeed(MakeByteSpan(seed));

    FlatSigningProvider provider;
    std::string error;
    auto parsed = Parse("pkh(" + EncodeExtKey(master) + "/*)", provider, error, /*require_checksum=*/false);
    BOOST_REQUIRE_MESSAGE(parsed, error);
    WalletDescriptor descriptor(std::move(parsed), /*creation_time=*/0, /*range_start=*/0,
                                /*range_end=*/10, /*next_index=*/0);
    {
        LOCK(wallet->cs_wallet);
        wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        auto* spk_man = dynamic_cast<DescriptorScriptPubKeyMan*>(
            wallet->AddWalletDescriptor(descriptor, provider, "", /*internal=*/false));
        BOOST_REQUIRE(spk_man);
        BOOST_REQUIRE(spk_man->AddKey(master.key.GetPubKey().GetID(), master.key, bad_mnemonic, ""));
        wallet->AddActiveScriptPubKeyMan(spk_man->GetID(), /*internal=*/false);
    }

    auto iface = interfaces::MakeWallet(m_context, wallet);
    const auto result{iface->getPlatformPubKey(IdentityAuthKey{0, 0})};
    BOOST_REQUIRE(result);

    ExtKey256 expected;
    BOOST_REQUIRE(DeriveExtKey(master, IdentityAuthKeyPath(static_cast<uint32_t>(Params().ExtCoinType()), 0, 0), expected));
    BOOST_CHECK(result.value == expected.key.GetPubKey());
}

//! DashPay is descriptor-wallet-only: a legacy wallet must be refused even
//! with a perfectly recoverable HD chain seed, so no Platform keys can be
//! created that a later wallet migration would orphan.
BOOST_FIXTURE_TEST_CASE(legacy_wallet_has_no_platform_keys, FriendshipWalletSetup)
{
    auto wallet = std::make_shared<CWallet>(m_node.chain.get(), m_node.coinjoin_loader.get(), "", m_args,
                                            CreateMockWalletDatabase());
    wallet->LoadWallet();
    {
        LOCK(wallet->cs_wallet);
        auto* spk_man = wallet->GetOrCreateLegacyScriptPubKeyMan();
        BOOST_REQUIRE(spk_man);
        spk_man->GenerateNewHDChain(SecureString{DIP14_MNEMONIC}, "");
        // The refusal must come from the wallet type, not a missing seed:
        // the HD chain really holds the DIP-14 seed.
        CHDChain hd_chain;
        BOOST_REQUIRE(spk_man->GetHDChain(hd_chain));
        BOOST_CHECK(hd_chain.GetSeed() == Dip14Seed());
    }

    auto iface = interfaces::MakeWallet(m_context, wallet);
    const auto result{iface->getPlatformPubKey(IdentityAuthKey{0, 0})};
    BOOST_CHECK(result.status == PlatformKeyStatus::NOT_SUPPORTED);
}

//! Two independent wallet instances restored from the same recovery phrase:
//! the second is what a seed-only restore produces (fresh wallet database, no
//! platform records). Everything DashPay derives from the seed must come out
//! identical on both.
struct SeededWalletPair : public Dip14WalletSetup {
    SeededWalletPair() { std::tie(m_wallet_b, m_iface_b) = MakeSeededWallet(SecureString{DIP14_MNEMONIC}); }

    std::shared_ptr<CWallet> m_wallet_b;
    std::unique_ptr<interfaces::Wallet> m_iface_b;
};

//! Identity authentication and funding pubkeys must rederive byte-identical
//! after a seed-only restore, and must equal the raw DIP-13 derivation from
//! the known seed (pinning the interface to the vector-backed path math).
BOOST_FIXTURE_TEST_CASE(recovery_auth_key_rederivation, SeededWalletPair)
{
    const auto coin_type{static_cast<uint32_t>(Params().ExtCoinType())};
    for (uint32_t identity = 0; identity < 2; ++identity) {
        for (uint32_t key = 0; key < 5; ++key) {
            const auto original{m_iface->getPlatformPubKey(IdentityAuthKey{identity, key})};
            const auto restored{m_iface_b->getPlatformPubKey(IdentityAuthKey{identity, key})};
            BOOST_REQUIRE(original);
            BOOST_REQUIRE(restored);
            BOOST_CHECK(original.value == restored.value);

            ExtKey256 expected;
            BOOST_REQUIRE(DeriveExtKey(Dip14Master(), IdentityAuthKeyPath(coin_type, identity, key), expected));
            BOOST_CHECK(original.value == expected.key.GetPubKey());
        }
    }

    const auto funding_original{m_iface->getPlatformPubKey(RegistrationFundingKey{0})};
    const auto funding_restored{m_iface_b->getPlatformPubKey(RegistrationFundingKey{0})};
    BOOST_REQUIRE(funding_original);
    BOOST_REQUIRE(funding_restored);
    BOOST_CHECK(funding_original.value == funding_restored.value);
}

//! Independent BIP32 vectors generated from Dip14Seed() with Python's stdlib
//! HMAC-SHA512 and cryptography's secp256k1 implementation. These pin DIP-13's
//! distinct final-index rules: registration and unbound top-up are normal,
//! while invitations are hardened.
BOOST_AUTO_TEST_CASE(dip13_funding_vectors)
{
    const struct {
        PlatformKeyRequest request;
        const char* expected_key;
        const char* expected_chaincode;
    } vectors[]{
        {RegistrationFundingKey{7}, "804cc77cec9ce9ccf576a9b5978f5da67a86b9307f2686a26fa4cf873bc5645c",
         "5958f765e78779437d7a4b168297202d5153fd0add543097ad929c4bb412f061"},
        {TopupFundingKey{11}, "cd6adc39e772ed2b2186ecc42cbdb5b881fe755db086a9c8a94343815af8784a",
         "43503e58c2e61675d56adaf831b11735acfc5570918d46dad1385c292a8aadc5"},
        {InvitationFundingKey{13}, "4d731224222afd2b28539a8588fbb7579e85b5d6cec9e328a1c5a78f522581a8",
         "b2d0561c5974f9969b39a0d62c041277fbfeaa5e9efd88cf31072a2779d9f254"},
    };
    for (const auto& vector : vectors) {
        ExtKey256 key;
        BOOST_REQUIRE(DeriveExtKey(Dip14Master(), PlatformKeyPath(/*coin_type=*/1, vector.request), key));
        BOOST_CHECK_EQUAL(HexStr(Span{key.key.begin(), key.key.size()}), vector.expected_key);
        BOOST_CHECK_EQUAL(HexStr(key.chaincode), vector.expected_chaincode);
    }
}

//! Compact signatures from signPlatformDigest must recover to the pubkey
//! getPlatformPubKey reports for the same key, on both wallet instances.
BOOST_FIXTURE_TEST_CASE(sign_platform_digest_recovers_to_pubkey, SeededWalletPair)
{
    const uint256 digest{g_insecure_rand_ctx.rand256()};
    const PlatformKeyRequest request{IdentityAuthKey{0, 0}};

    const auto sig_original{m_iface->signPlatformDigest(request, digest)};
    const auto sig_restored{m_iface_b->signPlatformDigest(request, digest)};
    BOOST_REQUIRE(sig_original);
    BOOST_REQUIRE(sig_restored);

    const auto auth_key{m_iface->getPlatformPubKey(request)};
    BOOST_REQUIRE(auth_key);

    CPubKey recovered;
    BOOST_REQUIRE(recovered.RecoverCompact(digest, sig_original.value));
    BOOST_CHECK(recovered == auth_key.value);
    BOOST_REQUIRE(recovered.RecoverCompact(digest, sig_restored.value));
    BOOST_CHECK(recovered == auth_key.value);
}

//! Friendship xpubs and the payment destinations derived from them must
//! rederive identically after restore, and stay direction-sensitive.
BOOST_FIXTURE_TEST_CASE(recovery_friendship_xpub_rederivation, SeededWalletPair)
{
    const FriendshipKeychainRequest request{ACCOUNT, m_my_id, m_their_id, /*birth_time=*/0};
    const auto keychain_a{m_iface->ensureFriendshipReceivingKeychain(request)};
    const auto keychain_b{m_iface_b->ensureFriendshipReceivingKeychain(request)};
    BOOST_REQUIRE(keychain_a);
    BOOST_REQUIRE(keychain_b);
    BOOST_CHECK(keychain_a.value.pubkey == keychain_b.value.pubkey);
    BOOST_CHECK(keychain_a.value.chaincode == keychain_b.value.chaincode);

    for (uint32_t index = 0; index < 10; ++index) {
        BOOST_CHECK(PaymentDestinationVia(*m_iface, m_my_id, m_their_id, index) ==
                    PaymentDestinationVia(*m_iface_b, m_my_id, m_their_id, index));
    }

    const auto reversed{m_iface_b->ensureFriendshipReceivingKeychain(
        FriendshipKeychainRequest{ACCOUNT, m_their_id, m_my_id, /*birth_time=*/0})};
    BOOST_REQUIRE(reversed);
    BOOST_CHECK(reversed.value.pubkey != keychain_a.value.pubkey);
}

//! The ECDH secret used to decrypt incoming contact-request xpubs must
//! survive a restore and agree with the contact's own side — the precondition
//! for recovering contact xpubs from on-chain requests after a seed restore.
BOOST_FIXTURE_TEST_CASE(recovery_ecdh_symmetry_across_restore, SeededWalletPair)
{
    CKey contact_key;
    contact_key.MakeNewKey(/*fCompressed=*/true);

    const auto invalid{m_iface->platformECDHSecret(IdentityAuthKey{0, 0}, CPubKey{})};
    BOOST_CHECK(invalid.status == PlatformKeyStatus::INVALID_ARGUMENT);

    const auto secret_original{m_iface->platformECDHSecret(IdentityAuthKey{0, 0}, contact_key.GetPubKey())};
    const auto secret_restored{m_iface_b->platformECDHSecret(IdentityAuthKey{0, 0}, contact_key.GetPubKey())};
    BOOST_REQUIRE(secret_original);
    BOOST_REQUIRE(secret_restored);
    BOOST_CHECK(secret_original.value == secret_restored.value);

    const auto our_auth_key{m_iface->getPlatformPubKey(IdentityAuthKey{0, 0})};
    BOOST_REQUIRE(our_auth_key);
    SecureVector secret_contact_side;
    BOOST_REQUIRE(ComputeECDHSecret(contact_key, our_auth_key.value, secret_contact_side));
    BOOST_CHECK(secret_contact_side == secret_original.value);
}

//! A payment received before the loss must become spendable again once the
//! restored wallet re-imports the friendship keychain: the recovery contract
//! for DIP-15 funds.
BOOST_FIXTURE_TEST_CASE(recovery_import_after_restore_is_spendable, SeededWalletPair)
{
    // The payment targets the xpub the contact holds — exported by wallet A
    // before the loss.
    const auto original_keychain{m_iface->ensureFriendshipReceivingKeychain(
        FriendshipKeychainRequest{ACCOUNT, m_my_id, m_their_id, /*birth_time=*/0})};
    BOOST_REQUIRE(original_keychain);
    CTxDestination destination;
    BOOST_REQUIRE(DeriveFriendshipPaymentDestination(original_keychain.value, /*index=*/0, destination));
    const CScript script{GetScriptForDestination(destination)};
    CMutableTransaction payment;
    payment.vin.emplace_back(g_insecure_rand_ctx.rand256(), 0);
    payment.vout.emplace_back(COIN, script);

    // Restored wallet B: not ours until the friendship keychain is back.
    BOOST_CHECK_EQUAL(IsMineIn(*m_wallet_b, destination), ISMINE_NO);

    const auto imported{m_iface_b->ensureFriendshipReceivingKeychain(
        FriendshipKeychainRequest{ACCOUNT, m_my_id, m_their_id, /*birth_time=*/0})};
    BOOST_REQUIRE(imported);

    LOCK(m_wallet_b->cs_wallet);
    m_wallet_b->AddToWallet(MakeTransactionRef(payment), TxStateInMempool{});

    CCoinControl coin_control;
    coin_control.m_include_unsafe_inputs = true;
    BOOST_CHECK_EQUAL(AvailableCoins(*m_wallet_b, &coin_control).Size(), 1U);
}

//! Recovery re-runs re-import friendships it already imported; that must
//! update in place rather than duplicate spk_mans or change ownership.
BOOST_FIXTURE_TEST_CASE(recovery_import_idempotent, SeededWalletPair)
{
    const FriendshipKeychainRequest request{ACCOUNT, m_my_id, m_their_id, /*birth_time=*/0};
    BOOST_REQUIRE(m_iface->ensureFriendshipReceivingKeychain(request));
    size_t spk_mans_after_first;
    {
        LOCK(m_wallet->cs_wallet);
        spk_mans_after_first = m_wallet->GetAllScriptPubKeyMans().size();
    }

    BOOST_REQUIRE(m_iface->ensureFriendshipReceivingKeychain(
        FriendshipKeychainRequest{ACCOUNT, m_my_id, m_their_id, /*birth_time=*/1'600'000'000}));
    {
        LOCK(m_wallet->cs_wallet);
        BOOST_CHECK_EQUAL(m_wallet->GetAllScriptPubKeyMans().size(), spk_mans_after_first);
    }
    for (uint32_t index = 0; index < 5; ++index) {
        BOOST_CHECK_EQUAL(IsMine(PaymentDestination(m_my_id, m_their_id, index)), ISMINE_SPENDABLE);
    }

    // Normal use after import: observing a payment to index 0 advances the
    // descriptor's next_index and lets TopUp() grow its range past the
    // initial one.
    const CScript used_script{GetScriptForDestination(PaymentDestination(m_my_id, m_their_id, 0))};
    DescriptorScriptPubKeyMan* friendship_spk_man{nullptr};
    {
        LOCK(m_wallet->cs_wallet);
        WalletBatch batch{m_wallet->GetDatabase()};
        for (auto* spk_man : m_wallet->GetScriptPubKeyMans(used_script)) {
            friendship_spk_man = dynamic_cast<DescriptorScriptPubKeyMan*>(spk_man);
            if (!friendship_spk_man) continue;
            spk_man->MarkUnusedAddresses(batch, used_script, std::nullopt);
            BOOST_REQUIRE(spk_man->TopUp());
            break;
        }
    }
    BOOST_REQUIRE(friendship_spk_man);
    int32_t used_next_index;
    int32_t used_range_end;
    {
        LOCK(friendship_spk_man->cs_desc_man);
        const auto used_descriptor{friendship_spk_man->GetWalletDescriptor()};
        used_next_index = used_descriptor.next_index;
        used_range_end = used_descriptor.range_end;
    }
    BOOST_REQUIRE_EQUAL(used_next_index, 1);
    BOOST_REQUIRE_GT(used_range_end, 1000);

    // Re-import after use must still succeed and keep the grown range,
    // derivation progress, and the earliest creation time.
    BOOST_REQUIRE(m_iface->ensureFriendshipReceivingKeychain(
        FriendshipKeychainRequest{ACCOUNT, m_my_id, m_their_id, /*birth_time=*/1'700'000'000}));
    {
        LOCK(friendship_spk_man->cs_desc_man);
        const auto reimported{friendship_spk_man->GetWalletDescriptor()};
        BOOST_CHECK_EQUAL(reimported.next_index, used_next_index);
        BOOST_CHECK_EQUAL(reimported.range_end, used_range_end);
        BOOST_CHECK_EQUAL(reimported.creation_time, 0);
    }
}

BOOST_AUTO_TEST_SUITE_END()
