// Copyright 2018 Chia Network Inc

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//    http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "bls.hpp"
#include "secure.h"

#include <cstring>

namespace bls {

ChainCode ChainCode::FromBytes(const Bytes& bytes) {
    if (bytes.size() != ChainCode::SIZE) {
        throw std::invalid_argument("ChainCode::FromBytes: Invalid size");
    }
    ChainCode c = ChainCode();
    memcpy(c.chainCode.data(), bytes.begin(), ChainCode::SIZE);
    return c;
}

ChainCode::ChainCode(const ChainCode &cc) : chainCode(cc.chainCode) {}

ChainCode& ChainCode::operator=(const ChainCode &cc) {
    // Every byte is overwritten, so nothing of the old value survives.
    chainCode = cc.chainCode;
    return *this;
}

ChainCode::~ChainCode() {
    util::SecureWipe(chainCode.data(), chainCode.size());
}

// Comparator implementation.
bool operator==(ChainCode const &a,  ChainCode const &b) {
    return a.chainCode == b.chainCode;
}

bool operator!=(ChainCode const &a,  ChainCode const &b) {
    return !(a == b);
}

std::ostream &operator<<(std::ostream &os, ChainCode const &s) {
    util::SecVector<uint8_t> buffer(ChainCode::SIZE);
    s.Serialize(buffer.data());
    return os << Util::HexStr(buffer.data(), buffer.size());
}

void ChainCode::Serialize(uint8_t *buffer) const {
    memcpy(buffer, chainCode.data(), SIZE);
}

std::vector<uint8_t> ChainCode::Serialize() const {
    std::vector<uint8_t> data(SIZE);
    Serialize(data.data());
    return data;
}
} // end namespace bls
