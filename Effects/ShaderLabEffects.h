#pragma once

#include "pch_engine.h"
#include "../EngineExport.h"
#include "../Graph/EffectNode.h"

namespace ShaderLab::Graph { class EffectGraph; }

namespace ShaderLab::Effects
{
    // Describes a pre-built ShaderLab effect with embedded HLSL.
    struct ShaderLabEffectDescriptor
    {
        std::wstring name;
        std::wstring category;      // "Analysis", "Source", "Parameter"
        std::wstring subcategory;   // Optional second-tier grouping under
                                    // category (e.g. "Highlights", "Scopes",
                                    // "Gamut Mapping"). Empty = ungrouped,
                                    // shown directly under the category.
        Graph::CustomShaderType shaderType{ Graph::CustomShaderType::PixelShader };

        // Stable identifier and version for upgrade detection.
        std::wstring effectId;          // Stable ID (survives renames)
        uint32_t effectVersion{ 1 };   // Increment when HLSL or params change.
        // Scoping note: a saved graph stores `colorMath + <effect HLSL>` and
        // recompiles from that stored string, so a change to the SHARED
        // ColorMath library changes the stored text of all ~23 effects that
        // prepend it. Bump the ones whose BEHAVIOUR changes, not all 23 -- an
        // upgrade prompt is only useful where taking it changes output, and
        // blanket-bumping trains users to dismiss the badge. When you touch
        // ColorMath, map the changed function to its call sites and bump those.

        // Embedded HLSL source — compiled on first use.
        std::string hlslSource;

        // Named texture inputs.
        std::vector<std::wstring> inputNames;

        // Declared parameters with defaults + UI metadata.
        std::vector<Graph::ParameterDefinition> parameters;

        // Analysis output fields (for compute shaders).
        std::vector<Graph::AnalysisFieldDescriptor> analysisFields;
        Graph::AnalysisOutputType analysisOutputType{ Graph::AnalysisOutputType::None };

        // Compute shader thread group dimensions.
        uint32_t threadGroupX{ 8 };
        uint32_t threadGroupY{ 8 };
        uint32_t threadGroupZ{ 1 };

        // Hidden default properties (in cbuffer but not shown in Properties panel).
        // Set on node creation; auto-updated by the evaluator for dynamic values.
        std::map<std::wstring, Graph::PropertyValue> hiddenDefaults;
        // Trailing inputNames that are lookup tables. See
        // CustomEffectDefinition::lookupInputCount.
        uint32_t lookupInputCount{ 0 };

        // Host-computed cbuffer contents ("derived constants"). For values
        // that depend only on the node's parameters but are expensive --
        // a gamut boundary polygon, a fit scale per intensity -- which a
        // pixel shader would otherwise rebuild for every pixel. Called by the
        // evaluator whenever it packs the node's cbuffer; `write(name, data,
        // floatCount)` fills the cbuffer variable of that name if, and only
        // if, the compiled shader declares it, so a saved graph carrying an
        // older HLSL revision is simply unaffected. Implementations should
        // memoize on their inputs: a node is re-applied every frame its
        // upstream content changes.
        using DerivedConstantWriter =
            std::function<void(std::wstring_view name, const float* data, size_t floatCount)>;
        using DerivedConstantsFn = std::function<void(
            const std::map<std::wstring, Graph::PropertyValue>& props,
            const DerivedConstantWriter& write)>;
        DerivedConstantsFn deriveConstants;

        // Data-only effects have no visible image output pin. They produce
        // analysis output fields but their image output is internal only.
        bool dataOnly{ false };

        // Image-producing compute effects output a texture (not just analysis data).
        // Creates an output pin so downstream pixel shaders can read the result.
        bool hasImageOutput{ false };

        // Clock nodes are time-based animation sources (special render loop handling).
        bool isClock{ false };
    };

    // Registry of all ShaderLab pre-built effects.
    class SHADERLAB_API ShaderLabEffects
    {
    public:
        static ShaderLabEffects& Instance();

        const ShaderLabEffectDescriptor* FindByName(std::wstring_view name) const;
        const ShaderLabEffectDescriptor* FindById(std::wstring_view effectId) const;
        const std::vector<ShaderLabEffectDescriptor>& All() const { return m_effects; }

        // Computed library version: sum of all effect versions.
        uint32_t LibraryVersion() const
        {
            uint32_t v = 0;
            for (const auto& e : m_effects) v += e.effectVersion;
            return v;
        }
        std::vector<const ShaderLabEffectDescriptor*> ByCategory(std::wstring_view category) const;
        std::vector<std::wstring> Categories() const;

        // Create a fully-configured EffectNode from a descriptor.
        static Graph::EffectNode CreateNode(const ShaderLabEffectDescriptor& desc);

        // Re-derive the runtime flags CreateNode sets from the descriptor but
        // the JSON format does not carry (today: isClock). EVERY graph load
        // must call this -- a Clock whose flag is false never advances and
        // never emits Time / Progress, silently freezing everything bound to
        // it. Only the GUI's file-open path used to do this.
        static void RestoreRuntimeFlags(Graph::EffectGraph& graph);

    private:
        ShaderLabEffects();
        void RegisterAll();

        std::vector<ShaderLabEffectDescriptor> m_effects;
    };

    SHADERLAB_API void RegisterEngineD2DEffects(ID2D1Factory1* factory);

    // Configure the on-disk bytecode cache for this process. Pass an
    // empty `rootPath` to disable disk persistence (tests, headless
    // ad-hoc runs). When non-empty, the directory is created if
    // missing, and a background thread reaps entries older than
    // `staleThresholdSec` (default 90 days) so the cache doesn't
    // grow without bound. Pass 0 for staleThresholdSec to skip the
    // background reap (the user can still trigger it manually via
    // the status-bar broom button).
    //
    // Recommended: pass `%LOCALAPPDATA%\ShaderLab\bytecode\`. The
    // engine does NOT pick a default automatically -- the host owns
    // the path policy so headless/test runs can opt out cleanly.
    SHADERLAB_API void ConfigureBytecodeCache(
        std::wstring rootPath,
        uint64_t staleThresholdSec = 90ull * 24 * 60 * 60);

    // Shared HLSL color math functions (prepended to all ShaderLab shaders).
    SHADERLAB_API const std::string& GetColorMathHLSL();
}
