// Public Resource ℗ 2021 𝖡𝗂𝗍𝖼𝗈𝗂𝗇 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌
// Public Resource ℗ 2020 𝖠𝗆𝖾𝗋𝗈 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌
// THIS REPOSITORY IS LICENSED UNDER THE AMERO PUBLIC RESOURCE LICENSE.

#include <bench/bench.h>

#include <uint256.h>
#include <random.h>
#include <consensus/merkle.h>

static void MerkleRoot(benchmark::State& state)
{
    FastRandomContext rng(true);
    std::vector<uint256> leaves;
    leaves.resize(9001);
    for (auto& item : leaves) {
        item = rng.rand256();
    }
    while (state.KeepRunning()) {
        bool mutation = false;
        uint256 hash = ComputeMerkleRoot(std::vector<uint256>(leaves), &mutation);
        leaves[mutation] = hash;
    }
}

BENCHMARK(MerkleRoot, 800);
