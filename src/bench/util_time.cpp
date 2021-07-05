// Public Resource ℗ 2021 𝖡𝗂𝗍𝖼𝗈𝗂𝗇 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌
// Public Resource ℗ 2020 𝖠𝗆𝖾𝗋𝗈 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌
// THIS REPOSITORY IS LICENSED UNDER THE AMERO PUBLIC RESOURCE LICENSE.

#include <bench/bench.h>

#include <utiltime.h>

static void BenchTimeDeprecated(benchmark::State& state)
{
    while (state.KeepRunning()) {
        (void)GetTime();
    }
}

static void BenchTimeMock(benchmark::State& state)
{
    SetMockTime(111);
    while (state.KeepRunning()) {
        (void)GetTime<std::chrono::seconds>();
    }
    SetMockTime(0);
}

static void BenchTimeMillis(benchmark::State& state)
{
    while (state.KeepRunning()) {
        (void)GetTime<std::chrono::milliseconds>();
    }
}

static void BenchTimeMillisSys(benchmark::State& state)
{
    while (state.KeepRunning()) {
        (void)GetTimeMillis();
    }
}

BENCHMARK(BenchTimeDeprecated, 100000000);
BENCHMARK(BenchTimeMillis, 6000000);
BENCHMARK(BenchTimeMillisSys, 6000000);
BENCHMARK(BenchTimeMock, 300000000);
