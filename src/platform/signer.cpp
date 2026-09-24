// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/signer.h>

#include <hash.h>
#include <interfaces/wallet.h>
#include <logging.h>
#include <uint256.h>

#include <algorithm>

namespace platform {

namespace {

constexpr size_t COMPACT_SIGNATURE_SIZE{65};

const char* KindName(OperationKind kind)
{
    switch (kind) {
    case OperationKind::IDENTITY_CREATE:
        return "identity create";
    case OperationKind::DPNS_PREORDER:
        return "dpns preorder";
    case OperationKind::DPNS_DOMAIN:
        return "dpns domain";
    case OperationKind::PROFILE:
        return "profile";
    case OperationKind::CONTACT_REQUEST:
        return "contact request";
    }
    return "unknown";
}

uint8_t ExpectedVariant(OperationKind kind)
{
    return kind == OperationKind::IDENTITY_CREATE ? STATE_TRANSITION_VARIANT_IDENTITY_CREATE
                                                  : STATE_TRANSITION_VARIANT_BATCH;
}

} // namespace

SigningOperation::SigningOperation(interfaces::Wallet& wallet, OperationKind kind, std::vector<uint32_t> key_ids,
                                   std::optional<IdentityPublicKey> document_key,
                                   std::optional<wallet::RegistrationFundingKey> funding_key,
                                   std::unique_ptr<UnlockScope> unlock) :
    m_wallet(wallet),
    m_kind(kind),
    m_key_ids(std::move(key_ids)),
    m_document_key(std::move(document_key)),
    m_funding_key(funding_key),
    m_unlock(std::move(unlock))
{
}

bool SigningOperation::allowsKey(uint32_t key_id) const
{
    return std::find(m_key_ids.begin(), m_key_ids.end(), key_id) != m_key_ids.end();
}

bool SigningOperation::claimAssetLockSignature() const
{
    if (!m_funding_key || m_asset_lock_signed) return false;
    m_asset_lock_signed = true;
    return true;
}

bool WalletSigner::signForKey(uint32_t key_id, Span<const uint8_t> signable, std::vector<uint8_t>& signature_out) const
{
    const OperationKind kind{m_operation.kind()};
    if (!m_operation.allowsKey(key_id)) {
        LogPrintf("Platform signer: refusing key %u outside the %s operation\n", key_id, KindName(kind));
        return false;
    }
    if (signable.empty() || signable[0] != ExpectedVariant(kind)) {
        LogPrintf("Platform signer: refusing a preimage of %u bytes that is not a %s transition\n", signable.size(),
                  KindName(kind));
        return false;
    }
    uint256 digest;
    CHash256().Write(signable).Finalize(digest);
    auto result{m_operation.wallet().signPlatformDigest(wallet::IdentityAuthKey{0, key_id}, digest)};
    if (!result || result.value.size() != COMPACT_SIGNATURE_SIZE) {
        LogPrintf("Platform signer: the wallet did not sign the %s transition with key %u (status %d)\n",
                  KindName(kind), key_id, static_cast<int>(result.status));
        return false;
    }
    LogPrint(BCLog::PLATFORM, "Platform signer: signed %s transition (%u bytes) with key %u\n", KindName(kind),
             signable.size(), key_id);
    signature_out = std::move(result.value);
    return true;
}

bool WalletSigner::signAssetLockSighash(const std::array<uint8_t, 32>& sighash, std::vector<uint8_t>& signature_out) const
{
    const auto& funding_key{m_operation.fundingKey()};
    if (!m_operation.claimAssetLockSignature()) {
        LogPrintf("Platform signer: refusing a%s asset lock signature for the %s operation\n",
                  funding_key ? " second" : "n", KindName(m_operation.kind()));
        return false;
    }
    auto result{m_operation.wallet().signPlatformDigest(*funding_key, uint256{sighash})};
    if (!result || result.value.size() != COMPACT_SIGNATURE_SIZE) {
        LogPrintf("Platform signer: the wallet did not sign the asset lock (status %d)\n", static_cast<int>(result.status));
        return false;
    }
    LogPrint(BCLog::PLATFORM, "Platform signer: signed the asset lock sighash with funding key %u\n",
             funding_key->identity_index);
    signature_out = std::move(result.value);
    return true;
}

} // namespace platform
