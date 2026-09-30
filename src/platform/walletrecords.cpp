// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/walletrecords.h>

#include <clientversion.h>
#include <crypto/common.h>
#include <streams.h>

#include <algorithm>

namespace platform {

namespace {
//! Whether a persisted state byte names an enumerator. A record from another
//! build is refused rather than mapped onto a step the flow never wrote.
bool KnownState(uint8_t state)
{
    using State = IdentityRecord::State;
    switch (static_cast<State>(state)) {
    case State::NONE:
    case State::FUNDING_SENT:
    case State::FUNDING_LOCKED:
    case State::IDENTITY_BROADCAST:
    case State::IDENTITY_CONFIRMED:
    case State::PREORDER_BROADCAST:
    case State::PREORDER_WAIT:
    case State::DOMAIN_BROADCAST:
    case State::REGISTERED:
    case State::CONTESTED_PENDING:
    case State::NEEDS_UNLOCK:
    case State::FAILED:
        return true;
    }
    return false;
}

bool KnownStatusKind(uint8_t kind) { return kind <= static_cast<uint8_t>(StatusKind::INTERNAL); }

//! The fields written after started_at. A record without any of them ends
//! there, which is also how records written before they existed look.
bool HasExtension(const IdentityRecord& r)
{
    return !r.signed_identity_create.empty() || !r.signed_preorder.empty() || !r.signed_domain.empty() ||
           !r.signed_profile.empty() || !r.profile_display_name.empty() || r.last_failure.has_value();
}

bool CanonicalSigned(const IdentityRecord::SignedTransition& signed_transition)
{
    return !signed_transition.empty() || (signed_transition.nonce == 0 && signed_transition.protocol_version == 0);
}
} // namespace

std::vector<unsigned char> SerializeIdentityRecord(const IdentityRecord& r)
{
    CDataStream s(SER_DISK, CLIENT_VERSION);
    s << records::CURRENT_VERSION << static_cast<uint8_t>(r.state) << r.funding_txid << r.funding_key_index
      << r.funding_amount << r.identity_id << r.auth_key_id << r.encryption_key_id << r.decryption_key_id << r.label
      << r.normalized_label << r.preorder_salt << static_cast<uint8_t>(r.contested)
      << static_cast<uint8_t>(r.resume_state) << r.last_error << r.started_at;
    if (HasExtension(r)) {
        s << r.signed_identity_create;
        for (const auto* signed_transition : {&r.signed_preorder, &r.signed_domain, &r.signed_profile}) {
            s << signed_transition->bytes << signed_transition->nonce << signed_transition->protocol_version;
        }
        s << r.profile_display_name << static_cast<uint8_t>(r.last_failure.has_value());
        if (r.last_failure) {
            const auto& failure{*r.last_failure};
            s << failure.operation << failure.time << static_cast<uint8_t>(failure.status.has_value());
            if (failure.status) {
                s << static_cast<uint8_t>(failure.status->kind) << failure.status->consensus_code
                  << failure.status->message;
            }
        }
    }
    const auto span = MakeUCharSpan(s);
    return {span.begin(), span.end()};
}

bool DeserializeIdentityRecord(const std::vector<unsigned char>& data, IdentityRecord& r)
{
    try {
        CDataStream s(data, SER_DISK, CLIENT_VERSION);
        uint8_t version{0}, state{0}, contested{0}, resume_state{0};
        s >> version;
        if (version != records::CURRENT_VERSION) return false;
        IdentityRecord out;
        s >> state >> out.funding_txid >> out.funding_key_index >> out.funding_amount >> out.identity_id >>
            out.auth_key_id >> out.encryption_key_id >> out.decryption_key_id >> out.label >> out.normalized_label >>
            out.preorder_salt >> contested >> resume_state >> out.last_error >> out.started_at;
        if (!s.empty()) {
            uint8_t has_failure{0};
            s >> out.signed_identity_create;
            for (auto* signed_transition : {&out.signed_preorder, &out.signed_domain, &out.signed_profile}) {
                s >> signed_transition->bytes >> signed_transition->nonce >> signed_transition->protocol_version;
            }
            s >> out.profile_display_name >> has_failure;
            if (has_failure > 1) return false;
            if (has_failure == 1) {
                IdentityRecord::Failure failure;
                uint8_t has_status{0};
                s >> failure.operation >> failure.time >> has_status;
                if (has_status > 1) return false;
                if (has_status == 1) {
                    uint8_t kind{0};
                    Status status;
                    s >> kind >> status.consensus_code >> status.message;
                    if (!KnownStatusKind(kind)) return false;
                    status.kind = static_cast<StatusKind>(kind);
                    failure.status = std::move(status);
                }
                out.last_failure = std::move(failure);
            }
            if (!HasExtension(out)) return false;
        }
        // Every byte must be the canonical encoding of a value this build
        // writes, so what is accepted serializes back identically.
        if (!s.empty() || contested > 1 || !KnownState(state) || !KnownState(resume_state) ||
            !CanonicalSigned(out.signed_preorder) || !CanonicalSigned(out.signed_domain) ||
            !CanonicalSigned(out.signed_profile)) {
            return false;
        }
        out.state = static_cast<IdentityRecord::State>(state);
        out.contested = contested == 1;
        out.resume_state = static_cast<IdentityRecord::State>(resume_state);
        r = std::move(out);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::vector<unsigned char> EncodeContactOutRecord(int64_t created_at)
{
    std::vector<unsigned char> out(8);
    WriteLE64(out.data(), static_cast<uint64_t>(created_at));
    return out;
}

std::optional<int64_t> DecodeContactOutRecord(const std::vector<unsigned char>& data)
{
    if (data.size() != 8) return std::nullopt;
    return static_cast<int64_t>(ReadLE64(data.data()));
}

std::vector<unsigned char> EncodePaymentCursor(uint32_t next_index)
{
    std::vector<unsigned char> out(4);
    WriteLE32(out.data(), next_index);
    return out;
}

uint32_t DecodePaymentCursor(const std::vector<unsigned char>& data)
{
    if (data.size() != 4) return 0;
    return ReadLE32(data.data());
}

uint32_t ComputePaymentCursor(uint32_t window, const std::function<std::optional<CScript>(uint32_t)>& derive,
                              const std::set<CScript>& wallet_output_scripts)
{
    uint32_t cursor{0};
    for (uint32_t index = 0; index < window; ++index) {
        const auto script{derive(index)};
        if (!script) break;
        if (wallet_output_scripts.count(*script)) cursor = index + 1;
    }
    return cursor;
}

bool IsRecordSetCurrent(const std::vector<unsigned char>& version_record, bool have_platform_records)
{
    if (version_record.empty()) return !have_platform_records;
    return version_record == EncodeRecordVersion();
}

std::vector<unsigned char> EncodeRecordVersion() { return {records::CURRENT_VERSION}; }

} // namespace platform
