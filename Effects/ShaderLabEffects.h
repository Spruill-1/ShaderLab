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
        // Scoping note: a saved graph stores the effect HLSL and recompiles
        // from it. The ~23 effects that #include "shaderlab_colormath.hlsli"
        // pick up a ColorMath change directly; graphs that carry an inline
        // copy keep the math they were saved with until upgraded. Bump the
        // effects whose BEHAVIOUR changes, not all 23 -- an upgrade prompt is
        // only useful where taking it changes output. When you touch
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

        // inputNames is the maximum; the node shows the pins in use plus one.
        // See CustomEffectDefinition::variadicInputs.
        bool variadicInputs{ false };

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

        // Image-output size of a D3D11 compute effect that depends on its
        // inputs' sizes. `inputSizes` is indexed by pin, {0, 0} for an
        // unwired pin. Returning {0, 0} falls back to the evaluator's usual
        // sizing (OutputWidth/OutputHeight, then input 0). A size past the
        // D3D11 texture limit is refused with a node error.
        using ImageOutputSizeFn = std::function<D2D1_SIZE_U(
            const std::map<std::wstring, Graph::PropertyValue>& props,
            const std::vector<D2D1_SIZE_U>& inputSizes)>;
        ImageOutputSizeFn deriveImageOutputSize;

        // Data-only effects have no visible image output pin. They produce
        // analysis output fields but their image output is internal only.
        bool dataOnly{ false };

        // Image-producing compute effects output a texture (not just analysis data).
        // Creates an output pin so downstream pixel shaders can read the result.
        bool hasImageOutput{ false };

        // Clock nodes are time-based animation sources (special render loop handling).
        bool isClock{ false };

        // File a user effect was loaded from; empty for built-ins.
        std::wstring sourcePath;
        bool IsUserEffect() const { return !sourcePath.empty(); }
    };

    // Result of LoadUserEffects. Every file that did not load has an entry
    // in `errors`.
    struct UserEffectLoadReport
    {
        std::vector<std::wstring> directories;  // every directory scanned
        std::vector<std::wstring> loaded;       // effect names now registered
        std::vector<std::wstring> errors;       // "<file>: <reason>"
    };

    // What to do when a user effect's id or name matches one already
    // registered.
    enum class UserEffectConflict
    {
        Reject,     // keep the existing effect; report the incoming one as an error
        Replace,    // the incoming definition takes the existing one's place
    };

    // Registry of all ShaderLab pre-built effects, plus any user effects
    // registered from outside the build (LoadUserEffects).
    class SHADERLAB_API ShaderLabEffects
    {
    public:
        static ShaderLabEffects& Instance();

        const ShaderLabEffectDescriptor* FindByName(std::wstring_view name) const;
        const ShaderLabEffectDescriptor* FindById(std::wstring_view effectId) const;
        const std::vector<ShaderLabEffectDescriptor>& All() const { return m_effects; }

        // Computed library version: sum of all built-in effect versions.
        // User effects are excluded so the number identifies the build.
        uint32_t LibraryVersion() const
        {
            uint32_t version = 0;
            for (const auto& effect : m_effects)
                if (!effect.IsUserEffect()) version += effect.effectVersion;
            return version;
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

        // User effects
        // Register every shader node in each saved graph (*.json) in
        // `directory` as an effect. An optional top-level "category" picks the
        // menu group (default "User"). Effects that fail to compile are
        // reported, not added. A missing directory is not an error.
        // Call at startup, before any descriptor pointer is held: this appends
        // to the registry and may replace entries in place.
        const UserEffectLoadReport& LoadUserEffects(const std::wstring& directory);
        const UserEffectLoadReport& UserEffectReport() const { return m_userReport; }

        // %LOCALAPPDATA%\ShaderLab\effects, or empty if it cannot be resolved.
        static std::wstring DefaultUserEffectsDirectory();

    private:
        ShaderLabEffects();
        void RegisterAll();

        std::vector<ShaderLabEffectDescriptor> m_effects;
        UserEffectLoadReport m_userReport;
    };

    // The conflict policy LoadUserEffects applies; public so tests can call it.
    SHADERLAB_API UserEffectConflict ResolveUserEffectConflict(
        const ShaderLabEffectDescriptor& existing,
        const ShaderLabEffectDescriptor& incoming);

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

    // Shared HLSL color math functions. Shaders get them with
    // #include "shaderlab_colormath.hlsli"; the text has an include guard.
    inline constexpr const char* cColorMathIncludeName = "shaderlab_colormath.hlsli";
    SHADERLAB_API const std::string& GetColorMathHLSL();

    // Gamut boundary search and the ICtCp Gamut Boundary LUT's geometry,
    // stamp and reader, as #include "shaderlab_gamut.hlsli". It includes the
    // color math itself and has an include guard.
    inline constexpr const char* cGamutIncludeName = "shaderlab_gamut.hlsli";
    SHADERLAB_API const std::string& GetGamutHLSL();
}
