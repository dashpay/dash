// Differential fuzz driver for the relic -> blst migration.
//
// Uses only the public dashbls API; compile the same source against the old
// and new libraries, run with the same arguments, and diff the output.
//
//   fuzzgen <seed> <iterations>
//
// Covers: random byte-string decoding in every mode (the dominant
// consensus-facing attack surface), decode/re-encode round trips, and
// legacy + basic sign/verify/aggregate cycles on derived keys.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dashbls/bls.hpp>

using namespace bls;
using std::vector;

// xorshift128+ so both builds see identical byte streams with no libc
// dependency
static uint64_t s0, s1;
static uint64_t rng()
{
    uint64_t x = s0;
    const uint64_t y = s1;
    s0 = y;
    x ^= x << 23;
    s1 = x ^ y ^ (x >> 17) ^ (y >> 26);
    return s1 + y;
}

static void fill(vector<uint8_t>& v)
{
    for (size_t i = 0; i < v.size(); i += 8) {
        const uint64_t r = rng();
        for (size_t j = 0; j < 8 && i + j < v.size(); j++)
            v[i + j] = (uint8_t)(r >> (8 * j));
    }
}

static void dump(const vector<uint8_t>& v)
{
    for (auto b : v) printf("%02x", b);
}

static void probeG1(const vector<uint8_t>& in, bool fLegacy)
{
    try {
        G1Element e = G1Element::FromBytes(Bytes(in), fLegacy);
        dump(e.Serialize(fLegacy));
        printf(" v%d", (int)e.IsValid());
    } catch (const std::exception&) {
        printf("EXC");
    }
    printf(";");
}

static void probeG2(const vector<uint8_t>& in, bool fLegacy)
{
    try {
        G2Element e = G2Element::FromBytes(Bytes(in), fLegacy);
        dump(e.Serialize(fLegacy));
        printf(" v%d", (int)e.IsValid());
    } catch (const std::exception&) {
        printf("EXC");
    }
    printf(";");
}

int main(int argc, char** argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: fuzzgen <seed> <iterations>\n");
        return 2;
    }
    s0 = strtoull(argv[1], nullptr, 0) * 0x9E3779B97F4A7C15ULL + 1;
    s1 = s0 ^ 0xD1B54A32D192ED03ULL;
    const long iters = strtol(argv[2], nullptr, 0);

    LegacySchemeMPL legacy;
    BasicSchemeMPL basic;

    for (long i = 0; i < iters; i++) {
        printf("i%ld: ", i);

        // 1. random decodes: fully random, and "plausible" (top bits biased
        //    into the interesting flag space)
        vector<uint8_t> b48(48), b96(96);
        fill(b48);
        fill(b96);
        switch (i % 4) {
            case 0: break;
            case 1: b48[0] &= 0x1f; b96[0] &= 0x1f; break;   // no flags
            case 2: b48[0] |= 0x80; b96[0] |= 0x80; break;   // sign/compress
            case 3: b48[0] = (uint8_t)(rng() & 0xe0); b96[0] = b48[0]; break;
        }
        probeG1(b48, false);
        probeG1(b48, true);
        probeG2(b96, false);
        probeG2(b96, true);

        // 2. valid-point cross-mode round trips
        vector<uint8_t> seed(32);
        fill(seed);
        PrivateKey sk = basic.KeyGen(seed);
        G1Element pk = sk.GetG1Element();
        probeG1(pk.Serialize(true), true);
        probeG1(pk.Serialize(false), true);   // modern bytes into legacy codec

        // 3. legacy + basic signing over a random 32-byte hash
        vector<uint8_t> h(32);
        fill(h);
        G2Element lsig = legacy.Sign(sk, Bytes(h));
        dump(lsig.Serialize(true));
        printf(" %d", (int)legacy.Verify(pk, Bytes(h), lsig));
        printf(";");
        G2Element bsig = basic.Sign(sk, Bytes(h));
        dump(bsig.Serialize(false));
        printf(" %d", (int)basic.Verify(pk, Bytes(h), bsig));
        printf(";");

        // 4. secure aggregation with 3 keys
        vector<PrivateKey> sks;
        vector<G1Element> pks;
        vector<G2Element> lsigs;
        for (int k = 0; k < 3; k++) {
            vector<uint8_t> s2(32);
            fill(s2);
            sks.push_back(basic.KeyGen(s2));
            pks.push_back(sks.back().GetG1Element());
            lsigs.push_back(legacy.Sign(sks.back(), Bytes(h)));
        }
        G2Element agg = legacy.AggregateSecure(pks, lsigs, Bytes(h));
        dump(agg.Serialize(true));
        printf(" %d", (int)legacy.VerifySecure(pks, agg, Bytes(h)));
        printf(";");

        // 5. threshold 2-of-3 recovery
        vector<Bytes> ids;
        vector<vector<uint8_t>> idStore(3, vector<uint8_t>(32));
        for (auto& id : idStore) fill(id);
        PrivateKey sh0 = Threshold::PrivateKeyShare({sks[0], sks[1]}, Bytes(idStore[0]));
        PrivateKey sh1 = Threshold::PrivateKeyShare({sks[0], sks[1]}, Bytes(idStore[1]));
        try {
            PrivateKey rec = Threshold::PrivateKeyRecover(
                {sh0, sh1}, {Bytes(idStore[0]), Bytes(idStore[1])});
            dump(rec.Serialize());
        } catch (const std::exception&) {
            printf("EXC");
        }
        printf("\n");
    }
    return 0;
}
