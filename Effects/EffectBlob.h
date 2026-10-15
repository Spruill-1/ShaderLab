#pragma once

#include "pch_engine.h"
#include "../EngineExport.h"

// Effect blobs: effects built ahead of time, inside the program that ships
// them. A blob holds the effects' description (the saved-graph JSON the
// Effect Designer writes) and the compiled bytecode of every build the
// program may ask for, so registering it compiles no HLSL. Each build is
// stored on its own, its key in the clear and its bytecode compressed, so a
// generated header can leave builds out with #if and the bytecode cache can
// decompress a build only when it is first used.
namespace ShaderLab::Effects
{
    inline constexpr uint32_t cEffectBlobFormat = 3;

    struct EffectBlobOptions
    {
        // Parameters the program binds to an analysis field (statistics into
        // a tone mapper's peak). Every combination of them gets a build with
        // those parameters served on the GPU, in every option combination,
        // beside the all-CPU builds. At most cMaxBlobGpuParameters.
        std::vector<std::wstring> gpuBoundParameters;
    };
    inline constexpr size_t cMaxBlobGpuParameters = 4;

    // One stored build and what selects it.
    struct EffectBlobBuildInfo
    {
        std::wstring effect;
        // Parameters served on the GPU, in the effect's order; empty for the
        // all-CPU layout.
        std::vector<std::wstring> gpuParameters;
        // Each option this build fixes, with the label of its value. An
        // option left out is read at run time.
        std::vector<std::pair<std::wstring, std::wstring>> options;
        std::vector<uint8_t> record;   // the stored form
        size_t bytecodeBytes{ 0 };     // before compression
        bool IsGeneric() const { return gpuParameters.empty() && options.empty(); }
    };

    struct EffectBlobEffectInfo
    {
        std::wstring name;
        // Specialised options, in the effect's order, with their labels.
        std::vector<std::pair<std::wstring, std::vector<std::wstring>>> options;
        // The GPU layouts built, beside the all-CPU one.
        std::vector<std::vector<std::wstring>> gpuLayouts;
    };

    struct EffectBlobBuild
    {
        std::string description;                // UTF-8 JSON
        std::vector<EffectBlobEffectInfo> effects;
        std::vector<EffectBlobBuildInfo> builds;
        std::wstring error;                     // set, with no builds, on failure

        size_t BytecodeBytes() const
        {
            size_t total = 0;
            for (const auto& build : builds) total += build.bytecodeBytes;
            return total;
        }
        size_t StoredBytes() const
        {
            size_t total = description.size();
            for (const auto& build : builds) total += build.record.size();
            return total;
        }
    };

    // The single-file form, for EffectGraphRegisterEffect.
    SHADERLAB_API std::vector<uint8_t> EffectBlobFile(const EffectBlobBuild& build);

    // Compiles every shader node in `json` into blob builds. `source` names
    // the input in errors.
    SHADERLAB_API EffectBlobBuild BuildEffectBlob(std::wstring_view json, const std::wstring& source,
                                                  const EffectBlobOptions& options);

    // The C++ header form: the description and each build as arrays, each
    // build behind an #if on the macros that select it, and an
    // EFFECTGRAPH_EFFECT_SOURCE named `symbol` for EffectGraphRegisterEffects.
    SHADERLAB_API std::string GenerateEffectHeader(const EffectBlobBuild& build, const std::string& symbol,
                                                   const std::wstring& sourceName);

    // The macro that selects an effect, option value or GPU layout in a
    // generated header: EFFECTGRAPH_ plus the names, uppercased, each run of
    // anything outside [A-Z0-9] one '_' ("Off (float)" -> OFF_FLOAT).
    SHADERLAB_API std::string EffectMacro(std::wstring_view effect);
    SHADERLAB_API std::string OptionMacro(std::wstring_view effect, std::wstring_view option, std::wstring_view label);
    SHADERLAB_API std::string LayoutMacro(std::wstring_view effect, const std::vector<std::wstring>& gpuParameters);

    enum class EffectBlobStatus
    {
        Registered,
        Corrupt,        // not a blob, truncated, or builds that match none of its effects
        Incompatible,   // built against other shared HLSL headers or cache schema
        NotRegistered,  // valid, but its effects' names or ids are taken by newer versions
        NoneIncluded,   // a header whose selection left out every effect's generic build
    };

    struct EffectBlobRegistration
    {
        EffectBlobStatus status{ EffectBlobStatus::Corrupt };
        std::vector<std::wstring> effects;   // the names registered
        std::wstring error;
    };

    // A blob as a generated header holds it.
    struct EffectBlobParts
    {
        uint32_t format{ 0 };
        uint64_t contractHash{ 0 };
        std::span<const uint8_t> description;
        std::vector<std::span<const uint8_t>> records;
    };

    // Registers the effects that have their generic build, and adds every
    // build to the bytecode cache as a prebuilt entry, decompressed on first
    // use. RegisterEffectBlob copies the blob; RegisterEffectBlobParts keeps
    // pointers into memory that must outlive the process's use of the effects
    // (a generated header's arrays). Same startup rule as LoadUserEffects.
    SHADERLAB_API EffectBlobRegistration RegisterEffectBlob(std::span<const uint8_t> blob);
    SHADERLAB_API EffectBlobRegistration RegisterEffectBlobParts(const EffectBlobParts& parts);
}
