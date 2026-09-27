// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/identityflow.h>

#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <logging.h>
#include <platform/client.h>
#include <platform/helpers.h>
#include <platform/signer.h>
#include <platform/walletrecords.h>
#include <primitives/transaction.h>
#include <qt/clientmodel.h>
#include <qt/optionsmodel.h>
#include <qt/platform/platformservice.h>
#include <qt/walletmodel.h>
#include <random.h>
#include <streams.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <util/translation.h>
#include <wallet/coincontrol.h>
#include <wallet/platformtypes.h>
#include <wallet/wallet.h>

#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <string_view>

using interfaces::Wallet;
using platform::OperationKind;
using platform::StatusKind;

namespace {

//! How long a step waits for Platform to confirm what it sent before it
//! sends it again (Platform took up to 68 s on testnet), and the backoff of
//! its proved re-queries in that time.
constexpr int64_t CONFIRM_WINDOW_SECONDS{180};
constexpr int64_t FIRST_POLL_INTERVAL_SECONDS{5};
constexpr int64_t MAX_POLL_INTERVAL_SECONDS{30};
//! How long the passphrase the user entered to fund a registration keeps the
//! wallet unlocked for its InstantSend lock; it usually takes seconds.
constexpr int64_t FUNDING_LOCK_WAIT_SECONDS{60};
constexpr int FUNDING_LOCK_POLL_MS{1'000};
//! How long an unconfirmed funding payment may stay outside the mempool,
//! with the node refusing to rebroadcast it, before it is abandoned.
constexpr int64_t FUNDING_DEAD_SECONDS{30};
//! Confirmations after which the funding payment, or a spend of its coins
//! that conflicts with it, is final where no ChainLock came.
constexpr int FUNDING_FINAL_DEPTH{8};
//! Retryable failures before a step gives up.
constexpr int MAX_RETRIES{10};
//! Index of the identity-funding credit output within the asset lock
//! payload's creditOutputs. We always create a single credit output, so
//! this is 0. Note this is NOT the OP_RETURN vout in the transaction's
//! output list.
constexpr uint32_t ASSET_LOCK_OUTPUT_INDEX{0};
constexpr uint32_t KEY_MASTER{0};
constexpr uint32_t KEY_HIGH{1};
constexpr uint32_t KEY_ENCRYPTION{2};
constexpr uint32_t KEY_DECRYPTION{3};

//! Consensus codes the flow steers on (rs-dpp consensus/codes.rs).
constexpr uint32_t CODE_ASSET_LOCK_OUTPOINT_CONSUMED{10504};
//! Platform has not committed the ChainLocked core height yet.
constexpr uint32_t CODE_ASSET_LOCK_CORE_HEIGHT_AHEAD{10506};
//! The quorum that signed the InstantSend lock rotated out.
constexpr uint32_t CODE_ASSET_LOCK_INSTANT_SIGNATURE{10513};
//! A document id derived another way than the protocol version Platform
//! runs derives it (protocol version 14 changed the derivation).
constexpr uint32_t CODE_INVALID_DOCUMENT_TRANSITION_ID{10405};
constexpr uint32_t CODE_DOCUMENT_ALREADY_PRESENT{40100};
constexpr uint32_t CODE_DUPLICATE_UNIQUE_INDEX{40105};
constexpr uint32_t CODE_CONTEST_ALREADY_CONTESTANT{40112};
constexpr uint32_t CODE_IDENTITY_ALREADY_EXISTS{40200};
constexpr uint32_t CODE_INVALID_IDENTITY_NONCE{40204};
constexpr uint32_t CODE_DATA_TRIGGER_CONDITION{40500};
//! A brand-new identity has used no contract: its first transition on one
//! carries identity contract nonce 1 (Drive accepts a new nonce below 24
//! for an identity without one, and each next one above the last).
constexpr uint64_t FIRST_CONTRACT_NONCE{1};
//! Domains signed again at a fresh nonce before the preorder is signed again
//! too (the domain can only apply over a preorder Platform has).
constexpr int MAX_DOMAIN_RESIGNS{2};
//! Broadcasts of the profile signed at registration before it is given up.
constexpr int MAX_PROFILE_SENDS{3};

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

bool SameStatus(const std::optional<platform::Status>& a, const platform::Status* b)
{
    if (!a || !b) return !a && !b;
    return a->kind == b->kind && a->consensus_code == b->consensus_code && a->message == b->message;
}

bool BroadcastAccepted(const platform::Status& status)
{
    return status.kind == StatusKind::OK || status.kind == StatusKind::ALREADY_EXISTS;
}

bool IsConsensus(const platform::Status& status, uint32_t code)
{
    return status.kind == StatusKind::CONSENSUS && status.consensus_code == code;
}

//! The consensus code in a failure text written before the flow kept the
//! typed result ("… (consensus error 10405)."), so it is worded like a new one.
std::optional<uint32_t> StoredConsensusCode(const std::string& text)
{
    static constexpr std::string_view MARKER{"(consensus error "};
    const auto pos{text.rfind(MARKER)};
    if (pos == std::string::npos) return std::nullopt;
    const auto digits{text.substr(pos + MARKER.size(), text.find(')', pos) - pos - MARKER.size())};
    return ToIntegral<uint32_t>(digits);
}

//! The key a new identity signs documents with: the HIGH authentication key
//! the registration itself adds, before any proved read can show it.
std::optional<platform::IdentityPublicKey> RegisteredDocumentKey(interfaces::Wallet& wallet)
{
    const auto pubkey{wallet.getPlatformPubKey(wallet::IdentityAuthKey{0, KEY_HIGH})};
    if (!pubkey) return std::nullopt;
    platform::IdentityPublicKey key;
    key.id = KEY_HIGH;
    key.purpose = platform::IdentityPublicKey::Purpose::AUTHENTICATION;
    key.security_level = platform::IdentityPublicKey::SecurityLevel::HIGH;
    key.type = platform::IdentityPublicKey::Type::ECDSA_SECP256K1;
    key.data.assign(pubkey.value.begin(), pubkey.value.end());
    return key;
}

//! Marks a signing step for its duration, so the passphrase dialog's nested
//! event loop cannot re-enter the flow from a timer tick.
class SigningGuard
{
public:
    explicit SigningGuard(bool& flag) :
        m_flag(flag)
    {
        m_flag = true;
    }
    ~SigningGuard() { m_flag = false; }

private:
    bool& m_flag;
};

} // namespace

const std::vector<IdentityFlow::KeySpec>& IdentityFlow::RegistrationKeys()
{
    using Purpose = platform::IdentityPublicKey::Purpose;
    using Level = platform::IdentityPublicKey::SecurityLevel;
    static const std::vector<KeySpec> keys{
        {KEY_MASTER, Purpose::AUTHENTICATION, Level::MASTER, false},
        {KEY_HIGH, Purpose::AUTHENTICATION, Level::HIGH, false},
        {KEY_ENCRYPTION, Purpose::ENCRYPTION, Level::MEDIUM, true},
        {KEY_DECRYPTION, Purpose::DECRYPTION, Level::MEDIUM, true},
    };
    return keys;
}

IdentityFlow::Severity IdentityFlow::SeverityOf(const platform::Status& status)
{
    switch (status.kind) {
    case StatusKind::CONSENSUS:
        return Severity::FATAL;
    case StatusKind::INTERNAL:
        return Severity::RETRYABLE;
    case StatusKind::OK:
    case StatusKind::PROVEN_ABSENT:
    case StatusKind::ALREADY_EXISTS:
    case StatusKind::UNAVAILABLE:
    case StatusKind::REJECTED:
    case StatusKind::CHAIN_ID_MISMATCH:
    case StatusKind::UNSUPPORTED_PROTOCOL_VERSION:
        break;
    }
    return Severity::TRANSIENT;
}

IdentityFlow::IdentityFlow(PlatformService& service, QObject* parent) :
    QObject(parent),
    m_service(service)
{
    m_funding_lock_poll = new QTimer(this);
    m_funding_lock_poll->setInterval(FUNDING_LOCK_POLL_MS);
    connect(m_funding_lock_poll, &QTimer::timeout, this, &IdentityFlow::advance);
    // A monotonic bound: a wall clock stepped back would keep it unlocked.
    m_registration_unlock_timer = new QTimer(this);
    m_registration_unlock_timer->setSingleShot(true);
    m_registration_unlock_timer->setInterval(std::chrono::seconds{FUNDING_LOCK_WAIT_SECONDS});
    connect(m_registration_unlock_timer, &QTimer::timeout, this, &IdentityFlow::releaseRegistrationUnlock);
    load();
}

bool IdentityFlow::load()
{
    const auto data{m_service.readRecord(platform::records::IDENTITY)};
    if (data.empty()) return false;
    Record record;
    if (!platform::DeserializeIdentityRecord(data, record)) return false;
    m_record = std::move(record);
    m_saved_record = data;
    return true;
}

bool IdentityFlow::save()
{
    auto data{platform::SerializeIdentityRecord(m_record)};
    // A wait does not write the wallet on every tick.
    if (data == m_saved_record) return true;
    if (!m_service.writeRecord(platform::records::IDENTITY, data)) return false;
    m_saved_record = std::move(data);
    return true;
}

void IdentityFlow::eraseRecord()
{
    m_service.writeRecord(platform::records::IDENTITY, {});
    m_saved_record.clear();
}

void IdentityFlow::reload()
{
    releaseRegistrationUnlock();
    if (!load()) {
        m_record = Record{};
        m_saved_record.clear();
    }
    m_retries = 0;
    m_funding_dead_since = 0;
    m_funding_wait = FundingWait::NONE;
    m_wait_started = m_next_poll = m_poll_interval = 0;
    m_prefer_chain_proof = false;
    m_identity_create_sent = false;
    m_preorder_sent = false;
    m_domain_resigns = 0;
    m_profile_sent = false;
    m_profile_sends = 0;
    Q_EMIT stateChanged();
}

std::optional<platform::Identifier> IdentityFlow::identityId() const
{
    switch (m_record.state) {
    case State::IDENTITY_CONFIRMED:
    case State::PREORDER_BROADCAST:
    case State::PREORDER_WAIT:
    case State::DOMAIN_BROADCAST:
    case State::REGISTERED:
    case State::CONTESTED_PENDING:
        return m_record.identity_id;
    case State::NEEDS_UNLOCK:
        if (m_record.resume_state >= State::IDENTITY_CONFIRMED) return m_record.identity_id;
        return std::nullopt;
    case State::FAILED:
        // Failing to register a name leaves the identity on Platform.
        if (m_record.resume_state >= State::IDENTITY_CONFIRMED && m_record.resume_state <= State::CONTESTED_PENDING) {
            return m_record.identity_id;
        }
        return std::nullopt;
    case State::NONE:
    case State::FUNDING_SENT:
    case State::FUNDING_LOCKED:
    case State::IDENTITY_BROADCAST:
        return std::nullopt;
    }
    return std::nullopt;
}

bool IdentityFlow::HoldsUnconsumedFunding(const Record& record)
{
    switch (record.state) {
    case State::FUNDING_SENT:
    case State::FUNDING_LOCKED:
    case State::IDENTITY_BROADCAST:
        return true;
    case State::NEEDS_UNLOCK:
        return record.resume_state == State::FUNDING_SENT || record.resume_state == State::FUNDING_LOCKED ||
               record.resume_state == State::IDENTITY_BROADCAST;
    case State::FAILED:
        // What reset() re-uses; a failure while funding means the payment
        // is missing from the wallet or a final spend of its coins conflicts
        // with it, so it can never confirm.
        return record.resume_state == State::FUNDING_LOCKED || record.resume_state == State::IDENTITY_BROADCAST;
    case State::NONE:
    case State::IDENTITY_CONFIRMED:
    case State::PREORDER_BROADCAST:
    case State::PREORDER_WAIT:
    case State::DOMAIN_BROADCAST:
    case State::REGISTERED:
    case State::CONTESTED_PENDING:
        break;
    }
    return false;
}

bool IdentityFlow::setState(State state)
{
    if (m_record.state == state) return save();
    const State previous{m_record.state};
    m_record.state = state;
    m_record.last_error.clear();
    m_record.last_failure.reset();
    m_retries = 0;
    m_wait_started = m_next_poll = m_poll_interval = 0;
    if (!save()) {
        m_record.state = previous;
        const QString error{tr("Dash Core could not save the registration progress to the wallet.")};
        m_record.last_error = error.toStdString();
        Q_EMIT failed(tr("Save progress"), error, {});
        return false;
    }
    LogPrintf("Platform identity flow: state=%d name=%s\n", static_cast<int>(state), m_record.normalized_label);
    Q_EMIT stateChanged();
    return true;
}

void IdentityFlow::fail(const QString& step, const platform::Status& status, PlatformUi::Context context, Severity severity)
{
    fail(step, PlatformUi::Describe(status, context, step, QString::fromStdString(m_record.label)).text, severity, &status);
}

void IdentityFlow::fail(const QString& step, const QString& error, Severity severity, const platform::Status* status)
{
    const bool fatal{severity == Severity::FATAL || (severity == Severity::RETRYABLE && ++m_retries > MAX_RETRIES)};
    // A wait that fails the same way on every tick keeps when it began.
    const bool repeated{!fatal && m_record.last_error == error.toStdString() && m_record.last_failure &&
                        m_record.last_failure->operation == step.toStdString() &&
                        SameStatus(m_record.last_failure->status, status)};
    if (!repeated) {
        m_record.last_error = error.toStdString();
        Record::Failure failure;
        failure.operation = step.toStdString();
        failure.time = GetTime();
        if (status) failure.status = *status;
        m_record.last_failure = std::move(failure);
    }
    if (fatal && m_record.state != State::FAILED) {
        // Where the attempt failed decides what reset() keeps.
        m_record.resume_state = m_record.state;
        m_record.state = State::FAILED;
        m_retries = 0;
        releaseRegistrationUnlock();
    }
    save();
    Q_EMIT failed(step, lastErrorText(), lastErrorDetails());
    if (fatal) Q_EMIT stateChanged();
}

QString IdentityFlow::takenText() const
{
    return tr("“%1” was registered by someone else. Choose another username.").arg(QString::fromStdString(m_record.label));
}

QString IdentityFlow::lostVoteText() const
{
    return tr("Masternodes gave “%1” to someone else. Choose another username.").arg(QString::fromStdString(m_record.label));
}

QString IdentityFlow::lastErrorText() const
{
    const auto& failure{m_record.last_failure};
    std::optional<platform::Status> status{failure ? failure->status : std::nullopt};
    if (!failure) {
        if (const auto code{StoredConsensusCode(m_record.last_error)})
            status = platform::Status{StatusKind::CONSENSUS, *code, {}};
    }
    if (!status) return QString::fromStdString(m_record.last_error);
    // Where it failed decides what some codes mean.
    const State step{m_record.state == State::FAILED || m_record.state == State::NEEDS_UNLOCK ? m_record.resume_state
                                                                                              : m_record.state};
    const auto context{step <= State::IDENTITY_BROADCAST ? PlatformUi::Context::IDENTITY_CREATE
                                                         : PlatformUi::Context::NAME_REGISTER};
    return PlatformUi::Describe(*status, context, {}, QString::fromStdString(m_record.label)).text;
}

QString IdentityFlow::lastErrorDetails() const
{
    if (m_record.last_error.empty()) return {};
    const auto& failure{m_record.last_failure};
    // A record written before the details were kept points to the log.
    if (!failure) return QStringLiteral("Message: see debug.log (category platform)");
    return PlatformUi::Details(failure->status ? &*failure->status : nullptr,
                               QString::fromStdString(failure->operation), failure->time);
}

void IdentityFlow::needUnlock(State resume_state)
{
    m_record.resume_state = resume_state;
    setState(State::NEEDS_UNLOCK);
}

void IdentityFlow::releaseRegistrationUnlock()
{
    m_funding_lock_poll->stop();
    m_registration_unlock_timer->stop();
    m_registration_unlock.reset();
}

bool IdentityFlow::signedForCurrentVersion(const Record::SignedTransition& signed_transition) const
{
    // Unknown on either side (no verified read had shown a version): sent,
    // and a refusal as stale signs it again.
    const uint32_t current{m_service.protocolVersion()};
    return signed_transition.protocol_version == 0 || current == 0 || signed_transition.protocol_version == current;
}

bool IdentityFlow::pollDue() const { return GetTime() >= m_next_poll; }

void IdentityFlow::schedulePoll()
{
    const int64_t now{GetTime()};
    if (m_wait_started == 0) m_wait_started = now;
    m_poll_interval = std::clamp(m_poll_interval * 3 / 2, FIRST_POLL_INTERVAL_SECONDS, MAX_POLL_INTERVAL_SECONDS);
    m_next_poll = now + m_poll_interval;
}

bool IdentityFlow::waitExpired() const
{
    return m_wait_started != 0 && GetTime() - m_wait_started >= CONFIRM_WINDOW_SECONDS;
}

void IdentityFlow::restartWait()
{
    m_wait_started = m_next_poll = m_poll_interval = 0;
    schedulePoll();
}

void IdentityFlow::retryAfterUnlock()
{
    if (m_record.state != State::NEEDS_UNLOCK || m_signing || m_service.stopped()) return;
    const State resume{m_record.resume_state};
    m_record.resume_state = State::NONE;
    if (setState(resume)) {
        advance();
    } else {
        m_record.resume_state = resume;
    }
}

void IdentityFlow::reset()
{
    if (m_record.state != State::FAILED) return;
    Record record;
    switch (m_record.resume_state) {
    case State::IDENTITY_CONFIRMED:
    case State::PREORDER_BROADCAST:
    case State::PREORDER_WAIT:
    case State::DOMAIN_BROADCAST:
    case State::CONTESTED_PENDING:
    case State::REGISTERED:
        // The identity exists on Platform: only the name is given up, and
        // the next one is paid from the identity's credits.
        record.state = State::IDENTITY_CONFIRMED;
        record.identity_id = m_record.identity_id;
        record.auth_key_id = m_record.auth_key_id;
        record.encryption_key_id = m_record.encryption_key_id;
        record.decryption_key_id = m_record.decryption_key_id;
        record.started_at = m_record.started_at;
        break;
    case State::FUNDING_LOCKED:
    case State::IDENTITY_BROADCAST:
        // The asset lock is on chain and unconsumed: register from it again.
        record = m_record;
        record.state = State::FUNDING_LOCKED;
        record.identity_id = {};
        record.resume_state = State::NONE;
        record.last_error.clear();
        record.last_failure.reset();
        // Signed again, under one unlock, by the next attempt.
        record.signed_identity_create.clear();
        record.signed_preorder = {};
        record.signed_domain = {};
        record.signed_profile = {};
        break;
    case State::NONE:
    case State::FUNDING_SENT:
    case State::NEEDS_UNLOCK:
    case State::FAILED:
        // Nothing of the attempt is or can get on chain (the funding payment
        // is missing from the wallet or a final spend conflicts with it):
        // start over.
        break;
    }
    m_record = record;
    m_retries = 0;
    m_funding_dead_since = 0;
    m_funding_wait = FundingWait::NONE;
    m_wait_started = m_next_poll = m_poll_interval = 0;
    m_prefer_chain_proof = false;
    m_identity_create_sent = false;
    m_preorder_sent = false;
    m_domain_resigns = 0;
    if (record.state == State::NONE) {
        eraseRecord();
    } else {
        save();
    }
    Q_EMIT stateChanged();
}

bool IdentityFlow::start(const QString& label, CAmount funding_amount, QString& error, const QString& display_name)
{
    if (m_record.state == State::FAILED) reset();
    if (m_record.state == State::REGISTERED) {
        // A second funded registration would replace the identity record.
        error = tr("This wallet already has a DashPay username.");
        return false;
    }
    const bool name_only{m_record.AwaitsUsername()};
    if (active() && !name_only) {
        error = tr("A username registration is already running.");
        return false;
    }

    const std::string label_str{label.toStdString()};
    if (!platform::helpers::IsValidUsername(label_str)) {
        error = tr("This can't be a username. Use 3 to 63 letters, numbers and single hyphens, starting and ending "
                   "with a letter or number.");
        return false;
    }
    if (!m_service.writesAllowed(error)) return false;
    const std::string normalized{platform::helpers::NormalizeLabel(label_str)};

    if (name_only) {
        m_record.label = label_str;
        m_record.normalized_label = normalized;
        m_record.contested = platform::helpers::IsContestedUsername(label_str);
        GetStrongRandBytes(m_record.preorder_salt);
        if (!save()) {
            m_record.label.clear();
            m_record.normalized_label.clear();
            m_record.preorder_salt.fill(0);
            error = tr("Dash Core could not save the registration progress to the wallet.");
            return false;
        }
        Q_EMIT stateChanged();
        return true;
    }

    if (!m_service.registrationAllowed(error)) return false;
    Wallet& wallet{m_service.walletModel().wallet()};

    // The one passphrase of the registration: the wallet stays unlocked
    // while the funding payment gets its InstantSend lock, and the
    // registration is signed then (see broadcastIdentityCreate()).
    releaseRegistrationUnlock();
    WalletModel::UnlockContext unlock{m_service.walletModel().requestUnlock()};
    if (!unlock.isValid()) {
        error = tr("The wallet stayed locked, so nothing was sent. Try again and enter your passphrase.");
        return false;
    }
    const auto funding_pubkey{wallet.getPlatformPubKey(wallet::RegistrationFundingKey{0})};
    if (!funding_pubkey) {
        error = tr("Dash Core could not derive the key for the funding payment from this wallet.");
        return false;
    }

    auto res{wallet.createAssetLockTransaction(funding_amount, funding_pubkey.value, wallet::CCoinControl{})};
    if (!res) {
        error = QString::fromStdString(util::ErrorString(res).translated);
        return false;
    }
    const CTransactionRef& tx{*res};

    // Persist the full flow state, including the preorder salt, before the
    // irreversible broadcast.
    m_funding_dead_since = 0;
    m_funding_wait = FundingWait::NONE;
    m_prefer_chain_proof = false;
    m_identity_create_sent = false;
    m_preorder_sent = false;
    m_domain_resigns = 0;
    m_profile_sent = false;
    m_record = Record{};
    m_record.state = State::FUNDING_SENT;
    m_record.funding_txid = tx->GetHash();
    m_record.funding_key_index = 0;
    m_record.funding_amount = funding_amount;
    m_record.auth_key_id = KEY_HIGH;
    m_record.encryption_key_id = KEY_ENCRYPTION;
    m_record.decryption_key_id = KEY_DECRYPTION;
    m_record.label = label_str;
    m_record.normalized_label = normalized;
    m_record.contested = platform::helpers::IsContestedUsername(label_str);
    m_record.started_at = GetTime();
    m_record.profile_display_name = display_name.trimmed().toStdString();
    GetStrongRandBytes(m_record.preorder_salt);
    if (!save()) {
        // Broadcasting the asset lock now would fund an identity flow the
        // GUI can never resume.
        m_record = Record{};
        error = tr("Dash Core could not save the registration progress to the wallet, so the funding payment was "
                   "not sent.");
        return false;
    }

    if (const auto broadcast_error{wallet.commitTransaction(tx, {}, {})}) {
        // The mempool rejected the transaction: it can never confirm, but it
        // was committed to the wallet and would linger as a phantom pending
        // debit. Abandon it to release the inputs and roll the flow back.
        wallet.abandonTransaction(tx->GetHash());
        m_record = Record{};
        eraseRecord();
        // A mempool rejection reason is untranslated (only original is set).
        error = tr("The funding payment was not accepted by the network. %1")
                    .arg(QString::fromStdString(broadcast_error->translated.empty() ? broadcast_error->original
                                                                                   : broadcast_error->translated));
        return false;
    }

    // Bounded: the lock usually arrives within seconds, and the wallet is
    // locked again after FUNDING_LOCK_WAIT_SECONDS whether it did or not.
    m_registration_unlock.emplace(std::move(unlock));
    m_registration_unlock_timer->start();
    m_funding_lock_poll->start();
    Q_EMIT stateChanged();
    return true;
}

void IdentityFlow::advance()
{
    if (m_step_in_flight || m_signing || m_service.stopped()) return;
    switch (m_record.state) {
    case State::NONE:
    case State::FAILED:
    case State::NEEDS_UNLOCK:
        return;
    case State::REGISTERED:
        if (m_record.signed_profile.empty() || !pollDue()) return;
        if (!m_profile_sent && !signedForCurrentVersion(m_record.signed_profile)) {
            // Only a new identity's first profile is signed by the flow.
            dropSignedProfile("signed under an earlier protocol version");
            return;
        }
        if (m_profile_sent) {
            confirmProfile();
        } else {
            publishProfile();
        }
        return;
    case State::FUNDING_SENT:
        checkFundingLock();
        return;
    case State::FUNDING_LOCKED:
        broadcastIdentityCreate();
        return;
    case State::IDENTITY_BROADCAST:
        if (pollDue()) confirmIdentity();
        return;
    case State::IDENTITY_CONFIRMED:
        if (m_record.AwaitsUsername()) return;
        checkName();
        return;
    case State::PREORDER_BROADCAST:
        if (pollDue()) prepareDocumentStep(State::PREORDER_BROADCAST, &IdentityFlow::broadcastPreorder);
        return;
    case State::PREORDER_WAIT:
        if (pollDue()) prepareDocumentStep(State::PREORDER_WAIT, &IdentityFlow::broadcastDomain);
        return;
    case State::DOMAIN_BROADCAST:
    case State::CONTESTED_PENDING:
        if (pollDue()) confirmDomain();
        return;
    }
}

bool IdentityFlow::sendAllowed(const QString& step)
{
    QString refusal;
    if (m_service.writesAllowed(refusal)) return true;
    // Writes are refused for now (protocol version ahead of this build,
    // network changed): the step waits where it is.
    fail(step, refusal, Severity::TRANSIENT);
    return false;
}

bool IdentityFlow::unlockForStep(const QString& step, State resume_state, std::optional<WalletModel::UnlockContext>& unlock)
{
    if (!sendAllowed(step)) return false;
    // Inside the registration's own unlock this asks for nothing.
    unlock.emplace(m_service.walletModel().requestUnlock());
    if (unlock->isValid()) return true;
    unlock.reset();
    needUnlock(resume_state);
    return false;
}

util::Result<platform::Built> IdentityFlow::signDocument(
    OperationKind kind, const platform::IdentityPublicKey& key,
    const std::function<util::Result<platform::Built>(const platform::SigningOperation&)>& build)
{
    auto attempt{m_service.beginSigningOperation(kind, {key.id}, key, std::nullopt)};
    if (!attempt.op) {
        return util::Error{Untranslated(attempt.unlock_declined ? "the wallet is locked" : attempt.refusal.toStdString())};
    }
    return build(*attempt.op);
}

bool IdentityFlow::signUsername(const platform::IdentityPublicKey& key, uint64_t nonce, std::string& error)
{
    platform::PlatformClient& client{m_service.client()};
    const auto preorder{signDocument(OperationKind::DPNS_PREORDER, key, [&](const platform::SigningOperation& op) {
        return client.buildDpnsPreorder(op, m_record.identity_id, nonce, m_record.label, m_record.preorder_salt);
    })};
    if (!preorder) {
        error = util::ErrorString(preorder).original;
        return false;
    }
    // The domain follows the preorder at the next nonce: Drive takes it once
    // the preorder is committed, which the flow waits for before sending it.
    const auto domain{signDocument(OperationKind::DPNS_DOMAIN, key, [&](const platform::SigningOperation& op) {
        return client.buildDpnsDomain(op, m_record.identity_id, nonce + 1, m_record.label, m_record.preorder_salt);
    })};
    if (!domain) {
        error = util::ErrorString(domain).original;
        return false;
    }
    const uint32_t version{m_service.protocolVersion()};
    m_record.signed_preorder = {preorder->bytes, nonce, version};
    m_record.signed_domain = {domain->bytes, nonce + 1, version};
    return true;
}

bool IdentityFlow::signDomain(const platform::IdentityPublicKey& key, uint64_t nonce, std::string& error)
{
    const auto domain{signDocument(OperationKind::DPNS_DOMAIN, key, [&](const platform::SigningOperation& op) {
        return m_service.client().buildDpnsDomain(op, m_record.identity_id, nonce, m_record.label, m_record.preorder_salt);
    })};
    if (!domain) {
        error = util::ErrorString(domain).original;
        return false;
    }
    m_record.signed_domain = {domain->bytes, nonce, m_service.protocolVersion()};
    return true;
}

void IdentityFlow::signProfile(const platform::IdentityPublicKey& key)
{
    // Only a new identity: nothing has used its DashPay nonce yet, and it
    // has no profile a create could collide with.
    if (m_record.profile_display_name.empty() || !m_record.signed_profile.empty() || m_record.funding_amount == 0) {
        return;
    }
    platform::ProfileInput input;
    input.display_name = m_record.profile_display_name;
    const auto profile{signDocument(OperationKind::PROFILE, key, [&](const platform::SigningOperation& op) {
        return m_service.client().buildProfile(op, m_record.identity_id, FIRST_CONTRACT_NONCE, platform::Profile{}, input);
    })};
    if (!profile) {
        // The user can still add it from the dashboard.
        LogPrintf("Platform identity flow: profile not signed: %s\n", util::ErrorString(profile).original);
        m_record.profile_display_name.clear();
        return;
    }
    m_record.signed_profile = {profile->bytes, FIRST_CONTRACT_NONCE, m_service.protocolVersion()};
}

IdentityFlow::FundingWait IdentityFlow::fundingWait() const
{
    if (m_record.state != State::FUNDING_SENT) return FundingWait::NONE;
    Wallet& wallet{m_service.walletModel().wallet()};
    interfaces::WalletTxStatus status;
    interfaces::WalletOrderForm order_form;
    bool in_mempool{false};
    int num_blocks{0};
    const auto wtx{wallet.getWalletTxDetails(m_record.funding_txid, status, order_form, in_mempool, num_blocks)};
    if (!wtx.tx || status.is_islocked || status.depth_in_main_chain > 0) return FundingWait::NONE;
    if (status.depth_in_main_chain < 0) return FundingWait::ENDING;
    if (!status.is_abandoned) return FundingWait::NONE;
    std::vector<COutPoint> inputs;
    for (const CTxIn& input : wtx.tx->vin) {
        inputs.push_back(input.prevout);
    }
    for (const auto& coin : wallet.getCoins(inputs)) {
        // Another payment of this wallet spends it and is not abandoned.
        if (coin.is_spent) return FundingWait::ENDING;
    }
    return FundingWait::RELEASED;
}

bool IdentityFlow::releaseFunding(QString& error)
{
    if (m_signing || fundingWait() != FundingWait::RELEASED) {
        error = tr("The funding payment can't be released now.");
        return false;
    }
    Wallet& wallet{m_service.walletModel().wallet()};
    const CTransactionRef funding{wallet.getTx(m_record.funding_txid)};
    if (!funding) {
        error = tr("The funding payment is missing from this wallet.");
        return false;
    }
    std::vector<COutPoint> inputs;
    for (const CTxIn& input : funding->vin) {
        inputs.push_back(input.prevout);
    }
    wallet::CCoinControl coin_control;
    coin_control.m_allow_other_inputs = false;
    CAmount total{0};
    const auto coins{wallet.getCoins(inputs)};
    for (size_t i = 0; i < inputs.size(); ++i) {
        if (coins[i].depth_in_main_chain < 0) {
            error = tr("A coin of the funding payment is not in this wallet, so it can't be released.");
            return false;
        }
        coin_control.Select(inputs[i]);
        total += coins[i].txout.nValue;
    }

    // The passphrase dialog spins a nested event loop: no tick may run the
    // flow meanwhile.
    SigningGuard signing{m_signing};
    WalletModel::UnlockContext unlock{m_service.walletModel().requestUnlock()};
    if (!unlock.isValid()) {
        error = tr("The wallet stayed locked, so nothing was sent.");
        return false;
    }
    if (fundingWait() != FundingWait::RELEASED) {
        // The payment was accepted or locked while the passphrase was asked.
        error = tr("The funding payment can't be released now.");
        return false;
    }
    const auto destination{wallet.getNewDestination("")};
    if (!destination) {
        error = QString::fromStdString(util::ErrorString(destination).translated);
        return false;
    }
    // All of it back to this wallet, the fee paid out of it at the rate any
    // payment of this wallet pays now.
    const std::vector<wallet::CRecipient> recipients{
        wallet::CRecipient{GetScriptForDestination(*destination), total, /*fSubtractFeeFromAmount=*/true}};
    int change_pos{-1};
    CAmount fee{0};
    const auto tx{wallet.createTransaction(recipients, coin_control, /*sign=*/true, change_pos, fee)};
    if (!tx) {
        error = QString::fromStdString(util::ErrorString(tx).translated);
        return false;
    }
    const auto broadcast_error{wallet.commitTransaction(*tx, {}, {})};
    interfaces::WalletTxStatus status;
    interfaces::WalletOrderForm order_form;
    bool in_mempool{false};
    int num_blocks{0};
    wallet.getWalletTxDetails((*tx)->GetHash(), status, order_form, in_mempool, num_blocks);
    if (broadcast_error || !in_mempool) {
        // Refused like the funding payment, or never sent (-walletbroadcast=0):
        // nothing is left pending that would keep the registration ending.
        if (!wallet.abandonTransaction((*tx)->GetHash())) {
            LogPrintf("Platform identity flow: release %s not abandoned\n", (*tx)->GetHash().ToString());
        }
        const std::string reason{!broadcast_error                     ? "Dash Core is not broadcasting transactions."
                                 : broadcast_error->translated.empty() ? broadcast_error->original
                                                                       : broadcast_error->translated};
        error = tr("The payment was not accepted by the network. %1").arg(QString::fromStdString(reason));
        updateFundingWait();
        return false;
    }
    LogPrintf("Platform identity flow: released funding %s in %s (fee %d)\n", m_record.funding_txid.ToString(),
              (*tx)->GetHash().ToString(), fee);
    updateFundingWait();
    return true;
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
        fail(tr("Fund identity"), tr("The funding payment is missing from this wallet. No funds were spent."),
             Severity::FATAL);
        return;
    }

    if (status.depth_in_main_chain < 0) {
        // A spend of its coins that conflicts with it is in the chain. Only
        // a final one ends the registration: a reorganization may still
        // mine the payment, which nothing but the record would remember.
        if (status.is_conflict_chainlocked || -status.depth_in_main_chain >= FUNDING_FINAL_DEPTH) {
            fail(tr("Fund identity"), tr("The coins of the funding payment were spent by another payment."),
                 Severity::FATAL);
            return;
        }
        updateFundingWait();
        fail(tr("Fund identity"),
             tr("A payment that spends the coins of the funding payment was confirmed. The registration ends once "
                "it is final."),
             Severity::TRANSIENT);
        return;
    }
    if (status.is_islocked || status.is_chainlocked || status.depth_in_main_chain >= FUNDING_FINAL_DEPTH) {
        m_funding_dead_since = 0;
        m_funding_wait = FundingWait::NONE;
        // Straight on to the identity, while the registration's unlock lasts.
        if (setState(State::FUNDING_LOCKED)) broadcastIdentityCreate();
        return;
    }
    updateFundingWait();
    if (status.is_abandoned) {
        // Abandoning only frees the coins in this wallet: another node may
        // still hold the payment and mine it, so the record keeps it until it
        // confirms or a spend of its coins does (releaseFunding() sends one).
        fail(tr("Fund identity"),
             tr("The network is not accepting the funding payment now, so its coins were released. The "
                "registration continues if the payment confirms, and ends once its coins are spent elsewhere."),
             Severity::TRANSIENT);
        return;
    }

    if (status.depth_in_main_chain == 0 && !in_mempool) {
        // Unconfirmed and not in the local mempool: the broadcast was lost
        // (restart) or the mempool rejected the transaction, which no
        // islock/chainlock/confirmation poll would ever notice. Rebroadcast
        // to recover the transient case; one the node keeps refusing is
        // abandoned to release its inputs, and watched as abandoned above.
        if (wallet.resendTransaction(m_record.funding_txid)) {
            m_funding_dead_since = 0;
            return;
        }
        if (m_funding_dead_since == 0) m_funding_dead_since = GetTime();
        if (GetTime() - m_funding_dead_since >= FUNDING_DEAD_SECONDS) {
            wallet.abandonTransaction(m_record.funding_txid);
            updateFundingWait();
        }
        return;
    }
    m_funding_dead_since = 0;
}

void IdentityFlow::updateFundingWait()
{
    const FundingWait wait{fundingWait()};
    if (wait == m_funding_wait) return;
    m_funding_wait = wait;
    Q_EMIT stateChanged();
}

void IdentityFlow::broadcastIdentityCreate()
{
    if (!m_record.signed_identity_create.empty()) {
        sendIdentityCreate();
        return;
    }
    Wallet& wallet{m_service.walletModel().wallet()};
    interfaces::Node& node{m_service.clientModel().node()};

    // Build the asset lock proof: prefer the InstantSend lock, fall back to
    // a chain-locked block proof (also once Platform refused the islock's
    // signature because its quorum rotated out). The identity is funded by
    // the (single) credit output in the asset lock payload, at
    // ASSET_LOCK_OUTPUT_INDEX.
    platform::AssetLockProof proof;
    proof.output_index = ASSET_LOCK_OUTPUT_INDEX;
    proof.out_point = BurnOutpoint(m_record.funding_txid, ASSET_LOCK_OUTPUT_INDEX);
    auto islock{m_prefer_chain_proof ? std::vector<uint8_t>{} : node.llmq().getInstantSendLock(m_record.funding_txid)};
    if (!islock.empty()) {
        const auto wtx{wallet.getWalletTx(m_record.funding_txid)};
        if (!wtx.tx) {
            fail(tr("Create identity"), tr("The funding payment is missing from this wallet."), Severity::RETRYABLE);
            return;
        }
        CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
        stream << *wtx.tx;
        proof.is_instant = true;
        proof.transaction.assign(UCharCast(stream.data()), UCharCast(stream.data()) + stream.size());
        proof.instant_lock = std::move(islock);
    } else {
        interfaces::WalletTxStatus status;
        int num_blocks{0};
        int64_t block_time{0};
        if (!wallet.tryGetTxStatus(m_record.funding_txid, status, num_blocks, block_time) || !status.is_chainlocked) {
            return; // wait for a lock
        }
        proof.core_chain_locked_height = static_cast<uint32_t>(status.block_height);
    }

    if (m_service.protocolVersion() == 0) {
        // The client builds only under a protocol version a verified read
        // has shown, and a registration resumed after a restart has made
        // none yet: any proved answer establishes it. Not while writes wait
        // for an update: no read raises it then, and nothing is signed. (A
        // chain id mismatch is read through: a verified read clears it.)
        if (m_service.writesFrozen() && !sendAllowed(tr("Create identity"))) {
            releaseRegistrationUnlock();
            return;
        }
        // An unanswered read is asked again with the backoff of a wait.
        if (!pollDue()) return;
        m_step_in_flight = true;
        QPointer<IdentityFlow> self{this};
        m_service.client().resolveName(m_record.normalized_label, [self](platform::Result<platform::DpnsName> res) {
            if (!self) return;
            self->m_service.post([self, res = std::move(res)] {
                if (!self) return;
                self->m_step_in_flight = false;
                if (self->m_record.state != State::FUNDING_LOCKED) return;
                if (!res.ok() && !res.provenAbsent()) {
                    self->schedulePoll();
                    self->fail(tr("Create identity"), res.status, PlatformUi::Context::IDENTITY_CREATE,
                               Severity::TRANSIENT);
                    return;
                }
                if (self->m_service.protocolVersion() != 0) {
                    self->broadcastIdentityCreate();
                } else {
                    self->schedulePoll();
                }
            });
        });
        return;
    }

    // One unlock signs the whole registration: the identity, and (the new
    // identity has used no contract yet) the username preorder and domain
    // at DPNS nonces 1 and 2 and the profile at DashPay nonce 1. On an
    // encrypted wallet the public keys come from the seed too, so the unlock
    // comes before they are derived.
    SigningGuard signing{m_signing};
    std::optional<WalletModel::UnlockContext> unlock;
    if (!unlockForStep(tr("Create identity"), State::FUNDING_LOCKED, unlock)) {
        releaseRegistrationUnlock();
        return;
    }
    std::vector<uint32_t> key_ids;
    for (const auto& spec : RegistrationKeys()) {
        key_ids.push_back(spec.id);
    }
    auto attempt{m_service.beginSigningOperation(OperationKind::IDENTITY_CREATE, std::move(key_ids), std::nullopt,
                                                 wallet::RegistrationFundingKey{m_record.funding_key_index})};
    if (!attempt.op) {
        fail(tr("Create identity"), attempt.refusal, Severity::TRANSIENT);
        return;
    }

    platform::ContractBounds contact_request_bounds;
    contact_request_bounds.kind = platform::ContractBounds::Kind::SINGLE_CONTRACT_DOCUMENT_TYPE;
    contact_request_bounds.contract_id = platform::helpers::SystemContractId(platform::helpers::SystemContract::DASHPAY);
    contact_request_bounds.document_type = "contactRequest";
    std::vector<platform::NewIdentityKey> keys;
    for (const auto& spec : RegistrationKeys()) {
        const auto pubkey{wallet.getPlatformPubKey(wallet::IdentityAuthKey{0, spec.id})};
        if (!pubkey) {
            fail(tr("Create identity"), tr("Dash Core could not derive your identity's keys from this wallet."),
                 Severity::RETRYABLE);
            return;
        }
        platform::NewIdentityKey key;
        key.id = spec.id;
        key.purpose = spec.purpose;
        key.security_level = spec.security_level;
        key.pubkey = pubkey.value;
        if (spec.contact_request_bound) key.contract_bounds = contact_request_bounds;
        keys.push_back(std::move(key));
    }

    auto built{m_service.client().buildIdentityCreate(*attempt.op, proof, keys)};
    attempt.op.reset();
    if (!built) {
        fail(tr("Create identity"), {StatusKind::INTERNAL, 0, util::ErrorString(built).original},
             PlatformUi::Context::IDENTITY_CREATE, Severity::RETRYABLE);
        return;
    }
    // The identity id is derived from the asset lock.
    m_record.identity_id = built->object_id;
    m_record.signed_identity_create = built->bytes;
    if (!m_record.label.empty() && m_record.signed_preorder.empty()) {
        // What is not signed now is signed by its own step later.
        std::string error;
        if (const auto key{RegisteredDocumentKey(wallet)}; key && signUsername(*key, FIRST_CONTRACT_NONCE, error)) {
            signProfile(*key);
        } else {
            // The profile is only signed ahead: the user adds it later.
            LogPrintf("Platform identity flow: username not signed ahead: %s\n", error);
            m_record.profile_display_name.clear();
        }
    }
    // Everything signed is persisted (by the state change) before the
    // wallet is locked again and anything is broadcast.
    const bool saved{setState(State::IDENTITY_BROADCAST)};
    unlock.reset();
    releaseRegistrationUnlock();
    if (saved) sendIdentityCreate();
}

void IdentityFlow::sendIdentityCreate()
{
    if (!sendAllowed(tr("Create identity")) || !setState(State::IDENTITY_BROADCAST)) return;
    m_identity_create_sent = true;
    restartWait();
    QPointer<IdentityFlow> self{this};
    broadcast(tr("Create identity"), m_record.signed_identity_create, [self](const platform::Status& status) {
        if (!self || BroadcastAccepted(status)) return;
        if (IsConsensus(status, CODE_IDENTITY_ALREADY_EXISTS) || IsConsensus(status, CODE_ASSET_LOCK_OUTPOINT_CONSUMED)) {
            // An earlier broadcast was applied: the confirmation poll settles it.
            return;
        }
        // Not taken: the same transition goes out again at the next poll
        // (Platform behind Core, an unreachable or unverified node).
        self->m_identity_create_sent = false;
        if (IsConsensus(status, CODE_ASSET_LOCK_CORE_HEIGHT_AHEAD)) {
            self->fail(tr("Create identity"), status, PlatformUi::Context::IDENTITY_CREATE, Severity::TRANSIENT);
            return;
        }
        if (IsConsensus(status, CODE_ASSET_LOCK_INSTANT_SIGNATURE)) {
            // The islock's quorum is gone: the proof is rebuilt from the
            // ChainLock and the identity signed again.
            self->m_prefer_chain_proof = true;
            self->m_record.signed_identity_create.clear();
            if (self->setState(State::FUNDING_LOCKED)) {
                self->fail(tr("Create identity"), status, PlatformUi::Context::IDENTITY_CREATE, Severity::TRANSIENT);
            }
            return;
        }
        self->fail(tr("Create identity"), status, PlatformUi::Context::IDENTITY_CREATE, SeverityOf(status));
    });
}

void IdentityFlow::confirmIdentity()
{
    m_step_in_flight = true;
    QPointer<IdentityFlow> self{this};
    m_service.client().getIdentity(m_record.identity_id, [self](platform::Result<platform::Identity> res) {
        if (!self) return;
        self->m_service.post([self, res = std::move(res)] {
            if (!self) return;
            self->m_step_in_flight = false;
            if (self->m_record.state != State::IDENTITY_BROADCAST) return;
            if (res.ok()) {
                self->m_record.signed_identity_create.clear();
                self->setState(State::IDENTITY_CONFIRMED);
                return;
            }
            if (!res.provenAbsent()) {
                self->fail(tr("Confirm identity"), res.status, PlatformUi::Context::IDENTITY_CREATE, Severity::TRANSIENT);
                return;
            }
            // Not sent this session (it may never have left the process
            // before a restart), or not applied in the confirmation window:
            // the same signed transition goes out again. A record of an
            // earlier layout has none: it is built again once the wait
            // runs out.
            const bool have_signed{!self->m_record.signed_identity_create.empty()};
            if (have_signed && !self->m_identity_create_sent) {
                self->sendIdentityCreate();
                return;
            }
            self->schedulePoll();
            if (self->waitExpired()) {
                if (have_signed) {
                    self->sendIdentityCreate();
                } else {
                    self->setState(State::FUNDING_LOCKED);
                }
            }
        });
    });
}

void IdentityFlow::checkName()
{
    // Before any credits are spent on the name: one already registered to
    // this identity (its confirmation was lost, or another wallet with the
    // same seed registered it) is adopted, and one registered to anybody
    // else ends the attempt.
    m_step_in_flight = true;
    QPointer<IdentityFlow> self{this};
    m_service.client().resolveName(m_record.normalized_label, [self](platform::Result<platform::DpnsName> res) {
        if (!self) return;
        self->m_service.post([self, res = std::move(res)] {
            if (!self) return;
            self->m_step_in_flight = false;
            if (self->m_record.state != State::IDENTITY_CONFIRMED || self->m_record.AwaitsUsername()) return;
            if (res.ok()) {
                if (res.value->identity == self->m_record.identity_id) {
                    self->finishRegistration();
                } else {
                    self->fail(tr("Check username"), self->takenText(), Severity::FATAL);
                }
                return;
            }
            if (!res.provenAbsent()) {
                self->fail(tr("Check username"), res.status, PlatformUi::Context::NAME_REGISTER, Severity::TRANSIENT);
                return;
            }
            self->prepareDocumentStep(State::IDENTITY_CONFIRMED, &IdentityFlow::broadcastPreorder);
        });
    });
}

void IdentityFlow::prepareDocumentStep(State step, void (IdentityFlow::*build)(const platform::IdentityPublicKey& key,
                                                                               uint64_t nonce))
{
    // The key documents are signed with comes from the proved identity, so
    // an identity registered by another wallet with the same seed signs
    // with the key it actually carries; the nonce read shows which of the
    // username's transitions Platform has taken.
    m_step_in_flight = true;
    QPointer<IdentityFlow> self{this};
    m_service.client().getIdentity(m_record.identity_id, [self, step, build](platform::Result<platform::Identity> res) {
        if (!self) return;
        self->m_service.post([self, step, build, res = std::move(res)] {
            if (!self) return;
            self->m_step_in_flight = false;
            if (self->m_record.state != step) return;
            if (!res.ok()) {
                if (res.provenAbsent()) {
                    self->fail(tr("Read identity"),
                               tr("Your identity was not found on Dash Platform. If you just created it, wait a "
                                  "minute and try again."),
                               Severity::FATAL);
                } else {
                    self->fail(tr("Read identity"), res.status, PlatformUi::Context::NAME_REGISTER, SeverityOf(res.status));
                }
                return;
            }
            const auto key{self->m_service.documentSigningKey(*res.value)};
            if (!key) {
                self->fail(tr("Read identity"), tr("This wallet holds no key that can sign for your identity."),
                           Severity::FATAL);
                return;
            }
            if (step == State::IDENTITY_CONFIRMED && self->m_record.contested) {
                // A premium name locks credits for the masternode vote: a
                // preorder the identity cannot follow with a contest (the
                // reserve and both transitions' fees) would only spend its
                // credits.
                const auto required{self->m_service.contestedNameRequiredCredits()};
                if (!required) {
                    self->fail(tr("Check premium cost"),
                               tr("The cost of a premium username is not known yet. Dash Core will try again."),
                               Severity::TRANSIENT);
                    return;
                }
                if (res.value->balance < *required) {
                    const auto unit{self->m_service.walletModel().getOptionsModel()->getDisplayUnit()};
                    self->fail(tr("Check premium cost"),
                               tr("Your balance on Dash Platform is %1, but a premium username needs %2 for the "
                                  "masternode vote and its fees. Choose a username that is not premium.")
                                   .arg(PlatformUi::formatPlatformBalance(unit, res.value->balance),
                                        PlatformUi::formatPlatformBalance(unit, *required)),
                               Severity::FATAL);
                    return;
                }
            }
            self->m_step_in_flight = true;
            const auto dpns{platform::helpers::SystemContractId(platform::helpers::SystemContract::DPNS)};
            self->m_service.client().getIdentityContractNonce(
                self->m_record.identity_id, dpns, [self, step, build, key](platform::Result<uint64_t> nonce_res) {
                    if (!self) return;
                    self->m_service.post([self, step, build, key, nonce_res = std::move(nonce_res)] {
                        if (!self) return;
                        self->m_step_in_flight = false;
                        if (self->m_record.state != step) return;
                        // A proven absence means the identity never used the
                        // contract: the next nonce is 1, as after a value of 0.
                        if (!nonce_res.ok() && !nonce_res.provenAbsent()) {
                            self->fail(tr("Read identity nonce"), nonce_res.status, PlatformUi::Context::NAME_REGISTER,
                                       SeverityOf(nonce_res.status));
                            return;
                        }
                        (self->*build)(*key, nonce_res.ok() ? *nonce_res.value + 1 : FIRST_CONTRACT_NONCE);
                    });
                });
        });
    });
}

void IdentityFlow::broadcastPreorder(const platform::IdentityPublicKey& key, uint64_t nonce)
{
    bool is_signed{!m_record.signed_preorder.empty()};
    if (is_signed && nonce > m_record.signed_preorder.nonce) {
        // Platform has taken the preorder's nonce: the preorder was applied
        // (or, rarely, the nonce went to another transition, which the
        // domain's wait for the preorder settles).
        if (setState(State::PREORDER_WAIT)) advance();
        return;
    }
    if (is_signed && !signedForCurrentVersion(m_record.signed_preorder)) {
        // Platform upgraded since the username was signed.
        m_record.signed_preorder = {};
        m_record.signed_domain = {};
        is_signed = false;
    }
    const bool signed_ahead{is_signed};
    if (!is_signed) {
        // Not signed ahead (an existing identity, a record of an earlier
        // layout, a transition refused as stale or signed under an earlier
        // protocol version): the preorder and the domain are signed now,
        // under one unlock.
        SigningGuard signing{m_signing};
        std::optional<WalletModel::UnlockContext> unlock;
        if (!unlockForStep(tr("Reserve username"), m_record.state, unlock)) return;
        std::string error;
        if (!signUsername(key, nonce, error)) {
            fail(tr("Reserve username"), {StatusKind::INTERNAL, 0, error}, PlatformUi::Context::NAME_REGISTER,
                 Severity::RETRYABLE);
            return;
        }
    } else if (m_record.state == State::PREORDER_BROADCAST && m_preorder_sent && !waitExpired()) {
        // Sent and not taken yet.
        schedulePoll();
        return;
    }
    sendPreorder(signed_ahead);
}

void IdentityFlow::sendPreorder(bool signed_ahead)
{
    // Persisted (with what was signed) before it is broadcast.
    if (!sendAllowed(tr("Reserve username")) || !setState(State::PREORDER_BROADCAST)) return;
    m_preorder_sent = true;
    restartWait();
    QPointer<IdentityFlow> self{this};
    broadcast(tr("Reserve username"), m_record.signed_preorder.bytes, [self, signed_ahead](const platform::Status& status) {
        if (!self) return;
        if (BroadcastAccepted(status)) return; // the nonce read confirms it
        if (IsConsensus(status, CODE_DUPLICATE_UNIQUE_INDEX) || IsConsensus(status, CODE_DOCUMENT_ALREADY_PRESENT)) {
            // The saltedDomainHash unique index is taken: only this record
            // knows the salt, so an earlier preorder of ours was applied and
            // this one's nonce was not used. The domain is signed at the
            // nonce Platform shows next.
            self->m_record.signed_domain = {};
            self->setState(State::PREORDER_WAIT);
            return;
        }
        if (IsConsensus(status, CODE_INVALID_IDENTITY_NONCE)) {
            // Taken already (by this preorder, sent before) or stale: the
            // nonce read decides, now.
            self->m_next_poll = 0;
            return;
        }
        self->m_preorder_sent = false;
        if (signed_ahead && IsConsensus(status, CODE_INVALID_DOCUMENT_TRANSITION_ID)) {
            // Signed before Platform upgraded: CheckTx refused it without
            // spending its nonce, so both are signed again at the same one.
            self->m_record.signed_preorder = {};
            self->m_record.signed_domain = {};
            self->m_next_poll = 0;
            self->save();
            return;
        }
        // Not taken: sent again at the next poll.
        self->fail(tr("Reserve username"), status, PlatformUi::Context::NAME_REGISTER, SeverityOf(status));
    });
}

void IdentityFlow::broadcastDomain(const platform::IdentityPublicKey& key, uint64_t next_nonce)
{
    bool is_signed{!m_record.signed_domain.empty()};
    const uint64_t signed_nonce{m_record.signed_domain.nonce};
    if (is_signed && signed_nonce > next_nonce) {
        // The node answering has not seen the preorder's nonce yet; after
        // the confirmation window the username is signed again.
        schedulePoll();
        if (waitExpired()) {
            m_record.signed_preorder = {};
            m_record.signed_domain = {};
            setState(State::PREORDER_BROADCAST);
        }
        return;
    }
    if (is_signed && signed_nonce < next_nonce) {
        // Its nonce was spent: by this domain (the name is registered, or
        // Platform refused it in the block) or by another transition.
        m_step_in_flight = true;
        QPointer<IdentityFlow> self{this};
        m_service.client().resolveName(m_record.normalized_label,
                                       [self, key, next_nonce](platform::Result<platform::DpnsName> res) {
                                           if (!self) return;
                                           self->m_service.post([self, key, next_nonce, res = std::move(res)] {
                                               if (!self) return;
                                               self->m_step_in_flight = false;
                                               if (self->m_record.state != State::PREORDER_WAIT) return;
                                               if (res.ok() && res.value->identity == self->m_record.identity_id) {
                                                   self->finishRegistration();
                                               } else if (res.ok()) {
                                                   self->fail(tr("Confirm username"), self->takenText(), Severity::FATAL);
                                               } else if (!res.provenAbsent()) {
                                                   self->fail(tr("Confirm username"), res.status,
                                                              PlatformUi::Context::NAME_REGISTER, Severity::TRANSIENT);
                                               } else if (++self->m_domain_resigns > MAX_DOMAIN_RESIGNS) {
                                                   // Domains keep being spent without registering the name:
                                                   // send the preorder again too, which the domain's data
                                                   // trigger needs to see.
                                                   self->m_domain_resigns = 0;
                                                   self->m_record.signed_preorder = {};
                                                   self->m_record.signed_domain = {};
                                                   self->setState(State::PREORDER_BROADCAST);
                                               } else {
                                                   self->m_record.signed_domain = {};
                                                   self->broadcastDomain(key, next_nonce);
                                               }
                                           });
                                       });
        return;
    }
    if (is_signed && !signedForCurrentVersion(m_record.signed_domain)) {
        // Platform upgraded since the domain was signed.
        m_record.signed_domain = {};
        is_signed = false;
    }
    const bool signed_ahead{is_signed};
    if (!is_signed) {
        // Missing (a record of an earlier layout), signed under an earlier
        // protocol version, or its nonce went to a domain that did not
        // register the name: signed at the next nonce.
        SigningGuard signing{m_signing};
        std::optional<WalletModel::UnlockContext> unlock;
        if (!unlockForStep(tr("Register username"), State::PREORDER_WAIT, unlock)) return;
        std::string error;
        if (!signDomain(key, next_nonce, error)) {
            fail(tr("Register username"), {StatusKind::INTERNAL, 0, error}, PlatformUi::Context::NAME_REGISTER,
                 Severity::RETRYABLE);
            return;
        }
        if (!save()) return;
    }
    sendDomain(signed_ahead);
}

void IdentityFlow::sendDomain(bool signed_ahead)
{
    if (!sendAllowed(tr("Register username"))) return;
    QPointer<IdentityFlow> self{this};
    broadcast(tr("Register username"), m_record.signed_domain.bytes, [self, signed_ahead](const platform::Status& status) {
        if (!self) return;
        if (BroadcastAccepted(status) || IsConsensus(status, CODE_DOCUMENT_ALREADY_PRESENT) ||
            IsConsensus(status, CODE_DUPLICATE_UNIQUE_INDEX) || IsConsensus(status, CODE_CONTEST_ALREADY_CONTESTANT)) {
            // Accepted, or an earlier domain broadcast of ours was applied
            // (the label or contest index is taken): the proved resolve
            // decides whose name it is.
            if (self->setState(State::DOMAIN_BROADCAST)) self->restartWait();
            return;
        }
        if (IsConsensus(status, CODE_DATA_TRIGGER_CONDITION)) {
            // The preorder is not visible to this node yet: wait, and step
            // back to send the username again once the wait runs out.
            self->schedulePoll();
            if (self->waitExpired()) {
                self->m_record.signed_preorder = {};
                self->m_record.signed_domain = {};
                self->setState(State::PREORDER_BROADCAST);
            }
            return;
        }
        if (IsConsensus(status, CODE_INVALID_IDENTITY_NONCE)) {
            // Stale: the next nonce read decides what to sign again.
            self->schedulePoll();
            return;
        }
        if (signed_ahead && IsConsensus(status, CODE_INVALID_DOCUMENT_TRANSITION_ID)) {
            // Signed before Platform upgraded: signed again at the same nonce.
            self->m_record.signed_domain = {};
            self->m_next_poll = 0;
            self->save();
            return;
        }
        self->fail(tr("Register username"), status, PlatformUi::Context::NAME_REGISTER, SeverityOf(status));
    });
}

void IdentityFlow::broadcast(const QString& step, const std::vector<uint8_t>& bytes,
                             std::function<void(const platform::Status&)> on_result)
{
    m_step_in_flight = true;
    const State expected{m_record.state};
    QPointer<IdentityFlow> self{this};
    m_service.client().broadcastStateTransition(bytes, [self, step, expected,
                                                        on_result = std::move(on_result)](platform::Status status) {
        if (!self) return;
        self->m_service.post([self, step, expected, on_result, status = std::move(status)] {
            if (!self) return;
            self->m_step_in_flight = false;
            LogPrint(BCLog::PLATFORM, "Platform identity flow: %s broadcast: kind=%d code=%u %s\n", step.toStdString(),
                     static_cast<int>(status.kind), status.consensus_code, status.message);
            if (self->m_record.state == expected) on_result(status);
        });
    });
}

void IdentityFlow::checkContestedOutcome()
{
    // Proof-verified contested-resource vote state: detects a poll that
    // finished locked (name unusable) or awarded before/without the domain
    // document becoming visible, and a domain transition that was never
    // applied (no contest at all).
    m_step_in_flight = true;
    QPointer<IdentityFlow> self{this};
    m_service.client().getContestedNameState(
        m_record.normalized_label, [self](platform::Result<platform::ContestedNameState> res) {
            if (!self) return;
            self->m_service.post([self, res = std::move(res)] {
                if (!self) return;
                self->m_step_in_flight = false;
                if (self->m_record.state != State::CONTESTED_PENDING) return;
                if (res.provenAbsent()) {
                    self->schedulePoll();
                    // No contest was ever opened: the domain was lost. Send
                    // it again once the wait runs out.
                    if (self->waitExpired()) self->setState(State::PREORDER_WAIT);
                    return;
                }
                if (!res.ok()) {
                    self->fail(tr("Check vote"), res.status, PlatformUi::Context::NAME_REGISTER, Severity::TRANSIENT);
                    return;
                }
                // The contest exists: a later node that has not seen it yet
                // is not a lost domain. The vote runs for up to two weeks,
                // so it is checked at the longest interval.
                self->m_wait_started = GetTime();
                self->m_poll_interval = MAX_POLL_INTERVAL_SECONDS;
                self->schedulePoll();
                const platform::ContestedNameState& state{*res.value};
                switch (state.outcome) {
                case platform::ContestedNameState::Outcome::LOCKED:
                    self->fail(tr("Check vote"),
                               tr("Masternodes have locked “%1”, so nobody can register it. Choose another username.")
                                   .arg(QString::fromStdString(self->m_record.label)),
                               Severity::FATAL);
                    break;
                case platform::ContestedNameState::Outcome::WON:
                    if (state.winner != self->m_record.identity_id) {
                        self->fail(tr("Check vote"), self->lostVoteText(), Severity::FATAL);
                    }
                    // Awarded to us: keep polling; the domain document is what
                    // finally flips the flow to REGISTERED.
                    break;
                case platform::ContestedNameState::Outcome::OPEN:
                    break;
                }
            });
        });
}

void IdentityFlow::confirmDomain()
{
    m_step_in_flight = true;
    QPointer<IdentityFlow> self{this};
    m_service.client().resolveName(m_record.normalized_label, [self](platform::Result<platform::DpnsName> res) {
        if (!self) return;
        self->m_service.post([self, res = std::move(res)] {
            if (!self) return;
            self->m_step_in_flight = false;
            if (self->m_record.state != State::DOMAIN_BROADCAST && self->m_record.state != State::CONTESTED_PENDING) {
                return;
            }
            if (res.ok()) {
                if (res.value->identity == self->m_record.identity_id) {
                    self->finishRegistration();
                } else if (!self->m_record.contested) {
                    self->fail(tr("Confirm username"), self->takenText(), Severity::FATAL);
                } else {
                    self->fail(tr("Check vote"), self->lostVoteText(), Severity::FATAL);
                }
                return;
            }
            if (!res.provenAbsent()) {
                self->fail(tr("Confirm username"), res.status, PlatformUi::Context::NAME_REGISTER, Severity::TRANSIENT);
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

            self->schedulePoll();
            // Not applied in the confirmation window: the domain goes out
            // again, signed anew only if its nonce was spent meanwhile.
            if (self->waitExpired()) self->setState(State::PREORDER_WAIT);
        });
    });
}

void IdentityFlow::finishRegistration()
{
    // The username is registered: what was signed for it can never apply.
    m_record.signed_identity_create.clear();
    m_record.signed_preorder = {};
    m_record.signed_domain = {};
    // A profile chosen in the wizard goes out right away.
    if (setState(State::REGISTERED)) advance();
}

void IdentityFlow::publishProfile()
{
    QString refusal;
    if (!m_service.writesAllowed(refusal)) return;
    m_step_in_flight = true;
    restartWait();
    QPointer<IdentityFlow> self{this};
    m_service.client().broadcastStateTransition(m_record.signed_profile.bytes, [self](platform::Status status) {
        if (!self) return;
        self->m_service.post([self, status = std::move(status)] {
            if (!self) return;
            self->m_step_in_flight = false;
            LogPrint(BCLog::PLATFORM, "Platform identity flow: profile broadcast: kind=%d code=%u %s\n",
                     static_cast<int>(status.kind), status.consensus_code, status.message);
            // The records were replaced (rebuilt, disabled) meanwhile.
            if (self->m_record.state != State::REGISTERED || self->m_record.signed_profile.empty()) return;
            ++self->m_profile_sends;
            if (BroadcastAccepted(status) || IsConsensus(status, CODE_INVALID_IDENTITY_NONCE)) {
                // Sent, or its nonce is taken (by this profile, sent before,
                // or by another change): the proved read tells which.
                self->m_profile_sent = true;
            } else if (status.kind == StatusKind::CONSENSUS) {
                // A profile made elsewhere: the user edits it from the
                // dashboard instead.
                self->dropSignedProfile(strprintf("refused (consensus error %u)", status.consensus_code));
            }
        });
    });
}

void IdentityFlow::confirmProfile()
{
    m_step_in_flight = true;
    QPointer<IdentityFlow> self{this};
    m_service.client().getProfile(m_record.identity_id, [self](platform::Result<platform::Profile> res) {
        if (!self) return;
        self->m_service.post([self, res = std::move(res)] {
            if (!self) return;
            self->m_step_in_flight = false;
            if (self->m_record.state != State::REGISTERED || self->m_record.signed_profile.empty()) return;
            if (res.ok() && res.value->display_name == self->m_record.profile_display_name) {
                self->dropSignedProfile({});
                return;
            }
            if (res.ok()) {
                self->dropSignedProfile("another profile was published");
                return;
            }
            self->schedulePoll();
            if (!self->waitExpired()) return;
            // Not applied in the window: sent again, a few times; after
            // that its nonce went to another change and the user adds the
            // profile from the dashboard.
            if (self->m_profile_sends >= MAX_PROFILE_SENDS) {
                self->dropSignedProfile("not applied");
            } else {
                self->m_profile_sent = false;
            }
        });
    });
}

void IdentityFlow::dropSignedProfile(const std::string& reason)
{
    m_record.signed_profile = {};
    // The name stays once it is published, and only then.
    if (!reason.empty()) {
        LogPrintf("Platform identity flow: profile not published: %s\n", reason);
        m_record.profile_display_name.clear();
    }
    m_profile_sent = false;
    m_profile_sends = 0;
    save();
    Q_EMIT stateChanged();
}
