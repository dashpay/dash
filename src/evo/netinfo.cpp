// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/netinfo.h>

#include <chainparams.h>
#include <netbase.h>
#include <span.h>
#include <util/check.h>
#include <util/system.h>

namespace {
static std::unique_ptr<const CChainParams> g_main_params{nullptr};
static std::once_flag g_main_params_flag;
static const CService empty_service{CService()};

static constexpr std::string_view SAFE_CHARS_IPV4{"1234567890."};
static constexpr std::string_view SAFE_CHARS_IPV4_6{"abcdefABCDEF1234567890.:[]"};

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
} // anonymous namespace

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
            } else if constexpr (std::is_same_v<T1, std::monostate> && !std::is_same_v<T2, std::monostate>) {
                // lhs is monostate and rhs is not, rhs is greater
                return true;
            } else if constexpr (!std::is_same_v<T1, std::monostate> && std::is_same_v<T2, std::monostate>) {
                // rhs is monostate but lhs is not, lhs is greater
                return false;
            }
            return false;
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
            } else {
                return 0;
            }
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
            if constexpr (std::is_same_v<T1, std::monostate>) {
                // Empty underlying data isn't a valid entry
                return false;
            } else if constexpr (std::is_same_v<T1, CService>) {
                // Type code should be truthful as it decides what underlying type is used when (de)serializing
                if (m_type != NetInfoType::Service) return false;
                // Underlying data must meet surface-level validity checks for its type
                if (!input.IsValid()) return false;
            } else {
                return false;
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
            } else {
                return "[invalid entry]";
            }
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
            } else {
                return "[invalid entry]";
            }
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
            } else {
                return "[invalid entry]";
            }
        },
        m_data);
}

bool NetInfoInterface::IsEqual(const std::shared_ptr<NetInfoInterface>& lhs, const std::shared_ptr<NetInfoInterface>& rhs)
{
    if (lhs == rhs) {
        // Points to the same object or both blank
        return true;
    }

    if (!lhs || !rhs) {
        // Unequal initialization status
        return false;
    }

    if (const auto lhs_ptr{std::dynamic_pointer_cast<MnNetInfo>(lhs)}) {
        if (const auto rhs_ptr{std::dynamic_pointer_cast<MnNetInfo>(rhs)}) {
            // Successful downcasting of both lhs and rhs, can now deep compare
            return *lhs_ptr == *rhs_ptr;
        }
    }

    if (const auto lhs_ptr{std::dynamic_pointer_cast<ExtNetInfo>(lhs)}) {
        if (const auto rhs_ptr{std::dynamic_pointer_cast<ExtNetInfo>(rhs)}) {
            // Successful downcasting of both lhs and rhs, can now deep compare
            return *lhs_ptr == *rhs_ptr;
        }
    }

    // Downcasting failed, lhs and rhs are differing types
    return false;
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

    if (IsNodeOnMainnet() != (service.GetPort() == MainParams().GetDefaultPort())) {
        // Must use mainnet port on mainnet.
        // Must NOT use mainnet port on other networks.
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
        // Absolute duplicates are prohibited *across* lists
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

NetInfoStatus ExtNetInfo::ValidateService(const CService& service, bool is_primary)
{
    if (!service.IsValid()) {
        return NetInfoStatus::BadAddress;
    }
    if (!service.IsIPv4() && !service.IsIPv6()) {
        return NetInfoStatus::BadType;
    }
    if (Params().RequireRoutableExternalIP() && !service.IsRoutable()) {
        return NetInfoStatus::NotRoutable;
    }
    if (IsBadPort(service.GetPort()) || service.GetPort() == 0) {
        return NetInfoStatus::BadPort;
    }
    if (is_primary) {
        if (!service.IsIPv4()) {
            return NetInfoStatus::BadType;
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
    // Contains invalid characters, unlikely to pass Lookup(), fast-fail
    if (!MatchCharsFilter(addr, SAFE_CHARS_IPV4_6)) {
        return NetInfoStatus::BadInput;
    }

    if (auto service_opt{Lookup(addr, /*portDefault=*/port, /*fAllowLookup=*/false)}) {
        const auto ret{ValidateService(*service_opt, /*is_primary=*/m_data.find(purpose) == m_data.end())};
        if (ret == NetInfoStatus::Success) {
            return ProcessCandidate(purpose, NetInfoEntry{*service_opt});
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
        // Absolute duplicates are prohibited *across* lists
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
                if (auto ret{ValidateService(*service_opt, /*is_primary=*/entry == *entries.begin())};
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
