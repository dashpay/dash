// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/contactflow.h>

#include <crypto/aes.h>
#include <interfaces/wallet.h>
#include <platform/client.h>
#include <platform/statetransitions.h>
#include <qt/platform/platformservice.h>
#include <qt/walletmodel.h>
#include <random.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <wallet/platformtypes.h>

#include <QPointer>
#include <QTimer>

#include <algorithm>

using interfaces::Wallet;

namespace {
//! DashPay contactRequest encryptedPublicKey is exactly IV(16) || AES-256-CBC
//! ciphertext of the 65-byte serialized contact xpub, fixed at 96 bytes by
//! the DashPay v1 schema.
constexpr size_t ENCRYPTED_XPUB_SIZE{96};
} // namespace

ContactFlow::ContactFlow(PlatformService& service, QObject* parent) :
    QObject(parent),
    m_service(service)
{
}

bool ContactFlow::encryptXpub(const platform::IdentityPublicKey& their_key,
                              const std::vector<uint8_t>& our_xpub, std::vector<uint8_t>& out) const
{
    Wallet& wallet{m_service.walletModel().wallet()};
    CPubKey counterparty{their_key.data.begin(), their_key.data.end()};
    if (!counterparty.IsValid()) return false;

    // Our identity authentication key 0 (sender_key_index) performs the ECDH.
    const auto secret{wallet.platformECDHSecret(wallet::IdentityAuthKey{/*identity_index=*/0, /*key_index=*/0},
                                                counterparty)};
    if (!secret || secret.value.size() != AES256_KEYSIZE) {
        return false;
    }

    unsigned char iv[AES_BLOCKSIZE];
    GetStrongRandBytes(Span{iv});

    // AES-256-CBC with PKCS7 padding over the serialized xpub.
    AES256CBCEncrypt enc(secret.value.data(), iv, /*padIn=*/true);
    std::vector<uint8_t> cipher(our_xpub.size() + AES_BLOCKSIZE);
    const int written = enc.Encrypt(our_xpub.data(), our_xpub.size(), cipher.data());
    if (written <= 0) return false;
    cipher.resize(written);

    out.clear();
    out.insert(out.end(), iv, iv + AES_BLOCKSIZE);
    out.insert(out.end(), cipher.begin(), cipher.end());
    return out.size() == ENCRYPTED_XPUB_SIZE;
}

bool ContactFlow::decryptXpub(uint32_t our_key_id, const std::vector<uint8_t>& their_pubkey,
                              const std::vector<uint8_t>& encrypted, std::vector<uint8_t>& out) const
{
    if (encrypted.size() != ENCRYPTED_XPUB_SIZE) return false;
    Wallet& wallet{m_service.walletModel().wallet()};
    CPubKey counterparty{their_pubkey.begin(), their_pubkey.end()};
    if (!counterparty.IsValid()) return false;

    const auto secret{wallet.platformECDHSecret(wallet::IdentityAuthKey{/*identity_index=*/0, our_key_id},
                                                counterparty)};
    if (!secret || secret.value.size() != AES256_KEYSIZE) {
        return false;
    }

    unsigned char iv[AES_BLOCKSIZE];
    std::copy(encrypted.begin(), encrypted.begin() + AES_BLOCKSIZE, iv);
    AES256CBCDecrypt dec(secret.value.data(), iv, /*padIn=*/true);
    std::vector<uint8_t> plain(encrypted.size());
    const int written = dec.Decrypt(encrypted.data() + AES_BLOCKSIZE, encrypted.size() - AES_BLOCKSIZE, plain.data());
    if (written <= 0) return false;
    plain.resize(written);
    out = std::move(plain);
    return true;
}

void ContactFlow::sendRequest(const platform::Identifier& to_identity, uint32_t recipient_key_id,
                              const platform::IdentityPublicKey& recipient_key)
{
    auto my_id{m_service.myIdentityId()};
    if (!my_id) {
        Q_EMIT requestFailed(QString::fromStdString(HexStr(to_identity)), tr("register a username first"));
        return;
    }

    CPubKey xpubkey;
    uint256 chaincode;
    QString keychain_error;
    // A brand-new outgoing friendship cannot have received funds yet, but the
    // wallet may re-run this path for a friendship first established earlier
    // (e.g. re-deriving after a restore), so do not bound rescans with the
    // current time; 0 (genesis/unknown) is the safe lower bound here.
    if (!prepareReceivingKeychain(to_identity, xpubkey, chaincode, /*creation_time=*/0, keychain_error)) {
        Q_EMIT requestFailed(QString::fromStdString(HexStr(to_identity)), keychain_error);
        return;
    }

    // Contact-format serialized xpub: 33-byte pubkey || 32-byte chain code
    // (dashj serializeContactPub — no BIP32 metadata that would leak the path).
    std::vector<uint8_t> our_xpub;
    our_xpub.insert(our_xpub.end(), xpubkey.begin(), xpubkey.end());
    our_xpub.insert(our_xpub.end(), chaincode.begin(), chaincode.end());

    std::vector<uint8_t> encrypted;
    if (!encryptXpub(recipient_key, our_xpub, encrypted)) {
        Q_EMIT requestFailed(QString::fromStdString(HexStr(to_identity)), tr("failed to encrypt the contact request"));
        return;
    }

    platform::ContactRequest doc;
    doc.owner_id = *my_id;
    doc.to_user_id = to_identity;
    doc.encrypted_public_key = std::move(encrypted);
    doc.sender_key_index = 0;
    doc.recipient_key_index = recipient_key_id;
    doc.account_reference = 0; // account 0 for this version

    const auto id_hex{QString::fromStdString(HexStr(to_identity))};
    QPointer<ContactFlow> self{this};
    m_service.client().getIdentityContractNonce(*my_id, platform::DASHPAY_CONTRACT_ID,
        [self, doc, id_hex](platform::Result<uint64_t> nonce_res) {
        if (!self) return;
        self->m_service.post([self, doc, id_hex, nonce_res = std::move(nonce_res)] {
            if (!self) return;
            if (!nonce_res.ok()) {
                Q_EMIT self->requestFailed(id_hex, tr("could not fetch identity nonce"));
                return;
            }
            Wallet& w{self->m_service.walletModel().wallet()};
            const auto signer = [&w](const uint256& digest, std::vector<uint8_t>& sig) {
                auto res{w.signPlatformDigest(wallet::IdentityAuthKey{0, 1}, digest)};
                if (!res) return false;
                sig = std::move(res.value);
                return true;
            };
            auto built{platform::st::BuildContactRequest(doc.owner_id, *nonce_res.value + 1, doc,
                                                         /*signature_public_key_id=*/1, signer)};
            if (!built.ok()) {
                Q_EMIT self->requestFailed(id_hex, QString::fromStdString(built.error));
                return;
            }
            QPointer<ContactFlow> inner{self};
            self->m_service.client().broadcastStateTransition(built.value->bytes,
                [inner, id_hex](platform::Result<platform::BroadcastResult> res) {
                if (!inner) return;
                inner->m_service.post([inner, id_hex, res = std::move(res)] {
                    if (!inner) return;
                    if (res.ok() && (res.value->accepted ||
                                     res.value->error.find("already") != std::string::npos)) {
                        platform::Identifier to{};
                        const auto bytes{ParseHex(id_hex.toStdString())};
                        if (bytes.size() != to.size()) {
                            Q_EMIT inner->requestFailed(id_hex, tr("invalid contact identity"));
                            return;
                        }
                        std::copy(bytes.begin(), bytes.end(), to.begin());
                        inner->confirmRequest(to, /*attempts_left=*/12);
                    } else {
                        Q_EMIT inner->requestFailed(id_hex, res.ok() ? QString::fromStdString(res.value->error)
                                                                     : tr("broadcast failed"));
                    }
                });
            });
        });
    });
}

void ContactFlow::accept(const platform::ContactRequest& incoming)
{
    const QString id_hex{QString::fromStdString(HexStr(incoming.owner_id))};
    QPointer<ContactFlow> self{this};
    m_service.client().getIdentity(incoming.owner_id,
        [self, incoming, id_hex](platform::Result<std::optional<platform::Identity>> result) {
        if (!self) return;
        self->m_service.post([self, incoming, id_hex, result = std::move(result)] {
            if (!self) return;
            if (!result.ok() || !result.value->has_value()) {
                Q_EMIT self->requestFailed(id_hex, tr("could not verify the sender identity"));
                return;
            }
            const auto& keys{(**result.value).public_keys};
            const auto it{std::find_if(keys.begin(), keys.end(), [&incoming](const auto& key) {
                return key.id == incoming.sender_key_index && !key.disabled_at &&
                       key.type == platform::IdentityPublicKey::Type::ECDSA_SECP256K1;
            })};
            if (it == keys.end()) {
                Q_EMIT self->requestFailed(id_hex, tr("the sender encryption key is unavailable"));
                return;
            }
            std::vector<uint8_t> their_xpub;
            if (!self->decryptXpub(incoming.recipient_key_index, it->data,
                                   incoming.encrypted_public_key, their_xpub)) {
                Q_EMIT self->requestFailed(id_hex, tr("could not decrypt the contact request"));
                return;
            }
            QString import_error;
            // The friendship chain cannot have received funds before the
            // contact request document existed, so its created_at timestamp
            // (ms since epoch; 0 when the document carries none) safely
            // bounds later rescans of the imported descriptor.
            const int64_t creation_time{static_cast<int64_t>(incoming.created_at / 1000)};
            if (!self->importKeychains(incoming.owner_id, their_xpub, creation_time, import_error)) {
                Q_EMIT self->requestFailed(id_hex, import_error);
                return;
            }
            self->m_service.writeRecord("contact/key/" + id_hex.toStdString(), their_xpub);
            self->m_service.writeRecord("contact/in/" + id_hex.toStdString(),
                                        std::vector<unsigned char>(incoming.document_id.begin(), incoming.document_id.end()));
            self->sendRequest(incoming.owner_id, it->id, *it);
        });
    });
}

bool ContactFlow::importKeychains(const platform::Identifier& their_identity,
                                  const std::vector<uint8_t>& their_xpub_serialized,
                                  int64_t creation_time, QString& error)
{
    if (their_xpub_serialized.size() != 65) {
        error = tr("invalid friendship public key in contact request");
        return false;
    }
    // Validate the decrypted material before it is persisted; the contact's
    // chain itself never enters the wallet (see ensureFriendshipReceivingKeychain).
    const CPubKey pubkey{their_xpub_serialized.begin(), their_xpub_serialized.begin() + 33};
    if (!pubkey.IsFullyValid()) {
        error = tr("invalid friendship public key in contact request");
        return false;
    }
    CPubKey our_pubkey;
    uint256 our_chaincode;
    return prepareReceivingKeychain(their_identity, our_pubkey, our_chaincode, creation_time, error);
}

bool ContactFlow::prepareReceivingKeychain(const platform::Identifier& their_identity, CPubKey& pubkey,
                                           uint256& chaincode, int64_t creation_time, QString& error)
{
    const auto my_id{m_service.myIdentityId()};
    if (!my_id) {
        error = tr("register a username first");
        return false;
    }
    const uint256 my_hash{std::vector<uint8_t>(my_id->begin(), my_id->end())};
    const uint256 their_hash{std::vector<uint8_t>(their_identity.begin(), their_identity.end())};
    Wallet& wallet{m_service.walletModel().wallet()};
    const auto keychain{wallet.ensureFriendshipReceivingKeychain(
        wallet::FriendshipKeychainRequest{/*account=*/0, my_hash, their_hash, creation_time})};
    if (!keychain) {
        error = keychain.status == wallet::PlatformKeyStatus::WALLET_LOCKED
                    ? tr("unable to derive friendship key (unlock the wallet)")
                    : tr("unable to import the friendship keychain into the wallet");
        return false;
    }
    pubkey = keychain.value.pubkey;
    chaincode = keychain.value.chaincode;

    const std::string label{
        m_service.contactAddressLabel(QString::fromStdString(HexStr(their_identity))).toStdString()};

    // Ranged descriptors cannot carry an address-book label, so label the
    // receiving chain explicitly for transaction history.
    for (uint32_t i = 0; i < 20; ++i) {
        CTxDestination destination;
        if (!wallet::DeriveFriendshipPaymentDestination(keychain.value, i, destination)) break;
        wallet.setAddressBook(destination, label, "receive");
    }
    return true;
}

void ContactFlow::confirmRequest(const platform::Identifier& to_identity, int attempts_left)
{
    const auto my_id{m_service.myIdentityId()};
    if (!my_id) return;
    const QString id_hex{QString::fromStdString(HexStr(to_identity))};
    QPointer<ContactFlow> self{this};
    m_service.client().getContactRequests(*my_id, /*to_me=*/false, 0,
        [self, to_identity, id_hex, attempts_left](platform::Result<std::vector<platform::ContactRequest>> result) {
        if (!self) return;
        self->m_service.post([self, to_identity, id_hex, attempts_left, result = std::move(result)] {
            if (!self) return;
            if (result.ok() && std::any_of(result.value->begin(), result.value->end(),
                    [&to_identity](const auto& request) { return request.to_user_id == to_identity; })) {
                self->m_service.writeRecord("contact/out/" + id_hex.toStdString(), {1});
                Q_EMIT self->requestSent(id_hex);
                Q_EMIT self->contactAdded(id_hex);
                return;
            }
            if (attempts_left <= 1) {
                Q_EMIT self->requestFailed(id_hex, result.ok() ? tr("contact request was not confirmed")
                                                               : QString::fromStdString(result.error));
                return;
            }
            QTimer::singleShot(2500, self, [self, to_identity, attempts_left] {
                if (self) self->confirmRequest(to_identity, attempts_left - 1);
            });
        });
    });
}
