#pragma once

// The gpu-bench script op: how many GPU milliseconds a node chain costs per
// frame. It draws the node into a full-size offscreen target (D2D evaluates
// lazily at DrawImage and shades only the pixels asked for, so a bare frame
// or a small readback tile would time nothing), and it re-dirties every
// Source each iteration, which is the video workload: nothing can be served
// from cache. Needs --gpu-timing.

#include <winrt/Windows.Data.Json.h>

#include <cstdint>
#include <optional>

namespace ShaderLab::Headless
{
    class HeadlessEngine;

    struct GpuBenchRequest
    {
        uint32_t nodeId{ 0 };
        uint32_t iterations{ 20 };              // clamped to [1, 500]
        std::optional<uint32_t> width;          // unset: the node's output size
        std::optional<uint32_t> height;
        uint32_t fallbackWidth{ 1024 };         // when the output has no size
        uint32_t fallbackHeight{ 1024 };
    };

    // The step's body: timings, or {"error": ...}.
    winrt::Windows::Data::Json::JsonObject RunGpuBench(HeadlessEngine& engine, const GpuBenchRequest& request);
}
