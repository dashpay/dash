// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <key.h>
#include <key_io.h>
#include <pubkey.h>
#include <script/descriptor.h>
#include <script/standard.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util/descriptor.h>

//! The converter of mocked descriptors, needs to be initialized when the target is.
MockedDescriptorConverter MOCKED_DESC_CONVERTER;

/** Test a successfully parsed descriptor. */
static void TestDescriptor(const Descriptor& desc, FlatSigningProvider& sig_provider, std::string& dummy)
{
    // Trivial helpers.
    (void)desc.IsRange();
    (void)desc.IsSolvable();
    (void)desc.IsSingleType();
    (void)desc.GetOutputType();

    // Serialization to string representation.
    (void)desc.ToString();
    (void)desc.ToPrivateString(sig_provider, dummy);
    (void)desc.ToNormalizedString(sig_provider, dummy);

    // Serialization to Script.
    DescriptorCache cache;
    std::vector<CScript> out_scripts;
    (void)desc.Expand(0, sig_provider, out_scripts, sig_provider, &cache);
    (void)desc.ExpandPrivate(0, sig_provider, sig_provider);
    (void)desc.ExpandFromCache(0, cache, out_scripts, sig_provider);

    // If we could serialize to script we must be able to infer using the same provider.
    if (!out_scripts.empty()) {
        assert(InferDescriptor(out_scripts.back(), sig_provider));
    }
}

void initialize_descriptor_parse()
{
    ECC_Start();
    SelectParams(CBaseChainParams::MAIN);
}

void initialize_mocked_descriptor_parse()
{
    initialize_descriptor_parse();
    MOCKED_DESC_CONVERTER.Init();
}

FUZZ_TARGET(mocked_descriptor_parse, .init = initialize_mocked_descriptor_parse)
{
    // Key derivation is expensive. Deriving deep derivation paths take a lot of compute and we'd
    // rather spend time elsewhere in this target, like on the actual descriptor syntax. So rule
    // out strings which could correspond to a descriptor containing a too large derivation path.
    if (HasDeepDerivPath(buffer)) return;

    const std::string mocked_descriptor{buffer.begin(), buffer.end()};
    if (const auto descriptor = MOCKED_DESC_CONVERTER.GetDescriptor(mocked_descriptor)) {
        FlatSigningProvider signing_provider;
        std::string error;
        const auto desc = Parse(*descriptor, signing_provider, error);
        if (desc) TestDescriptor(*desc, signing_provider, error);
    }
}

FUZZ_TARGET(descriptor_parse, .init = initialize_descriptor_parse)
{
    // See comment above for rationale.
    if (HasDeepDerivPath(buffer)) return;

    const std::string descriptor(buffer.begin(), buffer.end());
    FlatSigningProvider signing_provider;
    std::string error;
    for (const bool require_checksum : {true, false}) {
        const auto desc = Parse(descriptor, signing_provider, error, require_checksum);
        if (desc) TestDescriptor(*desc, signing_provider, error);
    }
}
