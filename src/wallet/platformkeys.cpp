// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/platformkeys.h>

#include <crypto/hmac_sha256.h>
#include <secp256k1.h>
#include <secp256k1_ecdh.h>

#include <algorithm>
#include <cassert>
#include <type_traits>

namespace wallet::platformkeys {

Path IdentityAuthKeyPath(uint32_t coin_type, uint32_t identity_index, uint32_t key_index)
{
    // dashj DerivationPathFactory.blockchainIdentityECDSADerivationPath(index):
    // m/9'/coin'/5'/0'(sub-feature)/0'(key type ECDSA)/identity'/key'
    return {
        PathElement::Hardened(FEATURE_PURPOSE),
        PathElement::Hardened(coin_type),
        PathElement::Hardened(FEATURE_IDENTITIES),
        PathElement::Hardened(IDENTITY_AUTHENTICATION),
        PathElement::Hardened(AUTH_KEY_TYPE_ECDSA),
        PathElement::Hardened(identity_index),
        PathElement::Hardened(key_index),
    };
}

Path PlatformKeyPath(uint32_t coin_type, const PlatformKeyRequest& request)
{
    return std::visit(
        [coin_type](const auto& key) -> Path {
            using Key = std::decay_t<decltype(key)>;
            if constexpr (std::is_same_v<Key, IdentityAuthKey>) {
                return IdentityAuthKeyPath(coin_type, key.identity_index, key.key_index);
            } else {
                uint32_t subfeature;
                PathElement index;
                if constexpr (std::is_same_v<Key, RegistrationFundingKey>) {
                    subfeature = IDENTITY_REGISTRATION_FUNDING;
                    index = PathElement::Normal(key.identity_index);
                } else if constexpr (std::is_same_v<Key, TopupFundingKey>) {
                    subfeature = IDENTITY_TOPUP_FUNDING;
                    index = PathElement::Normal(key.funding_index);
                } else {
                    static_assert(std::is_same_v<Key, InvitationFundingKey>);
                    subfeature = IDENTITY_INVITATION_FUNDING;
                    index = PathElement::Hardened(key.invitation_index);
                }
                return {
                    PathElement::Hardened(FEATURE_PURPOSE),
                    PathElement::Hardened(coin_type),
                    PathElement::Hardened(FEATURE_IDENTITIES),
                    PathElement::Hardened(subfeature),
                    index,
                };
            }
        },
        request);
}

Path FriendshipPath(uint32_t coin_type, uint32_t account, Span<const uint8_t> user_a_id, Span<const uint8_t> user_b_id)
{
    // dashj FriendKeyChain.getContactPath():
    // m/9'/coin'/15'/account'/identity_a/identity_b with two 256-bit ids
    // NOT hardened (DIP-14/DIP-15), enabling watch-only xpub derivation.
    assert(user_a_id.size() == 32);
    assert(user_b_id.size() == 32);
    std::array<uint8_t, 32> a, b;
    std::copy(user_a_id.begin(), user_a_id.end(), a.begin());
    std::copy(user_b_id.begin(), user_b_id.end(), b.begin());
    return {
        PathElement::Hardened(FEATURE_PURPOSE),
        PathElement::Hardened(coin_type),
        PathElement::Hardened(FEATURE_DASHPAY),
        PathElement::Hardened(account),
        PathElement::Normal256(a),
        PathElement::Normal256(b),
    };
}

bool DeriveExtKey(const CExtKey& parent, const Path& path, ExtKey256& out)
{
    out = {};
    if (!parent.key.IsValid()) return false;

    CKey key{parent.key};
    ChainCode chaincode{parent.chaincode};
    std::array<uint8_t, 4> parent_fingerprint{};

    for (size_t i{0}; i < path.size(); ++i) {
        const auto& element{path[i]};
        if (i + 1 == path.size()) {
            const CKeyID parent_id{key.GetPubKey().GetID()};
            std::copy_n(parent_id.begin(), parent_fingerprint.size(), parent_fingerprint.begin());
        }

        CKey child_key;
        ChainCode child_cc;
        bool ok{false};
        if (const auto* index32 = std::get_if<uint32_t>(&element.index)) {
            if (*index32 >> 31) return false; // must use the hardened flag instead
            ok = key.Derive(child_key, child_cc, *index32 | (element.hardened ? 0x80000000u : 0), chaincode);
        } else {
            const auto& index256 = std::get<std::array<uint8_t, 32>>(element.index);
            ok = key.Derive256(child_key, child_cc, index256, element.hardened, chaincode);
        }
        if (!ok) return false;
        key = child_key;
        chaincode = child_cc;
    }

    out.key = key;
    out.chaincode = chaincode;
    out.parent_fingerprint = parent_fingerprint;
    return true;
}

bool DerivePubKey(const ExtPubKey256& parent, const PathElement& element, ExtPubKey256& out)
{
    // CPubKey::Derive() asserts a valid compressed parent; contact xpubs are
    // externally supplied, so reject them here instead. A syntactically
    // compressed but invalid curve point is caught by pubkey parsing inside.
    if (element.hardened || !parent.pubkey.IsCompressed()) return false;
    if (const auto* index32 = std::get_if<uint32_t>(&element.index)) {
        if (*index32 >> 31) return false;
        return parent.pubkey.Derive(out.pubkey, out.chaincode, *index32, parent.chaincode);
    }
    const auto& index256 = std::get<std::array<uint8_t, 32>>(element.index);
    return parent.pubkey.Derive256(out.pubkey, out.chaincode, index256, parent.chaincode);
}

bool ComputeECDHSecret(const CKey& key, const CPubKey& counterparty, SecureVector& secret_out)
{
    if (!key.IsValid() || !counterparty.IsValid()) return false;

    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_parse(secp256k1_context_static, &pubkey, counterparty.data(), counterparty.size())) {
        return false;
    }

    secret_out.assign(32, 0);
    // Default KDF: SHA256 of the compressed shared point — identical to
    // dashj's Secp256k1ECDHAgreement (DashPay contact request encryption).
    if (!secp256k1_ecdh(secp256k1_context_static, secret_out.data(), &pubkey,
                        UCharCast(key.begin()), nullptr, nullptr)) {
        secret_out.clear();
        return false;
    }
    return true;
}

bool ComputeAccountReferenceMac(const CKey& key, const CompactXpub& compact_xpub, uint256& mac_out)
{
    if (!key.IsValid()) return false;
    CHMAC_SHA256{key.begin(), key.size()}.Write(compact_xpub.data(), compact_xpub.size()).Finalize(mac_out.begin());
    return true;
}

} // namespace wallet::platformkeys

namespace wallet {

bool CompactXpubBytes(const FriendshipXpub& xpub, CompactXpub& compact_out)
{
    if (!xpub.pubkey.IsCompressed()) return false;
    auto it{std::copy(xpub.parent_fingerprint.begin(), xpub.parent_fingerprint.end(), compact_out.begin())};
    it = std::copy(xpub.chaincode.begin(), xpub.chaincode.end(), it);
    std::copy(xpub.pubkey.begin(), xpub.pubkey.end(), it);
    return true;
}

bool DeriveFriendshipPaymentDestination(const FriendshipXpub& xpub, uint32_t index, CTxDestination& destination_out)
{
    platformkeys::ExtPubKey256 child;
    if (!platformkeys::DerivePubKey({xpub.pubkey, xpub.chaincode}, platformkeys::PathElement::Normal(index), child)) {
        return false;
    }
    destination_out = PKHash{child.pubkey};
    return true;
}

} // namespace wallet
