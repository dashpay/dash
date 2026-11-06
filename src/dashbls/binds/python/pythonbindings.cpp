// Copyright 2020 Chia Network Inc

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//    http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <array>
#include <mutex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <dashbls/bls.hpp>
#include <dashbls/elements.hpp>
#include <dashbls/hdkeys.hpp>
#include <dashbls/privatekey.hpp>
#include <dashbls/schemes.hpp>

namespace py = pybind11;
using namespace bls;

namespace {
inline int PyLong_AsByteArray(PyLongObject* obj, uint8_t* buf, Py_ssize_t size, bool is_le, bool is_signed)
{
    return _PyLong_AsByteArray(obj, buf, size, is_le, is_signed
#if PY_VERSION_HEX >= 0x030d0000
                               , /*with_exceptions=*/true
#endif // PY_VERSION_HEX >= 0x030d0000
    );
}

// relic's context is process-wide here (MULTI is unset, see setup.py), so a
// released GIL leaves threads racing on its error code and PRNG state. The
// GIL goes first so a waiter cannot strand the holder that must retake it.
std::mutex &RelicMutex()
{
    static std::mutex mutex;
    return mutex;
}

struct RelicGuard {
    RelicGuard() : lock(RelicMutex()) {}

    py::gil_scoped_release release;
    std::lock_guard<std::mutex> lock;
};

// Measure before copying, so an oversized input fails as ValueError instead of
// bad_alloc, and measure the header rather than py::len, because a subclass may
// override __len__ while the copy below still takes the backing payload.
std::string CopyChecked(const py::bytes &b, size_t limit, const char *who, const char *what)
{
    const auto signed_size = PyBytes_Size(b.ptr());
    if (signed_size < 0) {
        throw py::error_already_set();
    }
    const auto size = static_cast<size_t>(signed_size);
    if (size > limit) {
        throw std::invalid_argument(
            std::string(who) + ": " + what + " must be at most " + std::to_string(limit) +
            " bytes, got " + std::to_string(size));
    }
    return std::string(b);
}

// md_xmd caps a tag at 255 bytes but compares signed, so a tag at or beyond
// 2 GiB truncates negative, slips the guard and is widened back to a huge
// length.
std::string CopyDst(const py::bytes &dst, const char *who)
{
    return CopyChecked(dst, 255, who, "domain separation tag");
}

// ep_map_dst takes the message length as an int and md_xmd checks only the
// output and tag lengths, so nothing on relic's side catches a message at or
// beyond 2 GiB narrowing negative on the way in.
std::string CopyMsg(const py::bytes &msg, const char *who)
{
    return CopyChecked(msg, std::numeric_limits<int>::max(), who, "message");
}

// The augmented schemes prepend a serialized G1Element and hand the result to
// the same int-typed length, so the message has to leave room for the prefix.
std::string CopyAugMsg(const py::bytes &msg, const char *who)
{
    return CopyChecked(
        msg, std::numeric_limits<int>::max() - G1Element::SIZE, who, "message");
}

// relic is built ALLOC=AUTO, so md_hmac stages the seed on the stack and a few
// MiB of it takes the process down. Nothing near this cap is a legitimate IKM
// (the spec floor is 32 bytes), so keep it far below any thread's stack.
std::string CopySeed(const py::bytes &seed, const char *who)
{
    return CopyChecked(seed, 64 * 1024, who, "seed");
}

std::vector<uint8_t> ToVec(const std::string &s)
{
    return std::vector<uint8_t>(s.begin(), s.end());
}

// Bytes is a pointer and a length, so a strided view has no counterpart to
// mirror; ask CPython for a contiguous one and let it raise, rather than
// reinterpreting info.ptr as contiguous and reading past the caller's view.
template <size_t N>
std::array<uint8_t, N> CopyBuffer(const py::buffer &b, const char *what)
{
    auto *view = new Py_buffer();
    if (PyObject_GetBuffer(b.ptr(), view, PyBUF_C_CONTIGUOUS | PyBUF_FORMAT) != 0) {
        delete view;
        throw py::error_already_set();
    }
    py::buffer_info info(view);

    if (info.format != py::format_descriptor<uint8_t>::format() || info.ndim != 1)
        throw std::runtime_error("Incompatible buffer format!");

    if (info.size != static_cast<py::ssize_t>(N)) {
        throw std::invalid_argument(
            std::string("Length of bytes object not equal to ") + what);
    }

    std::array<uint8_t, N> data;
    const auto *data_ptr = reinterpret_cast<const uint8_t *>(info.ptr);
    std::copy(data_ptr, data_ptr + N, data.data());
    return data;
}
} // anonymous namespace

PYBIND11_MODULE(dashbls, m)
{
    py::class_<PrivateKey>(m, "PrivateKey")
        .def_property_readonly_static(
            "PRIVATE_KEY_SIZE",
            [](py::object self) { return PrivateKey::PRIVATE_KEY_SIZE; })
        .def(
            "from_bytes",
            [](py::buffer const b) {
                auto data = CopyBuffer<PrivateKey::PRIVATE_KEY_SIZE>(b, "PrivateKey::SIZE");
                RelicGuard guard;
                return PrivateKey::FromBytes(data);
            })
        .def(
            "__bytes__",
            [](const PrivateKey &k) {
                uint8_t *output =
                    Util::SecAlloc<uint8_t>(PrivateKey::PRIVATE_KEY_SIZE);
                {
                    RelicGuard guard;
                    k.Serialize(output);
                }
                py::bytes ret = py::bytes(
                    reinterpret_cast<char *>(output),
                    PrivateKey::PRIVATE_KEY_SIZE);
                Util::SecFree(output);
                return ret;
            })
        .def(
            "__deepcopy__",
            [](const PrivateKey &k, const py::object &memo) {
                RelicGuard guard;
                return PrivateKey(k);
            })
        .def("get_g1", [](const PrivateKey &k) {
                RelicGuard guard;
                return k.GetG1Element();
            })
        .def("aggregate", &PrivateKey::Aggregate, py::call_guard<RelicGuard>())
        .def(py::self == py::self, py::call_guard<RelicGuard>())
        .def(py::self != py::self, py::call_guard<RelicGuard>())
        .def("__repr__", [](const PrivateKey &k) {
            RelicGuard guard;
            uint8_t *output = Util::SecAlloc<uint8_t>(PrivateKey::PRIVATE_KEY_SIZE);
            k.Serialize(output);
            std::string ret =
                "<PrivateKey " +
                Util::HexStr(output, PrivateKey::PRIVATE_KEY_SIZE) + ">";
            Util::SecFree(output);
            return ret;
        });

    py::class_<Util>(m, "Util").def("hash256", [](const py::bytes &message) {
        // Hash256 takes a size_t but md_map_sh256 narrows it to an int
        const auto str = CopyMsg(message, "Util.hash256");
        uint8_t output[BLS::MESSAGE_HASH_LEN];
        {
            RelicGuard guard;
            Util::Hash256(output, (const uint8_t *)str.data(), str.size());
        }
        return py::bytes(
            reinterpret_cast<char *>(output), BLS::MESSAGE_HASH_LEN);
    });

    py::class_<BasicSchemeMPL>(m, "BasicSchemeMPL")
        .def("sk_to_g1", [](const PrivateKey &seckey){
            RelicGuard guard;
            return BasicSchemeMPL().SkToG1(seckey);
        })
        .def(
            "key_gen",
            [](const py::bytes &b) {
                const auto str = CopySeed(b, "BasicSchemeMPL.key_gen");
                RelicGuard guard;
                return BasicSchemeMPL().KeyGen(ToVec(str));
            })
        .def("derive_child_sk", [](const PrivateKey& sk, uint32_t index){
            RelicGuard guard;
            return BasicSchemeMPL().DeriveChildSk(sk, index);
        })
        .def("derive_child_sk_unhardened", [](const PrivateKey& sk, uint32_t index){
            RelicGuard guard;
            return BasicSchemeMPL().DeriveChildSkUnhardened(sk, index);
        })
        .def("derive_child_pk_unhardened", [](const G1Element& pk, uint32_t index){
            RelicGuard guard;
            return BasicSchemeMPL().DeriveChildPkUnhardened(pk, index);
        })
        .def("aggregate", [](const vector<G2Element> &signatures) {
            RelicGuard guard;
            return BasicSchemeMPL().Aggregate(signatures);
        })
        .def(
            "sign",
            [](const PrivateKey &pk, const py::bytes &msg) {
                const auto s = CopyMsg(msg, "BasicSchemeMPL.sign");
                RelicGuard guard;
                return BasicSchemeMPL().Sign(pk, ToVec(s));
            })
        .def(
            "verify",
            [](const G1Element &pk,
               const py::bytes &msg,
               const G2Element &sig) {
                const auto s = CopyMsg(msg, "BasicSchemeMPL.verify");
                RelicGuard guard;
                return BasicSchemeMPL().Verify(pk, ToVec(s), sig);
            })
        .def(
            "aggregate_verify",
            [](const vector<G1Element> &pks,
               const vector<py::bytes> &msgs,
               const G2Element &sig) {
                vector<vector<uint8_t>> vecs(msgs.size());
                for (size_t i = 0; i < msgs.size(); ++i) {
                    vecs[i] = ToVec(
                        CopyMsg(msgs[i], "BasicSchemeMPL.aggregate_verify"));
                }
                RelicGuard guard;
                return BasicSchemeMPL().AggregateVerify(pks, vecs, sig);
            })
        .def(
            "g2_from_message",
            [](const py::bytes &msg) {
                const auto msg_str = CopyMsg(msg, "BasicSchemeMPL.g2_from_message");
                RelicGuard guard;
                const auto msg_bytes = Bytes((const uint8_t *)msg_str.c_str(), msg_str.size());
                return G2Element::FromMessage(
                    msg_bytes,
                    (const uint8_t *)BasicSchemeMPL::CIPHERSUITE_ID.c_str(),
                    BasicSchemeMPL::CIPHERSUITE_ID.size()
                );
            });

    py::class_<AugSchemeMPL>(m, "AugSchemeMPL")
        .def("sk_to_g1", [](const PrivateKey &seckey){
            RelicGuard guard;
            return AugSchemeMPL().SkToG1(seckey);
        })
        .def(
            "key_gen",
            [](const py::bytes &b) {
                const auto str = CopySeed(b, "AugSchemeMPL.key_gen");
                RelicGuard guard;
                return AugSchemeMPL().KeyGen(ToVec(str));
            })
        .def("derive_child_sk", [](const PrivateKey& sk, uint32_t index){
            RelicGuard guard;
            return AugSchemeMPL().DeriveChildSk(sk, index);
        })
        .def("derive_child_sk_unhardened", [](const PrivateKey& sk, uint32_t index){
            RelicGuard guard;
            return AugSchemeMPL().DeriveChildSkUnhardened(sk, index);
        })
        .def("derive_child_pk_unhardened", [](const G1Element& pk, uint32_t index){
            RelicGuard guard;
            return AugSchemeMPL().DeriveChildPkUnhardened(pk, index);
        })
        .def("aggregate", [](const vector<G2Element>& signatures) {
            RelicGuard guard;
            return AugSchemeMPL().Aggregate(signatures);
        })
        .def(
            "sign",
            [](const PrivateKey &pk, const py::bytes &msg) {
                const auto s = CopyAugMsg(msg, "AugSchemeMPL.sign");
                RelicGuard guard;
                return AugSchemeMPL().Sign(pk, ToVec(s));
            })
        .def(
            "sign",
            [](const PrivateKey &pk,
               const py::bytes &msg,
               const G1Element &prepend_pk) {
                const auto s = CopyAugMsg(msg, "AugSchemeMPL.sign");
                RelicGuard guard;
                return AugSchemeMPL().Sign(pk, ToVec(s), prepend_pk);
            })
        .def(
            "verify",
            [](const G1Element &pk,
               const py::bytes &msg,
               const G2Element &sig) {
                const auto s = CopyAugMsg(msg, "AugSchemeMPL.verify");
                RelicGuard guard;
                return AugSchemeMPL().Verify(pk, ToVec(s), sig);
            })
        .def(
            "aggregate_verify",
            [](const vector<G1Element> &pks,
               const vector<py::bytes> &msgs,
               const G2Element &sig) {
                vector<vector<uint8_t>> vecs(msgs.size());
                for (size_t i = 0; i < msgs.size(); ++i) {
                    vecs[i] = ToVec(
                        CopyAugMsg(msgs[i], "AugSchemeMPL.aggregate_verify"));
                }
                RelicGuard guard;
                return AugSchemeMPL().AggregateVerify(pks, vecs, sig);
            })
        .def(
            "g2_from_message",
            [](const py::bytes &msg) {
                const auto msg_str = CopyMsg(msg, "AugSchemeMPL.g2_from_message");
                RelicGuard guard;
                const auto msg_bytes = Bytes((const uint8_t *)msg_str.c_str(), msg_str.size());
                return G2Element::FromMessage(
                    msg_bytes,
                    (const uint8_t *)AugSchemeMPL::CIPHERSUITE_ID.c_str(),
                    AugSchemeMPL::CIPHERSUITE_ID.size()
                );
            });

    py::class_<PopSchemeMPL>(m, "PopSchemeMPL")
        .def("sk_to_g1", [](const PrivateKey &seckey){
            RelicGuard guard;
            return PopSchemeMPL().SkToG1(seckey);
        })
        .def(
            "key_gen",
            [](const py::bytes &b) {
                const auto str = CopySeed(b, "PopSchemeMPL.key_gen");
                RelicGuard guard;
                return PopSchemeMPL().KeyGen(ToVec(str));
            })
        .def("derive_child_sk", [](const PrivateKey& sk, uint32_t index){
            RelicGuard guard;
            return PopSchemeMPL().DeriveChildSk(sk, index);
        })
        .def("derive_child_sk_unhardened", [](const PrivateKey& sk, uint32_t index){
            RelicGuard guard;
            return PopSchemeMPL().DeriveChildSkUnhardened(sk, index);
        })
        .def("derive_child_pk_unhardened", [](const G1Element& pk, uint32_t index){
            RelicGuard guard;
            return PopSchemeMPL().DeriveChildPkUnhardened(pk, index);
        })
        .def("aggregate", [](const vector<G2Element>& signatures) {
            RelicGuard guard;
            return PopSchemeMPL().Aggregate(signatures);
        })
        .def(
            "sign",
            [](const PrivateKey &pk, const py::bytes &msg) {
                const auto s = CopyMsg(msg, "PopSchemeMPL.sign");
                RelicGuard guard;
                return PopSchemeMPL().Sign(pk, ToVec(s));
            })
        .def(
            "verify",
            [](const G1Element &pk,
               const py::bytes &msg,
               const G2Element &sig) {
                const auto s = CopyMsg(msg, "PopSchemeMPL.verify");
                RelicGuard guard;
                return PopSchemeMPL().Verify(pk, ToVec(s), sig);
            })
        .def(
            "aggregate_verify",
            [](const vector<G1Element> &pks,
               const vector<py::bytes> &msgs,
               const G2Element &sig) {
                vector<vector<uint8_t>> vecs(msgs.size());
                for (size_t i = 0; i < msgs.size(); ++i) {
                    vecs[i] =
                        ToVec(CopyMsg(msgs[i], "PopSchemeMPL.aggregate_verify"));
                }
                RelicGuard guard;
                return PopSchemeMPL().AggregateVerify(pks, vecs, sig);
            })
        .def(
            "g2_from_message",
            [](const py::bytes &msg) {
                const auto msg_str = CopyMsg(msg, "PopSchemeMPL.g2_from_message");
                RelicGuard guard;
                const auto msg_bytes = Bytes((const uint8_t *)msg_str.c_str(), msg_str.size());
                return G2Element::FromMessage(
                    msg_bytes,
                    (const uint8_t *)PopSchemeMPL::CIPHERSUITE_ID.c_str(),
                    PopSchemeMPL::CIPHERSUITE_ID.size()
                );
            })
        .def("pop_prove", [](const PrivateKey& privateKey){
            RelicGuard guard;
            return PopSchemeMPL().PopProve(privateKey);
        })
        .def("pop_verify", [](const G1Element& pubkey, const G2Element& signature){
            RelicGuard guard;
            return PopSchemeMPL().PopVerify(pubkey, signature);
        })
        .def(
            "fast_aggregate_verify",
            [](const vector<G1Element> &pks,
               const py::bytes &msg,
               const G2Element &sig) {
                const auto s = CopyMsg(msg, "PopSchemeMPL.fast_aggregate_verify");
                RelicGuard guard;
                return PopSchemeMPL().FastAggregateVerify(pks, ToVec(s), sig);
            });

    py::class_<G1Element>(m, "G1Element")
        .def_property_readonly_static(
            "SIZE", [](py::object self) { return G1Element::SIZE; })
        .def(py::init([](){
            RelicGuard guard;
            return G1Element();
        }))
        .def(py::init(&G1Element::FromByteVector), py::call_guard<RelicGuard>())
        .def(py::init([](py::int_ pyint) {
            std::array<uint8_t, G1Element::SIZE> buffer{};
            if (PyLong_AsByteArray(
                    (PyLongObject *)pyint.ptr(),
                    buffer.data(),
                    buffer.size(),
                    0,
                    0) < 0) {
                throw std::invalid_argument("Failed to cast int to G1Element");
            }
            RelicGuard guard;
            return G1Element::FromBytes(buffer);
        }))
        .def(py::init([](py::buffer const b) {
            auto data = CopyBuffer<G1Element::SIZE>(b, "G1Element::SIZE");
            RelicGuard guard;
            return G1Element::FromBytes(data);
        }))
        .def(
            "from_bytes",
            [](py::buffer const b) {
                auto data = CopyBuffer<G1Element::SIZE>(b, "G1Element::SIZE");
                RelicGuard guard;
                return G1Element::FromBytes(data);
            })
        .def(
            "from_bytes_unchecked",
            [](py::buffer const b) {
              auto data = CopyBuffer<G1Element::SIZE>(b, "G1Element::SIZE");
              RelicGuard guard;
              return G1Element::FromBytesUnchecked(data);
            })
        .def("generator", &G1Element::Generator, py::call_guard<RelicGuard>())
        .def_static(
            "from_message",
            [](const py::bytes &msg, const py::bytes &dst) {
                const auto msg_str = CopyMsg(msg, "G1Element.from_message");
                const auto dst_str = CopyDst(dst, "G1Element.from_message");
                RelicGuard guard;
                return G1Element::FromMessage(
                    Bytes((const uint8_t *)msg_str.c_str(), msg_str.size()),
                    (const uint8_t *)dst_str.c_str(),
                    (int)dst_str.size());
            },
            py::arg("msg"),
            py::arg("dst"))
        .def("pair", &G1Element::Pair, py::call_guard<RelicGuard>())
        .def("negate", &G1Element::Negate, py::call_guard<RelicGuard>())
        .def("get_fingerprint", &G1Element::GetFingerprint, py::call_guard<RelicGuard>())

        .def(py::self == py::self, py::call_guard<RelicGuard>())
        .def(py::self != py::self, py::call_guard<RelicGuard>())
        .def(
            "__deepcopy__",
            [](const G1Element &g1, const py::object &memo) {
                RelicGuard guard;
                return G1Element(g1);
            })
        .def(
            "__add__",
            [](G1Element &self, G1Element &other) {
                RelicGuard guard;
                return self + other;
            },
            py::is_operator())
        .def(
            "__mul__",
            [](G1Element &self, const PrivateKey &other) {
                RelicGuard guard;
                return self * other;
            },
            py::is_operator())
        .def(
            "__rmul__",
            [](G1Element &self, const PrivateKey &other) {
                RelicGuard guard;
                return other * self;
            },
            py::is_operator())
        .def(
            "__and__",
            [](G1Element &self, G2Element &other) {
                RelicGuard guard;
                return self & other;
            },
            py::is_operator())
        .def(
            "__repr__",
            [](const G1Element &ele) {
                RelicGuard guard;
                std::stringstream s;
                s << ele;
                return "<G1Element " + s.str() + ">";
            })
        .def(
            "__str__",
            [](const G1Element &ele) {
                RelicGuard guard;
                std::stringstream s;
                s << ele;
                return s.str();
            })
        .def(
            "__bytes__",
            [](const G1Element &ele) {
                vector<uint8_t> out;
                {
                    RelicGuard guard;
                    out = ele.Serialize();
                }
                py::bytes ans = py::bytes(
                    reinterpret_cast<const char *>(out.data()), G1Element::SIZE);
                return ans;
            })
        .def("__deepcopy__", [](const G1Element &ele, const py::object &memo) {
            RelicGuard guard;
            return G1Element(ele);
        });

    py::class_<G2Element>(m, "G2Element")
        .def_property_readonly_static(
            "SIZE", [](py::object self) { return G2Element::SIZE; })
        .def(py::init([](){
            RelicGuard guard;
            return G2Element();
        }))
        .def(py::init(&G2Element::FromByteVector), py::call_guard<RelicGuard>())
        .def(py::init([](py::buffer const b) {
            auto data = CopyBuffer<G2Element::SIZE>(b, "G2Element::SIZE");
            RelicGuard guard;
            return G2Element::FromBytes(data);
        }))
        .def(py::init([](py::int_ pyint) {
            std::array<uint8_t, G2Element::SIZE> buffer{};
            if (PyLong_AsByteArray(
                    (PyLongObject *)pyint.ptr(),
                    buffer.data(),
                    buffer.size(),
                    0,
                    0) < 0) {
                throw std::invalid_argument("Failed to cast int to G2Element");
            }
            RelicGuard guard;
            return G2Element::FromBytes(buffer);
        }))
        .def(
            "from_bytes",
            [](py::buffer const b) {
                auto data = CopyBuffer<G2Element::SIZE>(b, "G2Element::SIZE");
                RelicGuard guard;
                return G2Element::FromBytes(data);
            })
        .def(
            "from_bytes_unchecked",
            [](py::buffer const b) {
              auto data = CopyBuffer<G2Element::SIZE>(b, "G2Element::SIZE");
              RelicGuard guard;
              return G2Element::FromBytesUnchecked(data);
            })
        .def("generator", &G2Element::Generator, py::call_guard<RelicGuard>())
        .def_static(
            "from_message",
            [](const py::bytes &msg, const py::bytes &dst) {
                const auto msg_str = CopyMsg(msg, "G2Element.from_message");
                const auto dst_str = CopyDst(dst, "G2Element.from_message");
                RelicGuard guard;
                return G2Element::FromMessage(
                    Bytes((const uint8_t *)msg_str.c_str(), msg_str.size()),
                    (const uint8_t *)dst_str.c_str(),
                    (int)dst_str.size());
            },
            py::arg("msg"),
            py::arg("dst"))
        .def("pair", &G2Element::Pair, py::call_guard<RelicGuard>())
        .def("negate", &G2Element::Negate, py::call_guard<RelicGuard>())
        .def(
            "__deepcopy__",
            [](const G2Element &g2, const py::object &memo) {
                RelicGuard guard;
                return G2Element(g2);
            })
        .def(py::self == py::self, py::call_guard<RelicGuard>())
        .def(py::self != py::self, py::call_guard<RelicGuard>())

        .def(
            "__add__",
            [](G2Element &self, G2Element &other) {
                RelicGuard guard;
                return self + other;
            },
            py::is_operator())
        .def(
            "__mul__",
            [](G2Element &self, const PrivateKey &other) {
                RelicGuard guard;
                return self * other;
            },
            py::is_operator())
        .def(
            "__rmul__",
            [](G2Element &self, const PrivateKey &other) {
                RelicGuard guard;
                return other * self;
            },
            py::is_operator())
        .def(
            "__repr__",
            [](const G2Element &ele) {
                RelicGuard guard;
                std::stringstream s;
                s << ele;
                return "<G2Element " + s.str() + ">";
            })
        .def(
            "__str__",
            [](const G2Element &ele) {
                RelicGuard guard;
                std::stringstream s;
                s << ele;
                return s.str();
            })
        .def(
            "__bytes__",
            [](const G2Element &ele) {
                vector<uint8_t> out;
                {
                    RelicGuard guard;
                    out = ele.Serialize();
                }
                py::bytes ans = py::bytes(
                    reinterpret_cast<const char *>(out.data()), G2Element::SIZE);
                return ans;
            })
        .def("__deepcopy__", [](const G2Element &ele, const py::object &memo) {
            RelicGuard guard;
            return G2Element(ele);
        });

    py::class_<GTElement>(m, "GTElement")
        .def_property_readonly_static(
            "SIZE", [](py::object self) { return GTElement::SIZE; })
        .def(py::init(&GTElement::FromByteVector), py::call_guard<RelicGuard>())
        .def(py::init([](py::buffer const b) {
            auto data = CopyBuffer<GTElement::SIZE>(b, "GTElement::SIZE");
            RelicGuard guard;
            return GTElement::FromBytes(data);
        }))
        .def(py::init([](py::int_ pyint) {
            std::array<uint8_t, GTElement::SIZE> buffer{};
            if (PyLong_AsByteArray(
                    (PyLongObject *)pyint.ptr(),
                    buffer.data(),
                    buffer.size(),
                    0,
                    0) < 0) {
                throw std::invalid_argument("Failed to cast int to GTElement");
            }
            RelicGuard guard;
            return GTElement::FromBytes(buffer);
        }))
        .def(
            "from_bytes",
            [](py::buffer const b) {
                auto data = CopyBuffer<GTElement::SIZE>(b, "GTElement::SIZE");
                RelicGuard guard;
                return GTElement::FromBytes(data);
            })
        .def(
            "from_bytes_unchecked",
            [](py::buffer const b) {
                auto data = CopyBuffer<GTElement::SIZE>(b, "GTElement::SIZE");
                RelicGuard guard;
                return GTElement::FromBytesUnchecked(data);
            })
        .def("unity", &GTElement::Unity, py::call_guard<RelicGuard>())
        .def(py::self == py::self, py::call_guard<RelicGuard>())
        .def(py::self != py::self, py::call_guard<RelicGuard>())
        .def(
            "__deepcopy__",
            [](const GTElement &gt, const py::object &memo) {
                RelicGuard guard;
                return GTElement(gt);
            })
        .def(
            "__repr__",
            [](const GTElement &ele) {
                RelicGuard guard;
                std::stringstream s;
                s << ele;
                return "<GTElement " + s.str() + ">";
            })
        .def(
            "__str__",
            [](const GTElement &ele) {
                RelicGuard guard;
                std::stringstream s;
                s << ele;
                return s.str();
            })
        .def(
            "__bytes__",
            [](const GTElement &ele) {
                uint8_t *out = new uint8_t[GTElement::SIZE];
                {
                    RelicGuard guard;
                    ele.Serialize(out);
                }
                py::bytes ans =
                    py::bytes(reinterpret_cast<char *>(out), GTElement::SIZE);
                delete[] out;
                return ans;
            })
        .def(
            "__mul__",
            [](GTElement &self, GTElement &other) {
                RelicGuard guard;
                return self * other;
            },
            py::is_operator())
        .def("__deepcopy__", [](const GTElement &ele, const py::object &memo) {
            RelicGuard guard;
            return GTElement(ele);
        });

    m.attr("PublicKeyMPL") = m.attr("G1Element");
    m.attr("SignatureMPL") = m.attr("G2Element");

#ifdef VERSION_INFO
    m.attr("__version__") = VERSION_INFO;
#else
    m.attr("__version__") = "dev";
#endif
}
