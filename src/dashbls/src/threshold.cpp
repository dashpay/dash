// Copyright (c) 2021 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <memory>

#include "threshold.hpp"

#include "schemes.hpp"

static std::unique_ptr<bls::CoreMPL> pThresholdScheme(new bls::LegacySchemeMPL);

namespace bls {

    namespace Poly {

        static const int nIdSize{32};

        template <typename BLSType>
        BLSType Evaluate(const std::vector<BLSType>& vecIn, const Bytes& id);

        template <typename BLSType>
        BLSType LagrangeInterpolate(const std::vector<BLSType>& vec, const std::vector<Bytes>& ids);
    } // end namespace Poly

    struct PolyOpsBase {
        static bool IsZero(const blst_scalar& a) {
            std::array<unsigned char, sizeof(blst_scalar)> zero{};
            return memcmp(&a, &zero, sizeof(blst_scalar)) == 0;
        }

        void MulFP(blst_scalar& r, const blst_scalar& a, const blst_scalar& b) {
            blst_sk_mul_n_check(&r, &a, &b);
        }

        void AddFP(blst_scalar& r, const blst_scalar& a, const blst_scalar& b) {
            blst_sk_add_n_check(&r, &a, &b);
        }

        void SubFP(blst_scalar& r, const blst_scalar& a, const blst_scalar& b) {
            blst_sk_sub_n_check(&r, &a, &b);
        }

        void DivFP(blst_scalar& r, const blst_scalar& a, const blst_scalar& b) {
            blst_scalar iv;
            blst_sk_inverse(&iv, &b);
            blst_sk_mul_n_check(&r, &a, &iv);
        }

        void ScalarFromBytes(blst_scalar& r, const uint8_t* bytes) {
            // reduces mod the group order, like bn_read_bin + bn_mod
            blst_scalar_from_be_bytes(&r, bytes, Poly::nIdSize);
        }
    };

    template<typename G>
    struct PolyOps;

    template<>
    struct PolyOps<PrivateKey> : PolyOpsBase {
        PrivateKey Add(const PrivateKey& a, const PrivateKey& b) {
            return PrivateKey::Aggregate({a, b});
        }

        PrivateKey Mul(const PrivateKey& a, const blst_scalar& b) {
            return a * b;
        }
    };

    template<>
    struct PolyOps<G1Element> : PolyOpsBase {
        G1Element Add(const G1Element& a, const G1Element& b) {
            return a + b;
        }

        G1Element Mul(const G1Element& a, const blst_scalar& b) {
            return a * b;
        }
    };

    template<>
    struct PolyOps<G2Element> : PolyOpsBase {
        G2Element Add(const G2Element& a, const G2Element& b) {
            return a + b;
        }

        G2Element Mul(const G2Element& a, const blst_scalar& b) {
            return a * b;
        }
    };

    template<typename BLSType>
    BLSType Poly::Evaluate(const std::vector<BLSType>& vecIn, const Bytes& id) {
        typedef PolyOps<BLSType> Ops;
        Ops ops;
        if (vecIn.size() < 2) {
            throw std::length_error("At least 2 coefficients required");
        }

        blst_scalar x;
        ops.ScalarFromBytes(x, id.begin());

        BLSType y = vecIn.back();
        for (int i = (int) vecIn.size() - 2; i >= 0; i--) {
            y = ops.Mul(y, x);
            y = ops.Add(y, vecIn[i]);
        }

        return y;
    }

    template<typename BLSType>
    BLSType Poly::LagrangeInterpolate(const std::vector<BLSType>& vec, const std::vector<Bytes>& ids) {
        typedef PolyOps<BLSType> Ops;
        Ops ops;

        if (vec.size() < 2) {
            throw std::length_error("At least 2 shares required");
        }
        if (vec.size() != ids.size()) {
            throw std::length_error("Numbers of shares and ids must be equal");
        }

        /*
            delta_{i,S}(0) = prod_{j != i} S[j] / (S[j] - S[i]) = a / b
            where a = prod S[j], b = S[i] * prod_{j != i} (S[j] - S[i])
        */
        const size_t k = vec.size();

        std::vector<blst_scalar> delta(k);
        std::vector<blst_scalar> ids2(k);

        for (size_t i = 0; i < k; i++) {
            ops.ScalarFromBytes(ids2[i], ids[i].begin());
        }

        blst_scalar a, b, v;

        a = ids2[0];
        for (size_t i = 1; i < k; i++) {
            ops.MulFP(a, a, ids2[i]);
        }
        if (PolyOpsBase::IsZero(a)) {
            throw std::invalid_argument("Zero id");
        }
        for (size_t i = 0; i < k; i++) {
            b = ids2[i];
            for (size_t j = 0; j < k; j++) {
                if (j != i) {
                    ops.SubFP(v, ids2[j], ids2[i]);
                    if (PolyOpsBase::IsZero(v)) {
                        throw std::invalid_argument("Duplicate id");
                    }
                    ops.MulFP(b, b, v);
                }
            }
            ops.DivFP(delta[i], a, b);
        }

        /*
            f(0) = sum_i f(S[i]) delta_{i,S}(0)
        */
        BLSType r;
        for (size_t i = 0; i < k; i++) {
            r = ops.Add(r, ops.Mul(vec[i], delta[i]));
        }

        return r;
    }

    PrivateKey Threshold::PrivateKeyShare(const std::vector<PrivateKey>& sks, const Bytes& id) {
        return Poly::Evaluate(sks, id);
    }

    PrivateKey Threshold::PrivateKeyRecover(const std::vector<PrivateKey>& sks, const std::vector<Bytes>& ids) {
        return Poly::LagrangeInterpolate(sks, ids);
    }

    G1Element Threshold::PublicKeyShare(const std::vector<G1Element>& pks, const Bytes& id) {
        return Poly::Evaluate(pks, id);
    }

    G1Element Threshold::PublicKeyRecover(const std::vector<G1Element>& sks, const std::vector<Bytes>& ids) {
        return Poly::LagrangeInterpolate(sks, ids);
    }

    G2Element Threshold::SignatureShare(const std::vector<G2Element>& sigs, const Bytes& id) {
        return Poly::Evaluate(sigs, id);
    }

    G2Element Threshold::SignatureRecover(const std::vector<G2Element>& sigs, const std::vector<Bytes>& ids) {
        return Poly::LagrangeInterpolate(sigs, ids);
    }

    G2Element Threshold::Sign(const PrivateKey& privateKey, const Bytes& vecMessage) {
        return pThresholdScheme->Sign(privateKey, vecMessage);
    }

    bool Threshold::Verify(const G1Element& pubKey, const Bytes& vecMessage, const G2Element& signature) {
        return pThresholdScheme->Verify(pubKey, vecMessage, signature);
    }
}
