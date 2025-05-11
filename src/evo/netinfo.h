// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_NETINFO_H
#define BITCOIN_EVO_NETINFO_H

#include <evo/common.h>
#include <netaddress.h>
#include <serialize.h>
#include <streams.h>

#include <variant>

class CService;

/** Maximum entries that can be stored in an ExtNetInfo */
static constexpr uint8_t NETINFO_EXTENDED_LIMIT{32};

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
    template <typename Stream> NetInfoEntry(deserialize_type, Stream& s) { s >> *this; }

    ~NetInfoEntry() = default;

    bool operator<(const NetInfoEntry& rhs) const;
    bool operator==(const NetInfoEntry& rhs) const;
    bool operator!=(const NetInfoEntry& rhs) const { return !(*this == rhs); }

    template <typename Stream>
    void Serialize(Stream& s_) const
    {
        OverrideStream<Stream> s(&s_, s_.GetType(), s_.GetVersion() | ADDRV2_FORMAT);
        if (const auto* data_ptr{std::get_if<CService>(&m_data)};
            m_type == NetInfoType::Service && data_ptr && data_ptr->IsValid()) {
            s << m_type << *data_ptr;
        } else {
            s << NetInfoType::Invalid;
        }
    }

    void Serialize(CSizeComputer& s) const
    {
        auto size = ::GetSerializeSize(uint8_t{}, s.GetVersion());
        if (m_type == NetInfoType::Service) {
            size += ::GetSerializeSize(CService{}, s.GetVersion() | ADDRV2_FORMAT);
        }
        s.seek(size);
    }

    template <typename Stream>
    void Unserialize(Stream& s_)
    {
        OverrideStream<Stream> s(&s_, s_.GetType(), s_.GetVersion() | ADDRV2_FORMAT);
        s >> m_type;
        if (m_type == NetInfoType::Service) {
            m_data = CService{};
            try {
                CService& service{std::get<CService>(m_data)};
                s >> service;
                if (!service.IsValid()) {
                    throw std::ios_base::failure("Invalid CService");
                }
            } catch (const std::ios_base::failure&) {
                Clear();
            }
        } else {
            // Invalid type, reset to mark object as invalid
            Clear();
        }
    }

    void Clear()
    {
        m_type = NetInfoType::Invalid;
        m_data = std::monostate{};
    }

    std::optional<std::reference_wrapper<const CService>> GetAddrPort() const;
    uint16_t GetPort() const;
    bool IsEmpty() const { return *this == NetInfoEntry{}; }
    bool IsTriviallyValid() const;
    std::string ToString() const;
    std::string ToStringAddr() const;
    std::string ToStringAddrPort() const;
};

template<> struct is_serializable_enum<NetInfoEntry::NetInfoType> : std::true_type {};

using NetInfoList = std::vector<std::reference_wrapper<const NetInfoEntry>>;

class NetInfoInterface
{
public:
    static bool IsEqual(const std::shared_ptr<NetInfoInterface>& lhs, const std::shared_ptr<NetInfoInterface>& rhs);

public:
    virtual ~NetInfoInterface() = default;

    virtual NetInfoStatus AddEntry(const std::string& service) = 0;
    virtual NetInfoList GetEntries() const = 0;

    virtual const CService& GetPrimary() const = 0;
    virtual bool IsEmpty() const = 0;
    virtual NetInfoStatus Validate() const = 0;
    virtual std::string ToString() const = 0;

    virtual void Clear() = 0;
};

class MnNetInfo final : public NetInfoInterface
{
private:
    NetInfoEntry m_addr{};

private:
    static NetInfoStatus ValidateService(const CService& service);

public:
    MnNetInfo() = default;
    template <typename Stream> MnNetInfo(deserialize_type, Stream& s) { s >> *this; }

    ~MnNetInfo() = default;

    bool operator==(const MnNetInfo& rhs) const { return m_addr == rhs.m_addr; }
    bool operator!=(const MnNetInfo& rhs) const { return !(*this == rhs); }

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        if (const auto& service{m_addr.GetAddrPort()}; service.has_value()) {
            s << service->get();
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

    NetInfoStatus AddEntry(const std::string& service) override;
    NetInfoList GetEntries() const override;

    const CService& GetPrimary() const override;
    bool IsEmpty() const override { return *this == MnNetInfo(); }
    NetInfoStatus Validate() const override;
    std::string ToString() const override;

    void Clear() override { m_addr.Clear(); }
};

class ExtNetInfo final : public NetInfoInterface
{
private:
    static constexpr uint8_t CURRENT_VERSION{1};

    bool HasDuplicates() const;
    bool IsDuplicateCandidate(const NetInfoEntry& candidate) const;
    NetInfoStatus ProcessCandidate(const NetInfoEntry& candidate);
    static NetInfoStatus ValidateService(const CService& service, bool is_primary);

private:
    uint8_t m_version{CURRENT_VERSION};
    std::vector<NetInfoEntry> m_data{};

public:
    ExtNetInfo() = default;
    template <typename Stream> ExtNetInfo(deserialize_type, Stream& s) { s >> *this; }

    ~ExtNetInfo() = default;

    bool operator==(const ExtNetInfo& rhs) const { return m_version == rhs.m_version && m_data == rhs.m_data; }
    bool operator!=(const ExtNetInfo& rhs) const { return !(*this == rhs); }

    SERIALIZE_METHODS(ExtNetInfo, obj)
    {
        READWRITE(obj.m_version);
        if (obj.m_version == 0 || obj.m_version > CURRENT_VERSION) {
            return; // Don't bother with unknown versions
        }
        READWRITE(obj.m_data);
    }

    NetInfoStatus AddEntry(const std::string& input) override;
    NetInfoList GetEntries() const override;

    const CService& GetPrimary() const override;
    bool IsEmpty() const override { return *this == ExtNetInfo(); }
    NetInfoStatus Validate() const override;
    std::string ToString() const override;

    void Clear() override
    {
        m_version = CURRENT_VERSION;
        m_data.clear();
    }
};

/* Selects NetInfoInterface implementation to use based on object version */
template <typename T1>
std::shared_ptr<NetInfoInterface> MakeNetInfo(const T1& obj)
{
    assert(obj.nVersion > 0);
    if (obj.nVersion >= ProTxVersion::ExtAddr) {
        return std::make_shared<ExtNetInfo>();
    }
    return std::make_shared<MnNetInfo>();
}

class NetInfoSerWrapper
{
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
    template <typename Stream> NetInfoSerWrapper(deserialize_type, Stream& s) { s >> *this; }

    ~NetInfoSerWrapper() = default;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        if (const auto ptr{std::dynamic_pointer_cast<ExtNetInfo>(m_data)}) {
            assert(m_is_extended);
            s << ptr;
        } else if (const auto ptr{std::dynamic_pointer_cast<MnNetInfo>(m_data)}) {
            assert(!m_is_extended);
            s << ptr;
        } else {
            throw std::ios_base::failure("Improperly constructed NetInfoInterface");
        }
    }

    void Serialize(CSizeComputer& s) const
    {
        if (m_is_extended) {
            s.seek(::GetSerializeSize(ExtNetInfo{}, s.GetVersion()));
        } else {
            s.seek(::GetSerializeSize(MnNetInfo{}, s.GetVersion()));
        }
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        if (m_is_extended) {
            std::shared_ptr<ExtNetInfo> ptr;
            s >> ptr;
            m_data = std::move(ptr);
        } else {
            std::shared_ptr<MnNetInfo> ptr;
            s >> ptr;
            m_data = std::move(ptr);
        }
    }
};

#endif // BITCOIN_EVO_NETINFO_H
