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

#ifndef SRC_BLSHKDF_HPP_
#define SRC_BLSHKDF_HPP_

#include <math.h>
#include <cassert>
#include "util.hpp"

namespace bls {

class HKDF256 {
    /**
     * Implements HKDF as specified in RFC5869: https://tools.ietf.org/html/rfc5869,
     * with sha256 as the hash function.
     **/
 public:
    static const uint8_t HASH_LEN = 32;

    static void Extract(uint8_t* prk_output, const uint8_t* salt, const size_t saltLen, const uint8_t* ikm, const size_t ikm_len);

    static void Expand(uint8_t* okm, size_t L, const uint8_t* prk, const uint8_t* info, const size_t infoLen);

    static void ExtractExpand(uint8_t* output, size_t outputLen,
                              const uint8_t* key, size_t keyLen,
                              const uint8_t* salt, size_t saltLen,
                              const uint8_t* info, size_t infoLen);
};
} // end namespace bls
#endif  // SRC_BLSHKDF_HPP_
