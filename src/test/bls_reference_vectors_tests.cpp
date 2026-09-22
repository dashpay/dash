// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Reference vectors for the CBLS* wrapper layer.
//
// GenerateReferenceVectors() exercises the public API of bls/bls.h (both the
// legacy and the basic scheme, explicit flags as well as the global
// bls::bls_legacy_scheme switch) with fixed inputs and records every result as
// a "key=value" line. The recorded lines live in
// test/data/bls_reference_vectors.json and pin the behaviour of the wrapper
// layer, including the consensus-visible quirks of the legacy codec, across
// changes of the underlying implementation.
//
// To regenerate the file after an intentional behaviour change, run the
// test with BLS_REFERENCE_VECTORS_DUMP set to the path of the JSON file:
//   BLS_REFERENCE_VECTORS_DUMP=src/test/data/bls_reference_vectors.json src/test/test_dash -t bls_reference_vectors_tests

#include <bls/bls.h>
#include <bls/bls_batchverifier.h>
#include <bls/bls_ies.h>
#include <clientversion.h>
#include <crypto/sha256.h>
#include <hash.h>
#include <streams.h>
#include <test/data/bls_reference_vectors.json.h>
#include <test/util/json.h>
#include <uint256.h>
#include <univalue.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

// BLS12-381 group order r, base field modulus p (big-endian).
constexpr const char* ORDER_HEX = "73eda753299d7d483339d80809a1d80553bda402fffe5bfeffffffff00000001";
constexpr const char* ORDER_M1_HEX = "73eda753299d7d483339d80809a1d80553bda402fffe5bfeffffffff00000000";
constexpr const char* ORDER_P1_HEX = "73eda753299d7d483339d80809a1d80553bda402fffe5bfeffffffff00000002";
constexpr const char* ORDER_X2_HEX = "e7db4ea6533afa906673b0101343b00aa77b4805fffcb7fdfffffffe00000002";
constexpr const char* P_HEX = "1a0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaab";
constexpr const char* P_P1_HEX = "1a0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaac";

using Bytes = std::vector<uint8_t>;

Bytes DeterministicBytes(const std::string& tag, uint32_t i, size_t n)
{
    Bytes out;
    for (uint32_t counter = 0; out.size() < n; counter++) {
        const uint8_t suffix[8] = {
            uint8_t(i >> 24), uint8_t(i >> 16), uint8_t(i >> 8), uint8_t(i),
            uint8_t(counter >> 24), uint8_t(counter >> 16), uint8_t(counter >> 8), uint8_t(counter)};
        uint8_t digest[CSHA256::OUTPUT_SIZE];
        CSHA256().Write(reinterpret_cast<const uint8_t*>(tag.data()), tag.size()).Write(suffix, sizeof(suffix)).Finalize(digest);
        const size_t take = std::min<size_t>(sizeof(digest), n - out.size());
        out.insert(out.end(), digest, digest + take);
    }
    return out;
}

CBLSSecretKey Sk(uint32_t i)
{
    Bytes b = DeterministicBytes("dashbls-ref-sk", i, 32);
    b[0] &= 0x3f; // always below the group order
    CBLSSecretKey sk;
    sk.SetBytes(b, false);
    assert(sk.IsValid());
    return sk;
}

uint256 Msg(uint32_t i) { return uint256(DeterministicBytes("dashbls-ref-msg", i, 32)); }
uint256 IdHash(uint32_t i) { return uint256(DeterministicBytes("dashbls-ref-id", i, 32)); }

struct GlobalScheme {
    const bool prev;
    explicit GlobalScheme(bool legacy) : prev(bls::bls_legacy_scheme.load()) { bls::bls_legacy_scheme.store(legacy); }
    ~GlobalScheme() { bls::bls_legacy_scheme.store(prev); }
};

const char* G(bool legacy) { return legacy ? "L" : "B"; }
std::string F(bool flag) { return flag ? "1" : "0"; }

struct Emitter {
    std::vector<std::string> lines;

    void Raw(std::string l) { lines.push_back(std::move(l)); }
    void Str(const std::string& k, const std::string& v) { Raw(k + "=" + v); }
    void Bool(const std::string& k, bool v) { Str(k, v ? "1" : "0"); }
    void Hash(const std::string& k, const uint256& h) { Str(k, h.ToString()); }
    void Hex(const std::string& k, const Bytes& b) { Str(k, HexStr(b)); }
    // Both encodings of a wrapper object, or INVALID
    template <typename T>
    void Obj(const std::string& k, const T& o)
    {
        Str(k, o.IsValid() ? o.ToString(true) + ":" + o.ToString(false) : "INVALID");
    }
};

// Outcome of stream deserialization: "throw", "invalid" or the object
template <typename T>
void UnserializeProbe(Emitter& e, const std::string& k, const Bytes& bytes)
{
    CDataStream ss(bytes, SER_NETWORK, PROTOCOL_VERSION);
    T obj;
    try {
        ss >> obj;
    } catch (const std::ios_base::failure&) {
        e.Str(k, "throw");
        return;
    }
    e.Obj(k, obj);
}

template <typename T>
Bytes Ser(const T& obj)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << obj;
    return Bytes(UCharCast(ss.data()), UCharCast(ss.data() + ss.size()));
}

// ---------------------------------------------------------------------------
// A. Keys
void SectionKeys(Emitter& e)
{
    for (uint32_t i = 0; i < 4; i++) {
        const std::string t = "A." + ToString(i);
        CBLSSecretKey sk = Sk(i);
        e.Obj(t + ".sk", sk);
        e.Obj(t + ".pk", sk.GetPublicKey());
        for (bool g : {true, false}) {
            GlobalScheme gs(g);
            CBLSSecretKey sk2 = Sk(i);
            CBLSPublicKey pk2 = sk2.GetPublicKey();
            e.Hash(t + ".sk.hash.g" + G(g), sk2.GetHash());
            e.Hash(t + ".pk.hash.g" + G(g), pk2.GetHash());
            e.Hex(t + ".pk.ser.g" + G(g), Ser(pk2));
        }
        // The cached hash is not invalidated by a change of the global scheme
        CBLSPublicKey pk3 = sk.GetPublicKey();
        uint256 h1, h2;
        {
            GlobalScheme gs(true);
            h1 = pk3.GetHash();
        }
        {
            GlobalScheme gs(false);
            h2 = pk3.GetHash();
        }
        e.Bool(t + ".pk.hash.stable", h1 == h2);
    }
    e.Obj("A.invalid.sk", CBLSSecretKey());
    e.Obj("A.invalid.pk", CBLSSecretKey().GetPublicKey());
    e.Obj("A.invalid.sig", CBLSSecretKey().Sign(Msg(0), true));
    e.Obj("A.id.zero", CBLSId(uint256{}));
    e.Obj("A.id.h0", CBLSId(IdHash(0)));
}

// ---------------------------------------------------------------------------
// B. Secret key codec
void SectionSecretKeyCodec(Emitter& e)
{
    const std::vector<std::pair<std::string, Bytes>> probes{
        {"rm1", ParseHex(ORDER_M1_HEX)},
        {"r", ParseHex(ORDER_HEX)},
        {"rp1", ParseHex(ORDER_P1_HEX)},
        {"2r", ParseHex(ORDER_X2_HEX)},
        {"ff", Bytes(32, 0xff)},
        {"zero", Bytes(32, 0x00)},
        {"one", [] { Bytes b(32, 0); b[31] = 1; return b; }()},
        {"sk0", Sk(0).ToByteVector(false)},
    };
    for (const auto& [name, bytes] : probes) {
        const std::string t = "B." + name;
        for (bool f : {false, true}) {
            CBLSSecretKey sk;
            sk.SetBytes(bytes, f);
            e.Obj(t + ".f" + F(f), sk);
            if (sk.IsValid()) {
                e.Obj(t + ".f" + F(f) + ".pk", sk.GetPublicKey());
                e.Obj(t + ".f" + F(f) + ".sig", sk.Sign(Msg(0), true));
            }
        }
        for (bool g : {true, false}) {
            GlobalScheme gs(g);
            UnserializeProbe<CBLSSecretKey>(e, t + ".unser.g" + G(g), bytes);
        }
        CBLSSecretKey ctorKey(bytes);
        e.Obj(t + ".ctor", ctorKey);
    }
    CBLSSecretKey sk;
    e.Bool("B.hex.nonhex", sk.SetHexStr("zz" + HexStr(Bytes(31, 1)), false));
    e.Obj("B.hex.nonhex.obj", sk);
    e.Bool("B.hex.short", sk.SetHexStr(HexStr(Bytes(31, 1)), false));
    e.Bool("B.hex.long", sk.SetHexStr(HexStr(Bytes(33, 1)), false));
    e.Bool("B.hex.ok", sk.SetHexStr(Sk(1).ToString(false), false));
    e.Obj("B.hex.ok.obj", sk);
    e.Bool("B.hex.r.f0", sk.SetHexStr(ORDER_HEX, false));
    e.Bool("B.hex.r.f1", sk.SetHexStr(ORDER_HEX, true));
    e.Bool("B.hex.rp1.f1", sk.SetHexStr(ORDER_P1_HEX, true));
    e.Obj("B.hex.rp1.f1.obj", sk);
    e.Bool("B.len31", [] { CBLSSecretKey k; k.SetBytes(Bytes(31, 1), false); return k.IsValid(); }());
    e.Bool("B.len33", [] { CBLSSecretKey k; k.SetBytes(Bytes(33, 1), false); return k.IsValid(); }());
}

// ---------------------------------------------------------------------------
// C. G1 codec probes
Bytes WithByte(Bytes b, size_t idx, uint8_t v) { b[idx] = v; return b; }
Bytes OrByte(Bytes b, size_t idx, uint8_t v) { b[idx] |= v; return b; }
Bytes XorByte(Bytes b, size_t idx, uint8_t v) { b[idx] ^= v; return b; }
Bytes AndByte(Bytes b, size_t idx, uint8_t v) { b[idx] &= v; return b; }
Bytes Concat(Bytes a, const Bytes& b) { a.insert(a.end(), b.begin(), b.end()); return a; }

void ProbeG1(Emitter& e, const std::string& t, const Bytes& bytes)
{
    const CBLSSecretKey sk1 = Sk(1);
    const CBLSPublicKey pk1 = sk1.GetPublicKey();
    for (bool f : {true, false}) {
        const std::string k = t + ".f" + F(f);
        CBLSPublicKey pk;
        pk.SetBytes(bytes, f);
        e.Obj(k, pk);
        if (!pk.IsValid()) continue;
        CBLSPublicKey dh;
        e.Bool(k + ".dh", dh.DHKeyExchange(sk1, pk));
        e.Obj(k + ".dh.obj", dh);
        for (bool s : {true, false}) {
            const CBLSSignature sig = Sk(0).Sign(Msg(0), s);
            e.Bool(k + ".ver" + G(s), sig.VerifyInsecure(pk, Msg(0), s));
        }
        for (bool g : {true, false}) {
            GlobalScheme gs(g);
            CBLSPublicKey pkg;
            pkg.SetBytes(bytes, f);
            e.Hash(k + ".hash.g" + G(g), pkg.GetHash());
            std::vector<CBLSPublicKey> pks{pkg, pk1};
            std::vector<CBLSSignature> sigs{Sk(0).Sign(Msg(0), g), sk1.Sign(Msg(0), g)};
            CBLSSignature agg = CBLSSignature::AggregateSecure(sigs, pks, Msg(0));
            e.Obj(k + ".aggsec.g" + G(g), agg);
            e.Bool(k + ".versec.g" + G(g), agg.VerifySecureAggregated(pks, Msg(0)));
            CBLSPublicKey aggpk = CBLSPublicKey::AggregateInsecure(pks);
            e.Obj(k + ".aggpk.g" + G(g), aggpk);
        }
    }
    for (bool g : {true, false}) {
        GlobalScheme gs(g);
        UnserializeProbe<CBLSPublicKey>(e, t + ".unser.g" + G(g), bytes);
    }
}

void SectionG1(Emitter& e)
{
    const CBLSPublicKey pk0 = Sk(0).GetPublicKey();
    const Bytes L = pk0.ToByteVector(true);
    const Bytes B = pk0.ToByteVector(false);
    const Bytes p = ParseHex(P_HEX);
    const Bytes pp1 = ParseHex(P_P1_HEX);
    Bytes inf(48, 0);
    inf[0] = 0xc0;
    const std::vector<std::pair<std::string, Bytes>> probes{
        {"pkL", L},
        {"pkB", B},
        {"zeros", Bytes(48, 0)},
        {"inf", inf},
        {"badinf", WithByte(inf, 47, 0x01)},
        {"badinf.mid", WithByte(inf, 20, 0x10)},
        {"ff", Bytes(48, 0xff)},
        {"fflow", WithByte(Bytes(48, 0xff), 0, 0x1f)},
        {"L.or40", OrByte(L, 0, 0x40)},
        {"L.or20", OrByte(L, 0, 0x20)},
        {"L.orE0", OrByte(L, 0, 0xe0)},
        {"L.xor80", XorByte(L, 0, 0x80)},
        {"L.and1f", AndByte(L, 0, 0x1f)},
        {"B.xor20", XorByte(B, 0, 0x20)},
        {"B.and7f", AndByte(B, 0, 0x7f)},
        {"B.or40", OrByte(B, 0, 0x40)},
        {"B.tail", XorByte(B, 47, 0x01)},
        {"x0.80", WithByte(Bytes(48, 0), 0, 0x80)},
        {"x0.a0", WithByte(Bytes(48, 0), 0, 0xa0)},
        {"x1.00", WithByte(Bytes(48, 0), 47, 0x01)},
        {"x1.80", WithByte(WithByte(Bytes(48, 0), 47, 0x01), 0, 0x80)},
        {"x1.a0", WithByte(WithByte(Bytes(48, 0), 47, 0x01), 0, 0xa0)},
        {"x2.80", WithByte(WithByte(Bytes(48, 0), 47, 0x02), 0, 0x80)},
        {"x3.80", WithByte(WithByte(Bytes(48, 0), 47, 0x03), 0, 0x80)},
        {"x3.00", WithByte(Bytes(48, 0), 47, 0x03)},
        {"x5.80", WithByte(WithByte(Bytes(48, 0), 47, 0x05), 0, 0x80)},
        {"p.00", p},
        {"p.80", OrByte(p, 0, 0x80)},
        {"p.a0", OrByte(p, 0, 0xa0)},
        {"pp1.80", OrByte(pp1, 0, 0x80)},
        {"max1f", WithByte(Bytes(48, 0xff), 0, 0x1f)},
        {"max9f", WithByte(Bytes(48, 0xff), 0, 0x9f)},
    };
    for (const auto& [name, bytes] : probes) {
        ProbeG1(e, "C." + name, bytes);
    }
    for (size_t len : {47, 49, 0}) {
        CBLSPublicKey pk;
        pk.SetBytes(Bytes(len, 0x80), false);
        e.Bool("C.len" + ToString(len), pk.IsValid());
    }
}

// ---------------------------------------------------------------------------
// D. G2 codec probes
void ProbeG2(Emitter& e, const std::string& t, const Bytes& bytes)
{
    const CBLSPublicKey pk0 = Sk(0).GetPublicKey();
    for (bool f : {true, false}) {
        const std::string k = t + ".f" + F(f);
        CBLSSignature sig;
        sig.SetBytes(bytes, f);
        e.Obj(k, sig);
        if (!sig.IsValid()) continue;
        for (bool s : {true, false}) {
            e.Bool(k + ".ver" + G(s), sig.VerifyInsecure(pk0, Msg(0), s));
        }
        for (bool g : {true, false}) {
            GlobalScheme gs(g);
            CBLSSignature sigg;
            sigg.SetBytes(bytes, f);
            e.Hash(k + ".hash.g" + G(g), sigg.GetHash());
            e.Bool(k + ".ver2.g" + G(g), sigg.VerifyInsecure(pk0, Msg(0)));
            CBLSSignature agg = sigg;
            agg.AggregateInsecure(Sk(1).Sign(Msg(0), g));
            e.Obj(k + ".agg.g" + G(g), agg);
        }
    }
    for (bool g : {true, false}) {
        GlobalScheme gs(g);
        UnserializeProbe<CBLSSignature>(e, t + ".unser.g" + G(g), bytes);
    }
}

void SectionG2(Emitter& e)
{
    const CBLSSecretKey sk0 = Sk(0);
    const Bytes L = sk0.Sign(Msg(0), true).ToByteVector(true);
    const Bytes B = sk0.Sign(Msg(0), false).ToByteVector(false);
    const Bytes p = ParseHex(P_HEX);
    Bytes inf(96, 0);
    inf[0] = 0xc0;
    const Bytes zeros48(48, 0);
    const std::vector<std::pair<std::string, Bytes>> probes{
        {"sigL", L},
        {"sigB", B},
        {"sigL.asB", sk0.Sign(Msg(0), true).ToByteVector(false)},
        {"sigB.asL", sk0.Sign(Msg(0), false).ToByteVector(true)},
        {"zeros", Bytes(96, 0)},
        {"inf", inf},
        {"badinf", WithByte(inf, 95, 0x01)},
        {"badinf.mid", WithByte(inf, 50, 0x10)},
        {"ff", Bytes(96, 0xff)},
        {"ff.low", WithByte(WithByte(Bytes(96, 0xff), 0, 0x1f), 48, 0x1f)},
        {"L.or40", OrByte(L, 0, 0x40)},
        {"L.or20", OrByte(L, 0, 0x20)},
        {"L.orE0", OrByte(L, 0, 0xe0)},
        {"L.xor80", XorByte(L, 0, 0x80)},
        {"L.b48or80", OrByte(L, 48, 0x80)},
        {"L.b48or40", OrByte(L, 48, 0x40)},
        {"L.tail", XorByte(L, 95, 0x01)},
        {"B.xor20", XorByte(B, 0, 0x20)},
        {"B.and7f", AndByte(B, 0, 0x7f)},
        {"B.or40", OrByte(B, 0, 0x40)},
        {"B.b48or80", OrByte(B, 48, 0x80)},
        {"B.b48or40", OrByte(B, 48, 0x40)},
        {"B.b48or20", OrByte(B, 48, 0x20)},
        {"B.tail", XorByte(B, 95, 0x01)},
        {"x0.80", WithByte(Bytes(96, 0), 0, 0x80)},
        {"x0.a0", WithByte(Bytes(96, 0), 0, 0xa0)},
        {"c0.1", WithByte(WithByte(Bytes(96, 0), 47, 0x01), 0, 0x80)},
        {"c1.1", WithByte(WithByte(Bytes(96, 0), 95, 0x01), 0, 0x80)},
        {"c0.1.a0", WithByte(WithByte(Bytes(96, 0), 47, 0x01), 0, 0xa0)},
        {"c0.p", OrByte(Concat(p, zeros48), 0, 0x80)},
        {"c1.p", OrByte(Concat(zeros48, p), 0, 0x80)},
        {"c0.p.00", Concat(p, zeros48)},
        {"c1.p.00", Concat(zeros48, p)},
        {"pp", OrByte(Concat(p, p), 0, 0x80)},
        {"max", WithByte(Bytes(96, 0xff), 0, 0x1f)},
    };
    for (const auto& [name, bytes] : probes) {
        ProbeG2(e, "D." + name, bytes);
    }
    for (size_t len : {95, 97, 0}) {
        CBLSSignature sig;
        sig.SetBytes(Bytes(len, 0x80), false);
        e.Bool("D.len" + ToString(len), sig.IsValid());
    }
}

// ---------------------------------------------------------------------------
// E. Sign / verify
void SectionSignVerify(Emitter& e)
{
    for (uint32_t i = 0; i < 3; i++) {
        for (uint32_t j = 0; j < 2; j++) {
            for (bool f : {true, false}) {
                e.Obj("E.sig." + ToString(i) + "." + ToString(j) + ".f" + F(f), Sk(i).Sign(Msg(j), f));
            }
        }
    }
    const CBLSPublicKey pk0 = Sk(0).GetPublicKey();
    const CBLSPublicKey pk1 = Sk(1).GetPublicKey();
    for (bool f : {true, false}) {
        const CBLSSignature sig = Sk(0).Sign(Msg(0), f);
        const std::string t = std::string("E.ver.f") + F(f);
        e.Bool(t + ".ok", sig.VerifyInsecure(pk0, Msg(0), f));
        e.Bool(t + ".wrongmsg", sig.VerifyInsecure(pk0, Msg(1), f));
        e.Bool(t + ".wrongpk", sig.VerifyInsecure(pk1, Msg(0), f));
        e.Bool(t + ".cross", sig.VerifyInsecure(pk0, Msg(0), !f));
        e.Bool(t + ".invalidpk", sig.VerifyInsecure(CBLSPublicKey(), Msg(0), f));
        e.Bool(t + ".invalidsig", CBLSSignature().VerifyInsecure(pk0, Msg(0), f));
        for (bool g : {true, false}) {
            GlobalScheme gs(g);
            e.Bool(t + ".global.g" + G(g), sig.VerifyInsecure(pk0, Msg(0)));
        }
    }
}

// ---------------------------------------------------------------------------
// F. Aggregation
void SectionAggregation(Emitter& e)
{
    std::vector<CBLSSecretKey> sks;
    std::vector<CBLSPublicKey> pks;
    for (uint32_t i = 0; i < 4; i++) {
        sks.push_back(Sk(i));
        pks.push_back(sks.back().GetPublicKey());
    }
    for (bool g : {true, false}) {
        GlobalScheme gs(g);
        const std::string t = std::string("F.g") + G(g);
        std::vector<CBLSSignature> sigs;
        for (uint32_t i = 0; i < 4; i++) sigs.push_back(sks[i].Sign(Msg(0), g));

        // Insecure aggregation, incremental and static
        CBLSSecretKey skAgg = sks[0];
        CBLSPublicKey pkAgg = pks[0];
        CBLSSignature sigAgg = sigs[0];
        for (size_t n = 2; n <= 4; n++) {
            skAgg.AggregateInsecure(sks[n - 1]);
            pkAgg.AggregateInsecure(pks[n - 1]);
            sigAgg.AggregateInsecure(sigs[n - 1]);
            const std::string k = t + ".agg" + ToString(n);
            e.Obj(k + ".sk", skAgg);
            e.Obj(k + ".pk", pkAgg);
            e.Obj(k + ".sig", sigAgg);
            e.Bool(k + ".sk.static", CBLSSecretKey::AggregateInsecure(Span{sks}.first(n)) == skAgg);
            e.Bool(k + ".pk.static", CBLSPublicKey::AggregateInsecure(Span{pks}.first(n)) == pkAgg);
            e.Bool(k + ".sig.static", CBLSSignature::AggregateInsecure(Span{sigs}.first(n)) == sigAgg);
            e.Bool(k + ".pk.fromsk", skAgg.GetPublicKey() == pkAgg);
            e.Bool(k + ".ver", sigAgg.VerifyInsecure(pkAgg, Msg(0)));
        }
        e.Obj(t + ".agg0.sk", CBLSSecretKey::AggregateInsecure(Span{sks}.first(0)));
        e.Obj(t + ".agg0.pk", CBLSPublicKey::AggregateInsecure(Span{pks}.first(0)));
        e.Obj(t + ".agg0.sig", CBLSSignature::AggregateInsecure(Span{sigs}.first(0)));
        e.Obj(t + ".agg1.sk", CBLSSecretKey::AggregateInsecure(Span{sks}.first(1)));

        // Subtraction and objects at infinity
        CBLSSignature sub = sigAgg;
        sub.SubInsecure(sigs[0]);
        e.Obj(t + ".sub", sub);
        e.Bool(t + ".sub.ver", sub.VerifyInsecure(CBLSPublicKey::AggregateInsecure(Span{pks}.subspan(1)), Msg(0)));
        CBLSSignature infSig = sigs[0];
        infSig.SubInsecure(sigs[0]);
        e.Obj(t + ".infsig", infSig);
        e.Hash(t + ".infsig.hash", infSig.GetHash());
        CBLSSecretKey skRm1;
        skRm1.SetBytes(ParseHex(ORDER_M1_HEX), false);
        CBLSPublicKey neg;
        e.Bool(t + ".neg.dh", neg.DHKeyExchange(skRm1, pks[0]));
        e.Obj(t + ".neg", neg);
        CBLSPublicKey infPk = pks[0];
        infPk.AggregateInsecure(neg);
        e.Obj(t + ".infpk", infPk);
        e.Hash(t + ".infpk.hash", infPk.GetHash());
        for (bool f : {true, false}) {
            e.Bool(t + ".inf.ver.f" + F(f), infSig.VerifyInsecure(infPk, Msg(0), f));
            e.Bool(t + ".inf.ver.pk0.f" + F(f), infSig.VerifyInsecure(pks[0], Msg(0), f));
            e.Bool(t + ".inf.ver.sig0.f" + F(f), sigs[0].VerifyInsecure(infPk, Msg(0), f));
        }
        e.Bool(t + ".inf.eq.default", infSig == CBLSSignature());
        CBLSSignature infAgg = sigAgg;
        infAgg.AggregateInsecure(infSig);
        e.Bool(t + ".inf.agg.neutral", infAgg == sigAgg);

        // Aggregated verification with distinct / duplicate messages
        {
            std::vector<CBLSSignature> dsigs;
            std::vector<uint256> msgs;
            for (uint32_t i = 0; i < 4; i++) {
                msgs.push_back(Msg(i));
                dsigs.push_back(sks[i].Sign(Msg(i), g));
            }
            CBLSSignature dagg = CBLSSignature::AggregateInsecure(dsigs);
            e.Obj(t + ".vagg.distinct", dagg);
            e.Bool(t + ".vagg.distinct.ver", dagg.VerifyInsecureAggregated(pks, msgs));
            std::vector<uint256> wrong = msgs;
            wrong[2] = Msg(9);
            e.Bool(t + ".vagg.distinct.wrongmsg", dagg.VerifyInsecureAggregated(pks, wrong));
            std::vector<uint256> dup(4, Msg(0));
            e.Bool(t + ".vagg.dup.ver", sigAgg.VerifyInsecureAggregated(pks, dup));
            std::vector<uint256> two{Msg(0), Msg(0)};
            std::vector<CBLSPublicKey> twoPks{pks[0], pks[1]};
            CBLSSignature twoSig = CBLSSignature::AggregateInsecure(Span{sigs}.first(2));
            e.Bool(t + ".vagg.dup2.ver", twoSig.VerifyInsecureAggregated(twoPks, two));
            e.Bool(t + ".vagg.single.ver", sigs[0].VerifyInsecureAggregated(Span{pks}.first(1), Span{msgs}.first(1)));
            std::vector<CBLSPublicKey> swapped{pks[1], pks[0], pks[2], pks[3]};
            e.Bool(t + ".vagg.wrongpk", dagg.VerifyInsecureAggregated(swapped, msgs));
        }

        // Secure aggregation
        {
            CBLSSignature agg = CBLSSignature::AggregateSecure(sigs, pks, Msg(0));
            e.Obj(t + ".sec.agg", agg);
            e.Bool(t + ".sec.ver", agg.VerifySecureAggregated(pks, Msg(0)));
            std::vector<CBLSPublicKey> shuffled{pks[2], pks[0], pks[3], pks[1]};
            e.Bool(t + ".sec.ver.shuffled", agg.VerifySecureAggregated(shuffled, Msg(0)));
            std::vector<CBLSSignature> shuffledSigs{sigs[2], sigs[0], sigs[3], sigs[1]};
            e.Bool(t + ".sec.agg.shuffled", CBLSSignature::AggregateSecure(shuffledSigs, shuffled, Msg(0)) == agg);
            e.Bool(t + ".sec.ver.wrongmsg", agg.VerifySecureAggregated(pks, Msg(1)));
            std::vector<CBLSPublicKey> wrongSet{pks[0], pks[1], pks[2]};
            e.Bool(t + ".sec.ver.subset", agg.VerifySecureAggregated(wrongSet, Msg(0)));
            e.Bool(t + ".sec.ver.insecureagg", sigAgg.VerifySecureAggregated(pks, Msg(0)));
            std::vector<CBLSSignature> dupSigs{sigs[0], sigs[0], sigs[1]};
            std::vector<CBLSPublicKey> dupPks{pks[0], pks[0], pks[1]};
            CBLSSignature dupAgg = CBLSSignature::AggregateSecure(dupSigs, dupPks, Msg(0));
            e.Obj(t + ".sec.dup.agg", dupAgg);
            e.Bool(t + ".sec.dup.ver", dupAgg.VerifySecureAggregated(dupPks, Msg(0)));
            CBLSSignature single = CBLSSignature::AggregateSecure(Span{sigs}.first(1), Span{pks}.first(1), Msg(0));
            e.Obj(t + ".sec.single.agg", single);
            e.Bool(t + ".sec.single.ver", single.VerifySecureAggregated(Span{pks}.first(1), Msg(0)));
            e.Bool(t + ".sec.single.eq", single == sigs[0]);
            e.Obj(t + ".sec.empty", CBLSSignature::AggregateSecure(Span{sigs}.first(0), Span{pks}.first(0), Msg(0)));
            e.Obj(t + ".sec.mismatch", CBLSSignature::AggregateSecure(Span{sigs}.first(2), Span{pks}.first(3), Msg(0)));
            e.Bool(t + ".sec.ver.empty", agg.VerifySecureAggregated(Span{pks}.first(0), Msg(0)));
            e.Bool(t + ".sec.ver.invalidsig", CBLSSignature().VerifySecureAggregated(pks, Msg(0)));
            {
                GlobalScheme other(!g);
                e.Bool(t + ".sec.ver.otherscheme", agg.VerifySecureAggregated(pks, Msg(0)));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// G. Threshold
void SectionThreshold(Emitter& e)
{
    std::vector<CBLSSecretKey> msk{Sk(10), Sk(11), Sk(12)};
    std::vector<CBLSPublicKey> mpk;
    for (const auto& sk : msk) mpk.push_back(sk.GetPublicKey());
    std::vector<CBLSId> ids;
    for (uint32_t i = 0; i < 5; i++) ids.emplace_back(IdHash(i));

    std::vector<CBLSSecretKey> shares(5);
    std::vector<CBLSPublicKey> pkShares(5);
    for (size_t i = 0; i < 5; i++) {
        const std::string t = "G.share" + ToString(i);
        e.Bool(t + ".sk.ok", shares[i].SecretKeyShare(msk, ids[i]));
        e.Obj(t + ".sk", shares[i]);
        e.Bool(t + ".pk.ok", pkShares[i].PublicKeyShare(mpk, ids[i]));
        e.Obj(t + ".pk", pkShares[i]);
        e.Bool(t + ".pk.consistent", pkShares[i] == shares[i].GetPublicKey());
    }
    for (bool f : {true, false}) {
        const std::string t = std::string("G.f") + F(f);
        const CBLSSignature master = msk[0].Sign(Msg(1), f);
        std::vector<CBLSSignature> sigShares;
        for (size_t i = 0; i < 5; i++) {
            sigShares.push_back(shares[i].Sign(Msg(1), f));
            e.Obj(t + ".sigshare" + ToString(i), sigShares.back());
            e.Bool(t + ".sigshare" + ToString(i) + ".ver", sigShares.back().VerifyInsecure(pkShares[i], Msg(1), f));
        }
        auto recover = [&](const std::string& k, std::vector<size_t> idx) {
            std::vector<CBLSSignature> s;
            std::vector<CBLSId> d;
            for (size_t i : idx) {
                s.push_back(sigShares[i]);
                d.push_back(ids[i]);
            }
            CBLSSignature rec;
            e.Bool(k + ".ok", rec.Recover(s, d));
            e.Obj(k, rec);
            e.Bool(k + ".master", rec == master);
        };
        recover(t + ".rec012", {0, 1, 2});
        recover(t + ".rec123", {1, 2, 3});
        recover(t + ".rec420", {4, 2, 0});
        recover(t + ".rec01", {0, 1});
        recover(t + ".rec0", {0});
        recover(t + ".rec0123", {0, 1, 2, 3});
        recover(t + ".rec001", {0, 0, 1});
        recover(t + ".rec01234", {0, 1, 2, 3, 4});
        // size mismatch and invalid inputs
        {
            CBLSSignature rec;
            std::vector<CBLSSignature> s{sigShares[0], sigShares[1], sigShares[2]};
            std::vector<CBLSId> d{ids[0], ids[1]};
            e.Bool(t + ".rec.mismatch", rec.Recover(s, d));
            std::vector<CBLSId> dz{ids[0], CBLSId(uint256{}), ids[2]};
            e.Bool(t + ".rec.zeroid", rec.Recover(s, dz));
            std::vector<CBLSId> dinv{ids[0], CBLSId(), ids[2]};
            e.Bool(t + ".rec.invalidid", rec.Recover(s, dinv));
            std::vector<CBLSSignature> sinv{sigShares[0], CBLSSignature(), sigShares[2]};
            e.Bool(t + ".rec.invalidsig", rec.Recover(sinv, d));
            std::vector<CBLSId> dr{ids[0], CBLSId(uint256(ParseHex(ORDER_HEX))), ids[2]};
            e.Bool(t + ".rec.orderid", rec.Recover(s, dr));
        }
    }
    // Special ids
    const std::vector<std::pair<std::string, uint256>> specialIds{
        {"zero", uint256{}},
        {"one", uint256::ONE},
        {"five", [] { Bytes b(32, 0); b[31] = 5; return uint256(b); }()},
        {"rp5", [] { Bytes b = ParseHex(ORDER_HEX); b[31] += 5; return uint256(b); }()}, // r + 5, congruent to 5
        {"r", uint256(ParseHex(ORDER_HEX))},
        {"rm1", uint256(ParseHex(ORDER_M1_HEX))},
        {"ff", uint256(Bytes(32, 0xff))},
    };
    std::map<std::string, CBLSSecretKey> specialShares;
    for (const auto& [name, id] : specialIds) {
        const std::string t = "G.id." + name;
        CBLSId cid(id);
        CBLSSecretKey s;
        CBLSPublicKey p;
        e.Bool(t + ".sk.ok", s.SecretKeyShare(msk, cid));
        e.Obj(t + ".sk", s);
        e.Bool(t + ".pk.ok", p.PublicKeyShare(mpk, cid));
        e.Obj(t + ".pk", p);
        specialShares[name] = s;
    }
    e.Bool("G.id.congruent", specialShares["five"] == specialShares["rp5"]);
    {
        std::vector<CBLSSignature> s{specialShares["five"].Sign(Msg(1), true), specialShares["rp5"].Sign(Msg(1), true), specialShares["one"].Sign(Msg(1), true)};
        std::vector<CBLSId> d{CBLSId(specialIds[2].second), CBLSId(specialIds[3].second), CBLSId(specialIds[1].second)};
        CBLSSignature rec;
        e.Bool("G.id.congruent.rec", rec.Recover(s, d));
    }
    {
        CBLSSecretKey s;
        CBLSPublicKey p;
        std::vector<CBLSSecretKey> msk1{msk[0]};
        std::vector<CBLSPublicKey> mpk1{mpk[0]};
        e.Bool("G.msk1.sk.ok", s.SecretKeyShare(msk1, ids[0]));
        e.Obj("G.msk1.sk", s);
        e.Bool("G.msk1.pk.ok", p.PublicKeyShare(mpk1, ids[0]));
        e.Obj("G.msk1.pk", p);
        std::vector<CBLSSecretKey> mskInv{msk[0], CBLSSecretKey(), msk[2]};
        e.Bool("G.mskinv.sk.ok", s.SecretKeyShare(mskInv, ids[0]));
        e.Bool("G.mskinv.sk.valid", s.IsValid());
        std::vector<CBLSPublicKey> mpkInv{mpk[0], CBLSPublicKey(), mpk[2]};
        e.Bool("G.mpkinv.pk.ok", p.PublicKeyShare(mpkInv, ids[0]));
        e.Bool("G.idinv.sk.ok", s.SecretKeyShare(msk, CBLSId()));
        e.Bool("G.idinv.pk.ok", p.PublicKeyShare(mpk, CBLSId()));
        std::vector<CBLSSecretKey> msk0;
        e.Bool("G.msk0.sk.ok", s.SecretKeyShare(msk0, ids[0]));
    }
}

// ---------------------------------------------------------------------------
// H. Diffie-Hellman
void SectionDH(Emitter& e)
{
    for (uint32_t i = 0; i < 2; i++) {
        for (uint32_t j = 0; j < 3; j++) {
            CBLSPublicKey a, b;
            const std::string t = "H." + ToString(i) + "." + ToString(j);
            e.Bool(t + ".ok", a.DHKeyExchange(Sk(i), Sk(j).GetPublicKey()));
            e.Obj(t, a);
            b.DHKeyExchange(Sk(j), Sk(i).GetPublicKey());
            e.Bool(t + ".sym", a == b);
        }
    }
    CBLSPublicKey x;
    e.Bool("H.invalidsk", x.DHKeyExchange(CBLSSecretKey(), Sk(0).GetPublicKey()));
    e.Bool("H.invalidpk", x.DHKeyExchange(Sk(0), CBLSPublicKey()));
    CBLSSecretKey one;
    one.SetBytes([] { Bytes b(32, 0); b[31] = 1; return b; }(), false);
    e.Bool("H.one", x.DHKeyExchange(one, Sk(0).GetPublicKey()));
    e.Bool("H.one.eq", x == Sk(0).GetPublicKey());
}

// ---------------------------------------------------------------------------
// I. Stream serialization, version wrappers, lazy wrappers
void SectionSerialization(Emitter& e)
{
    for (bool g : {true, false}) {
        GlobalScheme gs(g);
        const std::string t = std::string("I.g") + G(g);
        CBLSSecretKey sk = Sk(2);
        CBLSPublicKey pk = sk.GetPublicKey();
        CBLSSignature sig = sk.Sign(Msg(2), g);
        CBLSId id(IdHash(2));

        e.Hex(t + ".sk.ser", Ser(sk));
        e.Hex(t + ".pk.ser", Ser(pk));
        e.Hex(t + ".sig.ser", Ser(sig));
        e.Hex(t + ".id.ser", Ser(id));
        UnserializeProbe<CBLSSecretKey>(e, t + ".sk.rt", Ser(sk));
        UnserializeProbe<CBLSPublicKey>(e, t + ".pk.rt", Ser(pk));
        UnserializeProbe<CBLSSignature>(e, t + ".sig.rt", Ser(sig));
        // opposite scheme bytes, garbage, zeros
        UnserializeProbe<CBLSPublicKey>(e, t + ".pk.opposite", pk.ToByteVector(!g));
        UnserializeProbe<CBLSSignature>(e, t + ".sig.opposite", sig.ToByteVector(!g));
        UnserializeProbe<CBLSPublicKey>(e, t + ".pk.garbage", Bytes(48, 0xff));
        UnserializeProbe<CBLSSignature>(e, t + ".sig.garbage", Bytes(96, 0xff));
        UnserializeProbe<CBLSPublicKey>(e, t + ".pk.zeros", Bytes(48, 0x00));
        UnserializeProbe<CBLSSignature>(e, t + ".sig.zeros", Bytes(96, 0x00));
        UnserializeProbe<CBLSSecretKey>(e, t + ".sk.zeros", Bytes(32, 0x00));
        UnserializeProbe<CBLSPublicKey>(e, t + ".pk.short", Bytes(47, 0x80));
        // version wrappers
        for (bool w : {true, false}) {
            CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
            ss << CBLSPublicKeyVersionWrapper(pk, w) << CBLSSignatureVersionWrapper(sig, w);
            e.Hex(t + ".vw.f" + F(w), Bytes(UCharCast(ss.data()), UCharCast(ss.data() + ss.size())));
            CBLSPublicKey pk2;
            CBLSSignature sig2;
            try {
                ss >> CBLSPublicKeyVersionWrapper(pk2, w) >> CBLSSignatureVersionWrapper(sig2, w);
                e.Bool(t + ".vw.f" + F(w) + ".rt", pk2 == pk && sig2 == sig);
            } catch (const std::ios_base::failure&) {
                e.Str(t + ".vw.f" + F(w) + ".rt", "throw");
            }
            CDataStream ss2(pk.ToByteVector(!w), SER_NETWORK, PROTOCOL_VERSION);
            try {
                ss2 >> CBLSPublicKeyVersionWrapper(pk2, w);
                e.Obj(t + ".vw.f" + F(w) + ".opposite", pk2);
            } catch (const std::ios_base::failure&) {
                e.Str(t + ".vw.f" + F(w) + ".opposite", "throw");
            }
        }
        // lazy wrappers
        for (bool l : {true, false}) {
            const std::string k = t + ".lazy.f" + F(l);
            CBLSLazyPublicKey lz;
            e.Bool(k + ".default.legacy", lz.IsLegacy());
            e.Hex(k + ".default.ser", Ser(lz));
            e.Obj(k + ".default.get", lz.Get());
            lz.Set(pk, l);
            e.Bool(k + ".legacy", lz.IsLegacy());
            e.Hex(k + ".ser", Ser(lz));
            e.Hash(k + ".hash", lz.GetHash());
            e.Str(k + ".str", lz.ToString());
            {
                CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                lz.Serialize(ss, !l);
                e.Hex(k + ".ser.other", Bytes(UCharCast(ss.data()), UCharCast(ss.data() + ss.size())));
                e.Bool(k + ".legacy.after", lz.IsLegacy());
                e.Hash(k + ".hash.after", lz.GetHash());
            }
            CBLSLazyPublicKey lz2;
            lz2.Set(pk, !l);
            e.Bool(k + ".eq.otherscheme", lz == lz2);
            e.Bool(k + ".hash.eq.otherscheme", lz.GetHash() == lz2.GetHash());
            CBLSLazyPublicKey lz3;
            lz3.Set(pk, l);
            e.Bool(k + ".eq.same", lz == lz3);
            // deserialize bytes of the other scheme and materialize
            CBLSLazyPublicKey lz4;
            {
                CDataStream ss(pk.ToByteVector(!l), SER_NETWORK, PROTOCOL_VERSION);
                lz4.Unserialize(ss, l);
            }
            e.Obj(k + ".unser.opposite.get", lz4.Get());
            e.Hex(k + ".unser.opposite.ser", Ser(lz4));
            CBLSLazyPublicKey lz5;
            {
                CDataStream ss(pk.ToByteVector(l), SER_NETWORK, PROTOCOL_VERSION);
                lz5.Unserialize(ss, l);
            }
            e.Obj(k + ".unser.get", lz5.Get());
            e.Bool(k + ".unser.eq", lz5 == lz3);
            e.Hash(k + ".unser.hash", lz5.GetHash());
            CBLSLazyPublicKey lz6;
            {
                CDataStream ss(Bytes(48, 0xff), SER_NETWORK, PROTOCOL_VERSION);
                lz6.Unserialize(ss, l);
            }
            e.Obj(k + ".unser.garbage.get", lz6.Get());
            e.Hex(k + ".unser.garbage.ser", Ser(lz6));
            CBLSLazyPublicKey lz7;
            {
                CDataStream ss(Bytes(48, 0x00), SER_NETWORK, PROTOCOL_VERSION);
                lz7.Unserialize(ss, l);
            }
            e.Obj(k + ".unser.zeros.get", lz7.Get());
            e.Bool(k + ".unser.zeros.eq.default", lz7 == CBLSLazyPublicKey());
            CBLSLazySignature ls;
            ls.Set(sig, l);
            e.Hex(k + ".sig.ser", Ser(ls));
            e.Hash(k + ".sig.hash", ls.GetHash());
            CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
            ss << CBLSLazyPublicKeyVersionWrapper(lz, !l);
            e.Hex(k + ".lazyvw", Bytes(UCharCast(ss.data()), UCharCast(ss.data() + ss.size())));
        }
    }
}

// ---------------------------------------------------------------------------
// J. IES
void SectionIES(Emitter& e)
{
    const CBLSSecretKey recipient = Sk(0);
    const CBLSSecretKey ephemeral = Sk(20);
    const Bytes plaintext = DeterministicBytes("dashbls-ref-ies", 0, 64);
    for (bool g : {true, false}) {
        GlobalScheme gs(g);
        const std::string t = std::string("J.g") + G(g);
        CBLSIESMultiRecipientBlobs blobs;
        blobs.InitEncrypt(2);
        blobs.ephemeralSecretKey = ephemeral;
        blobs.ephemeralPubKey = ephemeral.GetPublicKey();
        blobs.ivSeed = Msg(7);
        blobs.ivVector[0] = blobs.ivSeed;
        blobs.ivVector[1] = ::SerializeHash(blobs.ivVector[0]);
        e.Bool(t + ".enc0", blobs.Encrypt(0, recipient.GetPublicKey(), plaintext));
        e.Bool(t + ".enc1", blobs.Encrypt(1, Sk(1).GetPublicKey(), plaintext));
        e.Hex(t + ".blob0", blobs.blobs[0]);
        e.Hex(t + ".blob1", blobs.blobs[1]);
        e.Hex(t + ".ser", Ser(blobs));
        CBLSIESMultiRecipientBlobs::Blob dec;
        e.Bool(t + ".dec0", blobs.Decrypt(0, recipient, dec));
        e.Bool(t + ".dec0.eq", dec == plaintext);
        e.Bool(t + ".dec1", blobs.Decrypt(1, Sk(1), dec));
        e.Bool(t + ".dec1.eq", dec == plaintext);
        e.Bool(t + ".dec0.wrongkey", blobs.Decrypt(0, Sk(1), dec));
        e.Hex(t + ".dec0.wrongkey.data", dec);
        e.Bool(t + ".dec.invalidkey", blobs.Decrypt(0, CBLSSecretKey(), dec));
        e.Bool(t + ".dec.oob", blobs.Decrypt(2, recipient, dec));
        CBLSIESEncryptedBlob single{blobs.ephemeralPubKey, blobs.ivSeed, blobs.blobs[1]};
        e.Hex(t + ".single.ser", Ser(single));
        e.Bool(t + ".single.valid", single.IsValid());
        CDataStream out(SER_NETWORK, PROTOCOL_VERSION);
        e.Bool(t + ".single.dec", single.Decrypt(1, Sk(1), out));
        e.Bool(t + ".single.dec.eq", Bytes(UCharCast(out.data()), UCharCast(out.data() + out.size())) == plaintext);
        // deserialize the multi-recipient blobs again and decrypt
        CBLSIESMultiRecipientBlobs rt;
        {
            CDataStream ss(Ser(blobs), SER_NETWORK, PROTOCOL_VERSION);
            ss >> rt;
        }
        e.Bool(t + ".rt.dec0", rt.Decrypt(0, recipient, dec) && dec == plaintext);
    }
}

// ---------------------------------------------------------------------------
// K. Batch verifier
void SectionBatchVerifier(Emitter& e)
{
    for (bool g : {true, false}) {
        GlobalScheme gs(g);
        for (bool secure : {false, true}) {
            for (bool fallback : {false, true}) {
                const std::string t = std::string("K.g") + G(g) + ".s" + F(secure) + ".f" + F(fallback);
                CBLSBatchVerifier<int, int> verifier(secure, fallback);
                // source 1: two valid messages; source 2: one bad; source 3: one good, one bad;
                // source 4: same message hash as source 1 signed by another key
                auto push = [&](int source, int msgId, uint32_t signer, uint32_t claimed, uint32_t msg) {
                    verifier.PushMessage(source, msgId, Msg(msg), Sk(signer).Sign(Msg(msg), g), Sk(claimed).GetPublicKey());
                };
                push(1, 10, 1, 1, 0);
                push(1, 11, 1, 1, 1);
                push(2, 20, 2, 3, 2);
                push(3, 30, 3, 3, 3);
                push(3, 31, 3, 4, 4);
                push(4, 40, 4, 4, 0);
                verifier.Verify();
                std::string bad;
                for (int s : verifier.badSources) bad += ToString(s) + ",";
                e.Str(t + ".badsources", bad);
                bad.clear();
                for (int m : verifier.badMessages) bad += ToString(m) + ",";
                e.Str(t + ".badmessages", bad);
                // all valid
                CBLSBatchVerifier<int, int> ok(secure, fallback);
                for (int i = 0; i < 4; i++) {
                    ok.PushMessage(i, i, Msg(i), Sk(i).Sign(Msg(i), g), Sk(i).GetPublicKey());
                }
                ok.Verify();
                e.Bool(t + ".allvalid", ok.badSources.empty() && ok.badMessages.empty());
            }
        }
    }
}

std::vector<std::string> GenerateReferenceVectors()
{
    Emitter e;
    SectionKeys(e);
    SectionSecretKeyCodec(e);
    SectionG1(e);
    SectionG2(e);
    SectionSignVerify(e);
    SectionAggregation(e);
    SectionThreshold(e);
    SectionDH(e);
    SectionSerialization(e);
    SectionIES(e);
    SectionBatchVerifier(e);
    return e.lines;
}

} // namespace

BOOST_AUTO_TEST_SUITE(bls_reference_vectors_tests)

BOOST_AUTO_TEST_CASE(bls_reference_vectors)
{
    const bool prevScheme = bls::bls_legacy_scheme.load();
    const std::vector<std::string> actual = GenerateReferenceVectors();
    bls::bls_legacy_scheme.store(prevScheme);

    if (const char* dump = std::getenv("BLS_REFERENCE_VECTORS_DUMP")) {
        std::ofstream out(dump);
        out << "[\n";
        for (size_t i = 0; i < actual.size(); i++) {
            out << "  \"" << actual[i] << "\"" << (i + 1 < actual.size() ? "," : "") << "\n";
        }
        out << "]\n";
    }

    const UniValue expected = read_json(std::string(json_tests::bls_reference_vectors, json_tests::bls_reference_vectors + sizeof(json_tests::bls_reference_vectors)));
    BOOST_CHECK_EQUAL(actual.size(), expected.size());
    const size_t n = std::min<size_t>(actual.size(), expected.size());
    for (size_t i = 0; i < n; i++) {
        BOOST_CHECK_MESSAGE(actual[i] == expected[i].get_str(), "vector " << i << " differs: got '" << actual[i] << "', expected '" << expected[i].get_str() << "'");
    }
}

BOOST_AUTO_TEST_SUITE_END()
