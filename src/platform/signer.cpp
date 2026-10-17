// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/signer.h>

#include <hash.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <pubkey.h>
#include <uint256.h>
#include <util/time.h>

#include <algorithm>
#include <chrono>
#include <variant>

namespace platform {

namespace {

constexpr uint8_t KEY_TYPE_ECDSA_SECP256K1{static_cast<uint8_t>(IdentityPublicKey::Type::ECDSA_SECP256K1)};

bool Sign(interfaces::Wallet& wallet, const wallet::PlatformKeyRequest& key, const uint256& digest,
          std::array<uint8_t, 65>& sig_out)
{
    const auto signature{wallet.signPlatformDigest(key, digest)};
    if (!signature || signature.value.size() != sig_out.size()) return false;
    std::copy(signature.value.begin(), signature.value.end(), sig_out.begin());
    return true;
}

} // namespace

std::unique_ptr<dash_sdk::WalletSigner> MakeWalletSigner(interfaces::Wallet& wallet, uint32_t identity_index,
                                                         std::optional<wallet::AssetLockFundingKey> funding_key)
{
    return std::make_unique<dash_sdk::WalletSigner>(
        [&wallet, identity_index](uint32_t key_id, uint8_t key_type, std::span<const uint8_t> /*key_data*/,
                                  std::span<const uint8_t> signable, std::array<uint8_t, 65>& sig_out) {
            return key_type == KEY_TYPE_ECDSA_SECP256K1 &&
                   Sign(wallet, wallet::IdentityAuthKey{identity_index, key_id}, Hash(signable), sig_out);
        },
        [&wallet, funding_key](uint32_t /*key_ref*/, const std::array<uint8_t, 32>& sighash,
                               std::array<uint8_t, 65>& sig_out) {
            if (!funding_key) return false;
            const auto key{std::visit([](auto k) -> wallet::PlatformKeyRequest { return k; }, *funding_key)};
            return Sign(wallet, key, uint256{sighash}, sig_out);
        });
}

std::unique_ptr<dash_sdk::ContactKeys> MakeContactKeys(interfaces::Wallet& wallet, uint32_t identity_index,
                                                       const Identifier& sender)
{
    return std::make_unique<dash_sdk::ContactKeys>(
        [&wallet, identity_index](uint32_t sender_key_id, const std::array<uint8_t, 33>& recipient_key,
                                  std::array<uint8_t, 32>& secret_out) {
            const auto secret{wallet.platformECDHSecret(wallet::IdentityAuthKey{identity_index, sender_key_id},
                                                        CPubKey{recipient_key})};
            if (!secret || secret.value.size() != secret_out.size()) return false;
            std::copy(secret.value.begin(), secret.value.end(), secret_out.begin());
            return true;
        },
        [&wallet, sender](const std::array<uint8_t, 32>& recipient, uint32_t /*account_reference*/,
                          std::array<uint8_t, 69>& xpub_out) {
            // Account 0, as DashPay iOS sends it. No payment to this chain
            // can predate its publication, so it is born now.
            const auto keychain{wallet.ensureFriendshipReceivingKeychain(
                {/*account=*/0, uint256{sender}, uint256{recipient},
                 /*birth_time=*/TicksSinceEpoch<std::chrono::seconds>(NodeClock::now())})};
            return keychain && wallet::CompactXpubBytes(keychain.value, xpub_out);
        });
}

std::unique_ptr<dash_sdk::CoreContext> MakeCoreContext(interfaces::LLMQ& llmq, uint8_t platform_llmq_type)
{
    return std::make_unique<dash_sdk::CoreContext>(
        [&llmq, platform_llmq_type](uint8_t llmq_type, const std::array<uint8_t, 32>& quorum_hash,
                                    uint32_t /*core_chain_locked_height*/, std::vector<uint8_t>& key_out) {
            if (llmq_type != platform_llmq_type) return false;
            const uint256 hash{quorum_hash};
            for (auto& quorum : llmq.getPlatformQuorums(llmq_type)) {
                if (quorum.m_quorum_hash == hash) {
                    key_out = std::move(quorum.m_pubkey);
                    return true;
                }
            }
            return false;
        },
        dash_sdk::CoreContext::PlatformActivationHeightFn{});
}

} // namespace platform
