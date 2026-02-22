// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <governance/common.h>
#include <governance/validators.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <util/underlying.h>

#include <array>
#include <cinttypes>
#include <cstdint>
#include <string>

namespace {
std::string HexEncodeString(const std::string& input)
{
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    std::string out;
    out.reserve(input.size() * 2);
    for (const unsigned char ch : input) {
        out.push_back(HEX_DIGITS[ch >> 4]);
        out.push_back(HEX_DIGITS[ch & 0x0f]);
    }
    return out;
}

std::string SanitizeJsonString(std::string input)
{
    for (char& ch : input) {
        if (ch == '"' || ch == '\\' || static_cast<unsigned char>(ch) < 0x20) {
            ch = 'x';
        }
    }
    return input;
}

std::string MakeProposalJson(
    int64_t type,
    const std::string& name,
    int64_t start_epoch,
    int64_t end_epoch,
    double payment_amount,
    const std::string& payment_address,
    const std::string& url)
{
    return strprintf(
        "{\"type\":%" PRId64 ",\"name\":\"%s\",\"start_epoch\":%" PRId64 ",\"end_epoch\":%" PRId64 ",\"payment_amount\":%.17g,\"payment_address\":\"%s\",\"url\":\"%s\"}",
        type,
        SanitizeJsonString(name),
        start_epoch,
        end_epoch,
        payment_amount,
        SanitizeJsonString(payment_address),
        SanitizeJsonString(url));
}

void RunValidatorCase(const std::string& hex_data, bool allow_script, bool check_expiration)
{
    try {
        CProposalValidator validator(hex_data, allow_script);
        (void)validator.Validate(check_expiration);
        (void)validator.GetErrorMessages();
    } catch (const std::exception&) {
    } catch (...) {
    }
}
} // namespace

void initialize_governance_proposal_validator()
{
    SelectParams(CBaseChainParams::MAIN);
}

FUZZ_TARGET(governance_proposal_validator, .init = initialize_governance_proposal_validator)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());

    constexpr std::array<const char*, 2> kPaymentAddresses{
        "Xs7iEDx8nMwJHdiQnwvCnLBTP2sjmDGTJA", // P2PKH (mainnet)
        "7XuP9xVGyvkCAfW84QJkGfbiR7dX9TYaPH", // P2SH (mainnet)
    };

    const int64_t type = fuzzed_data_provider.ConsumeBool()
        ? ToUnderlying(GovernanceObject::PROPOSAL)
        : fuzzed_data_provider.ConsumeIntegral<int64_t>();

    const int64_t start_epoch = fuzzed_data_provider.ConsumeIntegral<int64_t>();
    const int64_t end_epoch = start_epoch + fuzzed_data_provider.ConsumeIntegralInRange<int64_t>(-4, 1024);

    double payment_amount = fuzzed_data_provider.ConsumeFloatingPointInRange<double>(-1000.0, 1000.0);
    if (fuzzed_data_provider.ConsumeBool()) {
        payment_amount = 1.0;
    }

    std::string random_name = fuzzed_data_provider.ConsumeRandomLengthString(96);
    std::string random_url = fuzzed_data_provider.ConsumeRandomLengthString(256);

    if (fuzzed_data_provider.ConsumeBool()) {
        random_name = "dash-proposal-" + random_name;
    }

    constexpr std::array<const char*, 4> kUrls{
        "https://dash.org/proposals/1",
        "http://[::1]/path",
        "http://[broken/path",
        "http://broken]/path",
    };

    const std::string payment_address = fuzzed_data_provider.ConsumeBool()
        ? std::string(kPaymentAddresses[fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, kPaymentAddresses.size() - 1)])
        : fuzzed_data_provider.ConsumeRandomLengthString(96);
    const std::string url = fuzzed_data_provider.ConsumeBool()
        ? std::string(kUrls[fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, kUrls.size() - 1)])
        : random_url;

    const std::string json_hex = HexEncodeString(MakeProposalJson(
        type,
        random_name,
        start_epoch,
        end_epoch,
        payment_amount,
        payment_address,
        url));

    const std::string malformed_json_hex = HexEncodeString("{" + fuzzed_data_provider.ConsumeRandomLengthString(128));
    const size_t oversized_payload_size = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(513, 2048);
    const std::string oversized_hex(oversized_payload_size * 2, 'a');
    const std::string random_hex = fuzzed_data_provider.ConsumeRandomLengthString(2048);

    for (const bool allow_script : {false, true}) {
        for (const bool check_expiration : {false, true}) {
            RunValidatorCase(json_hex, allow_script, check_expiration);
            RunValidatorCase(malformed_json_hex, allow_script, check_expiration);
            RunValidatorCase(oversized_hex, allow_script, check_expiration);
            RunValidatorCase(random_hex, allow_script, check_expiration);
        }
    }
}
