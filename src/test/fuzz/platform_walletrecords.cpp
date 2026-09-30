// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/walletrecords.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>

#include <cstdint>
#include <vector>

//! The wallet platform-data records are decoded from the wallet database,
//! which a backup can carry from another build; decoding must refuse rather
//! than misread them, and what this build wrote must read back identically.
FUZZ_TARGET(platform_walletrecords)
{
    FuzzedDataProvider provider{buffer.data(), buffer.size()};
    const std::vector<unsigned char> raw{provider.ConsumeBytes<unsigned char>(provider.ConsumeIntegralInRange(0, 512))};

    platform::IdentityRecord record;
    if (platform::DeserializeIdentityRecord(raw, record)) {
        // A record this build accepts must serialize back to the same bytes.
        assert(platform::SerializeIdentityRecord(record) == raw);
    }
    (void)platform::DecodePaymentCursor(raw);
    (void)platform::DecodeContactOutRecord(raw);
    (void)platform::IsRecordSetCurrent(raw, provider.ConsumeBool());
}
