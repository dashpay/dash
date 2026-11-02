// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef SRC_TEST_VECTORS_HPP_
#define SRC_TEST_VECTORS_HPP_

// Reference consensus vectors.
//
// GenerateReferenceVectors() deterministically exercises the public dashbls
// API - legacy and basic serialization (including malformed input), legacy
// signing, secure aggregation, threshold signing, extended keys and the
// private key edge cases - and emits one "name=value" line per result.
//
// test-vectors/reference.txt holds the output of the relic-based library;
// runtest compares the current library against it line by line, and
// tools/vectorgen.cpp regenerates it. Only the public API is used so the
// same code builds against any backend.

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "bls.hpp"

namespace bls_test_vectors {

using namespace bls;
using std::vector;

struct Emitter {
    std::vector<std::string> lines;

    static std::string hex(const uint8_t* p, size_t n)
    {
        static const char* digits = "0123456789abcdef";
        std::string s;
        s.reserve(n * 2);
        for (size_t i = 0; i < n; i++) {
            s.push_back(digits[p[i] >> 4]);
            s.push_back(digits[p[i] & 0x0f]);
        }
        return s;
    }

    void raw(const std::string& line) { lines.push_back(line); }
    void dump(const std::string& name, const vector<uint8_t>& v) { lines.push_back(name + "=" + hex(v.data(), v.size())); }
    template <size_t N>
    void dump(const std::string& name, const std::array<uint8_t, N>& v) { lines.push_back(name + "=" + hex(v.data(), N)); }
    void dumpb(const std::string& name, bool v) { lines.push_back(name + "=" + (v ? "1" : "0")); }
    void dumpu(const std::string& name, uint32_t v)
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%08x", v);
        lines.push_back(name + "=" + buf);
    }
    void exc(const std::string& name) { lines.push_back(name + "=EXC"); }
};

inline vector<uint8_t> seedN(int i)
{
    vector<uint8_t> s(32);
    for (int j = 0; j < 32; j++) s[j] = (uint8_t)(0x11 * (i + 1) + j * 7);
    return s;
}

inline vector<uint8_t> hashN(int i)
{
    vector<uint8_t> h(32);
    for (int j = 0; j < 32; j++) h[j] = (uint8_t)(0xA5 ^ (i * 31 + j * 13));
    return h;
}

inline std::string tag(const char* base, int i)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "%s.%d", base, i);
    return std::string(buf);
}

// ---------------------------------------------------------------- section A/B
inline void KeysAndSignatures(Emitter& e)
{
    LegacySchemeMPL legacy;
    BasicSchemeMPL basic;

    for (int i = 0; i < 8; i++) {
        PrivateKey sk = basic.KeyGen(seedN(i));
        e.dump(tag("A.sk", i), sk.Serialize());

        G1Element pk = sk.GetG1Element();
        e.dump(tag("A.pk.basic", i), pk.Serialize(false));
        e.dump(tag("A.pk.legacy", i), pk.Serialize(true));
        e.dumpu(tag("A.fp.basic", i), pk.GetFingerprint(false));
        e.dumpu(tag("A.fp.legacy", i), pk.GetFingerprint(true));
        e.dump(tag("A.skG2.basic", i), sk.GetG2Element().Serialize(false));
        e.dump(tag("A.skG2.legacy", i), sk.GetG2Element().Serialize(true));

        // round-trips through both codecs
        e.dump(tag("A.pk.rt.legacy", i),
             G1Element::FromBytes(Bytes(pk.Serialize(true)), true).Serialize(true));
        e.dump(tag("A.pk.rt.basic", i),
             G1Element::FromBytes(Bytes(pk.Serialize(false)), false).Serialize(false));

        // legacy sign over a 32-byte hash; serialize under both codecs
        vector<uint8_t> h = hashN(i);
        G2Element lsig = legacy.Sign(sk, Bytes(h));
        e.dump(tag("B.lsig.legacy", i), lsig.Serialize(true));
        e.dump(tag("B.lsig.basic", i), lsig.Serialize(false));
        e.dumpb(tag("B.lverify", i), legacy.Verify(pk, Bytes(h), lsig));
        // wrong message must fail
        vector<uint8_t> h2 = hashN(i + 100);
        e.dumpb(tag("B.lverify.bad", i), legacy.Verify(pk, Bytes(h2), lsig));
        // legacy sig round-trip through legacy codec
        e.dump(tag("B.lsig.rt", i),
             G2Element::FromBytes(Bytes(lsig.Serialize(true)), true).Serialize(true));

        // basic scheme (arbitrary-length message)
        vector<uint8_t> msg = {(uint8_t)i, 1, 2, 3, 200, 255};
        G2Element bsig = basic.Sign(sk, msg);
        e.dump(tag("B.bsig.basic", i), bsig.Serialize(false));
        e.dump(tag("B.bsig.legacy", i), bsig.Serialize(true));
        e.dumpb(tag("B.bverify", i), basic.Verify(pk, msg, bsig));

        // EIP-2333 derivation
        e.dump(tag("A.child.hard", i), basic.DeriveChildSk(sk, 0x8000002a).Serialize());
        e.dump(tag("A.child.soft", i), basic.DeriveChildSkUnhardened(sk, 1337).Serialize());
    }
}

// ---------------------------------------------------------------- section C/D
inline void SecureAggregation(Emitter& e)
{
    LegacySchemeMPL legacy;
    BasicSchemeMPL basic;

    for (int n : {1, 2, 3, 10}) {
        vector<PrivateKey> sks;
        vector<G1Element> pks;
        vector<G2Element> lsigs, bsigs;
        vector<uint8_t> h = hashN(n);
        for (int i = 0; i < n; i++) {
            sks.push_back(basic.KeyGen(seedN(200 + n * 16 + i)));
            pks.push_back(sks.back().GetG1Element());
            lsigs.push_back(legacy.Sign(sks.back(), Bytes(h)));
            bsigs.push_back(basic.Sign(sks.back(), Bytes(h)));
        }
        G2Element lagg = legacy.AggregateSecure(pks, lsigs, Bytes(h));
        e.dump(tag("C.aggsec.legacy", n), lagg.Serialize(true));
        e.dumpb(tag("C.versec.legacy", n), legacy.VerifySecure(pks, lagg, Bytes(h)));

        G2Element bagg = basic.AggregateSecure(pks, bsigs, Bytes(h));
        e.dump(tag("C.aggsec.basic", n), bagg.Serialize(false));
        e.dumpb(tag("C.versec.basic", n), basic.VerifySecure(pks, bagg, Bytes(h)));

        // cross checks must fail
        e.dumpb(tag("C.versec.legacy.bad", n),
              legacy.VerifySecure(pks, lagg, Bytes(hashN(n + 50))));

        // plain aggregation
        e.dump(tag("C.agg.G2", n), basic.Aggregate(bsigs).Serialize(false));
        e.dump(tag("C.agg.G1", n), basic.Aggregate(pks).Serialize(false));
        e.dump(tag("C.agg.sk", n), PrivateKey::Aggregate(sks).Serialize());
    }

    // legacy multi-message AggregateVerify
    {
        int n = 4;
        vector<PrivateKey> sks;
        vector<G1Element> pks;
        vector<G2Element> sigs;
        vector<vector<uint8_t>> msgs;
        for (int i = 0; i < n; i++) {
            sks.push_back(basic.KeyGen(seedN(300 + i)));
            pks.push_back(sks.back().GetG1Element());
            msgs.push_back(hashN(300 + i));
            sigs.push_back(legacy.Sign(sks.back(), Bytes(msgs.back())));
        }
        G2Element agg = basic.Aggregate(sigs);  // plain point sum
        vector<Bytes> msgRefs;
        for (auto& m : msgs) msgRefs.push_back(Bytes(m));
        e.dump("D.lagg", agg.Serialize(true));
        e.dumpb("D.laggverify", legacy.AggregateVerify(pks, msgRefs, agg));
        vector<Bytes> badRefs;
        badRefs.push_back(Bytes(msgs[1]));
        for (size_t i = 1; i < msgs.size(); i++) badRefs.push_back(Bytes(msgs[i]));
        e.dumpb("D.laggverify.bad", legacy.AggregateVerify(pks, badRefs, agg));
    }
}

// ------------------------------------------------------------------ section E
inline void ThresholdVectors(Emitter& e)
{
    BasicSchemeMPL basic;
    const int m = 3, n = 5;

    // polynomial coefficients (the "master" keys)
    vector<PrivateKey> coeffs;
    vector<G1Element> vvec;
    vector<G2Element> sigCoeffs;
    vector<uint8_t> h = hashN(77);
    for (int i = 0; i < m; i++) {
        coeffs.push_back(basic.KeyGen(seedN(400 + i)));
        vvec.push_back(coeffs.back().GetG1Element());
        sigCoeffs.push_back(Threshold::Sign(coeffs[i], Bytes(h)));
    }
    e.dump("E.mastersig", Threshold::Sign(coeffs[0], Bytes(h)).Serialize(true));
    e.dumpb("E.masterverify",
          Threshold::Verify(vvec[0], Bytes(h), Threshold::Sign(coeffs[0], Bytes(h))));

    vector<vector<uint8_t>> ids;
    for (int i = 0; i < n; i++) ids.push_back(hashN(500 + i));

    vector<PrivateKey> skShares;
    vector<G2Element> sigShares;
    for (int i = 0; i < n; i++) {
        PrivateKey sh = Threshold::PrivateKeyShare(coeffs, Bytes(ids[i]));
        skShares.push_back(sh);
        e.dump(tag("E.skshare", i), sh.Serialize());
        G1Element pkShare = Threshold::PublicKeyShare(vvec, Bytes(ids[i]));
        e.dump(tag("E.pkshare", i), pkShare.Serialize(true));
        e.dumpb(tag("E.sharematch", i), pkShare == sh.GetG1Element());
        G2Element sigShare = Threshold::SignatureShare(sigCoeffs, Bytes(ids[i]));
        e.dump(tag("E.sigshare.poly", i), sigShare.Serialize(true));
        G2Element sigShare2 = Threshold::Sign(sh, Bytes(h));
        e.dump(tag("E.sigshare.sign", i), sigShare2.Serialize(true));
        sigShares.push_back(sigShare2);
    }

    // recover from first m shares
    vector<PrivateKey> mSk(skShares.begin(), skShares.begin() + m);
    vector<G2Element> mSig(sigShares.begin(), sigShares.begin() + m);
    vector<Bytes> mIds;
    for (int i = 0; i < m; i++) mIds.push_back(Bytes(ids[i]));

    PrivateKey skRec = Threshold::PrivateKeyRecover(mSk, mIds);
    e.dump("E.skrecover", skRec.Serialize());
    e.dumpb("E.skrecover.match", skRec == coeffs[0]);

    vector<G1Element> mPk;
    for (int i = 0; i < m; i++) mPk.push_back(skShares[i].GetG1Element());
    e.dump("E.pkrecover", Threshold::PublicKeyRecover(mPk, mIds).Serialize(true));

    G2Element sigRec = Threshold::SignatureRecover(mSig, mIds);
    e.dump("E.sigrecover", sigRec.Serialize(true));
    e.dumpb("E.sigrecover.verify", Threshold::Verify(vvec[0], Bytes(h), sigRec));

    // recovery from a different share subset must give the same signature
    vector<G2Element> mSig2(sigShares.begin() + 1, sigShares.begin() + 1 + m);
    vector<Bytes> mIds2;
    for (int i = 1; i < 1 + m; i++) mIds2.push_back(Bytes(ids[i]));
    e.dumpb("E.sigrecover.same",
          Threshold::SignatureRecover(mSig2, mIds2) == sigRec);
}

// ------------------------------------------------------------------ section F
inline void ExtendedKeys(Emitter& e)
{
    vector<uint8_t> seed = seedN(600);
    ExtendedPrivateKey esk = ExtendedPrivateKey::FromSeed(Bytes(seed));
    e.dump("F.esk", esk.Serialize());
    e.dump("F.esk.pk.legacy", esk.GetPublicKey().Serialize(true));
    e.dump("F.esk.pk.basic", esk.GetPublicKey().Serialize(false));
    e.dump("F.esk.chaincode", esk.GetChainCode().Serialize());

    // legacy derivation chain (fLegacy defaults true on these APIs)
    ExtendedPrivateKey c1l = esk.PrivateChild(1, true);
    ExtendedPrivateKey c2l = c1l.PrivateChild(0x8000002a, true);
    e.dump("F.child.legacy", c2l.Serialize());
    e.dump("F.child.legacy.sk", c2l.GetPrivateKey().Serialize());

    // modern derivation chain
    ExtendedPrivateKey c1b = esk.PrivateChild(1, false);
    ExtendedPrivateKey c2b = c1b.PrivateChild(0x8000002a, false);
    e.dump("F.child.basic", c2b.Serialize());
    e.dump("F.child.basic.sk", c2b.GetPrivateKey().Serialize());

    // extended public keys and public derivation
    ExtendedPublicKey epkl = esk.GetExtendedPublicKey(true);
    e.dump("F.epk.legacy", epkl.Serialize(true));
    ExtendedPublicKey epkb = esk.GetExtendedPublicKey(false);
    e.dump("F.epk.basic", epkb.Serialize(false));
    e.dump("F.epk.child.legacy", epkl.PublicChild(1, true).Serialize(true));
    e.dump("F.epk.child.basic", epkb.PublicChild(1, false).Serialize(false));
    e.dumpb("F.pubpriv.match.legacy",
          epkl.PublicChild(1, true).GetPublicKey() ==
              esk.PrivateChild(1, true).GetPublicKey());

    // round-trips
    e.dump("F.esk.rt",
         ExtendedPrivateKey::FromBytes(Bytes(esk.Serialize())).Serialize());
    e.dump("F.epk.rt.legacy",
         ExtendedPublicKey::FromBytes(Bytes(epkl.Serialize(true)), true).Serialize(true));
}

// ------------------------------------------------------------------ section G
inline void PrivateKeyEdges(Emitter& e)
{
    // in-range key round-trip, both modOrder values
    PrivateKey sk = BasicSchemeMPL().KeyGen(seedN(700));
    vector<uint8_t> raw = sk.Serialize();
    e.dump("G.inrange.mod0", PrivateKey::FromBytes(Bytes(raw), false).Serialize());
    e.dump("G.inrange.mod1", PrivateKey::FromBytes(Bytes(raw), true).Serialize());

    // value >= group order: modOrder=true reduces, modOrder=false throws
    vector<uint8_t> big(32, 0xff);
    try { e.dump("G.big.mod1", PrivateKey::FromBytes(Bytes(big), true).Serialize()); }
    catch (const std::exception&) { e.exc("G.big.mod1"); }
    try { e.dump("G.big.mod0", PrivateKey::FromBytes(Bytes(big), false).Serialize()); }
    catch (const std::exception&) { e.exc("G.big.mod0"); }

    // the group order itself, and order-1
    // r = 0x73eda753299d7d483339d80809a1d80553bda402fffe5bfeffffffff00000001
    vector<uint8_t> order = {
        0x73, 0xed, 0xa7, 0x53, 0x29, 0x9d, 0x7d, 0x48, 0x33, 0x39, 0xd8,
        0x08, 0x09, 0xa1, 0xd8, 0x05, 0x53, 0xbd, 0xa4, 0x02, 0xff, 0xfe,
        0x5b, 0xfe, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01};
    try { e.dump("G.order.mod1", PrivateKey::FromBytes(Bytes(order), true).Serialize()); }
    catch (const std::exception&) { e.exc("G.order.mod1"); }
    try { e.dump("G.order.mod0", PrivateKey::FromBytes(Bytes(order), false).Serialize()); }
    catch (const std::exception&) { e.exc("G.order.mod0"); }
    vector<uint8_t> orderm1 = order;
    orderm1[31] = 0x00;
    try { e.dump("G.orderm1.mod0", PrivateKey::FromBytes(Bytes(orderm1), false).Serialize()); }
    catch (const std::exception&) { e.exc("G.orderm1.mod0"); }

    // zero key
    vector<uint8_t> zero(32, 0x00);
    try {
        PrivateKey z = PrivateKey::FromBytes(Bytes(zero), false);
        e.dump("G.zero.sk", z.Serialize());
        e.dump("G.zero.pk", z.GetG1Element().Serialize(false));
    } catch (const std::exception&) { e.exc("G.zero.sk"); }

    // FromSeedBIP32
    e.dump("G.bip32seed", PrivateKey::FromSeedBIP32(Bytes(seedN(701))).Serialize());

    // DH key exchange: sk_a * pk_b == sk_b * pk_a
    PrivateKey a = BasicSchemeMPL().KeyGen(seedN(702));
    PrivateKey b = BasicSchemeMPL().KeyGen(seedN(703));
    G1Element dh = a * b.GetG1Element();
    e.dump("G.dh", dh.Serialize(false));
    e.dumpb("G.dh.sym", dh == (b * a.GetG1Element()));
}

// ------------------------------------------------------------------ section H
// Crafted byte strings through every decode path. For each: either the
// round-trip hex (proving accept + canonical re-encoding) or EXC.
inline void decodeProbe(Emitter& e, const std::string& name, const vector<uint8_t>& in, bool fLegacy)
{
    bool isG1 = in.size() == 48;
    try {
        if (isG1) {
            G1Element el = G1Element::FromBytes(Bytes(in), fLegacy);
            e.dump(name + ".ser", el.Serialize(fLegacy));
            e.dumpb(name + ".valid", el.IsValid());
        } else {
            G2Element el = G2Element::FromBytes(Bytes(in), fLegacy);
            e.dump(name + ".ser", el.Serialize(fLegacy));
            e.dumpb(name + ".valid", el.IsValid());
        }
    } catch (const std::exception&) { e.exc(name); }
}

inline void decodeProbeUnchecked(Emitter& e, const std::string& name, const vector<uint8_t>& in, bool fLegacy)
{
    bool isG1 = in.size() == 48;
    try {
        if (isG1) {
            G1Element el = G1Element::FromBytesUnchecked(Bytes(in), fLegacy);
            e.dump(name + ".ser", el.Serialize(fLegacy));
            e.dumpb(name + ".valid", el.IsValid());
        } else {
            G2Element el = G2Element::FromBytesUnchecked(Bytes(in), fLegacy);
            e.dump(name + ".ser", el.Serialize(fLegacy));
            e.dumpb(name + ".valid", el.IsValid());
        }
    } catch (const std::exception&) { e.exc(name); }
}

inline vector<uint8_t> fromHex(const char* hex)
{
    vector<uint8_t> out;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
        auto nyb = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return c - 'A' + 10;
        };
        out.push_back((uint8_t)((nyb(hex[i]) << 4) | nyb(hex[i + 1])));
    }
    return out;
}

inline void EdgeDecodes(Emitter& e)
{
    BasicSchemeMPL basic;
    PrivateKey sk = basic.KeyGen(seedN(800));
    G1Element pk = sk.GetG1Element();
    G2Element sig = LegacySchemeMPL().Sign(sk, Bytes(hashN(800)));

    // 1. canonical infinity, both sizes, both modes
    vector<uint8_t> inf48(48, 0);
    inf48[0] = 0xc0;
    vector<uint8_t> inf96(96, 0);
    inf96[0] = 0xc0;
    decodeProbe(e, "H.g1.inf.basic", inf48, false);
    decodeProbe(e, "H.g1.inf.legacy", inf48, true);
    decodeProbe(e, "H.g2.inf.basic", inf96, false);
    decodeProbe(e, "H.g2.inf.legacy", inf96, true);

    // 2. non-canonical infinity (0xc0 with a trailing nonzero byte)
    vector<uint8_t> badInf48 = inf48;
    badInf48[47] = 0x01;
    vector<uint8_t> badInf96 = inf96;
    badInf96[95] = 0x01;
    decodeProbe(e, "H.g1.badinf.basic", badInf48, false);
    decodeProbe(e, "H.g1.badinf.legacy", badInf48, true);
    decodeProbe(e, "H.g2.badinf.basic", badInf96, false);
    decodeProbe(e, "H.g2.badinf.legacy", badInf96, true);

    // 3. all zeros
    decodeProbe(e, "H.g1.zeros.basic", vector<uint8_t>(48, 0), false);
    decodeProbe(e, "H.g1.zeros.legacy", vector<uint8_t>(48, 0), true);
    decodeProbe(e, "H.g2.zeros.basic", vector<uint8_t>(96, 0), false);
    decodeProbe(e, "H.g2.zeros.legacy", vector<uint8_t>(96, 0), true);
    decodeProbeUnchecked(e, "H.g1.zeros.legacy.unchk", vector<uint8_t>(48, 0), true);
    decodeProbeUnchecked(e, "H.g2.zeros.legacy.unchk", vector<uint8_t>(96, 0), true);

    // 4. x >= field prime (0xff filler), with/without legacy sign bit
    vector<uint8_t> ff48(48, 0xff);
    vector<uint8_t> ff96(96, 0xff);
    decodeProbe(e, "H.g1.ff.basic", ff48, false);
    decodeProbe(e, "H.g1.ff.legacy", ff48, true);
    decodeProbe(e, "H.g2.ff.basic", ff96, false);
    decodeProbe(e, "H.g2.ff.legacy", ff96, true);
    vector<uint8_t> ff48low = ff48;
    ff48low[0] = 0x1f;  // below prime top byte range but x still >= p
    decodeProbe(e, "H.g1.fflow.legacy", ff48low, true);

    // 5. legacy bytes fed to the modern decoder and vice versa
    decodeProbe(e, "H.g1.legacyAsBasic", pk.Serialize(true), false);
    decodeProbe(e, "H.g1.basicAsLegacy", pk.Serialize(false), true);
    decodeProbe(e, "H.g2.legacyAsBasic", sig.Serialize(true), false);
    decodeProbe(e, "H.g2.basicAsLegacy", sig.Serialize(false), true);

    // 6. valid encodings with flipped sign bit (the negated point)
    vector<uint8_t> pkl = pk.Serialize(true);
    pkl[0] ^= 0x80;
    decodeProbe(e, "H.g1.negated.legacy", pkl, true);
    vector<uint8_t> pkb = pk.Serialize(false);
    pkb[0] ^= 0x20;
    decodeProbe(e, "H.g1.negated.basic", pkb, false);

    // 7. on-curve but non-subgroup points (found by scanning small x with a
    //    blst helper; each x satisfies y^2 = x^3 + 4 (+4i) with the point
    //    outside the r-torsion). Encodings below use y = the "smaller" root
    //    (sign bit 0), in each codec's byte layout.
    //    G1: x = 4 -> not in subgroup.
    vector<uint8_t> nsG1(48, 0);
    nsG1[47] = 0x04;
    decodeProbe(e, "H.g1.nonsub.legacy", nsG1, true);
    decodeProbeUnchecked(e, "H.g1.nonsub.legacy.unchk", nsG1, true);
    vector<uint8_t> nsG1b = nsG1;
    nsG1b[0] |= 0x80;  // modern compression flag
    decodeProbe(e, "H.g1.nonsub.basic", nsG1b, false);
    decodeProbeUnchecked(e, "H.g1.nonsub.basic.unchk", nsG1b, false);
    //    G2: x = (1, 0) -> not in subgroup (legacy layout: c0 || c1).
    vector<uint8_t> nsG2(96, 0);
    nsG2[47] = 0x01;
    decodeProbe(e, "H.g2.nonsub.legacy", nsG2, true);
    decodeProbeUnchecked(e, "H.g2.nonsub.legacy.unchk", nsG2, true);
    //    modern layout: c1 || c0 with flags on byte 0
    vector<uint8_t> nsG2b(96, 0);
    nsG2b[95] = 0x01;
    nsG2b[0] |= 0x80;
    decodeProbe(e, "H.g2.nonsub.basic", nsG2b, false);
    decodeProbeUnchecked(e, "H.g2.nonsub.basic.unchk", nsG2b, false);

    // 8. x not on curve (no square root exists): x = 1 for G1
    vector<uint8_t> noc(48, 0);
    noc[47] = 0x01;
    decodeProbe(e, "H.g1.nocurve.legacy", noc, true);
    vector<uint8_t> nocb = noc;
    nocb[0] |= 0x80;
    decodeProbe(e, "H.g1.nocurve.basic", nocb, false);

    // 9. modern G2 with the reserved top bits of byte 48 set
    vector<uint8_t> flag48 = sig.Serialize(false);
    flag48[48] |= 0xe0;
    decodeProbe(e, "H.g2.flag48.basic", flag48, false);

    // 10. wrong length
    try {
        G1Element::FromBytes(Bytes(vector<uint8_t>(47, 0)), false);
        e.raw("H.len47=NOEXC");
    } catch (const std::exception&) { e.exc("H.len47"); }

    // 11. legacy sign bit set on degenerate x values (exercises the
    //     sqrt-failure and x>=p fallback paths with stored sign = 1)
    vector<uint8_t> s1(48, 0);
    s1[0] = 0x80;
    s1[47] = 0x01;  // x = 1, not on curve
    decodeProbe(e, "H.g1.nocurve.sign.legacy", s1, true);
    vector<uint8_t> sp(48, 0xff);
    sp[0] = 0x9f;  // sign bit + x = 0x1fff... >= p
    decodeProbe(e, "H.g1.overp.sign.legacy", sp, true);
    vector<uint8_t> g2s(96, 0);
    g2s[0] = 0x80;
    g2s[47] = 0x02;  // x = (2, 0), sign bit set
    decodeProbe(e, "H.g2.x2.sign.legacy", g2s, true);
    vector<uint8_t> g2s3(96, 0);
    g2s3[0] = 0x80;
    g2s3[47] = 0x03;  // x = (3, 0), sign bit set
    decodeProbe(e, "H.g2.x3.sign.legacy", g2s3, true);
    vector<uint8_t> g2nc(96, 0);
    g2nc[47] = 0x02;  // x = (2, 0), sign bit clear
    decodeProbe(e, "H.g2.x2.legacy", g2nc, true);
    vector<uint8_t> g2sp(96, 0xff);
    g2sp[0] = 0x9f;  // c0 >= p (sign set), c1 >= p
    decodeProbe(e, "H.g2.overp.sign.legacy", g2sp, true);
}

// ------------------------------------------------------------------ section I
// Verification semantics with infinity elements and empty inputs.
inline void VerifyEdges(Emitter& e)
{
    BasicSchemeMPL basic;
    LegacySchemeMPL legacy;
    vector<uint8_t> msg = {9, 9, 9};
    vector<uint8_t> h = hashN(900);

    e.dumpb("I.basic.verify.infinf", basic.Verify(G1Element(), msg, G2Element()));
    e.dumpb("I.legacy.verify.infinf", legacy.Verify(G1Element(), Bytes(h), G2Element()));

    PrivateKey sk = basic.KeyGen(seedN(900));
    G2Element sig = basic.Sign(sk, msg);
    e.dumpb("I.basic.verify.infpk", basic.Verify(G1Element(), msg, sig));
    e.dumpb("I.basic.verify.infsig", basic.Verify(sk.GetG1Element(), msg, G2Element()));

    // AggregateVerify argument invariants
    e.dumpb("I.aggver.empty.infsig",
          basic.AggregateVerify(vector<G1Element>{}, vector<Bytes>{}, G2Element()));
    e.dumpb("I.aggver.empty.realsig",
          basic.AggregateVerify(vector<G1Element>{}, vector<Bytes>{}, sig));
    e.dumpb("I.aggver.mismatch",
          basic.AggregateVerify(vector<G1Element>{sk.GetG1Element()}, vector<Bytes>{}, sig));

    // VerifySecure with an infinity pubkey in the list
    vector<G1Element> pks = {sk.GetG1Element(), G1Element()};
    e.dumpb("I.versec.withinf", basic.VerifySecure(pks, sig, Bytes(h)));
}


inline std::vector<std::string> GenerateReferenceVectors()
{
    Emitter e;
    e.raw("# dashbls reference vectors v1");
    KeysAndSignatures(e);
    SecureAggregation(e);
    ThresholdVectors(e);
    ExtendedKeys(e);
    PrivateKeyEdges(e);
    EdgeDecodes(e);
    VerifyEdges(e);
    e.raw("# end");
    return e.lines;
}

}  // namespace bls_test_vectors

#endif  // SRC_TEST_VECTORS_HPP_
