// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/client.h>
#include <platform/signer.h>

#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <pubkey.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <wallet/context.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace platform;

namespace {

//! The mnemonic of platformkeys_tests' DIP-14 seed.
const SecureString MNEMONIC{"birth kingdom trash renew flavor utility donkey gasp regular alert pave layer"};

struct PlatformClientSetup : public TestingSetup {
    PlatformClientSetup()
    {
        m_context.args = &m_args;
        m_context.chain = m_node.chain.get();
        m_wallet = std::make_shared<wallet::CWallet>(m_node.chain.get(), "", wallet::CreateMockWalletDatabase());
        m_wallet->LoadWallet();
        {
            LOCK(m_wallet->cs_wallet);
            m_wallet->SetWalletFlag(wallet::WALLET_FLAG_DESCRIPTORS);
            m_wallet->SetupDescriptorScriptPubKeyMans(MNEMONIC, "");
        }
        m_iface = interfaces::MakeWallet(m_context, m_wallet);
    }

    wallet::WalletContext m_context;
    std::shared_ptr<wallet::CWallet> m_wallet;
    std::unique_ptr<interfaces::Wallet> m_iface;
};

//! The consumer's thread: tasks the client posts wait here until the test
//! runs them.
class TestExecutor
{
public:
    Executor executor()
    {
        return [this](std::function<void()> task) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex) {
            LOCK(m_mutex);
            m_tasks.push_back(std::move(task));
        };
    }

    size_t pending() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        return m_tasks.size();
    }

    //! Wait until n tasks are posted.
    bool WaitFor(size_t n) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        for (int i = 0; i < 1000 && pending() < n; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        return pending() >= n;
    }

    //! Run and destroy every posted task on this thread.
    void Drain() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        std::vector<std::function<void()>> tasks;
        WITH_LOCK(m_mutex, tasks.swap(m_tasks));
        for (auto& task : tasks)
            task();
    }

private:
    Mutex m_mutex;
    std::vector<std::function<void()>> m_tasks GUARDED_BY(m_mutex);
};

//! Records the thread it is released on.
class TestUnlock final : public UnlockScope
{
public:
    explicit TestUnlock(std::optional<std::thread::id>& released_on) :
        m_released_on(released_on)
    {
    }
    ~TestUnlock() override { m_released_on = std::this_thread::get_id(); }

private:
    std::optional<std::thread::id>& m_released_on;
};

class TestLLMQ final : public interfaces::LLMQ
{
public:
    std::vector<PlatformQuorum> quorums;

    ChainLockInfo getBestChainLock() override { return {}; }
    CreditPoolCounts getCreditPoolCounts() override { return {}; }
    InstantSendCounts getInstantSendCounts() override { return {}; }
    size_t getPendingAssetUnlocks() override { return 0; }
    std::vector<QuorumInfo> getQuorumStats() override { return {}; }
    std::vector<PlatformQuorum> getPlatformQuorums(uint8_t) override { return quorums; }
    std::vector<uint8_t> getInstantSendLock(const uint256&) override { return {}; }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(platform_client_tests, PlatformClientSetup)

//! Through the real client with no endpoints: callbacks run on the
//! consumer's executor, never on a worker, a write's unlock scope being
//! released there just before its callback; shutdown() waits for a call
//! that is inside a wallet callback and releases its unlock scope; and no
//! callback runs after shutdown().
BOOST_AUTO_TEST_CASE(client_threads_and_shutdown)
{
    TestLLMQ llmq;
    TestExecutor executor;
    auto client{MakeSdkPlatformClient(ClientConfig{}, llmq, executor.executor())};
    BOOST_REQUIRE(client);
    const auto consumer{std::this_thread::get_id()};

    // Results wait for the executor; a write keeps the wallet unlocked until
    // its result is delivered.
    std::optional<StatusKind> read_status;
    std::optional<std::thread::id> read_thread;
    client->getIdentity(Identifier{}, [&](Result<Identity> result) {
        read_status = result.status.kind;
        read_thread = std::this_thread::get_id();
    });
    std::optional<std::thread::id> unlock_released_on;
    std::optional<std::thread::id> unlock_released_before_callback;
    client->registerName("alice", Identifier{}, /*key_id=*/1,
                         Signing{*m_iface, 0, std::make_unique<TestUnlock>(unlock_released_on)},
                         [&](Result<Identifier>) { unlock_released_before_callback = unlock_released_on; });
    BOOST_REQUIRE(executor.WaitFor(2));
    BOOST_CHECK(!read_status);
    BOOST_CHECK(!unlock_released_on);
    executor.Drain();
    BOOST_CHECK(read_status == StatusKind::UNAVAILABLE);
    BOOST_CHECK(read_thread == consumer);
    BOOST_CHECK(unlock_released_before_callback == consumer);

    // A registration blocked in the wallet signer, which needs cs_wallet:
    // shutdown() must not return before the holder releases it.
    const auto pubkey{m_iface->getPlatformPubKey(wallet::IdentityAuthKey{0, 0})};
    BOOST_REQUIRE(pubkey);
    IdentityPublicKey key;
    key.data.assign(pubkey.value.begin(), pubkey.value.end());
    std::promise<void> locked;
    std::atomic_bool releasing{false};
    std::thread holder{[&] {
        LOCK(m_wallet->cs_wallet);
        locked.set_value();
        // Longer than the SDK runtime's own 2 s stop timeout.
        std::this_thread::sleep_for(std::chrono::milliseconds{3500});
        releasing = true;
    }};
    locked.get_future().wait();
    unlock_released_on.reset();
    bool write_called{false};
    client->registerIdentity({key}, ChainAssetLockProof{},
                             Signing{*m_iface, 0, std::make_unique<TestUnlock>(unlock_released_on)},
                             [&](Result<Identity>) { write_called = true; });
    // Time for a worker to reach the signer; there is no network to wait on.
    std::this_thread::sleep_for(std::chrono::seconds{1});
    client->shutdown();
    BOOST_CHECK(releasing);
    holder.join();

    // shutdown() released the write's unlock here; its result, handed to the
    // executor, no longer calls back.
    BOOST_CHECK(unlock_released_on == consumer);
    executor.Drain();
    BOOST_CHECK(!write_called);

    // Calls after shutdown() are dropped on the calling thread.
    unlock_released_on.reset();
    client->registerName("alice", Identifier{}, 1, Signing{*m_iface, 0, std::make_unique<TestUnlock>(unlock_released_on)},
                         [&](Result<Identifier>) { write_called = true; });
    BOOST_CHECK(unlock_released_on == consumer);
    BOOST_CHECK_EQUAL(executor.pending(), 0U);
    BOOST_CHECK(!write_called);
}

//! The signer answers with the wallet's RFC6979 compact signature over
//! SHA256d(signable) for each published identity key, by identity index,
//! and over the sighash for the asset lock's funding key. Expected values
//! are computed independently in Python (stdlib HMAC-SHA512 BIP32 and the
//! functional test framework's secp256k1) from the DIP-14 seed, on the
//! mainnet coin type that TestingSetup's chain uses.
BOOST_AUTO_TEST_CASE(wallet_signer_vectors)
{
    std::vector<uint8_t> signable(64);
    for (size_t i = 0; i < signable.size(); ++i)
        signable[i] = i;
    std::array<uint8_t, 65> sig;

    const struct {
        uint32_t identity_index;
        uint32_t key_id;
        const char* signature;
    } identity_keys[]{
        {0, 0,
         "2001450b14681ad74139462b838ff42b46f4f742a1e41134949084a10b62589c64471aed532f76c7d5878439baf57166f98ad85c0358a"
         "5fa59ea65c8c196fc2e22"},
        {0, 1,
         "1ff1882b86979e490de505496ef06d0c79d91f2deccd3e2bd575ddb0f0ab0481bc508d0e4f79611fc15d4e779cd9c19d8f5291a0255bf"
         "475f37d9ef662971b5412"},
        {0, 2,
         "1ffed3a6bcc4cec33c32a9149c7c94c43643a2c0a58d7e55a57cba85b817331a1910715c1670978a4ce8e1f6d1fa062cdf6cf40e54a8f"
         "941711dde613a0e9d002e"},
        {0, 3,
         "1fc40d13035242431cfb237c0e68bfbd0b3246600ee216b2c5e279158f54a1af276173fc25abfa25ddbd0486f7857d3e61b74a9c228b1"
         "3a51b070937b37de57d7d"},
        {1, 0,
         "20b4be2547b6d1a826da05a0b230b4b95ec333341662b2a09fbf2d429b5ad689392dffd24624290e53efe75a58af9f025ad97817365cc"
         "ec1a495a73341301d2e2d"},
    };
    for (const auto& row : identity_keys) {
        const auto signer{MakeWalletSigner(*m_iface, row.identity_index, std::nullopt)};
        BOOST_REQUIRE(signer->SignIdentityKey(row.key_id, /*key_type=*/0, {}, {signable.data(), signable.size()}, sig));
        BOOST_CHECK_EQUAL(HexStr(sig), row.signature);
    }

    std::array<uint8_t, 32> sighash;
    sighash.fill(0x42);
    const auto registration{MakeWalletSigner(*m_iface, 0, wallet::RegistrationFundingKey{0})};
    BOOST_REQUIRE(registration->SignAssetLock(/*key_ref=*/0, sighash, sig));
    BOOST_CHECK_EQUAL(HexStr(sig),
                      "207982873b927f462833f7ac2a5ddfa56d4f5a224a2f594be99f737530455d5b682da131a012cd943ce5"
                      "e5a85534814189777b5f6be1de65579eb568d247599c1c");
    const auto top_up{MakeWalletSigner(*m_iface, 0, wallet::TopupFundingKey{3})};
    BOOST_REQUIRE(top_up->SignAssetLock(/*key_ref=*/3, sighash, sig));
    BOOST_CHECK_EQUAL(HexStr(sig),
                      "1fe4adc78e2d294bd18f1020be29e6589eb7a5f8e98548097b11e9ff01587e284e3a097070bd7a6ccdbc"
                      "dc1a11c608c8248e1cef5fc34d65d6861fc4a25807566a");

    // A write without an asset lock has no funding key to sign with, and
    // no ECDSA_HASH160 key is signed with yet.
    const auto document{MakeWalletSigner(*m_iface, 0, std::nullopt)};
    BOOST_CHECK(!document->SignAssetLock(0, sighash, sig));
    BOOST_CHECK(!document->SignIdentityKey(0, /*key_type=*/2, {}, {signable.data(), signable.size()}, sig));

    // ECDH with our ENCRYPTION key (identity 0, key 2) and the recipient key
    // 7*G: SHA256 of the compressed shared point.
    const auto contact{MakeContactKeys(*m_iface, 0, Identifier{})};
    std::array<uint8_t, 33> recipient_key;
    const auto recipient{ParseHex("025cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bc")};
    std::copy(recipient.begin(), recipient.end(), recipient_key.begin());
    std::array<uint8_t, 32> secret;
    BOOST_REQUIRE(contact->SharedSecret(/*sender_key_id=*/2, recipient_key, secret));
    BOOST_CHECK_EQUAL(HexStr(secret), "893d3a804b2252c11400aeddcf83599fc9f3ac8b6067729c1c95455745cd410f");
}

//! The compact xpub published to a recipient is our receiving chain from the
//! sender towards that recipient, not the reverse.
BOOST_AUTO_TEST_CASE(contact_xpub_is_our_receiving_chain)
{
    Identifier sender, recipient;
    sender.fill(0x11);
    recipient.fill(0x22);
    const auto contact{MakeContactKeys(*m_iface, 0, sender)};
    std::array<uint8_t, 69> xpub;
    BOOST_REQUIRE(contact->CompactXpub(recipient, /*account_reference=*/0, xpub));

    const auto chain{m_iface->ensureFriendshipReceivingKeychain(
        {/*account=*/0, uint256{sender}, uint256{recipient}, /*birth_time=*/0})};
    BOOST_REQUIRE(chain);
    wallet::CompactXpub expected;
    BOOST_REQUIRE(wallet::CompactXpubBytes(chain.value, expected));
    BOOST_CHECK(xpub == expected);
}

//! A quorum key is answered only for the Platform LLMQ type and a hash the
//! node retains.
BOOST_AUTO_TEST_CASE(core_context_matches_type_and_hash)
{
    TestLLMQ llmq;
    const uint256 hash{uint256S("0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20")};
    llmq.quorums.push_back({hash, std::vector<uint8_t>(48, 0xab), 100});
    const auto context{MakeCoreContext(llmq, /*platform_llmq_type=*/6)};

    std::array<uint8_t, 32> quorum_hash;
    std::copy(hash.begin(), hash.end(), quorum_hash.begin());
    std::array<uint8_t, 48> key;
    BOOST_REQUIRE(context->QuorumPublicKey(6, quorum_hash, 0, key));
    BOOST_CHECK(std::all_of(key.begin(), key.end(), [](uint8_t b) { return b == 0xab; }));

    BOOST_CHECK(!context->QuorumPublicKey(5, quorum_hash, 0, key));
    quorum_hash[0] ^= 1;
    BOOST_CHECK(!context->QuorumPublicKey(6, quorum_hash, 0, key));
    uint32_t height;
    BOOST_CHECK(!context->PlatformActivationHeight(height));
}

//! An unusable proxy gives no client: it never falls back to a direct
//! connection.
BOOST_AUTO_TEST_CASE(unusable_proxy_gives_no_client)
{
    TestLLMQ llmq;
    TestExecutor executor;
    ClientConfig config;
    config.proxy = ProxyConfig{std::string{}, true};
    BOOST_CHECK(!MakeSdkPlatformClient(config, llmq, executor.executor()));
}

BOOST_AUTO_TEST_SUITE_END()
