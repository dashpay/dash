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

#ifndef SRC_BLSHDKEYS_HPP_
#define SRC_BLSHDKEYS_HPP_

#include <math.h>

#include "hkdf.hpp"
#include "privatekey.hpp"
#include "util.hpp"

namespace bls {

class HDKeys {
    /**
     * Implements HD keys as specified in EIP2333.
     **/
public:
    static const uint8_t HASH_LEN = 32;

    static PrivateKey KeyGen(const std::vector<uint8_t>& seed);

    static PrivateKey KeyGen(const Bytes& seed);

    static void IKMToLamportSk(
        uint8_t* outputLamportSk,
        const uint8_t* ikm,
        size_t ikmLen,
        const uint8_t* salt,
        size_t saltLen);

    static void ParentSkToLamportPK(
        uint8_t* outputLamportPk,
        const PrivateKey& parentSk,
        uint32_t index);

    static PrivateKey DeriveChildSk(const PrivateKey& parentSk, uint32_t index);

    static PrivateKey DeriveChildSkUnhardened(
        const PrivateKey& parentSk,
        uint32_t index);

    static G1Element DeriveChildG1Unhardened(
        const G1Element& pk,
        uint32_t index);

    static G2Element DeriveChildG2Unhardened(
        const G2Element& pk,
        uint32_t index);
};
}  // end namespace bls
#endif  // SRC_BLSHDKEYS_HPP_
