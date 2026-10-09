#pragma once

// Tiny shared header for ShaderLab test code split across multiple TUs.
// Owns the global pass / fail counters and the TEST() reporter, so
// Tests/TestRunner.cpp and Tests/Math/*.cpp can all log into the same
// summary line.

#include <cstdio>

namespace ShaderLab::Tests
{
    inline int g_passed = 0;
    inline int g_failed = 0;

    inline void TEST(const char* name, bool result)
    {
        if (result)
        {
            std::printf("  [PASS] %s\n", name);
            ++g_passed;
        }
        else
        {
            std::printf("  [FAIL] %s\n", name);
            ++g_failed;
        }
        std::fflush(stdout);
    }

    // Real-time assertions (rates, deadlines, playback tracking) need an
    // otherwise idle machine. Off on WARP and under CI (shared runners);
    // SHADERLAB_TIMING_TESTS=1 or 0 overrides. Set by main().
    inline bool g_timingAssertions = true;

    // TEST() when timing assertions are on; otherwise the result is printed
    // as [info] and not counted.
    inline void TIMING_TEST(const char* name, bool result)
    {
        if (g_timingAssertions)
        {
            TEST(name, result);
            return;
        }
        std::printf("  [info] %s: %s (timing, not asserted)\n", name, result ? "met" : "missed");
        std::fflush(stdout);
    }
}
