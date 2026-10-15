#pragma once

#include "../EngineExport.h"
#include "../Rendering/EvaluatorOptions.h"
#include <atomic>

namespace ShaderLab::Performance
{
    // Process defaults for Rendering::EvaluatorOptions. An evaluator follows
    // these until GraphEvaluator::SetOptions pins its own, and reads them once
    // per frame. The GUI drives its evaluator through these setters; headless
    // pins options per evaluator from its command line. See EvaluatorOptions
    // for what each setting does.
    //
    // Written by UI and MCP threads, read by render threads. Every setting
    // uses acquire/release: relaxed would become visible just as soon, but
    // would not order the setting against the writes that preceded it. The
    // telemetry counters below are relaxed; they order nothing.
    SHADERLAB_API Rendering::EvaluatorOptions DefaultEvaluatorOptions();
    SHADERLAB_API void SetDefaultEvaluatorOptions(const Rendering::EvaluatorOptions& options);

    // Per-field shims onto the defaults above.
    SHADERLAB_API bool IsGpuBindingsEnabled();
    SHADERLAB_API void SetGpuBindingsEnabled(bool enabled);

    SHADERLAB_API bool IsSkipUnneededCpuReadbackEnabled();
    SHADERLAB_API void SetSkipUnneededCpuReadbackEnabled(bool enabled);

    SHADERLAB_API bool IsAsyncAnalysisReadbackEnabled();
    SHADERLAB_API void SetAsyncAnalysisReadbackEnabled(bool enabled);

    SHADERLAB_API uint32_t CpuAnalysisHintThrottleMs();
    SHADERLAB_API void SetCpuAnalysisHintThrottleMs(uint32_t ms);

    SHADERLAB_API bool IsEffectOutputCachingEnabled();
    SHADERLAB_API void SetEffectOutputCachingEnabled(bool enabled);

    SHADERLAB_API bool IsSubRectInputDemandEnabled();
    SHADERLAB_API void SetSubRectInputDemandEnabled(bool enabled);

    SHADERLAB_API bool IsComputeCommandListEnabled();
    SHADERLAB_API void SetComputeCommandListEnabled(bool enabled);

    // Process-wide telemetry, summed over every evaluator.
    // Dispatches whose CPU readback was skipped.
    SHADERLAB_API uint64_t SkippedCpuReadbacks();
    SHADERLAB_API void IncrementSkippedCpuReadbacks();
}
