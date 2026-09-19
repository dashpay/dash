// Copyright (c) 2018-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DASH_CRYPTO_BLS_H
#define DASH_CRYPTO_BLS_H

#include <hash.h>
#include <serialize.h>
#include <span.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <blst.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <ranges>
#include <utility>
#include <vector>

namespace bls {
    //! Selects the scheme used wherever no explicit flag is given (the
    //! flag-less Serialize/Unserialize/GetHash overloads, aggregation and
    //! aggregated verification): the legacy scheme from before the v19 hard
    //! fork, or the basic (IETF) scheme after it.
    extern std::atomic<bool> bls_legacy_scheme;
}

// reversed BLS12-381
constexpr int BLS_CURVE_ID_SIZE{32};
constexpr int BLS_CURVE_SECKEY_SIZE{32};
constexpr int BLS_CURVE_PUBKEY_SIZE{48};
constexpr int BLS_CURVE_SIG_SIZE{96};

class CBLSSecretKey;
class CBLSSignature;
class CBLSPublicKey;

/**
 * A secret scalar kept in secure memory (see BLSInit). A default constructed
 * holder stands for the zero scalar and owns no memory.
 */
class CBLSSecretScalar
{
private:
    blst_scalar* m_data{nullptr};

public:
    CBLSSecretScalar() = default;
    CBLSSecretScalar(const CBLSSecretScalar& o);
    CBLSSecretScalar& operator=(const CBLSSecretScalar& o);
    CBLSSecretScalar(CBLSSecretScalar&& o) noexcept : m_data(std::exchange(o.m_data, nullptr)) {}
    CBLSSecretScalar& operator=(CBLSSecretScalar&& o) noexcept
    {
        std::swap(m_data, o.m_data);
        return *this;
    }
    ~CBLSSecretScalar();

    //! The scalar, or zero when nothing has been stored
    const blst_scalar& Get() const;
    //! Writable storage, allocated on first use
    blst_scalar& Mutable();
    bool IsZero() const;
    bool operator==(const CBLSSecretScalar& o) const;
};

/**
 * Common part of the BLS objects. The derived class C provides the codec
 * through four static hooks:
 *   bool DecodeImpl(Span<const uint8_t> in, bool specificLegacyScheme, ImplType& out)
 *   void EncodeImpl(const ImplType& in, bool specificLegacyScheme, uint8_t* out)
 *   bool IsIdentity(const ImplType& v)
 *   bool EqualImpl(const ImplType& a, const ImplType& b)
 */
template <typename ImplType, size_t _SerSize, typename C>
class CBLSWrapper
{
    friend class CBLSSecretKey;
    friend class CBLSPublicKey;
    friend class CBLSSignature;

protected:
    ImplType impl{};
    bool fValid{false};
    mutable uint256 cachedHash;

public:
    static constexpr size_t SerSize = _SerSize;

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

    bool operator==(const C& r) const
    {
        return fValid == r.fValid && C::EqualImpl(impl, r.impl);
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

    void SetBytes(Span<const uint8_t> vecBytes, const bool specificLegacyScheme)
    {
        if (vecBytes.size() != SerSize) {
            Reset();
            return;
        }

        if (std::ranges::all_of(vecBytes, [](uint8_t c) { return c == 0; })) {
            Reset();
        } else if (!C::DecodeImpl(vecBytes, specificLegacyScheme, impl) || C::IsIdentity(impl)) {
            // neither undecodable input nor the identity element make a valid object
            Reset();
        } else {
            fValid = true;
        }
        cachedHash.SetNull();
    }

    std::vector<uint8_t> ToByteVector(const bool specificLegacyScheme) const
    {
        std::vector<uint8_t> ret(SerSize, 0);
        if (fValid) {
            C::EncodeImpl(impl, specificLegacyScheme, ret.data());
        }
        return ret;
    }

    std::array<uint8_t, SerSize> ToBytes(const bool specificLegacyScheme) const
    {
        std::array<uint8_t, SerSize> ret{};
        if (fValid) {
            C::EncodeImpl(impl, specificLegacyScheme, ret.data());
        }
        return ret;
    }

    const uint256& GetHash() const
    {
        if (cachedHash.IsNull()) {
            cachedHash = ::SerializeHash(*this);
        }
        return cachedHash;
    }

    bool SetHexStr(const std::string& str, const bool specificLegacyScheme)
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
        SetBytes(b, specificLegacyScheme);
        return IsValid();
    }

    // cppcheck-suppress functionStatic
    inline void Serialize(CSizeComputer& s) const
    {
        s.seek(SerSize);
    }

    template <typename Stream>
    inline void Serialize(Stream& s, const bool specificLegacyScheme) const
    {
        const auto bytes{ToBytes(specificLegacyScheme)};
        s.write(AsBytes(Span{bytes.data(), SerSize}));
    }

    template <typename Stream>
    inline void Serialize(Stream& s) const
    {
        Serialize(s, bls::bls_legacy_scheme.load());
    }

    template <typename Stream>
    inline void Unserialize(Stream& s, const bool specificLegacyScheme)
    {
        std::array<uint8_t, SerSize> vecBytes{};
        s.read(AsWritableBytes(Span{vecBytes.data(), SerSize}));
        SetBytes(vecBytes, specificLegacyScheme);

        if (!CheckMalleable(vecBytes, specificLegacyScheme)) {
            // If CheckMalleable failed with specificLegacyScheme, we need to try again with the opposite scheme.
            // Probably we received the BLS object sent with legacy scheme, but in the meanwhile the fork activated.
            SetBytes(vecBytes, !specificLegacyScheme);
            if (!CheckMalleable(vecBytes, !specificLegacyScheme)) {
                // Both attempts failed
                throw std::ios_base::failure("malleable BLS object");
            } else {
                // Indeed the received vecBytes was in opposite scheme. But we can't keep it (mixing with the new scheme will lead to undefined behavior)
                // Therefore, resetting current object (basically marking it as invalid).
                Reset();
            }
        }
    }

    template <typename Stream>
    inline void Unserialize(Stream& s)
    {
        Unserialize(s, bls::bls_legacy_scheme.load());
    }

    inline bool CheckMalleable(Span<uint8_t> vecBytes, const bool specificLegacyScheme) const
    {
        const auto bytes{ToBytes(specificLegacyScheme)};
        if (memcmp(vecBytes.data(), bytes.data(), SerSize)) {
            // TODO not sure if this is actually possible with the BLS libs. I'm assuming here that somewhere deep inside
            // these libs masking might happen, so that 2 different binary representations could result in the same object
            // representation
            return false;
        }
        return true;
    }

    inline std::string ToString(const bool specificLegacyScheme) const
    {
        auto buf = ToBytes(specificLegacyScheme);
        return HexStr(buf);
    }

    inline std::string ToString() const
    {
        return ToString(bls::bls_legacy_scheme.load());
    }
};


class CBLSId : public CBLSWrapper<uint256, BLS_CURVE_ID_SIZE, CBLSId>
{
    friend class CBLSWrapper<uint256, BLS_CURVE_ID_SIZE, CBLSId>;

    static bool DecodeImpl(Span<const uint8_t> in, bool /*specificLegacyScheme*/, uint256& out)
    {
        memcpy(out.begin(), in.data(), BLS_CURVE_ID_SIZE);
        return true;
    }
    static void EncodeImpl(const uint256& in, bool /*specificLegacyScheme*/, uint8_t* out)
    {
        memcpy(out, in.begin(), BLS_CURVE_ID_SIZE);
    }
    static bool IsIdentity(const uint256& v) { return v.IsNull(); }
    static bool EqualImpl(const uint256& a, const uint256& b) { return a == b; }

public:
    using CBLSWrapper::operator=;
    using CBLSWrapper::operator==;
    using CBLSWrapper::operator!=;
    using CBLSWrapper::CBLSWrapper;

    CBLSId() = default;
    explicit CBLSId(const uint256& nHash);
};

//! CBLSSecretKey is invariant to BLS scheme for Creation / Serialization / Deserialization
class CBLSSecretKey : public CBLSWrapper<CBLSSecretScalar, BLS_CURVE_SECKEY_SIZE, CBLSSecretKey>
{
    friend class CBLSWrapper<CBLSSecretScalar, BLS_CURVE_SECKEY_SIZE, CBLSSecretKey>;
    friend class CBLSPublicKey;
    friend class CBLSSignature;

    //! The flag does not select a scheme here: with it set the bytes are
    //! reduced modulo the group order, without it values above the order
    //! are rejected.
    static bool DecodeImpl(Span<const uint8_t> in, bool modOrder, CBLSSecretScalar& out);
    static void EncodeImpl(const CBLSSecretScalar& in, bool /*specificLegacyScheme*/, uint8_t* out);
    static bool IsIdentity(const CBLSSecretScalar& s) { return s.IsZero(); }
    static bool EqualImpl(const CBLSSecretScalar& a, const CBLSSecretScalar& b) { return a == b; }

public:
    using CBLSWrapper::operator=;
    using CBLSWrapper::operator==;
    using CBLSWrapper::operator!=;
    using CBLSWrapper::CBLSWrapper;

    CBLSSecretKey() = default;
    explicit CBLSSecretKey(Span<const unsigned char> vecBytes)
    {
        // The second param here is not 'is_legacy', but `modOrder`
        SetBytes(vecBytes, false);
    }
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
    [[nodiscard]] CBLSSignature Sign(const uint256& hash, const bool specificLegacyScheme) const;
};

class CBLSPublicKey : public CBLSWrapper<blst_p1, BLS_CURVE_PUBKEY_SIZE, CBLSPublicKey>
{
    friend class CBLSWrapper<blst_p1, BLS_CURVE_PUBKEY_SIZE, CBLSPublicKey>;
    friend class CBLSSecretKey;
    friend class CBLSSignature;

    static bool DecodeImpl(Span<const uint8_t> in, bool specificLegacyScheme, blst_p1& out);
    static void EncodeImpl(const blst_p1& in, bool specificLegacyScheme, uint8_t* out);
    static bool IsIdentity(const blst_p1& p) { return blst_p1_is_inf(&p); }
    static bool EqualImpl(const blst_p1& a, const blst_p1& b) { return blst_p1_is_equal(&a, &b); }

public:
    using CBLSWrapper::operator=;
    using CBLSWrapper::operator==;
    using CBLSWrapper::operator!=;
    using CBLSWrapper::CBLSWrapper;

    CBLSPublicKey() = default;

    void AggregateInsecure(const CBLSPublicKey& o);
    static CBLSPublicKey AggregateInsecure(Span<CBLSPublicKey> pks);

    bool PublicKeyShare(Span<CBLSPublicKey> mpk, const CBLSId& id);
    bool DHKeyExchange(const CBLSSecretKey& sk, const CBLSPublicKey& pk);

};

class CBLSPublicKeyVersionWrapper {
private:
    CBLSPublicKey& obj;
    bool legacy;
public:
    CBLSPublicKeyVersionWrapper(CBLSPublicKey& obj, bool legacy)
            : obj(obj)
            , legacy(legacy)
    {}
    template <typename Stream>
    inline void Serialize(Stream& s) const {
        obj.Serialize(s, legacy);
    }
    template <typename Stream>
    inline void Unserialize(Stream& s) {
        obj.Unserialize(s, legacy);
    }
};

class CBLSSignature : public CBLSWrapper<blst_p2, BLS_CURVE_SIG_SIZE, CBLSSignature>
{
    friend class CBLSWrapper<blst_p2, BLS_CURVE_SIG_SIZE, CBLSSignature>;
    friend class CBLSSecretKey;

    static bool DecodeImpl(Span<const uint8_t> in, bool specificLegacyScheme, blst_p2& out);
    static void EncodeImpl(const blst_p2& in, bool specificLegacyScheme, uint8_t* out);
    static bool IsIdentity(const blst_p2& p) { return blst_p2_is_inf(&p); }
    static bool EqualImpl(const blst_p2& a, const blst_p2& b) { return blst_p2_is_equal(&a, &b); }

public:
    using CBLSWrapper::operator==;
    using CBLSWrapper::operator!=;
    using CBLSWrapper::CBLSWrapper;

    CBLSSignature() = default;
    explicit CBLSSignature(Span<const unsigned char> bytes, bool is_serialized_legacy)
    {
        SetBytes(bytes, is_serialized_legacy);
    }
    CBLSSignature(const CBLSSignature&) = default;
    CBLSSignature& operator=(const CBLSSignature&) = default;

    void AggregateInsecure(const CBLSSignature& o);
    static CBLSSignature AggregateInsecure(Span<CBLSSignature> sigs);
    static CBLSSignature AggregateSecure(Span<CBLSSignature> sigs, Span<CBLSPublicKey> pks, const uint256& hash);

    void SubInsecure(const CBLSSignature& o);
    [[nodiscard]] bool VerifyInsecure(const CBLSPublicKey& pubKey, const uint256& hash, const bool specificLegacyScheme) const;
    [[nodiscard]] bool VerifyInsecure(const CBLSPublicKey& pubKey, const uint256& hash) const;
    [[nodiscard]] bool VerifyInsecureAggregated(Span<CBLSPublicKey> pubKeys, Span<uint256> hashes) const;

    [[nodiscard]] bool VerifySecureAggregated(Span<CBLSPublicKey> pks, const uint256& hash) const;

    bool Recover(Span<CBLSSignature> sigs, Span<CBLSId> ids);
};

class CBLSSignatureVersionWrapper {
private:
    CBLSSignature& obj;
    bool legacy;
public:
    CBLSSignatureVersionWrapper(CBLSSignature& obj, bool legacy)
            : obj(obj)
            , legacy(legacy)
    {}
    template <typename Stream>
    inline void Serialize(Stream& s) const {
        obj.Serialize(s, legacy);
    }
    template <typename Stream>
    inline void Unserialize(Stream& s) {
        obj.Unserialize(s, legacy);
    }
};

template<typename BLSObject>
class CBLSLazyWrapper
{
private:
    mutable std::mutex mutex;

    mutable std::array<uint8_t, BLSObject::SerSize> vecBytes{};

    mutable BLSObject obj;
    mutable bool objInitialized{false};

    // Indicates if the value contained in vecBytes is valid
    mutable bool bufValid{false};
    mutable bool bufLegacyScheme{true};

    mutable uint256 hash;

public:
    CBLSLazyWrapper() :
        bufLegacyScheme(bls::bls_legacy_scheme.load())
    {}

    explicit CBLSLazyWrapper(const CBLSLazyWrapper& r)
    {
        *this = r;
    }

    CBLSLazyWrapper& operator=(const CBLSLazyWrapper& r)
    {
        std::unique_lock<std::mutex> l(r.mutex);
        bufValid = r.bufValid;
        bufLegacyScheme = r.bufLegacyScheme;
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

    template<typename Stream>
    inline void Serialize(Stream& s, const bool specificLegacyScheme) const
    {
        std::unique_lock<std::mutex> l(mutex);
        if (!objInitialized && !bufValid) {
            std::fill(vecBytes.begin(), vecBytes.end(), 0);
        } else if (!bufValid || (bufLegacyScheme != specificLegacyScheme)) {
            vecBytes = obj.ToBytes(specificLegacyScheme);
            bufValid = true;
            bufLegacyScheme = specificLegacyScheme;
            hash.SetNull();
        }
        s.write(MakeByteSpan(vecBytes));
    }

    template<typename Stream>
    inline void Serialize(Stream& s) const
    {
        Serialize(s, bufLegacyScheme);
    }

    template<typename Stream>
    inline void Unserialize(Stream& s, const bool specificLegacyScheme) const
    {
        std::unique_lock<std::mutex> l(mutex);
        s.read(AsWritableBytes(Span{vecBytes.data(), BLSObject::SerSize}));
        bufValid = std::any_of(vecBytes.begin(), vecBytes.end(), [](uint8_t c) { return c != 0; });
        bufLegacyScheme = specificLegacyScheme;
        objInitialized = false;
        hash.SetNull();
    }

    template<typename Stream>
    inline void Unserialize(Stream& s) const
    {
        Unserialize(s, bufLegacyScheme);
    }

    void Set(const BLSObject& _obj, const bool specificLegacyScheme)
    {
        std::unique_lock<std::mutex> l(mutex);
        bufValid = false;
        bufLegacyScheme = specificLegacyScheme;
        objInitialized = true;
        obj = _obj;
        hash.SetNull();
    }
    const BLSObject& Get() const
    {
        std::unique_lock<std::mutex> l(mutex);
        static BLSObject invalidObj;
        if (!bufValid && !objInitialized) {
            return invalidObj;
        }
        if (!objInitialized) {
            obj.SetBytes(vecBytes, bufLegacyScheme);
            if (!obj.IsValid()) {
                bufValid = false;
                return invalidObj;
            }
            if (!obj.CheckMalleable(vecBytes, bufLegacyScheme)) {
                bufValid = false;
                return invalidObj;
            }
            objInitialized = true;
        }
        return obj;
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

            if (bufValid && r.bufValid && bufLegacyScheme == r.bufLegacyScheme) {
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

    uint256 GetHash() const
    {
        std::unique_lock<std::mutex> l(mutex);
        if (!objInitialized && !bufValid) {
            std::fill(vecBytes.begin(), vecBytes.end(), 0);
            hash.SetNull();
        } else if (!bufValid) {
            vecBytes = obj.ToBytes(bufLegacyScheme);
            bufValid = true;
            hash.SetNull();
        }
        if (hash.IsNull()) {
            CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
            ss.write(MakeByteSpan(vecBytes));
            hash = ss.GetHash();
        }
        return hash;
    }

    bool IsLegacy() const
    {
        return bufLegacyScheme;
    }

    void SetLegacy(bool specificLegacyScheme)
    {
        bufLegacyScheme = specificLegacyScheme;
    }

    std::string ToString() const
    {
        return Get().ToString(bufLegacyScheme);
    }
};
using CBLSLazySignature = CBLSLazyWrapper<CBLSSignature>;
using CBLSLazyPublicKey = CBLSLazyWrapper<CBLSPublicKey>;

class CBLSLazyPublicKeyVersionWrapper {
private:
    CBLSLazyPublicKey& obj;
    bool legacy;
public:
    CBLSLazyPublicKeyVersionWrapper(CBLSLazyPublicKey& obj, bool legacy)
            : obj(obj)
            , legacy(legacy)
    {}
    template <typename Stream>
    inline void Serialize(Stream& s) const {
        obj.Serialize(s, legacy);
    }
    template <typename Stream>
    inline void Unserialize(Stream& s) {
        obj.Unserialize(s, legacy);
    }
};

using BLSVerificationVectorPtr = std::shared_ptr<std::vector<CBLSPublicKey>>;

bool BLSInit();

#endif // DASH_CRYPTO_BLS_H
