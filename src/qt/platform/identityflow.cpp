// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/identityflow.h>

#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <logging.h>
#include <platform/client.h>
#include <platform/statetransitions.h>
#include <platform/walletrecords.h>
#include <primitives/transaction.h>
#include <qt/clientmodel.h>
#include <qt/platform/platformservice.h>
#include <qt/walletmodel.h>
#include <random.h>
#include <script/script.h>
#include <streams.h>
#include <util/time.h>
#include <wallet/coincontrol.h>
#include <wallet/platformtypes.h>

#include <QPointer>

using interfaces::Wallet;

namespace {

constexpr char RECORD_KEY[]{"identity/0"};
//! Confirmation polls before assuming a broadcast was lost and stepping back
//! to rebuild + rebroadcast.
constexpr int MAX_CONFIRM_POLLS{6};
//! Index of the identity-funding credit output within the asset lock
//! payload's creditOutputs. We always create a single credit output, so
//! this is 0 — matching js-dash-sdk ASSET_LOCK_OUTPUT_INDEX. Note this is
//! NOT the OP_RETURN vout in the transaction's output list.
constexpr uint32_t ASSET_LOCK_OUTPUT_INDEX{0};

//! 36-byte outpoint (txid || LE index) referencing the asset lock burn
//! output, the preimage of the identity id.
std::array<uint8_t, 36> BurnOutpoint(const uint256& txid, uint32_t vout)
{
    std::array<uint8_t, 36> out{};
    std::copy(txid.begin(), txid.end(), out.begin());
    out[32] = static_cast<uint8_t>(vout);
    out[33] = static_cast<uint8_t>(vout >> 8);
    out[34] = static_cast<uint8_t>(vout >> 16);
    out[35] = static_cast<uint8_t>(vout >> 24);
    return out;
}

} // namespace

IdentityFlow::IdentityFlow(PlatformService& service, QObject* parent) :
    QObject(parent),
    m_service(service)
{
    load();
}

bool IdentityFlow::load()
{
    const auto data{m_service.readRecord(RECORD_KEY)};
    if (data.empty()) return false;
    Record record;
    if (!platform::DeserializeIdentityRecord(data, record)) return false;
    m_record = std::move(record);
    return true;
}

bool IdentityFlow::save()
{
    return m_service.writeRecord(RECORD_KEY, platform::SerializeIdentityRecord(m_record));
}

void IdentityFlow::reload()
{
    if (!load()) return;
    Q_EMIT stateChanged();
}

void IdentityFlow::setState(State state)
{
    if (m_record.state == state) return;
    m_record.state = state;
    m_record.last_error.clear();
    m_retries = 0;
    save();
    LogPrintf("Platform identity flow: state=%d name=%s\n", static_cast<int>(state),
              m_record.normalized_label);
    Q_EMIT stateChanged();
}

void IdentityFlow::fail(const QString& step, const QString& error, bool retryable)
{
    m_record.last_error = error.toStdString();
    if (!retryable || ++m_retries > 10) {
        m_record.state = State::FAILED;
        m_retries = 0;
    }
    save();
    Q_EMIT failed(step, error);
    if (m_record.state == State::FAILED) Q_EMIT stateChanged();
}

void IdentityFlow::reset()
{
    if (m_record.state != State::FAILED) return;
    m_record = Record{};
    m_service.writeRecord(RECORD_KEY, {});
    Q_EMIT stateChanged();
}

bool IdentityFlow::start(const QString& label, CAmount funding_amount, QString& error)
{
    const bool name_only{m_record.AwaitsUsername()};
    if (active() && !name_only) {
        error = tr("a username registration is already in progress");
        return false;
    }

    const std::string label_str{label.toStdString()};
    const std::string normalized{platform::st::NormalizeLabel(label_str)};
    if (normalized.empty()) {
        error = tr("invalid username");
        return false;
    }

    if (name_only) {
        m_record.label = label_str;
        m_record.normalized_label = normalized;
        m_record.contested = platform::st::IsContestedLabel(normalized);
        GetStrongRandBytes(m_record.preorder_salt);
        if (!save()) {
            m_record.label.clear();
            m_record.normalized_label.clear();
            m_record.preorder_salt.fill(0);
            error = tr("could not save the registration state to the wallet");
            return false;
        }
        Q_EMIT stateChanged();
        return true;
    }

    Wallet& wallet{m_service.walletModel().wallet()};

    const auto funding_pubkey{wallet.getPlatformPubKey(wallet::RegistrationFundingKey{0})};
    if (!funding_pubkey) {
        error = tr("unable to derive the funding key (is the wallet unlocked?)");
        return false;
    }

    auto res{wallet.createAssetLockTransaction(funding_amount, funding_pubkey.value, wallet::CCoinControl{})};
    if (!res) {
        error = QString::fromStdString(util::ErrorString(res).translated);
        return false;
    }
    const CTransactionRef& tx{*res};

    // Locate the OP_RETURN burn output.
    std::optional<uint32_t> burn_vout;
    for (uint32_t i = 0; i < tx->vout.size(); ++i) {
        const CScript& spk{tx->vout[i].scriptPubKey};
        if (!spk.empty() && spk[0] == OP_RETURN) {
            burn_vout = i;
            break;
        }
    }
    if (!burn_vout) {
        error = tr("internal error: asset lock transaction has no burn output");
        return false;
    }

    // Persist the full flow state — including the preorder salt — before the
    // irreversible broadcast.
    m_funding_dead_polls = 0;
    m_record = Record{};
    m_record.state = State::FUNDING_SENT;
    m_record.funding_txid = tx->GetHash();
    m_record.funding_vout = *burn_vout;
    m_record.funding_key_index = 0;
    m_record.funding_amount = funding_amount;
    m_record.label = label_str;
    m_record.normalized_label = normalized;
    m_record.contested = platform::st::IsContestedLabel(normalized);
    m_record.started_at = GetTime();
    GetStrongRandBytes(m_record.preorder_salt);
    if (!save()) {
        // The wallet DB write failed: broadcasting the asset lock now would
        // fund an identity flow the GUI can never resume. Abort before the
        // irreversible commit and leave the wallet untouched.
        m_record = Record{};
        error = tr("could not save the registration state to the wallet; the funding "
                   "transaction was not broadcast");
        return false;
    }

    if (const auto broadcast_error{wallet.commitTransaction(tx, {}, {})}) {
        // The mempool rejected the transaction: it can never confirm, but it
        // was committed to the wallet and would linger as a phantom pending
        // debit. Abandon it to release the inputs and roll the flow back so
        // the user can retry once the cause is addressed.
        wallet.abandonTransaction(tx->GetHash());
        m_record = Record{};
        m_service.writeRecord(RECORD_KEY, {});
        error = tr("the funding transaction was rejected: %1")
                    .arg(QString::fromStdString(broadcast_error->translated));
        return false;
    }

    Q_EMIT stateChanged();
    return true;
}

void IdentityFlow::resume()
{
    if (!active()) return;
    // Every step handler below re-derives its progress from ground truth
    // (wallet tx status, proved platform queries), so resuming is just
    // advancing.
    advance();
}

void IdentityFlow::advance()
{
    if (m_step_in_flight) return;
    switch (m_record.state) {
    case State::NONE:
    case State::REGISTERED:
    case State::FAILED:
        return;
    case State::FUNDING_SENT:
        checkFundingLock();
        return;
    case State::FUNDING_LOCKED:
        broadcastIdentityCreate();
        return;
    case State::IDENTITY_BROADCAST:
        confirmIdentity();
        return;
    case State::IDENTITY_CONFIRMED:
        if (m_record.AwaitsUsername()) return;
        broadcastPreorder();
        return;
    case State::PREORDER_BROADCAST:
        broadcastPreorder();
        return;
    case State::PREORDER_WAIT:
        broadcastDomain();
        return;
    case State::DOMAIN_BROADCAST:
    case State::CONTESTED_PENDING:
        confirmDomain();
        return;
    }
}

void IdentityFlow::checkFundingLock()
{
    Wallet& wallet{m_service.walletModel().wallet()};
    interfaces::WalletTxStatus status;
    interfaces::WalletOrderForm order_form;
    bool in_mempool{false};
    int num_blocks{0};
    const auto wtx{wallet.getWalletTxDetails(m_record.funding_txid, status, order_form, in_mempool, num_blocks)};
    if (!wtx.tx) {
        // The record was persisted but the transaction never made it into
        // the wallet (crash between save() and commitTransaction()): nothing
        // was spent or broadcast, so fail out and let the user start over.
        fail(tr("funding"), tr("the funding transaction is missing from the wallet"), /*retryable=*/false);
        return;
    }

    if (status.is_abandoned || (status.depth_in_main_chain < 0)) {
        fail(tr("funding"), tr("the funding transaction was abandoned or conflicted"), /*retryable=*/false);
        return;
    }
    if (status.is_islocked || status.is_chainlocked || status.depth_in_main_chain >= 8) {
        m_funding_dead_polls = 0;
        setState(State::FUNDING_LOCKED);
        return;
    }

    if (status.depth_in_main_chain == 0 && !in_mempool) {
        // Unconfirmed and not in the local mempool: the broadcast was lost
        // (restart) or the mempool rejected the transaction, which no
        // islock/chainlock/confirmation poll would ever notice. Rebroadcast
        // to recover the transient case; a transaction the node keeps
        // refusing is dead, so abandon it (releasing the inputs) and fail.
        if (wallet.resendTransaction(m_record.funding_txid)) {
            m_funding_dead_polls = 0;
            return;
        }
        if (++m_funding_dead_polls >= MAX_CONFIRM_POLLS) {
            wallet.abandonTransaction(m_record.funding_txid);
            fail(tr("funding"), tr("the funding transaction was rejected from the mempool"), /*retryable=*/false);
        }
        return;
    }
    m_funding_dead_polls = 0;
}

void IdentityFlow::broadcastIdentityCreate()
{
    Wallet& wallet{m_service.walletModel().wallet()};
    interfaces::Node& node{m_service.clientModel().node()};

    // Build the asset lock proof: prefer the InstantSend lock, fall back to
    // a chain-locked block proof. The identity is funded by the (single)
    // credit output in the asset lock payload, at ASSET_LOCK_OUTPUT_INDEX.
    const auto burn_outpoint{BurnOutpoint(m_record.funding_txid, ASSET_LOCK_OUTPUT_INDEX)};
    std::variant<platform::st::InstantAssetLockProof, platform::st::ChainAssetLockProof> proof;

    auto islock{node.llmq().getInstantSendLock(m_record.funding_txid)};
    if (!islock.empty()) {
        const auto wtx{wallet.getWalletTx(m_record.funding_txid)};
        if (!wtx.tx) {
            fail(tr("identity"), tr("funding transaction not found in wallet"), /*retryable=*/true);
            return;
        }
        CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
        stream << *wtx.tx;
        proof = platform::st::InstantAssetLockProof{
            .transaction = {UCharCast(stream.data()), UCharCast(stream.data()) + stream.size()},
            .instant_lock = std::move(islock),
            .output_index = ASSET_LOCK_OUTPUT_INDEX,
        };
    } else {
        interfaces::WalletTxStatus status;
        int num_blocks{0};
        int64_t block_time{0};
        if (!wallet.tryGetTxStatus(m_record.funding_txid, status, num_blocks, block_time) ||
            !status.is_chainlocked) {
            return; // wait for a lock
        }
        platform::st::ChainAssetLockProof chain_proof;
        chain_proof.core_chain_locked_height = static_cast<uint32_t>(status.block_height);
        chain_proof.out_point = burn_outpoint;
        proof = chain_proof;
    }

    // Identity keys 0 (MASTER) and 1 (HIGH), DIP-13 authentication keys.
    std::vector<platform::st::NewIdentityKey> keys;
    for (uint32_t key_index : {0u, 1u}) {
        const auto pubkey{wallet.getPlatformPubKey(wallet::IdentityAuthKey{0, key_index})};
        if (!pubkey) {
            fail(tr("identity"), tr("unable to derive identity keys (is the wallet unlocked?)"), /*retryable=*/true);
            return;
        }
        platform::st::NewIdentityKey key;
        key.id = key_index;
        key.purpose = platform::IdentityPublicKey::Purpose::AUTHENTICATION;
        key.security_level = key_index == 0 ? platform::IdentityPublicKey::SecurityLevel::MASTER
                                            : platform::IdentityPublicKey::SecurityLevel::HIGH;
        key.pubkey.assign(pubkey.value.begin(), pubkey.value.end());
        key.signer = [&wallet, key_index](const uint256& digest, std::vector<uint8_t>& sig) {
            auto res{wallet.signPlatformDigest(wallet::IdentityAuthKey{0, key_index}, digest)};
            if (!res) return false;
            sig = std::move(res.value);
            return true;
        };
        keys.push_back(std::move(key));
    }

    const auto asset_lock_signer = [&wallet, index = m_record.funding_key_index](const uint256& digest, std::vector<uint8_t>& sig) {
        auto res{wallet.signPlatformDigest(wallet::RegistrationFundingKey{index}, digest)};
        if (!res) return false;
        sig = std::move(res.value);
        return true;
    };

    auto built{platform::st::BuildIdentityCreate(proof, keys, asset_lock_signer)};
    if (!built.ok()) {
        fail(tr("identity"), QString::fromStdString(built.error), /*retryable=*/true);
        return;
    }

    // Record the (deterministic) identity id before broadcasting.
    m_record.identity_id = platform::st::IdentityIdFromOutpoint(burn_outpoint);
    setState(State::IDENTITY_BROADCAST);

    m_step_in_flight = true;
    QPointer<IdentityFlow> self{this};
    m_service.client().broadcastStateTransition(built.value->bytes, [self](platform::Result<platform::BroadcastResult> res) {
        if (!self) return;
        self->m_service.post([self, res = std::move(res)] {
            if (!self) return;
            self->m_step_in_flight = false;
            if (res.ok() && !res.value->accepted &&
                res.value->error.find("already") == std::string::npos) {
                self->fail(tr("identity"), QString::fromStdString(res.value->error), /*retryable=*/true);
                return;
            }
        });
    });
}

void IdentityFlow::confirmIdentity()
{
    m_step_in_flight = true;
    QPointer<IdentityFlow> self{this};
    m_service.client().getIdentity(m_record.identity_id, [self](platform::Result<std::optional<platform::Identity>> res) {
        if (!self) return;
        self->m_service.post([self, res = std::move(res)] {
            if (!self) return;
            self->m_step_in_flight = false;
            if (!res.ok()) {
                LogPrintf("Platform identity flow: confirmation failed: %s\n", res.error);
                if (++self->m_retries >= MAX_CONFIRM_POLLS) {
                    // The transition may never have left this process (for
                    // example, a crash immediately after persisting
                    // IDENTITY_BROADCAST). Rebuild and rebroadcast it
                    // idempotently after bounded failed/absent reads.
                    self->m_retries = 0;
                    self->setState(State::FUNDING_LOCKED);
                }
                return;
            }
            if (res.value->has_value()) {
                self->setState(State::IDENTITY_CONFIRMED);
            } else if (++self->m_retries >= MAX_CONFIRM_POLLS) {
                // Broadcast may have been lost (e.g. crash between persist and
                // send) — step back and rebroadcast.
                self->m_retries = 0;
                self->setState(State::FUNDING_LOCKED);
            }
        });
    });
}

void IdentityFlow::broadcastPreorder()
{
    m_step_in_flight = true;
    QPointer<IdentityFlow> self{this};
    m_service.client().getIdentityContractNonce(m_record.identity_id, platform::DPNS_CONTRACT_ID,
        [self](platform::Result<uint64_t> nonce_res) {
        if (!self) return;
        self->m_service.post([self, nonce_res = std::move(nonce_res)] {
            if (!self) return;
            self->m_step_in_flight = false;
            if (!nonce_res.ok()) return;

            Wallet& wallet{self->m_service.walletModel().wallet()};
            const auto signer = [&wallet](const uint256& digest, std::vector<uint8_t>& sig) {
                auto res{wallet.signPlatformDigest(wallet::IdentityAuthKey{0, 1}, digest)};
                if (!res) return false;
                sig = std::move(res.value);
                return true;
            };
            auto built{platform::st::BuildDpnsPreorder(self->m_record.identity_id, *nonce_res.value + 1,
                                                       self->m_record.label, self->m_record.preorder_salt,
                                                       /*signature_public_key_id=*/1, signer)};
            if (!built.ok()) {
                self->fail(tr("preorder"), QString::fromStdString(built.error), /*retryable=*/true);
                return;
            }
            self->setState(State::PREORDER_BROADCAST);
            self->m_step_in_flight = true;
            QPointer<IdentityFlow> inner{self};
            self->m_service.client().broadcastStateTransition(built.value->bytes, [inner](platform::Result<platform::BroadcastResult> res) {
                if (!inner) return;
                inner->m_service.post([inner, res = std::move(res)] {
                    if (!inner) return;
                    inner->m_step_in_flight = false;
                    if (res.ok() && (res.value->accepted ||
                                     res.value->error.find("already") != std::string::npos)) {
                        inner->setState(State::PREORDER_WAIT);
                    } else if (res.ok()) {
                        inner->fail(tr("preorder"), QString::fromStdString(res.value->error), /*retryable=*/true);
                    }
                });
            });
        });
    });
}

void IdentityFlow::broadcastDomain()
{
    m_step_in_flight = true;
    QPointer<IdentityFlow> self{this};
    m_service.client().getIdentityContractNonce(m_record.identity_id, platform::DPNS_CONTRACT_ID,
        [self](platform::Result<uint64_t> nonce_res) {
        if (!self) return;
        self->m_service.post([self, nonce_res = std::move(nonce_res)] {
            if (!self) return;
            self->m_step_in_flight = false;
            if (!nonce_res.ok()) return;

            Wallet& wallet{self->m_service.walletModel().wallet()};
            const auto signer = [&wallet](const uint256& digest, std::vector<uint8_t>& sig) {
                auto res{wallet.signPlatformDigest(wallet::IdentityAuthKey{0, 1}, digest)};
                if (!res) return false;
                sig = std::move(res.value);
                return true;
            };
            const auto& rec{self->m_record};
            auto built{platform::st::BuildDpnsDomain(rec.identity_id, *nonce_res.value + 1, rec.label,
                                                     rec.normalized_label, "dash", rec.preorder_salt,
                                                     /*signature_public_key_id=*/1, signer)};
            if (!built.ok()) {
                self->fail(tr("domain"), QString::fromStdString(built.error), /*retryable=*/true);
                return;
            }
            self->m_step_in_flight = true;
            QPointer<IdentityFlow> inner{self};
            self->m_service.client().broadcastStateTransition(built.value->bytes, [inner](platform::Result<platform::BroadcastResult> res) {
                if (!inner) return;
                inner->m_service.post([inner, res = std::move(res)] {
                    if (!inner) return;
                    inner->m_step_in_flight = false;
                    if (!res.ok()) return;
                    if (res.value->accepted ||
                        res.value->error.find("already") != std::string::npos) {
                        inner->setState(State::DOMAIN_BROADCAST);
                    } else if (res.value->error.find("preorder") != std::string::npos) {
                        // Preorder not visible yet — wait and retry.
                    } else {
                        inner->fail(tr("domain"), QString::fromStdString(res.value->error), /*retryable=*/true);
                    }
                });
            });
        });
    });
}

void IdentityFlow::checkContestedOutcome()
{
    // Proof-verified contested-resource vote state: detects a poll that
    // finished locked (name unusable) or awarded before/without the domain
    // document becoming visible, and feeds live tallies to the dashboard.
    QPointer<IdentityFlow> self{this};
    m_service.client().getContestedNameState(
        m_record.normalized_label,
        [self](platform::Result<platform::ContestedNameState> res) {
            if (!self) return;
            self->m_service.post([self, res = std::move(res)] {
                if (!self || !res.ok()) return;
                if (self->m_record.state != State::CONTESTED_PENDING) return;
                const platform::ContestedNameState& state{*res.value};
                switch (state.status) {
                case platform::ContestedNameState::Status::LOCKED:
                    self->fail(tr("vote"),
                               tr("masternodes voted to lock this name; it cannot be registered"),
                               /*retryable=*/false);
                    break;
                case platform::ContestedNameState::Status::WON:
                    if (state.winner && *state.winner != self->m_record.identity_id) {
                        self->fail(tr("vote"),
                                   tr("the contested name was awarded to another identity"),
                                   /*retryable=*/false);
                    }
                    // Awarded to us: keep polling; the domain document is
                    // what finally flips the flow to REGISTERED.
                    break;
                default:
                    break;
                }
            });
        });
}

void IdentityFlow::confirmDomain()
{
    m_step_in_flight = true;
    QPointer<IdentityFlow> self{this};
    m_service.client().resolveName(m_record.normalized_label, [self](platform::Result<std::optional<platform::DpnsName>> res) {
        if (!self) return;
        self->m_service.post([self, res = std::move(res)] {
            if (!self) return;
            self->m_step_in_flight = false;
            if (!res.ok()) return;

            if (res.value->has_value()) {
                if ((*res.value)->identity == self->m_record.identity_id) {
                    self->setState(State::REGISTERED);
                } else if (!self->m_record.contested) {
                    self->fail(tr("domain"), tr("the name was registered by another identity"), /*retryable=*/false);
                } else {
                    self->fail(tr("vote"), tr("the contested name was awarded to another identity"), /*retryable=*/false);
                }
                return;
            }

            if (self->m_record.contested) {
                // No document yet: consult the proof-verified vote state to
                // distinguish "vote in progress" from a decided outcome.
                if (self->m_record.state != State::CONTESTED_PENDING) {
                    self->setState(State::CONTESTED_PENDING);
                }
                self->checkContestedOutcome();
                return;
            }

            if (++self->m_retries >= MAX_CONFIRM_POLLS) {
                // Domain broadcast may have been lost — step back and
                // rebroadcast with the same persisted salt.
                self->m_retries = 0;
                self->setState(State::PREORDER_WAIT);
            }
        });
    });
}
