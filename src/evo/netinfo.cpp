// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/netinfo.h>

#include <chainparams.h>
#include <evo/providertx.h>
#include <netbase.h>
#include <span.h>
#include <util/check.h>
#include <util/system.h>

#include <univalue.h>

namespace {
static std::unique_ptr<const CChainParams> g_main_params{nullptr};
static std::once_flag g_main_params_flag;
static const CService empty_service{};

static constexpr std::string_view SAFE_CHARS_IPV4{"1234567890."};
static constexpr std::string_view SAFE_CHARS_IPV4_6{"abcdefABCDEF1234567890.:[]"};
static constexpr std::string_view SAFE_CHARS_RFC1035{"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-"};
static constexpr std::array<std::string_view, 2> TLDS_SPECIAL{".i2p", ".onion"};

bool IsNodeOnMainnet() { return Params().NetworkIDString() == CBaseChainParams::MAIN; }
const CChainParams& MainParams()
{
    // TODO: use real args here
    std::call_once(g_main_params_flag,
                   [&]() { g_main_params = CreateChainParams(ArgsManager{}, CBaseChainParams::MAIN); });
    return *Assert(g_main_params);
}

bool MatchCharsFilter(std::string_view input, std::string_view filter)
{
    return std::all_of(input.begin(), input.end(), [&filter](char c) { return filter.find(c) != std::string_view::npos; });
}

template <typename T1>
bool MatchSuffix(const std::string& str, const T1& list)
{
    if (str.empty()) return false;
    for (const auto& suffix : list) {
        if (suffix.size() > str.size()) continue;
        if (std::equal(suffix.rbegin(), suffix.rend(), str.rbegin())) return true;
    }
    return false;
}

uint16_t GetMainnetPurposePort(const uint8_t purpose)
{
    assert(IsValidPurpose(purpose));
    switch (purpose) {
    case Purpose::CORE_P2P:
        return MainParams().GetDefaultPort();
    case Purpose::PLATFORM_P2P:
        return MainParams().GetDefaultPlatformP2PPort();
    case Purpose::PLATFORM_HTTPS:
        return MainParams().GetDefaultPlatformHTTPPort();
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}
} // anonymous namespace

UniValue ArrFromService(const CService& addr)
{
    UniValue obj(UniValue::VARR);
    obj.push_back(addr.ToStringAddrPort());
    return obj;
}

bool IsServiceDeprecatedRPCEnabled()
{
    const auto args = gArgs.GetArgs("-deprecatedrpc");
    return std::find(args.begin(), args.end(), "service") != args.end();
}

bool NetInfoEntry::operator==(const NetInfoEntry& rhs) const
{
    if (m_type != rhs.m_type) return false;
    return std::visit(
        [](auto&& lhs, auto&& rhs) -> bool {
            if constexpr (std::is_same_v<decltype(lhs), decltype(rhs)>) {
                return lhs == rhs;
            }
            return false;
        },
        m_data, rhs.m_data);
}

bool NetInfoEntry::operator<(const NetInfoEntry& rhs) const
{
    if (m_type != rhs.m_type) return m_type < rhs.m_type;
    return std::visit(
        [](auto&& lhs, auto&& rhs) -> bool {
            using T1 = std::decay_t<decltype(lhs)>;
            using T2 = std::decay_t<decltype(rhs)>;
            if constexpr (std::is_same_v<T1, T2>) {
                // Both the same type, compare as usual
                return lhs < rhs;
            }
            // If lhs is monostate, it less than rhs; otherwise rhs is greater
            return std::is_same_v<T1, std::monostate>;
        },
        m_data, rhs.m_data);
}

std::optional<std::reference_wrapper<const CService>> NetInfoEntry::GetAddrPort() const
{
    if (const auto* data_ptr{std::get_if<CService>(&m_data)}; m_type == NetInfoType::Service && data_ptr) {
        ASSERT_IF_DEBUG(data_ptr->IsValid());
        return *data_ptr;
    }
    return std::nullopt;
}

uint16_t NetInfoEntry::GetPort() const
{
    return std::visit(
        [](auto&& input) -> uint16_t {
            using T1 = std::decay_t<decltype(input)>;
            if constexpr (std::is_same_v<T1, CService>) {
                return input.GetPort();
            }
            return 0;
        },
        m_data);
}

// NetInfoEntry is a dumb object that doesn't enforce validation rules, that is the responsibility of
// types that utilize NetInfoEntry (MnNetInfo and others). IsTriviallyValid() is there to check if a
// NetInfoEntry object is properly constructed.
bool NetInfoEntry::IsTriviallyValid() const
{
    if (m_type == NetInfoType::Invalid) return false;
    return std::visit(
        [this](auto&& input) -> bool {
            using T1 = std::decay_t<decltype(input)>;
            static_assert(std::is_same_v<T1, std::monostate> || std::is_same_v<T1, CService>, "Unexpected type");
            if constexpr (std::is_same_v<T1, std::monostate>) {
                // Empty underlying data isn't a valid entry
                return false;
            } else if constexpr (std::is_same_v<T1, CService>) {
                // Type code should be truthful as it decides what underlying type is used when (de)serializing
                if (m_type != NetInfoType::Service) return false;
                // Underlying data must meet surface-level validity checks for its type
                if (!input.IsValid()) return false;
            }
            return true;
        },
        m_data);
}

std::string NetInfoEntry::ToString() const
{
    return std::visit(
        [](auto&& input) -> std::string {
            using T1 = std::decay_t<decltype(input)>;
            if constexpr (std::is_same_v<T1, CService>) {
                return strprintf("CService(addr=%s, port=%u)", input.ToStringAddr(), input.GetPort());
            }
            return "[invalid entry]";
        },
        m_data);
}

std::string NetInfoEntry::ToStringAddr() const
{
    return std::visit(
        [](auto&& input) -> std::string {
            using T1 = std::decay_t<decltype(input)>;
            if constexpr (std::is_same_v<T1, CService>) {
                return input.ToStringAddr();
            }
            return "[invalid entry]";
        },
        m_data);
}

std::string NetInfoEntry::ToStringAddrPort() const
{
    return std::visit(
        [](auto&& input) -> std::string {
            using T1 = std::decay_t<decltype(input)>;
            if constexpr (std::is_same_v<T1, CService>) {
                return input.ToStringAddrPort();
            }
            return "[invalid entry]";
        },
        m_data);
}

std::shared_ptr<NetInfoInterface> NetInfoInterface::MakeNetInfo(const uint16_t nVersion)
{
    assert(nVersion > 0);
    if (nVersion >= ProTxVersion::ExtAddr) {
        return std::make_shared<ExtNetInfo>();
    }
    return std::make_shared<MnNetInfo>();
}

NetInfoStatus MnNetInfo::ValidateService(const CService& service)
{
    if (!service.IsValid()) {
        return NetInfoStatus::BadAddress;
    }
    if (!service.IsIPv4()) {
        return NetInfoStatus::BadType;
    }
    if (Params().RequireRoutableExternalIP() && !service.IsRoutable()) {
        return NetInfoStatus::NotRoutable;
    }

    if (IsNodeOnMainnet() != (service.GetPort() == GetMainnetPurposePort(Purpose::CORE_P2P))) {
        // Must use mainnet port on mainnet and any other port for other networks
        return NetInfoStatus::BadPort;
    }

    return NetInfoStatus::Success;
}

NetInfoStatus MnNetInfo::AddEntry(const uint8_t purpose, const std::string& input)
{
    if (purpose != Purpose::CORE_P2P || !IsEmpty()) {
        return NetInfoStatus::MaxLimit;
    }

    std::string addr;
    uint16_t port{Params().GetDefaultPort()};
    SplitHostPort(input, port, addr);
    // Contains invalid characters, unlikely to pass Lookup(), fast-fail
    if (!MatchCharsFilter(addr, SAFE_CHARS_IPV4)) {
        return NetInfoStatus::BadInput;
    }

    if (auto service_opt{Lookup(addr, /*portDefault=*/port, /*fAllowLookup=*/false)}) {
        const auto ret{ValidateService(*service_opt)};
        if (ret == NetInfoStatus::Success) {
            const NetInfoEntry candidate{*service_opt};
            if (m_addr == candidate) {
                // Not possible since we allow only one value at most
                return NetInfoStatus::Duplicate;
            }
            m_addr = candidate;
            ASSERT_IF_DEBUG(m_addr.GetAddrPort().has_value());
        }
        return ret;
    }
    return NetInfoStatus::BadInput;
}

NetInfoList MnNetInfo::GetEntries() const
{
    if (!IsEmpty()) {
        ASSERT_IF_DEBUG(m_addr.GetAddrPort().has_value());
        return {m_addr};
    }
    // If MnNetInfo is empty, we probably don't expect any entries to show up, so
    // we return a blank set instead.
    return {};
}

const CService& MnNetInfo::GetPrimary() const
{
    if (const auto& service_opt{m_addr.GetAddrPort()}) {
        return *service_opt;
    }
    return empty_service;
}

NetInfoStatus MnNetInfo::Validate() const
{
    if (!m_addr.IsTriviallyValid()) {
        return NetInfoStatus::Malformed;
    }
    return ValidateService(GetPrimary());
}

UniValue MnNetInfo::ToJson() const
{
    UniValue ret(UniValue::VOBJ);
    ret.pushKV(PurposeToString(Purpose::CORE_P2P, /*lower=*/true).data(), ArrFromService(GetPrimary()));
    return ret;
}

std::string MnNetInfo::ToString() const
{
    // Extra padding to account for padding done by the calling function.
    return strprintf("MnNetInfo()\n"
                     "    NetInfo(purpose=%s)\n"
                     "      %s\n",
                     PurposeToString(Purpose::CORE_P2P), m_addr.ToString());
}

bool ExtNetInfo::HasDuplicates(const std::vector<NetInfoEntry>* entries) const
{
    if (!entries) {
        // Entries list not provided, searching through all entries for exact match
        const auto& all_entries{GetEntries()};
        if (all_entries.empty()) return false;
        std::set<NetInfoEntry> known{};
        for (const NetInfoEntry& entry : all_entries) {
            if (auto [_, inserted] = known.insert(entry); !inserted) {
                return true;
            }
        }
        ASSERT_IF_DEBUG(known.size() == all_entries.size());
        return false;
    }
    // Entries list provided, searching through list for partial match
    if (Assert(entries)->empty()) return false;
    std::unordered_set<std::string> known{};
    for (const NetInfoEntry& entry : *entries) {
        if (auto [_, inserted] = known.insert(entry.ToStringAddr()); !inserted) {
            return true;
        }
    }
    ASSERT_IF_DEBUG(known.size() == entries->size());
    return false;
}

bool ExtNetInfo::IsDuplicateCandidate(const NetInfoEntry& candidate, const std::vector<NetInfoEntry>* entries) const
{
    if (!entries) {
        // Entries list not provided, searching through all entries for exact match
        const auto& all_entries{GetEntries()};
        if (all_entries.empty()) return false;
        return std::any_of(all_entries.begin(), all_entries.end(),
                           [&candidate](const auto& entry) { return candidate == entry; });
    }
    // Entries list provided, searching through list for partial match
    if (Assert(entries)->empty()) return false;
    const std::string& candidate_str{candidate.ToStringAddr()};
    return std::any_of(entries->begin(), entries->end(),
                       [&candidate_str](const auto& entry) { return candidate_str == entry.ToStringAddr(); });
}

NetInfoStatus ExtNetInfo::ProcessCandidate(const uint8_t purpose, const NetInfoEntry& candidate)
{
    assert(candidate.IsTriviallyValid());

    if (IsDuplicateCandidate(candidate, /*entries=*/nullptr)) {
        // Exact duplicates are prohibited *across* lists
        return NetInfoStatus::Duplicate;
    }
    if (auto it{m_data.find(purpose)}; it != m_data.end()) {
        // Existing entries list found, check limit
        auto& [_, entries] = *it;
        if (entries.size() >= NETINFO_EXTENDED_LIMIT) {
            return NetInfoStatus::MaxLimit;
        }
        if (IsDuplicateCandidate(candidate, &entries)) {
            // Partial duplicates are prohibited *within* a list
            return NetInfoStatus::Duplicate;
        }
        entries.push_back(candidate);
        return NetInfoStatus::Success;
    } else {
        // First entry for purpose code, create new entries list
        auto [_, status] = m_data.try_emplace(purpose, std::vector<NetInfoEntry>({candidate}));
        assert(status); // We did just check to see if our value already existed, try_emplace shouldn't fail
        return NetInfoStatus::Success;
    }
}

NetInfoStatus ExtNetInfo::ValidateService(const CService& service, const uint8_t purpose, bool is_primary)
{
    if (!service.IsValid()) {
        return NetInfoStatus::BadAddress;
    }
    if (!service.IsCJDNS() && !service.IsI2P() && !service.IsIPv4() && !service.IsIPv6() && !service.IsTor()) {
        return NetInfoStatus::BadType;
    }
    if (Params().RequireRoutableExternalIP() && !service.IsRoutable()) {
        return NetInfoStatus::NotRoutable;
    }

    const uint16_t service_port{service.GetPort()};
    if (service.IsI2P() && service_port != I2P_SAM31_PORT) {
        // I2P SAM 3.1 and earlier don't support arbitrary ports
        return NetInfoStatus::BadPort;
    } else if (!service.IsI2P() &&
               ((IsBadPort(service_port) && service_port != GetMainnetPurposePort(purpose)) || service_port == 0)) {
        return NetInfoStatus::BadPort;
    }

    if (is_primary) {
        if (!service.IsIPv4()) {
            return NetInfoStatus::BadType;
        }
        if (IsNodeOnMainnet() && service_port != GetMainnetPurposePort(purpose)) {
            // On mainnet, the primary address must use the fixed port assigned to the purpose
            return NetInfoStatus::BadPort;
        } else if (!IsNodeOnMainnet() && service_port == GetMainnetPurposePort(Purpose::CORE_P2P)) {
            // The mainnet CORE_P2P port may not be used for any purpose outside of mainnet
            return NetInfoStatus::BadPort;
        }
    }

    return NetInfoStatus::Success;
}

NetInfoStatus ExtNetInfo::AddEntry(const uint8_t purpose, const std::string& input)
{
    if (!IsValidPurpose(purpose)) {
        return NetInfoStatus::MaxLimit;
    }

    // We don't allow assuming ports, so we set the default value to 0 so that if no port is specified
    // it uses a fallback value of 0, which will return a NetInfoStatus::BadPort
    std::string addr;
    uint16_t port{0};
    SplitHostPort(input, port, addr);

    // Primary addresses are subject to stricter validation rules
    const bool is_primary{m_data.find(purpose) == m_data.end()};

    if (!MatchCharsFilter(addr, SAFE_CHARS_IPV4_6)) {
        if (!MatchCharsFilter(addr, SAFE_CHARS_RFC1035)) {
            // Neither IP:port safe nor domain-safe, we can safely assume it's bad input
            return NetInfoStatus::BadInput;
        }

        // Not IP:port safe but domain safe
        if (is_primary) {
            // Domains are not allowed as primary addresses
            return NetInfoStatus::BadType;
        } else if (MatchSuffix(addr, TLDS_SPECIAL)) {
            // Special domain, try storing it as CService
            CNetAddr netaddr;
            if (netaddr.SetSpecial(addr)) {
                const CService service{netaddr, port};
                const auto ret{ValidateService(service, purpose, /*is_primary=*/false)};
                if (ret == NetInfoStatus::Success) {
                    return ProcessCandidate(purpose, NetInfoEntry{service});
                }
                return ret; /* ValidateService() failed */
            }
        }
        return NetInfoStatus::BadInput; /* CService::SetSpecial() failed */
    }

    // IP:port safe, try to parse it as IP:port
    if (auto service_opt{Lookup(addr, /*portDefault=*/port, /*fAllowLookup=*/false)}) {
        const auto service{MaybeFlipIPv6toCJDNS(*service_opt)};
        const auto ret{ValidateService(service, purpose, is_primary)};
        if (ret == NetInfoStatus::Success) {
            return ProcessCandidate(purpose, NetInfoEntry{service});
        }
        return ret; /* ValidateService() failed */
    }
    return NetInfoStatus::BadInput; /* Lookup() failed */
}

NetInfoList ExtNetInfo::GetEntries() const
{
    NetInfoList ret;
    for (const auto& [_, entries] : m_data) {
        ret.insert(ret.end(), entries.begin(), entries.end());
    }
    return ret;
}

const CService& ExtNetInfo::GetPrimary() const
{
    if (const auto& it{m_data.find(Purpose::CORE_P2P)}; it != m_data.end()) {
        const auto& [_, entries] = *it;
        ASSERT_IF_DEBUG(!entries.empty());
        if (!entries.empty()) {
            if (const auto& service_opt{entries.begin()->GetAddrPort()}) {
                return *service_opt;
            }
        }
    }
    return empty_service;
}

bool ExtNetInfo::HasEntries(uint8_t purpose) const
{
    if (!IsValidPurpose(purpose)) return false;
    const auto& it{m_data.find(purpose)};
    return it != m_data.end() && !it->second.empty();
}

NetInfoStatus ExtNetInfo::Validate() const
{
    if (m_version == 0 || m_version > CURRENT_VERSION || m_data.empty()) {
        return NetInfoStatus::Malformed;
    }
    if (HasDuplicates(/*entries=*/nullptr)) {
        // Exact duplicates are prohibited *across* lists
        return NetInfoStatus::Duplicate;
    }
    for (const auto& [purpose, entries] : m_data) {
        if (!IsValidPurpose(purpose)) {
            return NetInfoStatus::Malformed;
        }
        if (entries.empty()) {
            // Purpose if present in map must have at least one entry
            return NetInfoStatus::Malformed;
        }
        if (HasDuplicates(&entries)) {
            // Partial duplicates are prohibited *within* a list
            return NetInfoStatus::Duplicate;
        }
        for (const auto& entry : entries) {
            if (!entry.IsTriviallyValid()) {
                // Trivially invalid NetInfoEntry, no point checking against consensus rules
                return NetInfoStatus::Malformed;
            }
            if (const auto& service_opt{entry.GetAddrPort()}) {
                if (auto ret{ValidateService(*service_opt, purpose, /*is_primary=*/entry == *entries.begin())};
                    ret != NetInfoStatus::Success) {
                    // Stores CService underneath but doesn't pass validation rules
                    return ret;
                }
            } else {
                // Doesn't store valid type underneath
                return NetInfoStatus::Malformed;
            }
        }
    }
    return NetInfoStatus::Success;
}

UniValue ExtNetInfo::ToJson() const
{
    UniValue ret(UniValue::VOBJ);
    for (const auto& [purpose, p_entries] : m_data) {
        UniValue arr(UniValue::VARR);
        for (const auto& entry : p_entries) {
            arr.push_back(entry.ToStringAddrPort());
        }
        ret.pushKV(PurposeToString(purpose, /*lower=*/true).data(), arr);
    }
    return ret;
}

std::string ExtNetInfo::ToString() const
{
    std::string ret{"ExtNetInfo()\n"};
    for (const auto& [purpose, entries] : m_data) {
        ret += strprintf("    NetInfo(purpose=%s)\n", PurposeToString(purpose));
        if (entries.empty()) {
            ret += "      [invalid list]\n";
        } else {
            for (const auto& entry : entries) {
                ret += strprintf("      %s\n", entry.ToString());
            }
        }
    }
    return ret;
}
