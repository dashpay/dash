// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DASH_CRYPTO_BLS_H
#define DASH_CRYPTO_BLS_H

#include <bls/scheme.h>
#include <hash.h>
#include <serialize.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <dashbls/bls.hpp>
#include <dashbls/privatekey.hpp>
#include <dashbls/elements.hpp>
#include <dashbls/schemes.hpp>
#include <dashbls/threshold.hpp>

#include <array>
#include <mutex>
#include <ranges>
#include <type_traits>

struct CBLSIdImplicit;

//! Public keys and signatures are encoded under a scheme. Secret keys and ids are not.
template <typename Impl>
struct BLSHasScheme : std::true_type {};

template <>
struct BLSHasScheme<bls::PrivateKey> : std::false_type {};

template <>
struct BLSHasScheme<CBLSIdImplicit> : std::false_type {};

//! Stands in for the scheme argument of types that have no scheme, so that none can be passed.
struct BLSNoScheme {};

// reversed BLS12-381
constexpr int BLS_CURVE_ID_SIZE{32};
constexpr int BLS_CURVE_SECKEY_SIZE{32};
constexpr int BLS_CURVE_PUBKEY_SIZE{48};
constexpr int BLS_CURVE_SIG_SIZE{96};

class CBLSSignature;
class CBLSPublicKey;

template <typename ImplType, size_t _SerSize, typename C>
class CBLSWrapper
{
    friend class CBLSSecretKey;
    friend class CBLSPublicKey;
    friend class CBLSSignature;

protected:
    ImplType impl;
    bool fValid{false};
    mutable uint256 cachedHash;

public:
    static constexpr size_t SerSize = _SerSize;
    using SchemeArg = std::conditional_t<BLSHasScheme<ImplType>::value, BLSScheme, BLSNoScheme>;
    //! BLSScheme::Basic for keys and signatures.
    static constexpr SchemeArg DEFAULT_SCHEME{};
    static_assert(BLSScheme{} == BLSScheme::Basic);

    explicit CBLSWrapper() = default;

    CBLSWrapper(const CBLSWrapper& ref) = default;
    CBLSWrapper& operator=(const CBLSWrapper& ref) = default;
    CBLSWrapper(CBLSWrapper&& ref) noexcept
    {
        std::swap(impl, ref.impl);
        std::swap(fValid, ref.fValid);
        std::swap(cachedHash, ref.cachedHash);
    }
    CBLSWrapper& operator=(CBLSWrapper&& ref) noexcept
    {
        std::swap(impl, ref.impl);
        std::swap(fValid, ref.fValid);
        std::swap(cachedHash, ref.cachedHash);
        return *this;
    }

    virtual ~CBLSWrapper() = default;

    bool operator==(const C& r) const
    {
        return fValid == r.fValid && impl == r.impl;
    }
    bool operator!=(const C& r) const
    {
        return !((*this) == r);
    }
    bool operator<(const C& r) const
    {
        return GetHash() < r.GetHash();
    }

    bool IsValid() const
    {
        return fValid;
    }

    void Reset()
    {
        *(static_cast<C*>(this)) = C();
    }

    void SetBytes(Span<const uint8_t> vecBytes, [[maybe_unused]] SchemeArg scheme = DEFAULT_SCHEME)
    {
        if (vecBytes.size() != SerSize) {
            Reset();
            return;
        }

        if (std::ranges::all_of(vecBytes, [](uint8_t c) { return c == 0; })) {
            Reset();
        } else {
            try {
                if constexpr (BLSHasScheme<ImplType>::value) {
                    impl = ImplType::FromBytes(bls::Bytes(vecBytes.data(), vecBytes.size()), scheme == BLSScheme::Legacy);
                } else if constexpr (std::is_same_v<ImplType, bls::PrivateKey>) {
                    // Out-of-range scalars are rejected rather than reduced.
                    impl = ImplType::FromBytes(bls::Bytes(vecBytes.data(), vecBytes.size()), /*modOrder=*/false);
                } else {
                    impl = ImplType::FromBytes(vecBytes.data());
                }
                if (impl == ImplType()) {
                    Reset();
                    cachedHash.SetNull();
                    return;
                }
                fValid = true;
            } catch (...) {
                Reset();
            }
        }
        cachedHash.SetNull();
    }

    std::vector<uint8_t> ToByteVector([[maybe_unused]] SchemeArg scheme = DEFAULT_SCHEME) const
    {
        if (!fValid) {
            return std::vector<uint8_t>(SerSize, 0);
        }
        if constexpr (BLSHasScheme<ImplType>::value) {
            return impl.Serialize(scheme == BLSScheme::Legacy);
        } else {
            return impl.Serialize();
        }
    }

    std::array<uint8_t, SerSize> ToBytes([[maybe_unused]] SchemeArg scheme = DEFAULT_SCHEME) const
    {
        if (!fValid) {
            return std::array<uint8_t, SerSize>{};
        }
        if constexpr (BLSHasScheme<ImplType>::value) {
            return impl.SerializeToArray(scheme == BLSScheme::Legacy);
        } else {
            return impl.SerializeToArray();
        }
    }

    const uint256& GetHash() const
    {
        if (cachedHash.IsNull()) {
            cachedHash = ::SerializeHash(*this);
        }
        return cachedHash;
    }

    bool SetHexStr(const std::string& str, SchemeArg scheme = DEFAULT_SCHEME)
    {
        if (!IsHex(str)) {
            Reset();
            return false;
        }
        auto b = ParseHex(str);
        if (b.size() != SerSize) {
            Reset();
            return false;
        }
        SetBytes(b, scheme);
        return IsValid();
    }

    // cppcheck-suppress functionStatic
    inline void Serialize(CSizeComputer& s) const
    {
        s.seek(SerSize);
    }

    template <typename Stream>
    inline void Serialize(Stream& s, SchemeArg scheme = DEFAULT_SCHEME) const
    {
        const auto bytes{ToBytes(scheme)};
        s.write(AsBytes(Span{bytes.data(), SerSize}));
    }

    template <typename Stream>
    inline void Unserialize(Stream& s, SchemeArg scheme = DEFAULT_SCHEME)
    {
        std::array<uint8_t, SerSize> vecBytes{};
        s.read(AsWritableBytes(Span{vecBytes.data(), SerSize}));
        SetBytes(vecBytes, scheme);
        if (CheckMalleable(vecBytes, scheme)) return;
        if constexpr (BLSHasScheme<ImplType>::value) {
            // The bytes may be a valid encoding under the other scheme. Such an object is not kept:
            // it reads as invalid instead of failing the whole deserialization.
            const BLSScheme opposite{scheme == BLSScheme::Legacy ? BLSScheme::Basic : BLSScheme::Legacy};
            SetBytes(vecBytes, opposite);
            if (CheckMalleable(vecBytes, opposite)) {
                Reset();
                return;
            }
        }
        throw std::ios_base::failure("malleable BLS object");
    }

    inline bool CheckMalleable(Span<uint8_t> vecBytes, const SchemeArg scheme) const
    {
        const auto bytes{ToBytes(scheme)};
        if (memcmp(vecBytes.data(), bytes.data(), SerSize)) {
            // TODO not sure if this is actually possible with the BLS libs. I'm assuming here that somewhere deep inside
            // these libs masking might happen, so that 2 different binary representations could result in the same object
            // representation
            return false;
        }
        return true;
    }

    inline std::string ToString(SchemeArg scheme = DEFAULT_SCHEME) const { return HexStr(ToBytes(scheme)); }
};

struct CBLSIdImplicit : public uint256
{
    CBLSIdImplicit() = default;
    // cppcheck-suppress noExplicitConstructor
    CBLSIdImplicit(const uint256& id)
    {
        memcpy(begin(), id.begin(), sizeof(uint256));
    }
    static CBLSIdImplicit FromBytes(const uint8_t* buffer)
    {
        CBLSIdImplicit instance;
        memcpy(instance.begin(), buffer, sizeof(CBLSIdImplicit));
        return instance;
    }
    [[nodiscard]] std::vector<uint8_t> Serialize() const { return {begin(), end()}; }
    [[nodiscard]] std::array<uint8_t, 32> SerializeToArray() const { return m_data; }
};

class CBLSId : public CBLSWrapper<CBLSIdImplicit, BLS_CURVE_ID_SIZE, CBLSId>
{
public:
    using CBLSWrapper::operator=;
    using CBLSWrapper::operator==;
    using CBLSWrapper::operator!=;
    using CBLSWrapper::CBLSWrapper;

    CBLSId() = default;
    explicit CBLSId(const uint256& nHash);
};

//! A non-dependent requires-expression turns a bad conversion into a hard error, so the
//! negative checks are instantiated through a template.
template <typename T>
constexpr bool BLSToBytesTakesScheme = requires(T& obj) { obj.ToBytes(BLSScheme::Basic); };
template <typename T>
constexpr bool BLSSetBytesTakesScheme = requires(T& obj, Span<const uint8_t> bytes) {
    obj.SetBytes(bytes, BLSScheme::Legacy);
};

static_assert(!BLSToBytesTakesScheme<CBLSId>);

//! CBLSSecretKey is invariant to BLS scheme for Creation / Serialization / Deserialization
class CBLSSecretKey : public CBLSWrapper<bls::PrivateKey, BLS_CURVE_SECKEY_SIZE, CBLSSecretKey>
{
public:
    using CBLSWrapper::operator=;
    using CBLSWrapper::operator==;
    using CBLSWrapper::operator!=;
    using CBLSWrapper::CBLSWrapper;

    CBLSSecretKey() = default;
    explicit CBLSSecretKey(Span<const unsigned char> vecBytes) { SetBytes(vecBytes); }
    CBLSSecretKey(const CBLSSecretKey&) = default;
    CBLSSecretKey& operator=(const CBLSSecretKey&) = default;

    void AggregateInsecure(const CBLSSecretKey& o);
    static CBLSSecretKey AggregateInsecure(Span<CBLSSecretKey> sks);

#ifndef BUILD_BITCOIN_INTERNAL
    //! MakeNewKey() is invariant to BLS scheme
    void MakeNewKey();
#endif
    //! SecretKeyShare() is invariant to BLS scheme
    bool SecretKeyShare(Span<CBLSSecretKey> msk, const CBLSId& id);

    //! GetPublicKey() is invariant to BLS scheme
    [[nodiscard]] CBLSPublicKey GetPublicKey() const;
    [[nodiscard]] CBLSSignature Sign(const uint256& hash) const;
};

static_assert(!BLSSetBytesTakesScheme<CBLSSecretKey>);

class CBLSPublicKey : public CBLSWrapper<bls::G1Element, BLS_CURVE_PUBKEY_SIZE, CBLSPublicKey>
{
    friend class CBLSSecretKey;
    friend class CBLSSignature;

public:
    using CBLSWrapper::operator=;
    using CBLSWrapper::operator==;
    using CBLSWrapper::operator!=;
    using CBLSWrapper::CBLSWrapper;

    CBLSPublicKey() = default;

    void AggregateInsecure(const CBLSPublicKey& o);
    static CBLSPublicKey AggregateInsecure(Span<CBLSPublicKey> pks);

    void MulInsecure(const CBLSSecretKey& factor);

    bool PublicKeyShare(Span<CBLSPublicKey> mpk, const CBLSId& id);
    bool DHKeyExchange(const CBLSSecretKey& sk, const CBLSPublicKey& pk);

};

//! A BLS object together with the encoding it has in the structure that owns it.
template <typename T>
struct BLSEncoded {
    T& obj;
    BLSScheme scheme;

    BLSEncoded(T& obj, BLSScheme scheme) :
        obj(obj),
        scheme(scheme)
    {
    }

    template <typename Stream>
    inline void Serialize(Stream& s) const
    {
        obj.Serialize(s, scheme);
    }
    template <typename Stream>
    inline void Unserialize(Stream& s)
    {
        obj.Unserialize(s, scheme);
    }
};

class CBLSSignature : public CBLSWrapper<bls::G2Element, BLS_CURVE_SIG_SIZE, CBLSSignature>
{
    friend class CBLSSecretKey;

public:
    using CBLSWrapper::operator==;
    using CBLSWrapper::operator!=;
    using CBLSWrapper::CBLSWrapper;

    CBLSSignature() = default;
    explicit CBLSSignature(Span<const unsigned char> bytes, BLSScheme scheme = BLSScheme::Basic)
    {
        SetBytes(bytes, scheme);
    }
    CBLSSignature(const CBLSSignature&) = default;
    CBLSSignature& operator=(const CBLSSignature&) = default;

    void AggregateInsecure(const CBLSSignature& o);
    static CBLSSignature AggregateInsecure(Span<CBLSSignature> sigs);
    static CBLSSignature AggregateSecure(Span<CBLSSignature> sigs, Span<CBLSPublicKey> pks, const uint256& hash);

    void SubInsecure(const CBLSSignature& o);
    void MulInsecure(const CBLSSecretKey& factor);
    [[nodiscard]] bool VerifyInsecure(const CBLSPublicKey& pubKey, const uint256& hash,
                                      const BLSScheme scheme = BLSScheme::Basic) const;
    [[nodiscard]] bool VerifyInsecureAggregated(Span<CBLSPublicKey> pubKeys, Span<uint256> hashes) const;

    [[nodiscard]] bool VerifySecureAggregated(Span<CBLSPublicKey> pks, const uint256& hash,
                                              BLSScheme scheme = BLSScheme::Basic) const;

    bool Recover(Span<CBLSSignature> sigs, Span<CBLSId> ids);
};

/**
 * Holds a BLS object as its serialized bytes and decodes it on first use.
 *
 * The object has no scheme of its own. The bytes it was read from are kept together with the scheme
 * they were read under so that decoding can be deferred, and writing it back under that same scheme
 * needs no decoding at all. Anything that depends on an encoding names the scheme.
 */
template <typename BLSObject>
class CBLSLazyWrapper
{
private:
    mutable std::mutex mutex;

    // The encoding of the object under bufScheme, meaningful when bufValid is set
    mutable std::array<uint8_t, BLSObject::SerSize> vecBytes{};
    mutable bool bufValid{false};
    mutable BLSScheme bufScheme{BLSScheme::Basic};

    mutable BLSObject obj;
    mutable bool objInitialized{false};

    // Hash of vecBytes, null until computed
    mutable uint256 hash;

    const BLSObject& GetLocked() const
    {
        static const BLSObject invalidObj;
        if (!bufValid && !objInitialized) {
            return invalidObj;
        }
        if (!objInitialized) {
            obj.SetBytes(vecBytes, bufScheme);
            if (!obj.IsValid() || !obj.CheckMalleable(vecBytes, bufScheme)) {
                bufValid = false;
                return invalidObj;
            }
            objInitialized = true;
        }
        return obj;
    }

    std::array<uint8_t, BLSObject::SerSize> EncodeLocked(const BLSScheme scheme) const
    {
        if (bufValid && bufScheme == scheme) {
            return vecBytes;
        }
        const auto bytes{GetLocked().ToBytes(scheme)};
        if (objInitialized && !bufValid) {
            vecBytes = bytes;
            bufValid = true;
            bufScheme = scheme;
            hash.SetNull();
        }
        return bytes;
    }

protected:
    template <typename Stream>
    void SerializeAs(Stream& s, const BLSScheme scheme) const
    {
        std::unique_lock<std::mutex> l(mutex);
        s.write(MakeByteSpan(EncodeLocked(scheme)));
    }

    template <typename Stream>
    void UnserializeAs(Stream& s, const BLSScheme scheme)
    {
        std::unique_lock<std::mutex> l(mutex);
        s.read(AsWritableBytes(Span{vecBytes.data(), BLSObject::SerSize}));
        bufValid = std::any_of(vecBytes.begin(), vecBytes.end(), [](uint8_t c) { return c != 0; });
        bufScheme = scheme;
        objInitialized = false;
        hash.SetNull();
    }

    uint256 GetHashAs(const BLSScheme scheme) const
    {
        std::unique_lock<std::mutex> l(mutex);
        const auto bytes{EncodeLocked(scheme)};
        const bool cacheable{bufValid && bufScheme == scheme};
        if (cacheable && !hash.IsNull()) {
            return hash;
        }
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss.write(MakeByteSpan(bytes));
        const uint256 ret{ss.GetHash()};
        if (cacheable) {
            hash = ret;
        }
        return ret;
    }

public:
    CBLSLazyWrapper() = default;

    explicit CBLSLazyWrapper(const CBLSLazyWrapper& r)
    {
        *this = r;
    }
    virtual ~CBLSLazyWrapper() = default;

    CBLSLazyWrapper& operator=(const CBLSLazyWrapper& r)
    {
        std::unique_lock<std::mutex> l(r.mutex);
        bufValid = r.bufValid;
        bufScheme = r.bufScheme;
        if (r.bufValid) {
            vecBytes = r.vecBytes;
        } else {
            std::fill(vecBytes.begin(), vecBytes.end(), 0);
        }
        objInitialized = r.objInitialized;
        if (r.objInitialized) {
            obj = r.obj;
        } else {
            obj.Reset();
        }
        hash = r.hash;
        return *this;
    }

    // cppcheck-suppress functionStatic
    inline void Serialize(CSizeComputer& s) const
    {
        s.seek(BLSObject::SerSize);
    }

    void Set(const BLSObject& _obj)
    {
        std::unique_lock<std::mutex> l(mutex);
        bufValid = false;
        objInitialized = true;
        obj = _obj;
        hash.SetNull();
    }

    const BLSObject& Get() const
    {
        std::unique_lock<std::mutex> l(mutex);
        return GetLocked();
    }

    bool operator==(const CBLSLazyWrapper& r) const
    {
        if (&r == this) return true;
        {
            std::scoped_lock lock(mutex, r.mutex);
            // If neither bufValid or objInitialized are set, then the object is the default object.
            const bool is_default{!bufValid && !objInitialized};
            const bool r_is_default{!r.bufValid && !r.objInitialized};
            // If both are default; they are equal.
            if (is_default && r_is_default) return true;
            // If one is default and the other isn't, we are not equal
            if (is_default != r_is_default) return false;

            if (bufValid && r.bufValid && bufScheme == r.bufScheme) {
                return vecBytes == r.vecBytes;
            }
            if (objInitialized && r.objInitialized) {
                return obj == r.obj;
            }
        }
        return Get() == r.Get();
    }

    bool operator!=(const CBLSLazyWrapper& r) const
    {
        return !(*this == r);
    }
};

//! Signatures only travel in messages and payloads that are always basic.
class CBLSLazySignature : public CBLSLazyWrapper<CBLSSignature>
{
public:
    using CBLSLazyWrapper::Serialize;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        CBLSLazyWrapper::SerializeAs(s, BLSScheme::Basic);
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        CBLSLazyWrapper::UnserializeAs(s, BLSScheme::Basic);
    }

    uint256 GetHash() const { return CBLSLazyWrapper::GetHashAs(BLSScheme::Basic); }

    std::string ToString() const { return CBLSLazyWrapper::Get().ToString(); }
};

//! A public key is encoded under the scheme its owner's version selects, so every encoding names one.
class CBLSLazyPublicKey : public CBLSLazyWrapper<CBLSPublicKey>
{
public:
    using CBLSLazyWrapper::Serialize;

    template <typename Stream>
    void Serialize(Stream& s, const BLSScheme scheme) const
    {
        CBLSLazyWrapper::SerializeAs(s, scheme);
    }

    template <typename Stream>
    void Unserialize(Stream& s, const BLSScheme scheme)
    {
        CBLSLazyWrapper::UnserializeAs(s, scheme);
    }

    uint256 GetHash(const BLSScheme scheme) const { return CBLSLazyWrapper::GetHashAs(scheme); }

    std::string ToString(const BLSScheme scheme) const { return CBLSLazyWrapper::Get().ToString(scheme); }
};

using BLSVerificationVectorPtr = std::shared_ptr<std::vector<CBLSPublicKey>>;

bool BLSInit();

#endif // DASH_CRYPTO_BLS_H
