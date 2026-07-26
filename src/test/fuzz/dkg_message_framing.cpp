// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls/bls.h>
#include <llmq/dkgmessages.h>
#include <llmq/net_dkg.h>
#include <llmq/params.h>
#include <protocol.h>
#include <serialize.h>
#include <streams.h>
#include <version.h>

#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>

#include <array>
#include <cstdint>
#include <string_view>

namespace {
const std::array<std::string_view, 4> DKG_MSG_TYPES = {
    NetMsgType::QCONTRIB,
    NetMsgType::QCOMPLAINT,
    NetMsgType::QJUSTIFICATION,
    NetMsgType::QPCOMMITMENT,
};

//! Typed deserialization exactly as the DKG worker performs it: one pass, no
//! trailing bytes tolerated, followed by the param-bound structural check.
template <typename Message>
bool TypedDeserializeSucceeds(Span<const uint8_t> payload, const Consensus::LLMQParams& params)
{
    CDataStream ds{payload, SER_NETWORK, PROTOCOL_VERSION};
    Message msg;
    try {
        ds >> msg;
    } catch (const std::exception&) {
        return false;
    }
    return ds.empty() && llmq::CheckDKGMessageStructure(msg, params);
}

bool TypedDeserializeSucceeds(std::string_view msg_type, Span<const uint8_t> payload,
                              const Consensus::LLMQParams& params)
{
    if (msg_type == NetMsgType::QCONTRIB) {
        return TypedDeserializeSucceeds<llmq::CDKGContribution>(payload, params);
    } else if (msg_type == NetMsgType::QCOMPLAINT) {
        return TypedDeserializeSucceeds<llmq::CDKGComplaint>(payload, params);
    } else if (msg_type == NetMsgType::QJUSTIFICATION) {
        return TypedDeserializeSucceeds<llmq::CDKGJustification>(payload, params);
    } else if (msg_type == NetMsgType::QPCOMMITMENT) {
        return TypedDeserializeSucceeds<llmq::CDKGPrematureCommitment>(payload, params);
    }
    return false;
}

//! Build a message that is well formed for @p params, serialize it, and require
//! the framing walk to accept the bytes our own serializer just produced. This
//! covers the equivalence from the other end: it does not depend on the fuzzer
//! synthesizing a valid BLS encoding by chance, so a framing/serializer drift is
//! caught even from an empty corpus.
template <typename Message>
void CheckSerializedMessageIsAccepted(std::string_view msg_type, const Message& msg,
                                      const Consensus::LLMQParams& params)
{
    CDataStream ds{SER_NETWORK, PROTOCOL_VERSION};
    ds << msg;
    assert(llmq::CheckDKGMessageStructure(msg, params));
    assert(llmq::CheckDKGMessageWireStructure(msg_type, ds, params));
}

void CheckWellFormedMessageIsAccepted(std::string_view msg_type, const Consensus::LLMQParams& params,
                                      FuzzedDataProvider& provider)
{
    const size_t size = params.size > 0 ? static_cast<size_t>(params.size) : 0;
    const size_t min_size = params.minSize > 0 ? static_cast<size_t>(params.minSize) : 0;
    const size_t threshold = params.threshold > 0 ? static_cast<size_t>(params.threshold) : 0;
    if (size == 0 || min_size > size) {
        return;
    }

    if (msg_type == NetMsgType::QCONTRIB) {
        llmq::CDKGContribution qc;
        qc.vvec = std::make_shared<std::vector<CBLSPublicKey>>(threshold);
        qc.contributions = std::make_shared<CBLSIESMultiRecipientObjects<CBLSSecretKey>>();
        const size_t blobs = provider.ConsumeIntegralInRange<size_t>(min_size, size);
        qc.contributions->blobs.resize(blobs);
        for (auto& blob : qc.contributions->blobs) {
            blob.resize(provider.ConsumeIntegralInRange<size_t>(0, 64));
        }
        CheckSerializedMessageIsAccepted(msg_type, qc, params);
    } else if (msg_type == NetMsgType::QCOMPLAINT) {
        llmq::CDKGComplaint qc;
        const size_t members = provider.ConsumeIntegralInRange<size_t>(0, size);
        qc.badMembers.assign(members, false);
        qc.complainForMembers.assign(members, false);
        for (size_t i = 0; i < members; ++i) {
            qc.badMembers[i] = provider.ConsumeBool();
            qc.complainForMembers[i] = provider.ConsumeBool();
        }
        CheckSerializedMessageIsAccepted(msg_type, qc, params);
    } else if (msg_type == NetMsgType::QJUSTIFICATION) {
        llmq::CDKGJustification qj;
        qj.contributions.resize(provider.ConsumeIntegralInRange<size_t>(0, size));
        CheckSerializedMessageIsAccepted(msg_type, qj, params);
    } else if (msg_type == NetMsgType::QPCOMMITMENT) {
        llmq::CDKGPrematureCommitment qc;
        const size_t members = provider.ConsumeIntegralInRange<size_t>(0, size);
        qc.validMembers.assign(members, false);
        for (size_t i = 0; i < members; ++i) {
            qc.validMembers[i] = provider.ConsumeBool();
        }
        CheckSerializedMessageIsAccepted(msg_type, qc, params);
    }
}

void initialize_dkg_message_framing()
{
    BLSInit();
}
} // namespace

/**
 * DKG network intake validates framing without materializing BLS objects, then
 * the DKG worker deserializes the retained bytes exactly once. Those are two
 * hand-maintained parsers over one wire format, so they can drift apart.
 *
 * The safety-critical direction is that the framing walk must never reject a
 * payload the worker would have accepted -- otherwise honest DKG messages are
 * silently dropped at intake and quorum formation degrades. The converse is not
 * asserted: framing deliberately accepts payloads whose BLS points fail to
 * decode, so that the worker can score the sender.
 */
FUZZ_TARGET(dkg_message_framing, .init = initialize_dkg_message_framing)
{
    FuzzedDataProvider fuzzed_data_provider{buffer.data(), buffer.size()};

    const std::string_view msg_type = DKG_MSG_TYPES.at(
        fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, DKG_MSG_TYPES.size() - 1));
    const Consensus::LLMQParams& params = Consensus::available_llmqs.at(
        fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, Consensus::available_llmqs.size() - 1));
    // Framing is scheme-independent (BLS wire sizes are fixed), but typed
    // deserialization is not; cover both so the equivalence is checked on each.
    bls::bls_legacy_scheme.store(fuzzed_data_provider.ConsumeBool());

    CheckWellFormedMessageIsAccepted(msg_type, params, fuzzed_data_provider);

    const std::vector<uint8_t> payload = fuzzed_data_provider.ConsumeRemainingBytes<uint8_t>();

    const CDataStream payload_stream{payload, SER_NETWORK, PROTOCOL_VERSION};
    const bool framing_ok = llmq::CheckDKGMessageWireStructure(msg_type, payload_stream, params);
    const bool typed_ok = TypedDeserializeSucceeds(msg_type, payload, params);

    assert(!typed_ok || framing_ok);
}
