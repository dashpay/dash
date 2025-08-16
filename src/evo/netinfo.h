// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_NETINFO_H
#define BITCOIN_EVO_NETINFO_H

#include <netaddress.h>
#include <serialize.h>
#include <streams.h>

#include <variant>

class CDeterministicMNStateDiff;
class CService;

class UniValue;

//! Maximum entries that can be stored in an ExtNetInfo per purpose code
static constexpr uint8_t MAX_ENTRIES_EXTNETINFO{4};

enum class NetInfoStatus : uint8_t {
    // Managing entries
    BadInput,
    Duplicate,
    MaxLimit,

    // Validation
    BadAddress,
    BadPort,
    BadType,
    NotRoutable,
    Malformed,

    Success
};

constexpr std::string_view NISToString(const NetInfoStatus code)
{
    switch (code) {
    case NetInfoStatus::BadAddress:
        return "invalid address";
    case NetInfoStatus::BadInput:
        return "invalid input";
    case NetInfoStatus::BadPort:
        return "invalid port";
    case NetInfoStatus::BadType:
        return "invalid address type";
    case NetInfoStatus::Duplicate:
        return "duplicate";
    case NetInfoStatus::NotRoutable:
        return "unroutable address";
    case NetInfoStatus::Malformed:
        return "malformed";
    case NetInfoStatus::MaxLimit:
        return "too many entries";
    case NetInfoStatus::Success:
        return "success";
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

// Purpose corresponds to the index position in the ExtNetInfo map (which is ordered),
// entries must be contiguous and cannot be changed once set without a format version
// update, which will necessitate a hard-fork.
namespace Purpose {
enum : uint8_t {
    // Mandatory for masternodes
    CORE_P2P = 0,
    // Mandatory for EvoNodes
    PLATFORM_P2P = 1,
    PLATFORM_HTTPS = 2,
};
} // namespace Purpose

constexpr bool IsValidPurpose(const uint8_t purpose)
{
    switch (purpose) {
    case Purpose::CORE_P2P:
    case Purpose::PLATFORM_P2P:
    case Purpose::PLATFORM_HTTPS:
        return true;
    } // no default case, so the compiler can warn about missing cases
    return false;
}

// Warning: Used in RPC code, altering existing values is a breaking change
constexpr std::string_view PurposeToString(const uint8_t purpose, const bool lower = false)
{
    switch (purpose) {
    case Purpose::CORE_P2P:
        return lower ? "core_p2p" : "CORE_P2P";
    case Purpose::PLATFORM_P2P:
        return lower ? "platform_p2p" : "PLATFORM_P2P";
    case Purpose::PLATFORM_HTTPS:
        return lower ? "platform_https" : "PLATFORM_HTTPS";
    } // no default case, so the compiler can warn about missing cases
    return "";
}

/* Creates a one-element array using CService::ToStringPortAddr() output. */
UniValue ArrFromService(const CService& addr);

/* Identical to IsDeprecatedRPCEnabled("service"). For use outside of RPC code. */
bool IsServiceDeprecatedRPCEnabled();

class NetInfoEntry
{
public:
    enum NetInfoType : uint8_t {
        Service = 0x01,
        Invalid = 0xff
    };

private:
    uint8_t m_type{NetInfoType::Invalid};
    std::variant<std::monostate, CService> m_data{std::monostate{}};

public:
    NetInfoEntry() = default;
    NetInfoEntry(const CService& service)
    {
        if (!service.IsValid()) return;
        m_type = NetInfoType::Service;
        m_data = service;
    }
    template <typename Stream>
    NetInfoEntry(deserialize_type, Stream& s) { s >> *this; }

    ~NetInfoEntry() = default;

    bool operator<(const NetInfoEntry& rhs) const;
    bool operator==(const NetInfoEntry& rhs) const;
    bool operator!=(const NetInfoEntry& rhs) const { return !(*this == rhs); }

    template <typename Stream>
    void Serialize(Stream& s_) const
    {
        OverrideStream<Stream> s(&s_, /*nType=*/0, s_.GetVersion() | ADDRV2_FORMAT);
        if (const auto* data_ptr{std::get_if<CService>(&m_data)};
            m_type == NetInfoType::Service && data_ptr && data_ptr->IsValid()) {
            s << m_type << *data_ptr;
        } else {
            s << NetInfoType::Invalid;
        }
    }

    template <typename Stream>
    void Unserialize(Stream& s_)
    {
        OverrideStream<Stream> s(&s_, /*nType=*/0, s_.GetVersion() | ADDRV2_FORMAT);
        s >> m_type;
        if (m_type == NetInfoType::Service) {
            m_data = CService{};
            try {
                CService& service{std::get<CService>(m_data)};
                s >> service;
                if (!service.IsValid()) { Clear(); } // Invalid CService, mark as invalid
            } catch (const std::ios_base::failure&) { Clear(); } // Deser failed, mark as invalid
        } else { Clear(); } // Invalid type code, mark as invalid
    }

    void Clear()
    {
        m_type = NetInfoType::Invalid;
        m_data = std::monostate{};
    }

    std::optional<CService> GetAddrPort() const;
    uint16_t GetPort() const;
    bool IsEmpty() const { return *this == NetInfoEntry{}; }
    bool IsTriviallyValid() const;
    std::string ToString() const;
    std::string ToStringAddr() const;
    std::string ToStringAddrPort() const;
};

template<> struct is_serializable_enum<NetInfoEntry::NetInfoType> : std::true_type {};

using NetInfoList = std::vector<NetInfoEntry>;

class NetInfoInterface
{
public:
    static std::shared_ptr<NetInfoInterface> MakeNetInfo(const uint16_t nVersion);

public:
    virtual ~NetInfoInterface() = default;

    virtual NetInfoStatus AddEntry(const uint8_t purpose, const std::string& service) = 0;
    virtual NetInfoList GetEntries(std::optional<uint8_t> purpose_opt = std::nullopt) const = 0;

    virtual CService GetPrimary() const = 0;
    virtual bool CanStorePlatform() const = 0;
    virtual bool HasEntries(uint8_t purpose) const = 0;
    virtual bool IsEmpty() const = 0;
    virtual NetInfoStatus Validate() const = 0;
    virtual UniValue ToJson() const = 0;
    virtual std::string ToString() const = 0;

    virtual void Clear() = 0;

    bool operator==(const NetInfoInterface& rhs) const { return typeid(*this) == typeid(rhs) && this->IsEqual(rhs); }
    bool operator!=(const NetInfoInterface& rhs) const { return !(*this == rhs); }

private:
    virtual bool IsEqual(const NetInfoInterface& rhs) const = 0;
};

class MnNetInfo final : public NetInfoInterface
{
private:
    NetInfoEntry m_addr{};

private:
    static NetInfoStatus ValidateService(const CService& service);

public:
    MnNetInfo() = default;
    template <typename Stream>
    MnNetInfo(deserialize_type, Stream& s) { s >> *this; }

    ~MnNetInfo() = default;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        if (const auto service_opt{m_addr.GetAddrPort()}) {
            s << *service_opt;
        } else {
            s << CService{};
        }
    }

    void Serialize(CSizeComputer& s) const
    {
        s.seek(::GetSerializeSize(CService{}, s.GetVersion()));
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        CService service;
        s >> service;
        m_addr = NetInfoEntry{service};
    }

    NetInfoStatus AddEntry(const uint8_t purpose, const std::string& service) override;
    NetInfoList GetEntries(std::optional<uint8_t> purpose_opt = std::nullopt) const override;

    CService GetPrimary() const override;
    bool HasEntries(uint8_t purpose) const override { return purpose == Purpose::CORE_P2P && !IsEmpty(); }
    bool IsEmpty() const override { return m_addr.IsEmpty(); }
    bool CanStorePlatform() const override { return false; }
    NetInfoStatus Validate() const override;
    UniValue ToJson() const override;
    std::string ToString() const override;

    void Clear() override { m_addr.Clear(); }

private:
    // operator== and operator!= are defined by the parent which then leverage the child's IsEqual() override
    // IsEqual() should only be called by NetInfoInterface::operator== otherwise static_cast assumption could fail
    bool IsEqual(const NetInfoInterface& rhs) const override
    {
        ASSERT_IF_DEBUG(typeid(*this) == typeid(rhs));
        const auto& rhs_obj{static_cast<const MnNetInfo&>(rhs)};
        return m_addr == rhs_obj.m_addr;
    }
};

class ExtNetInfo final : public NetInfoInterface
{
private:
    static constexpr uint8_t CURRENT_VERSION{1};

    //! Returns true if there are addr:port duplicates in the object
    bool HasAddrPortDuplicates() const;

    //! Returns true if candidate is an addr:port duplicate in the object
    bool IsAddrPortDuplicate(const NetInfoEntry& candidate) const;

    //! Returns true if there are addr duplicates within a given address list
    bool HasAddrDuplicates(const NetInfoList& entries) const;

    //! Returns true if candidate is an addr duplicate within a given address list
    bool IsAddrDuplicate(const NetInfoEntry& candidate, const NetInfoList& entries) const;

    NetInfoStatus ProcessCandidate(const uint8_t purpose, const NetInfoEntry& candidate);
    static NetInfoStatus ValidateService(const CService& service);

private:
    uint8_t m_version{CURRENT_VERSION};
    std::map<uint8_t, NetInfoList> m_data{};

    // memory only
    NetInfoList m_all_entries{};

public:
    ExtNetInfo() = default;
    template <typename Stream>
    ExtNetInfo(deserialize_type, Stream& s) { s >> *this; }

    ~ExtNetInfo() = default;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << m_version;
        if (m_version == 0 || m_version > CURRENT_VERSION) {
            return; // Don't bother with unknown versions
        }
        s << m_data;
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> m_version;
        if (m_version == 0 || m_version > CURRENT_VERSION) {
            return; // Don't bother with unknown versions
        }
        s >> m_data;

        // Regenerate internal cache
        m_all_entries.clear();
        for (const auto& [_, entries] : m_data) {
            m_all_entries.insert(m_all_entries.end(), entries.begin(), entries.end());
        }
    }

    NetInfoStatus AddEntry(const uint8_t purpose, const std::string& input) override;
    NetInfoList GetEntries(std::optional<uint8_t> purpose_opt = std::nullopt) const override;

    CService GetPrimary() const override;
    bool HasEntries(uint8_t purpose) const override;
    bool IsEmpty() const override { return m_version == CURRENT_VERSION && m_data.empty(); }
    bool CanStorePlatform() const override { return true; }
    NetInfoStatus Validate() const override;
    UniValue ToJson() const override;
    std::string ToString() const override;

    void Clear() override
    {
        m_version = CURRENT_VERSION;
        m_data.clear();
        m_all_entries.clear();
    }

private:
    // operator== and operator!= are defined by the parent which then leverage the child's IsEqual() override
    // IsEqual() should only be called by NetInfoInterface::operator== otherwise static_cast assumption could fail
    bool IsEqual(const NetInfoInterface& rhs) const override
    {
        ASSERT_IF_DEBUG(typeid(*this) == typeid(rhs));
        const auto& rhs_obj{static_cast<const ExtNetInfo&>(rhs)};
        return m_version == rhs_obj.m_version && m_data == rhs_obj.m_data;
    }
};

template <typename T1>
class NetInfoSerWrapper
{
private:
    // This wrapper uses is_extended to decide which implementation of NetInfoInterface
    // to (de)serialize. is_extended is generally decided by the object version. As the
    // serialization infrastructure used doesn't allow for rewinding, the version needs
    // to be placed *before* netInfo. This isn't the case for CDeterministicMNStateDiff
    // where the version is stored at the end of the structure.
    //
    // Complicating things, MnNetInfo is fixed-length and ExtNetInfo is variable length
    // To work around this for CDeterministicMNStateDiff, we use a magic to distinguish
    // between MnNetInfo and ExtNetInfo. This lets us ignore the version field entirely
    static constexpr std::array<uint8_t, 4> EXTADDR_MAGIC{0x23, 0x23, 0x23, 0x23};

private:
    std::shared_ptr<NetInfoInterface>& m_data;
    const bool m_is_extended{false};

public:
    NetInfoSerWrapper() = delete;
    NetInfoSerWrapper(const NetInfoSerWrapper&) = delete;
    NetInfoSerWrapper(std::shared_ptr<NetInfoInterface>& data, const bool is_extended) :
        m_data{data},
        m_is_extended{is_extended}
    {
    }

    ~NetInfoSerWrapper() = default;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        if (const auto ptr{std::dynamic_pointer_cast<ExtNetInfo>(m_data)}) {
            if constexpr (std::is_same_v<std::decay_t<T1>, CDeterministicMNStateDiff>) {
                s.write(MakeByteSpan(EXTADDR_MAGIC));
            }
            s << *ptr;
        } else if (const auto ptr{std::dynamic_pointer_cast<MnNetInfo>(m_data)}) {
            s << *ptr;
        } else {
            // NetInfoInterface::MakeNetInfo() supplied an unexpected implementation or we didn't call it and
            // are left with a nullptr. Neither should happen.
            assert(false);
        }
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        if constexpr (std::is_same_v<std::decay_t<T1>, CDeterministicMNStateDiff>) {
            constexpr size_t magic_size{EXTADDR_MAGIC.size()};
            std::vector<uint8_t> bytes(magic_size);
            for (size_t idx{0}; idx < magic_size; idx++) {
                s >> bytes[idx];
            }
            if (std::ranges::equal(bytes, EXTADDR_MAGIC)) {
                // First four bytes match magic word, deserialize rest as extended format
                m_data = std::make_shared<ExtNetInfo>(deserialize, s);
                return;
            }
            // Didn't match magic, read stream as legacy format
            size_t target_bytes{::GetSerializeSize(MnNetInfo{}, s.GetVersion())};
            bytes.resize(target_bytes);
            for (size_t idx{magic_size}; idx < target_bytes; idx++) {
                s >> bytes[idx];
            }
            // Transform raw bytes to MnNetInfo
            CDataStream ss(s.GetType(), s.GetVersion());
            ss.write(MakeByteSpan(bytes));
            m_data = std::make_shared<MnNetInfo>(deserialize, ss);
        } else {
            if (m_is_extended) {
                m_data = std::make_shared<ExtNetInfo>(deserialize, s);
            } else {
                m_data = std::make_shared<MnNetInfo>(deserialize, s);
            }
        }
    }
};

#endif // BITCOIN_EVO_NETINFO_H
