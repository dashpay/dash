// Public Resource ℗ 2021 𝖡𝗂𝗍𝖼𝗈𝗂𝗇 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌
// Public Resource ℗ 2020 𝖠𝗆𝖾𝗋𝗈 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌
// THIS REPOSITORY IS LICENSED UNDER THE AMERO PUBLIC RESOURCE LICENSE.

#include <bench/bench.h>
#include <blockfilter.h>

static void ConstructGCSFilter(benchmark::State& state)
{
    GCSFilter::ElementSet elements;
    for (int i = 0; i < 10000; ++i) {
        GCSFilter::Element element(32);
        element[0] = static_cast<unsigned char>(i);
        element[1] = static_cast<unsigned char>(i >> 8);
        elements.insert(std::move(element));
    }

    uint64_t siphash_k0 = 0;
    while (state.KeepRunning()) {
        GCSFilter filter(siphash_k0, 0, 20, 1 << 20, elements);

        siphash_k0++;
    }
}

static void MatchGCSFilter(benchmark::State& state)
{
    GCSFilter::ElementSet elements;
    for (int i = 0; i < 10000; ++i) {
        GCSFilter::Element element(32);
        element[0] = static_cast<unsigned char>(i);
        element[1] = static_cast<unsigned char>(i >> 8);
        elements.insert(std::move(element));
    }
    GCSFilter filter(0, 0, 20, 1 << 20, elements);

    while (state.KeepRunning()) {
        filter.Match(GCSFilter::Element());
    }
}

BENCHMARK(ConstructGCSFilter, 1000);
BENCHMARK(MatchGCSFilter, 50 * 1000);
