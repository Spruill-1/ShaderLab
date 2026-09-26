#include "pch_engine.h"
#include "ShaderLabEffects.h"
#include "ColorMathCpu.h"
#include "../Graph/EffectGraph.h"
#include "BytecodeCache.h"
#include "CustomComputeShaderEffect.h"
#include "CustomComputeBridgeEffect.h"
#include "CustomPixelShaderEffect.h"

#include <thread>

namespace ShaderLab::Effects
{
    void RegisterEngineD2DEffects(ID2D1Factory1* factory)
    {
        if (!factory) return;
        CustomPixelShaderEffect::RegisterEffect(factory);
        CustomComputeShaderEffect::RegisterEffect(factory);
        CustomComputeBridgeEffect::RegisterEffect(factory);
    }

    void ConfigureBytecodeCache(std::wstring rootPath, uint64_t staleThresholdSec)
    {
        BytecodeCache::Instance().SetDiskCacheRoot(rootPath);
        if (rootPath.empty() || staleThresholdSec == 0) return;
        // Background reap so engine init isn't blocked on a directory
        // walk. Detached jthread; the cache itself is robust to
        // concurrent access.
        std::thread([staleThresholdSec]() {
            BytecodeCache::Instance().ReapDisk(staleThresholdSec);
        }).detach();
    }

    // Shared color-math HLSL extracted to Effects/ColorMath.cpp
    // (Phase 5 partial). GetColorMathHLSL() prototype lives in
    // Effects/ShaderLabEffects.h.
    // -----------------------------------------------------------------------
    // Effect HLSL sources
    // -----------------------------------------------------------------------

    static const std::string s_luminanceHeatmapHLSL = R"HLSL(
// Luminance Heatmap -- D3D11 compute, false-color luminance visualization.
// MinNits / MaxNits are gpuBindable so a Luminance Statistics .Min / .Max
// can drive the heatmap range automatically per source-distribution
// without a CPU readback round-trip.
#include "shaderlab_params.hlsli"

Texture2D<float4>        Source      : register(t0);
RWTexture2D<float4>      ImageOutput : register(u1);

SHADERLAB_GPU_BUFFER(MinNits, t1)
SHADERLAB_GPU_BUFFER(MaxNits, t2)

cbuffer constants : register(b0) {
    uint  Width;
    uint  Height;
    SHADERLAB_PARAM(float, MinNits)     // default 0.0
    SHADERLAB_PARAM(float, MaxNits)     // default 10000.0
    uint  ColormapMode;                 // 0=Turbo, 1=Inferno
};

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= Width || dtid.y >= Height) return;

    SHADERLAB_LOAD_PARAM(float, MinNits)
    SHADERLAB_LOAD_PARAM(float, MaxNits)

    float4 color = Source.Load(int3(dtid.xy, 0));
    float nits = ScRGBLuminanceNits(color.rgb);
    float t = saturate((nits - MinNits) / max(MaxNits - MinNits, 0.001));
    float3 mapped = TurboColormap(t);
    // Turbo colormap outputs perceptual 0-1 values; keep in scRGB
    // (1.0 = 80 nits SDR white) for visible display.
    ImageOutput[dtid.xy] = float4(mapped, color.a);
}
)HLSL";

    static const std::string s_outOfGamutHLSL = R"HLSL(
// Gamut Highlight
// TargetGamut modes:
//   0 = Rec.709    (matrix conversion)
//   1 = DCI-P3     (matrix conversion)
//   2 = Rec.2020   (matrix conversion)
//   3 = Custom     (CIE xy chromaticity triangle test using
//                   RedPrimary/GreenPrimary/BluePrimary; bind these to
//                   `Working Space.RedPrimary` etc. for monitor-matched
//                   analysis, or set them manually.)
Texture2D Source : register(t0);
SamplerState InputSampler : register(s0);

cbuffer constants : register(b0) {
    uint TargetGamut;
    float OverlayR;
    float OverlayG;
    float OverlayB;
    float OverlayStrength;
    uint Mode;
    float2 RedPrimary;
    float2 GreenPrimary;
    float2 BluePrimary;
};

float4 main(
    float4 pos      : SV_POSITION,
    float4 scenePos : SCENE_POSITION,
    float4 uv0      : TEXCOORD0) : SV_TARGET
{
    // SCENE_POSITION is NOT optional -- D2D's pixel-shader input signature is
    // (SV_POSITION, SCENE_POSITION, TEXCOORD0..N), and omitting the middle one
    // binds the SCENE coordinate to the parameter named uv0 (see "D2D Custom
    // Effect Gotchas" in .github/copilot-instructions.md). uv0 is NORMALIZED once the signature
    // is right, so the input is read with Sample(), not Load(int3(...)).
    float4 color = Source.Sample(InputSampler, uv0.xy);
    if (color.a < 0.001) return color;

    // Skip near-black pixels where chromaticity is numerically unstable.
    float lum = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));
    if (lum < 0.001) return color;

    // Read all cbuffer vars at top so DXC keeps them resident even on
    // branches that don't directly reference every primary.
    float gamut = TargetGamut;
    float2 mr = RedPrimary;
    float2 mg = GreenPrimary;
    float2 mb = BluePrimary;

    float3 xyz = ScRGBToXYZ(color.rgb);
    float3 targetRGB = color.rgb;

    if (gamut < 0.5) {
        // Rec.709 (identity for our scRGB pipeline).
        targetRGB = color.rgb;
    } else if (gamut < 1.5) {
        targetRGB = mul(XYZ_TO_P3D65, xyz);
    } else if (gamut < 2.5) {
        targetRGB = mul(XYZ_TO_REC2020, xyz);
    } else {
        // Custom: CIE xy chromaticity triangle test against bound primaries.
        float sum = xyz.x + xyz.y + xyz.z;
        float2 xy = (sum > 0.0001) ? float2(xyz.x / sum, xyz.y / sum) : D65_WHITE;
        float2 v0 = mb - mr, v1 = mg - mr, v2 = xy - mr;
        float d00 = dot(v0, v0), d01 = dot(v0, v1), d02 = dot(v0, v2);
        float d11 = dot(v1, v1), d12 = dot(v1, v2);
        float inv = 1.0 / (d00 * d11 - d01 * d01);
        float u = (d11 * d02 - d01 * d12) * inv;
        float v = (d00 * d12 - d01 * d02) * inv;
        bool inside = (u >= 0) && (v >= 0) && (u + v <= 1.0);
        targetRGB = inside ? float3(1, 1, 1) : float3(-1, -1, -1);
    }

    bool oog = (targetRGB.r < 0.0 || targetRGB.g < 0.0 || targetRGB.b < 0.0);
    bool highlight = (Mode > 0.5) ? !oog : oog;
    if (highlight) {
        float3 overlay = float3(OverlayR, OverlayG, OverlayB);
        color.rgb = lerp(color.rgb, overlay, OverlayStrength);
    }
    return color;
}
)HLSL";

    static const std::string s_luminanceHighlightHLSL = R"HLSL(
// Luminance Highlight -- D3D11 compute, mirrors Gamut Highlight but on
// luminance. Pixels whose luminance falls outside (or inside, per Mode)
// the active nit range are tinted with an overlay color.
//
// MinNits / MaxNits are gpuBindable so a Luminance Statistics .Min / .Max
// (or Working Space.MinNits / .PeakNits) can drive the range
// automatically without a CPU readback round-trip.
//
// TargetRange selects the [min,max] nit window:
//   0 = SDR              (0 .. 80 nits)
//   1 = HDR 400          (0 .. 400 nits)
//   2 = HDR 1000         (0 .. 1000 nits)
//   3 = HDR 4000         (0 .. 4000 nits)
//   4 = HDR 10000        (0 .. 10000 nits)
//   5 = Custom           (use MinNits / MaxNits sliders directly)
#include "shaderlab_params.hlsli"

Texture2D<float4>        Source      : register(t0);
RWTexture2D<float4>      ImageOutput : register(u1);

SHADERLAB_GPU_BUFFER(MinNits, t1)
SHADERLAB_GPU_BUFFER(MaxNits, t2)

cbuffer constants : register(b0) {
    uint  Width;
    uint  Height;
    uint  TargetRange;
    SHADERLAB_PARAM(float, MinNits)
    SHADERLAB_PARAM(float, MaxNits)
    float OverlayR;
    float OverlayG;
    float OverlayB;
    float OverlayStrength;
    uint  Mode;                  // 0 = Out-of-Range, 1 = In-Range
};

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= Width || dtid.y >= Height) return;

    SHADERLAB_LOAD_PARAM(float, MinNits)
    SHADERLAB_LOAD_PARAM(float, MaxNits)

    float4 color = Source.Load(int3(dtid.xy, 0));
    if (color.a < 0.001) { ImageOutput[dtid.xy] = color; return; }

    // ScRGBLuminanceNits: scRGB luminance * 80 (1.0 = 80 nits SDR white).
    float nits = ScRGBLuminanceNits(color.rgb);

    // Pick effective range.
    float effMin = MinNits;
    float effMax = MaxNits;
    if (TargetRange < 0.5) {              // SDR
        effMin = 0.0;   effMax = 80.0;
    } else if (TargetRange < 1.5) {       // HDR 400
        effMin = 0.0;   effMax = 400.0;
    } else if (TargetRange < 2.5) {       // HDR 1000
        effMin = 0.0;   effMax = 1000.0;
    } else if (TargetRange < 3.5) {       // HDR 4000
        effMin = 0.0;   effMax = 4000.0;
    } else if (TargetRange < 4.5) {       // HDR 10000
        effMin = 0.0;   effMax = 10000.0;
    } else {                               // Custom
        effMin = MinNits;
        effMax = MaxNits;
    }

    bool outOfRange = (effMin >= effMax) || (nits < effMin) || (nits > effMax);
    bool highlight = (Mode > 0.5) ? !outOfRange : outOfRange;
    if (highlight) {
        float3 overlay = float3(OverlayR, OverlayG, OverlayB);
        color.rgb = lerp(color.rgb, overlay, OverlayStrength);
    }
    ImageOutput[dtid.xy] = color;
}
)HLSL";

    ShaderLabEffects& ShaderLabEffects::Instance()
    {
        static ShaderLabEffects instance;
        return instance;
    }

    // Gamut boundary table geometry, shared by the LUT generator's hidden
    // OutputWidth/Height and -- via GAMUT_LUT_W/H in GamutBoundaryHLSL() -- by
    // whatever consumer reads it. 256 hue columns (1.4 degrees each) by
    // 128 I rows = 32K entries, 512 KB as FP32 RGBA.
    static constexpr uint32_t kGamutLutW = 256;
    static constexpr uint32_t kGamutLutH = 128;
    static_assert((kGamutLutW & (kGamutLutW - 1)) == 0,
                  "GamutLutBoundaryRadius wraps hue with a mask");

    // The exact gamut boundary search, plus the table reader. A consumer
    // prepends this same source (running the search as its fallback) that the
    // LUT generator bakes, so a table can only ever be a sampled copy of the
    // exact answer -- never a second implementation that drifts.
    // Tests/Math/GamutTests.cpp slices it back out of the LUT's assembled
    // source, from `#define CUBE_EPS` up to the generator's own header.
    static const std::string& GamutBoundaryHLSL()
    {
        static const std::string s = []
        {
            std::string t = R"HLSL(
// Gamut boundary at a given I, solved against the target RGB cube.
//
// Replaces a 48-point walk of the xy PRIMARIES TRIANGLE. That walk was both
// slow and WRONG: the chromaticity triangle is the projection of the gamut
// over ALL luminances, but the set reachable at one luminance is the
// constant-Y slice of the cube, which is strictly smaller and shrinks to the
// white point as Y approaches peak. sRGB red at full intensity carries only
// 0.2126 of peak luminance, so against a 280-nit white there is no saturated
// red above ~60 nits at all -- yet the walk happily returned one. Measured
// overshoot of the old boundary: 1.5x at 92 nits, 3.4x at 202, 6.6x at 390.
//
// The consequence was that B came back too large, r/B too small, and the
// compressor concluded out-of-gamut colours were fine -- leaving the residue
// clamp in step 5 as the de-facto gamut mapper. That is what the comment on
// the luminance-restore cap was describing.
//
// Bisection, not a closed form: the ray from neutral passes through
// PQ per-channel, so the in-gamut set is not an analytic interval. It IS a
// single interval in practice -- 17,280 rays across sRGB/P3/BT.2020 found one
// apparent re-entry, and inspecting it showed a ray running along the R = 0
// face with R oscillating at the 1e-5 level from FP error in the PQ round
// trip, the real exit being G at -1.45e-3.
#define CUBE_EPS    1e-4
#define CUBE_COARSE 20
#define CUBE_REFINE 7

// Gamut boundary table geometry. Injected from kGamutLutW/H in C++ so the
// table generator, its hidden OutputWidth/Height and this reader cannot
// disagree. W must stay a power of two: the hue wrap below is a mask.
#define GAMUT_LUT_W @LUTW@
#define GAMUT_LUT_H @LUTH@

// Row spacing on the I axis. Rows sit at I = peakOutI * (1 - (1 - s)^K) for
// s = row / (H - 1), which concentrates them toward I(W) -- where B collapses
// from ~0.3 to exactly 0 (at peak white only neutral is in gamut), and where
// the knee lands every bright pixel. Uniform spacing gave that band ~4 rows.
// The generator and the reader both go through these two functions.
// K = 2, measured (LUT vs exact, dE ITP, float output, stats pinned): on a
// saturated BT.2020 gradient -- 99.8% of pixels at 0.95-0.98 of I(W) -- sRGB
// mean 0.42 -> 0.18, max 6.1 -> 3.3 (P3 max 3.2 -> 1.1) against uniform rows;
// real HDR content unchanged (bird mean 0.0036 -> 0.0034). K = 3 bought little
// more on the gradient and cost the bird's max (2.2 -> 3.0) as low rows thinned.
#define GAMUT_LUT_WARP_K 2.0
float GamutLutRowToI(float s, float peakOutI) { return peakOutI * (1.0 - pow(1.0 - saturate(s), GAMUT_LUT_WARP_K)); }
float GamutLutIToRow(float I, float peakOutI) { return 1.0 - pow(1.0 - saturate(I / max(peakOutI, 1e-6)), 1.0 / GAMUT_LUT_WARP_K); }

// True when this ICtCp coordinate reconstructs inside [0, peak] in the target
// RGB cube.
bool InTargetCube(float I, float2 ctcp, float peak, TargetXf t)
{
    // Tolerance scales with peak: it is absorbing FP error in the ICtCp round
    // trip, which is proportional to the magnitude being round-tripped. A flat
    // 1e-4 let pure blue miss by its own round-trip residue (G = -1.3e-4 at
    // peak 2.5) and read as out of gamut.
    float eps = max(1e-4, peak * 5e-4);
    float3 rgb = ScRGBToTarget(t, ICtCpToScRGB(float3(I, ctcp.x, ctcp.y)));
    return all(rgb >= -eps) && all(rgb <= peak + eps);
}

// The same test, also handing back the reconstructed scRGB, for a caller that
// needs that colour afterwards and should not rebuild it.
bool InTargetCubeRgb(float I, float2 ctcp, float peak, TargetXf t, out float3 scRGB)
{
    float eps = max(1e-4, peak * 5e-4);
    scRGB = ICtCpToScRGB(float3(I, ctcp.x, ctcp.y));
    float3 rgb = ScRGBToTarget(t, scRGB);
    return all(rgb >= -eps) && all(rgb <= peak + eps);
}

// Largest chroma radius along `dir` that stays inside the target cube.
//
// Finds the LAST inside sample, not the first exit, because the in-gamut set
// along a ray is NOT always a single interval. A straight line in (Ct, Cp) at
// constant I can leave the cube and come back: constant-I is not constant-Y,
// and the cube's image under PQ is not convex in this parameterisation. The
// excursions are shallow but they sit exactly on the cube VERTICES, which is
// where the common saturated UI colours live. Measured dip as a fraction of
// peak, and the resulting error if you stop at the first exit:
//     blue     0.073% -> 0.187% of peak   d = 1.14 .. 1.24   (worst)
//     yellow   0.047%                     d = 1.04 .. 1.07
//     green/cyan 0.046%                   d = 1.001
//     magenta  0.001%                     d = 1.000
// Plain bisection therefore reported B ~18% short for pure blue, which drove
// a legal sRGB blue 35% dark with a green cast (23.8 dE, 74 deg hue shift).
//
// A larger epsilon cannot fix it: near a grazing face crossing dC/dr is only
// ~0.26, so a tolerance deep enough to cover blue's dip admits ~18% of extra
// radius everywhere else.
//
// Coarse scan then refine: ~27 reconstructions, still below the 48-point
// polygon walk this replaced. It is also the argument for hoisting this into
// a cached LUT -- a re-entry-robust search is too expensive to want per pixel,
// and B depends only on (I, hue, gamut, peak), all of which are stable for the
// life of a capture.
float CubeBoundaryRadius(float I, float2 dir, float peak, TargetXf t)
{
    if (!InTargetCube(I, float2(0.0, 0.0), peak, t))
        return 0.0;   // above the peak's own I even neutral is out of range

    // 0.45 comfortably exceeds the presets' boundaries: widest measured is
    // BT.2020 at 0.44, sRGB at 0.32. A Custom target wider than BT.2020 would
    // be under-reported here (over-compressed, never out of gamut); no display
    // primaries come close.
    const float kMaxR = 0.45;
    const float step  = kMaxR / CUBE_COARSE;

    float last = 0.0;
    [unroll]
    for (int i = 1; i <= CUBE_COARSE; ++i)
    {
        float r = step * i;
        if (InTargetCube(I, dir * r, peak, t)) last = r;
    }

    float lo = last;
    float hi = min(last + step, kMaxR);
    [unroll]
    for (int k = 0; k < CUBE_REFINE; ++k)
    {
        float mid = 0.5 * (lo + hi);
        if (InTargetCube(I, dir * mid, peak, t)) lo = mid;
        else                                  hi = mid;
    }
    return lo;
}

// B(I, hue) read back from a table built by CubeBoundaryRadius above.
//
// u = hue (wraps), v = I / peakOutI (clamps). The generator writes column x at
// hue ((x + 0.5) / W) * 2pi - pi and row y at I = y / (H - 1) * peakOutI, so a
// hue landing exactly on a column centre reads that column with no blend.
//
// Load at integer texels, not Sample: this is read at coordinates COMPUTED
// here, not at this pixel's own texcoord, and D2D is free to place an input
// inside a larger intermediate. Integer Load from the origin is the same
// addressing the lane-3 analysis textures already rely on. The texture is a
// parameter rather than a global so this function can be shared -- and so the
// guard test that slices the boundary helpers out of the LUT's source
// compiles without dragging a register binding along.
float GamutLutBoundaryRadius(Texture2D<float4> lut, float I, float2 dir, float peakOutI)
{
    const float kTwoPi = 6.28318530718;
    float fx  = (atan2(dir.y, dir.x) / kTwoPi + 0.5) * GAMUT_LUT_W - 0.5;
    float fy  = GamutLutIToRow(I, peakOutI) * (GAMUT_LUT_H - 1);
    float x0f = floor(fx);
    float tx  = fx - x0f;
    int   x0  = ((int)x0f) & (GAMUT_LUT_W - 1);   // -1 wraps to W-1
    int   x1  = (x0 + 1)   & (GAMUT_LUT_W - 1);
    int   y0  = min((int)fy, GAMUT_LUT_H - 2);
    float ty  = fy - y0;
    float b00 = lut.Load(int3(x0, y0,     0)).r;
    float b10 = lut.Load(int3(x1, y0,     0)).r;
    float b01 = lut.Load(int3(x0, y0 + 1, 0)).r;
    float b11 = lut.Load(int3(x1, y0 + 1, 0)).r;
    return lerp(lerp(b00, b10, tx), lerp(b01, b11, tx), ty);
}

)HLSL";
            auto put = [&t](const char* key, uint32_t v)
            {
                auto at = t.find(key);
                t.replace(at, std::strlen(key), std::to_string(v));
            };
            put("@LUTW@", kGamutLutW);
            put("@LUTH@", kGamutLutH);
            return t;
        }();
        return s;
    }

    ShaderLabEffects::ShaderLabEffects()
    {
        RegisterAll();
    }

    const ShaderLabEffectDescriptor* ShaderLabEffects::FindByName(std::wstring_view name) const
    {
        for (const auto& e : m_effects)
            if (_wcsicmp(e.name.c_str(), name.data()) == 0)
                return &e;
        return nullptr;
    }

    const ShaderLabEffectDescriptor* ShaderLabEffects::FindById(std::wstring_view effectId) const
    {
        for (const auto& e : m_effects)
            if (e.effectId == effectId)
                return &e;
        // Legacy ID aliases: rename history. Saved graphs may reference
        // the old name; resolve them transparently so old .effectgraph
        // files keep loading and the "Update Effect" prompt fires once
        // they're loaded.
        if (effectId == L"Perceptual Gamut Map")
            return FindById(L"ICtCp Gamut Map");
        return nullptr;
    }

    std::vector<const ShaderLabEffectDescriptor*> ShaderLabEffects::ByCategory(std::wstring_view category) const
    {
        std::vector<const ShaderLabEffectDescriptor*> result;
        for (const auto& e : m_effects)
            if (e.category == category)
                result.push_back(&e);
        return result;
    }

    std::vector<std::wstring> ShaderLabEffects::Categories() const
    {
        std::vector<std::wstring> cats;
        for (const auto& e : m_effects)
        {
            bool found = false;
            for (const auto& c : cats) if (c == e.category) { found = true; break; }
            if (!found) cats.push_back(e.category);
        }
        return cats;
    }

    Graph::EffectNode ShaderLabEffects::CreateNode(const ShaderLabEffectDescriptor& desc)
    {
        using namespace Graph;

        EffectNode node;
        node.name = desc.name;
        node.type = (desc.shaderType == CustomShaderType::PixelShader)
            ? NodeType::PixelShader : NodeType::ComputeShader;

        // Parameter nodes (no HLSL) and data-only effects have no image output pin.
        if (!desc.hlslSource.empty() && !desc.dataOnly)
            node.outputPins.push_back({ L"Output", 0 });

        CustomEffectDefinition def;
        def.shaderType = desc.shaderType;
        def.hlslSource = std::wstring(desc.hlslSource.begin(), desc.hlslSource.end());
        def.inputNames = desc.inputNames;
        def.lookupInputCount = desc.lookupInputCount;
        def.parameters = desc.parameters;
        def.analysisFields = desc.analysisFields;
        def.analysisOutputType = desc.analysisOutputType;
        def.analysisOutputSize = 256;
        def.threadGroupX = desc.threadGroupX;
        def.threadGroupY = desc.threadGroupY;
        def.threadGroupZ = desc.threadGroupZ;
        def.shaderLabEffectId = desc.effectId;
        def.shaderLabEffectVersion = desc.effectVersion;
        CoCreateGuid(&def.shaderGuid);

        // Set up input pins from input names.
        for (uint32_t i = 0; i < desc.inputNames.size(); ++i)
            node.inputPins.push_back({ std::format(L"I{}", i), i });

        // Set default property values from parameter definitions.
        for (const auto& param : desc.parameters)
            node.properties[param.name] = param.defaultValue;

        // Set hidden default properties (cbuffer values not in Properties panel).
        for (const auto& [key, val] : desc.hiddenDefaults)
            node.properties[key] = val;

        node.customEffect = std::move(def);
        node.isClock = desc.isClock;
        return node;
    }

    void ShaderLabEffects::RestoreRuntimeFlags(Graph::EffectGraph& graph)
    {
        auto& lib = Instance();
        for (auto& node : const_cast<std::vector<Graph::EffectNode>&>(graph.Nodes()))
        {
            if (!node.customEffect.has_value() || node.customEffect->shaderLabEffectId.empty())
                continue;
            if (const auto* desc = lib.FindById(node.customEffect->shaderLabEffectId))
                node.isClock = desc->isClock;
        }
    }

    // Default edge length for synthetic sources and diagram-style viewers.
    // Held at 1024 rather than 512: two-input pixel-shader effects (Split
    // Comparison et al.) render displaced when fed inputs <= 512px, so a
    // smaller default silently breaks any A/B comparison built on these
    // nodes. See the "Known issues" entry in CHANGELOG.md. The parameter
    // minimums are deliberately left where they are -- a small diagram is
    // still useful standalone, and cheap for the O(N)-per-pixel viewers.
    static constexpr float kDefaultDiagramSize = 1024.0f;

    // ---- Derived constants (see ShaderLabEffectDescriptor::deriveConstants) --
    namespace
    {
        using PropMap = std::map<std::wstring, Graph::PropertyValue>;
        namespace CM = ColorMathCpu;

        double PropNumber(const PropMap& p, const wchar_t* name, double fallback)
        {
            auto it = p.find(name);
            if (it == p.end()) return fallback;
            if (auto* f = std::get_if<float>(&it->second)) return *f;
            if (auto* u = std::get_if<uint32_t>(&it->second)) return *u;
            if (auto* i = std::get_if<int32_t>(&it->second)) return *i;
            return fallback;
        }
        CM::V2 PropV2(const PropMap& p, const wchar_t* name, CM::V2 fallback)
        {
            auto it = p.find(name);
            if (it == p.end()) return fallback;
            if (auto* v = std::get_if<winrt::Windows::Foundation::Numerics::float2>(&it->second))
                return { v->x, v->y };
            return fallback;
        }

        // A handful of recently used tables, keyed by every input that shapes
        // them. The evaluator re-applies a node on every frame its upstream
        // changes, so without this a video under the effect would rebuild the
        // tables per frame for nothing.
        struct DerivedMemo
        {
            std::map<std::vector<double>, std::vector<float>> entries;
            const std::vector<float>& Get(const std::vector<double>& key,
                                          const std::function<std::vector<float>()>& build)
            {
                auto it = entries.find(key);
                if (it != entries.end()) return it->second;
                if (entries.size() >= 16) entries.clear();
                return entries.emplace(key, build()).first->second;
            }
        };

        // ComputeICtCpFitScale from the HLSL: the uniform Ct/Cp scale that
        // brings every source-boundary vertex inside the target boundary.
        template <size_t N>
        double FitScale(const std::array<CM::V2, N>& src, const std::array<CM::V2, N>& tgt)
        {
            double maxRatio = 1.0;
            for (size_t i = 0; i < N; ++i)
            {
                const CM::V2 dir = src[i];
                if (std::hypot(dir.x, dir.y) < 1e-8) continue;
                double bestT = 1e10;
                for (size_t j = 0; j < N; ++j)
                {
                    const CM::V2 a = tgt[j];
                    const CM::V2 b = tgt[(j + 1) % N];
                    const CM::V2 ab{ b.x - a.x, b.y - a.y };
                    const double denom = dir.x * ab.y - dir.y * ab.x;
                    if (std::abs(denom) < 1e-10) continue;
                    const double t = (a.x * ab.y - a.y * ab.x) / denom;
                    const double u = (a.x * dir.y - a.y * dir.x) / denom;
                    if (t > 0.0 && u >= 0.0 && u <= 1.0 && t < bestT) bestT = t;
                }
                if (bestT > 0.0 && bestT < 1e9) maxRatio = (std::max)(maxRatio, 1.0 / bestT);
            }
            return (maxRatio > 1.0) ? 1.0 / maxRatio : 1.0;
        }

        ShaderLabEffectDescriptor::DerivedConstantsFn MakeIctcpGamutMapDerived()
        {
            auto memo = std::make_shared<DerivedMemo>();
            return [memo](const PropMap& p, const ShaderLabEffectDescriptor::DerivedConstantWriter& write)
            {
                const int mode = static_cast<int>(PropNumber(p, L"Mode", 0.0));
                const int tg = static_cast<int>(PropNumber(p, L"TargetGamut", 0.0));
                const auto tgt = CM::GamutPrimaries(tg,
                    PropV2(p, L"TargetRedPrimary",   { 0.64, 0.33 }),
                    PropV2(p, L"TargetGreenPrimary", { 0.30, 0.60 }),
                    PropV2(p, L"TargetBluePrimary",  { 0.15, 0.06 }));
                if (mode == 2)
                {
                    const int sg = static_cast<int>(PropNumber(p, L"SourceGamut", 2.0));
                    const auto src = CM::GamutPrimaries(sg,
                        PropV2(p, L"SourceRedPrimary",   { 0.708, 0.292 }),
                        PropV2(p, L"SourceGreenPrimary", { 0.170, 0.797 }),
                        PropV2(p, L"SourceBluePrimary",  { 0.131, 0.046 }));
                    std::vector<double> key{ 2.0 };
                    for (const auto& v : tgt) { key.push_back(v.x); key.push_back(v.y); }
                    for (const auto& v : src) { key.push_back(v.x); key.push_back(v.y); }
                    const auto& lut = memo->Get(key, [&]
                    {
                        std::vector<float> out(256);
                        std::array<CM::V2, 48> sb{}, tb{};
                        for (size_t k = 0; k < out.size(); ++k)
                        {
                            const double I = static_cast<double>(k) / 255.0;
                            CM::SampleBoundary(src, I, sb);
                            CM::SampleBoundary(tgt, I, tb);
                            out[k] = static_cast<float>(FitScale(sb, tb));
                        }
                        return out;
                    });
                    write(L"FitScaleLut", lut.data(), lut.size());
                }
                else
                {
                    std::vector<double> key{ 0.0 };
                    for (const auto& v : tgt) { key.push_back(v.x); key.push_back(v.y); }
                    const auto& lut = memo->Get(key, [&]
                    {
                        constexpr size_t kLevels = 64;   // POLY_LEVELS in the HLSL
                        std::vector<float> out;
                        out.reserve(kLevels * 48 * 2);
                        std::array<CM::V2, 48> b{};
                        for (size_t lv = 0; lv < kLevels; ++lv)
                        {
                            CM::SampleBoundary(tgt, static_cast<double>(lv) / (kLevels - 1), b);
                            for (const auto& v : b)
                            {
                                out.push_back(static_cast<float>(v.x));
                                out.push_back(static_cast<float>(v.y));
                            }
                        }
                        return out;
                    });
                    write(L"TgtPolyLut", lut.data(), lut.size());
                }
            };
        }

        ShaderLabEffectDescriptor::DerivedConstantsFn MakeIctcpBoundaryDerived()
        {
            auto memo = std::make_shared<DerivedMemo>();
            return [memo](const PropMap& p, const ShaderLabEffectDescriptor::DerivedConstantWriter& write)
            {
                const int tg = static_cast<int>(PropNumber(p, L"TargetGamut", 0.0));
                const auto g = CM::GamutPrimaries(tg,
                    PropV2(p, L"RedPrimary",   { 0.64, 0.33 }),
                    PropV2(p, L"GreenPrimary", { 0.30, 0.60 }),
                    PropV2(p, L"BluePrimary",  { 0.15, 0.06 }));
                const double intensity = PropNumber(p, L"Intensity", 0.5);
                std::vector<double> key{ intensity };
                for (const auto& v : g) { key.push_back(v.x); key.push_back(v.y); }
                const auto& pts = memo->Get(key, [&]
                {
                    // Must match the level order the HLSL colours them in.
                    const double levels[6] = { 0.15, 0.3, 0.45, 0.6, 0.75, intensity };
                    std::vector<float> out;
                    out.reserve(6 * 24 * 2);
                    std::array<CM::V2, 24> b{};
                    for (double lv : levels)
                    {
                        CM::SampleBoundary(g, lv, b);
                        for (const auto& v : b)
                        {
                            out.push_back(static_cast<float>(v.x));
                            out.push_back(static_cast<float>(v.y));
                        }
                    }
                    return out;
                });
                write(L"BndPts", pts.data(), pts.size());
            };
        }
    }

    void ShaderLabEffects::RegisterAll()
    {
        const auto& colorMath = GetColorMathHLSL();

        // ---- Luminance Heatmap ----
        // D3D11 compute -- MinNits / MaxNits gpuBindable.
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Luminance Heatmap";
            desc.effectId = L"Luminance Heatmap"; desc.effectVersion = 3;
            desc.category = L"Analysis";
            desc.subcategory = L"Highlights";
            desc.shaderType = Graph::CustomShaderType::D3D11ComputeShader;
            desc.hasImageOutput = true;
            desc.threadGroupX = 8;
            desc.threadGroupY = 8;
            desc.threadGroupZ = 1;
            desc.hlslSource = colorMath + s_luminanceHeatmapHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                Graph::ParameterDefinition{ L"MinNits",      L"float",     0.0f,    0.0f, 10000.0f,    1.0f, {}, L"", true },
                Graph::ParameterDefinition{ L"MaxNits",      L"float", 10000.0f,    0.0f, 10000.0f,  100.0f, {}, L"", true },
                Graph::ParameterDefinition{ L"ColormapMode", L"float",     0.0f,    0.0f,     1.0f,    1.0f, { L"Turbo", L"Inferno" } },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Gamut Highlight ----
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Gamut Highlight";
            desc.effectId = L"Gamut Highlight"; desc.effectVersion = 5;
            desc.category = L"Analysis";
            desc.subcategory = L"Highlights";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + s_outOfGamutHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"TargetGamut",     L"float", 0.0f, 0.0f, 3.0f, 1.0f, { L"Rec.709", L"DCI-P3", L"Rec.2020", L"Custom" } },
                { L"OverlayR",        L"float", 1.0f,  0.0f, 1.0f, 0.01f },
                { L"OverlayG",        L"float", 0.0f,  0.0f, 1.0f, 0.01f },
                { L"OverlayB",        L"float", 1.0f,  0.0f, 1.0f, 0.01f },
                { L"OverlayStrength", L"float", 0.7f,  0.0f, 1.0f, 0.01f },
                { L"Mode",            L"float", 0.0f,  0.0f, 1.0f, 1.0f, { L"Out-of-Gamut", L"In-Gamut" } },
                // Custom-mode primaries (sRGB/Rec.709 D65 defaults). Bind
                // these to `Working Space.RedPrimary` etc. for monitor-matched
                // analysis. Hidden in the Properties panel and graph node
                // until TargetGamut is set to "Custom".
                { L"RedPrimary",   L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.64f, 0.33f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
                { L"GreenPrimary", L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.30f, 0.60f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
                { L"BluePrimary",  L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.15f, 0.06f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Luminance Highlight ----
        // Range-based companion to Gamut Highlight: tints pixels whose
        // luminance (in nits) falls outside the active nit range. The Mode
        // toggle inverts the test so the user can isolate either the
        // out-of-range or in-range pixels. TargetRange picks among common
        // HDR presets or a user-specified Custom range. For monitor-matched
        // analysis, set TargetRange to "Custom" and bind MinNits/MaxNits to
        // `Working Space.MinNits` / `Working Space.PeakNits`.
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Luminance Highlight";
            desc.effectId = L"Luminance Highlight"; desc.effectVersion = 6;
            desc.category = L"Analysis";
            desc.subcategory = L"Highlights";
            desc.shaderType = Graph::CustomShaderType::D3D11ComputeShader;
            desc.hasImageOutput = true;
            desc.threadGroupX = 8;
            desc.threadGroupY = 8;
            desc.threadGroupZ = 1;
            desc.hlslSource = colorMath + s_luminanceHighlightHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"TargetRange",     L"float", 0.0f,   0.0f, 5.0f, 1.0f,
                    { L"SDR (0-80)", L"HDR 400", L"HDR 1000", L"HDR 4000",
                      L"HDR 10000", L"Custom" } },
                Graph::ParameterDefinition{ L"MinNits",   L"float",    0.0f, 0.0f, 10000.0f,  1.0f, {}, L"TargetRange == 5", true },
                Graph::ParameterDefinition{ L"MaxNits",   L"float", 1000.0f, 0.0f, 10000.0f, 10.0f, {}, L"TargetRange == 5", true },
                { L"OverlayR",        L"float", 1.0f,   0.0f, 1.0f, 0.01f },
                { L"OverlayG",        L"float", 0.0f,   0.0f, 1.0f, 0.01f },
                { L"OverlayB",        L"float", 1.0f,   0.0f, 1.0f, 0.01f },
                { L"OverlayStrength", L"float", 0.7f,   0.0f, 1.0f, 0.01f },
                { L"Mode",            L"float", 0.0f,   0.0f, 1.0f, 1.0f, { L"Out-of-Range", L"In-Range" } },
            };
            m_effects.push_back(std::move(desc));
        }

        // Future effects will be added here as they're implemented.

        // ---- CIE Histogram (D3D11 Compute) ----
        // Builds a 2D scatter histogram of source image chromaticity in CIE xy space.
        // Output: a 2D texture where R = log2(count+1) normalized, usable as input
        // to the CIE Chromaticity Plot pixel shader.
        {
            static const std::string cieHistHLSL = R"HLSL(
// CIE xy Histogram: scatter source pixels into a 2D CIE xy histogram.
// Uses groupshared tiled accumulation to avoid data races.
// Output R: log-scaled density, G: avg luminance / 10000, A: 1 if hit.
//
// Multi-group (SHADERLAB_REDUCE_SCRATCH): SHADERLAB_REDUCE_GROUPS groups each
// scatter a share of the pixels into their own groupshared tile and write it
// to scratch as a partial; the last group to finish sums the partials and
// renders the diagram. It used to be one group scattering the whole frame.

#include "shaderlab_params.hlsli"

Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Output : register(u1);
SHADERLAB_REDUCE_SCRATCH

cbuffer Constants : register(b0) {
    uint Width;
    uint Height;
    uint OutputSize;
};

float3 ScRGBToXYZ(float3 rgb) {
    return float3(
        0.4123908 * rgb.r + 0.3575843 * rgb.g + 0.1804808 * rgb.b,
        0.2126390 * rgb.r + 0.7151687 * rgb.g + 0.0721923 * rgb.b,
        0.0193308 * rgb.r + 0.1191950 * rgb.g + 0.9505322 * rgb.b
    );
}

static const float2 CENTER = float2(0.3127, 0.3290);
static const float HALF_EXTENT = 0.50;

// Tile-based groupshared histogram.
// 48x48 = 2304 bins, well within 32KB groupshared limit.
#define TILE_SIZE 48
#define TILE_BINS (TILE_SIZE * TILE_SIZE)
// Scratch words: per-group [counts | lumSum] blocks.
#define PART_BASE   SHADERLAB_REDUCE_CLEARED
#define PART_STRIDE (2 * TILE_BINS)
groupshared uint gs_counts[TILE_BINS];
groupshared float gs_lumSum[TILE_BINS];
groupshared uint gs_maxCount;

[numthreads(32, 32, 1)]
void main(uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID) {
    uint tid = GTid.x + GTid.y * 32;
    uint g = Gid.x;
    uint totalThreads = 1024;
    uint totalPixels = Width * Height;
    uint outSize = max(OutputSize, 64u);

    // Clear groupshared tile.
    for (uint ti = tid; ti < TILE_BINS; ti += totalThreads) {
        gs_counts[ti] = 0;
        gs_lumSum[ti] = 0.0;
    }
    GroupMemoryBarrierWithGroupSync();

    // This group's share of the source pixels, scattered into its tile.
    for (uint pi = g * totalThreads + tid; pi < totalPixels;
         pi += totalThreads * SHADERLAB_REDUCE_GROUPS) {
        uint px = pi % Width;
        uint py = pi / Width;
        float4 src = Source[int2(px, py)];
        if (src.a < 0.01) continue;

        float3 xyz = ScRGBToXYZ(src.rgb);  // preserve negatives for wide-gamut chromaticity
        float sum = xyz.x + xyz.y + xyz.z;
        if (sum < 1e-7) continue;

        float cieX = xyz.x / sum;
        float cieY = xyz.y / sum;

        float u = (cieX - CENTER.x) / (2.0 * HALF_EXTENT) + 0.5;
        float v = 0.5 - (cieY - CENTER.y) / (2.0 * HALF_EXTENT);
        if (u < 0 || u >= 1 || v < 0 || v >= 1) continue;

        uint tx = min((uint)(u * TILE_SIZE), TILE_SIZE - 1);
        uint ty = min((uint)(v * TILE_SIZE), TILE_SIZE - 1);
        uint idx = ty * TILE_SIZE + tx;

        InterlockedAdd(gs_counts[idx], 1u);
        // Luminance accumulation has minor races but acceptable for avg.
        gs_lumSum[idx] += xyz.y * 80.0 / 10000.0;
    }
    GroupMemoryBarrierWithGroupSync();

    uint pb = PART_BASE + g * PART_STRIDE;
    for (uint wi = tid; wi < TILE_BINS; wi += totalThreads) {
        ShaderLabScratchStore(pb + wi, gs_counts[wi]);
        ShaderLabScratchStore(pb + TILE_BINS + wi, asuint(gs_lumSum[wi]));
    }

    if (!ShaderLabReduceIsLastGroup(tid))
        return;

    // Last group: sum every group's tile.
    for (uint si = tid; si < TILE_BINS; si += totalThreads) {
        uint c = 0;
        float l = 0.0;
        for (uint gg = 0; gg < SHADERLAB_REDUCE_GROUPS; ++gg) {
            uint b2 = PART_BASE + gg * PART_STRIDE;
            c += ShaderLabScratchLoad(b2 + si);
            l += asfloat(ShaderLabScratchLoad(b2 + TILE_BINS + si));
        }
        gs_counts[si] = c;
        gs_lumSum[si] = l;
    }

    // Find max count for log normalization.
    if (tid == 0) gs_maxCount = 1;
    GroupMemoryBarrierWithGroupSync();
    for (uint mi = tid; mi < TILE_BINS; mi += totalThreads) {
        InterlockedMax(gs_maxCount, gs_counts[mi]);
    }
    GroupMemoryBarrierWithGroupSync();

    float logMax = log2(float(gs_maxCount) + 1.0);

    // Write every output pixel (empty bins as 0), upscaling the tile.
    float scale = float(outSize) / float(TILE_SIZE);
    for (uint oi = tid; oi < outSize * outSize; oi += totalThreads) {
        uint ox = oi % outSize;
        uint oy = oi / outSize;
        uint tx = min((uint)(float(ox) / scale), TILE_SIZE - 1);
        uint ty = min((uint)(float(oy) / scale), TILE_SIZE - 1);
        uint idx = ty * TILE_SIZE + tx;
        uint cnt = gs_counts[idx];
        float4 o = float4(0, 0, 0, 0);
        if (cnt > 0) {
            float logDensity = log2(float(cnt) + 1.0) / logMax;
            float avgLum = gs_lumSum[idx] / float(cnt);
            o = float4(logDensity, avgLum, 0, 1.0);
        }
        Output[int2(ox, oy)] = o;
    }
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"CIE Histogram";
            desc.effectId = L"CIE Histogram"; desc.effectVersion = 3;
            desc.category = L"Analysis";
            desc.subcategory = L"Scopes";
            desc.shaderType = Graph::CustomShaderType::D3D11ComputeShader;
            desc.hlslSource = cieHistHLSL;
            desc.inputNames = { L"Source" };
            desc.hasImageOutput = true;
            desc.parameters = {
                { L"OutputSize", L"uint", 512.0f, 64.0f, 2048.0f, 64.0f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- CIE Chromaticity Plot ----
        {
            static const std::string ciePlotHLSL = R"HLSL(
// CIE 1931 xy Chromaticity Diagram
// Renders the spectral locus horseshoe with gamut triangle overlays.
// Input 0: CIE Histogram texture (from CIE Histogram compute effect).
//   Each pixel = log-normalized scatter density in CIE xy space.
Texture2D Histogram : register(t0);
SamplerState Sampler0 : register(s0);

cbuffer constants : register(b0) {
    uint ShowRec709;    // 1=show gamut triangle
    uint ShowP3;        // 1=show gamut triangle
    uint ShowRec2020;   // 1=show gamut triangle
    float Brightness;   // scatter dot brightness (default 2.0)
    float DiagramSize;  // output size in pixels (default 512)
    uint ShowMonitor;   // 1=show "monitor" gamut triangle drawn from
                        // RedPrimary/GreenPrimary/BluePrimary (typically
                        // bound to the Working Space node).
    float2 RedPrimary;
    float2 GreenPrimary;
    float2 BluePrimary;
};

// CIE 1931 2-degree observer spectral locus (sampled at 10nm, 380-700nm)
// 33 xy pairs forming the horseshoe boundary.
static const float2 LOCUS[] = {
    float2(0.1741, 0.0050), // 380nm
    float2(0.1740, 0.0050), // 390nm
    float2(0.1714, 0.0049), // 400nm
    float2(0.1644, 0.0051), // 410nm
    float2(0.1566, 0.0177), // 420nm
    float2(0.1440, 0.0297), // 430nm
    float2(0.1241, 0.0578), // 440nm
    float2(0.0913, 0.1327), // 450nm
    float2(0.0687, 0.2007), // 460nm
    float2(0.0454, 0.2950), // 470nm
    float2(0.0235, 0.4127), // 480nm
    float2(0.0082, 0.5384), // 490nm
    float2(0.0039, 0.6548), // 500nm
    float2(0.0139, 0.7502), // 510nm
    float2(0.0743, 0.8338), // 520nm
    float2(0.1547, 0.8059), // 530nm
    float2(0.2296, 0.7543), // 540nm
    float2(0.3016, 0.6923), // 550nm
    float2(0.3731, 0.6245), // 560nm
    float2(0.4441, 0.5547), // 570nm
    float2(0.5125, 0.4866), // 580nm
    float2(0.5752, 0.4242), // 590nm
    float2(0.6270, 0.3725), // 600nm
    float2(0.6658, 0.3340), // 610nm
    float2(0.6915, 0.3083), // 620nm
    float2(0.7079, 0.2920), // 630nm
    float2(0.7190, 0.2809), // 640nm
    float2(0.7260, 0.2740), // 650nm
    float2(0.7300, 0.2700), // 660nm
    float2(0.7320, 0.2680), // 670nm
    float2(0.7334, 0.2666), // 680nm
    float2(0.7340, 0.2660), // 690nm
    float2(0.7347, 0.2653), // 700nm
};
#define LOCUS_COUNT 33

// Winding-number test: is point p inside the closed spectral locus + purple line?
bool IsInsideLocus(float2 p) {
    int winding = 0;
    // Test against spectral locus segments
    for (uint i = 0; i < LOCUS_COUNT; i++) {
        float2 a = LOCUS[i];
        float2 b = LOCUS[(i + 1) % LOCUS_COUNT];
        // For the last point, close with purple line back to first
        if (i == LOCUS_COUNT - 1) b = LOCUS[0];
        if (a.y <= p.y) {
            if (b.y > p.y) {
                float cross = (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y);
                if (cross > 0) winding++;
            }
        } else {
            if (b.y <= p.y) {
                float cross = (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y);
                if (cross < 0) winding--;
            }
        }
    }
    return winding != 0;
}

// Draw gamut triangle outline
float GamutTriangle(float2 uv, float2 r, float2 g, float2 b, float thickness) {
    float d1 = abs(dot(normalize(float2(-(g.y-r.y), g.x-r.x)), uv - r));
    float d2 = abs(dot(normalize(float2(-(b.y-g.y), b.x-g.x)), uv - g));
    float d3 = abs(dot(normalize(float2(-(r.y-b.y), r.x-b.x)), uv - b));

    // Check if on the edge (between vertices)
    float t1 = dot(uv - r, g - r) / dot(g - r, g - r);
    float t2 = dot(uv - g, b - g) / dot(b - g, b - g);
    float t3 = dot(uv - b, r - b) / dot(r - b, r - b);

    float edge = 1e10;
    if (t1 >= 0 && t1 <= 1) edge = min(edge, d1);
    if (t2 >= 0 && t2 <= 1) edge = min(edge, d2);
    if (t3 >= 0 && t3 <= 1) edge = min(edge, d3);

    return smoothstep(thickness, 0.0, edge);
}

// Uses the scene coordinate for diagram geometry and derives its own histogram
// texel coords via GetDimensions(), so it never wants TEXCOORD0. Declared
// explicitly rather than relying on the missing-SCENE_POSITION accident.
float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : SCENE_POSITION) : SV_TARGET
{
    // Map pixel position to CIE xy space
    // x: 0 to 0.8, y: 0 to 0.9 (standard diagram range)
    float2 pixPos = uv0.xy;
    float size = max(DiagramSize, 128.0);
    float2 xy = float2(pixPos.x / size * 0.8, (1.0 - pixPos.y / size) * 0.9);

    float4 result = float4(0.0, 0.0, 0.0, 1.0); // black background

    // Render the visible gamut region with approximate spectral colors
    if (IsInsideLocus(xy)) {
        float3 xyY = float3(xy.x, xy.y, 0.5);
        float3 xyz = xyYToXYZ(xyY);
        float3 rgb = XYZToScRGB(xyz);
        // Normalize brightness, clamp negatives to show the full visible gamut
        // (not just Rec.709). Out-of-709 colors are desaturated toward white.
        float maxC = max(max(abs(rgb.r), abs(rgb.g)), max(abs(rgb.b), 0.001));
        rgb = rgb / maxC * 0.15 * Brightness;
        rgb = max(rgb, 0.0);
        result.rgb = rgb;
    }

    // Gamut triangles
    float thickness = 0.003;
    float lineBright = 0.4 * Brightness;
    if (ShowRec709 > 0.5) {
        float e = GamutTriangle(xy, GAMUT_709_R, GAMUT_709_G, GAMUT_709_B, thickness);
        result.rgb = lerp(result.rgb, float3(1,1,1) * lineBright, e * 0.8);
    }
    if (ShowP3 > 0.5) {
        float e = GamutTriangle(xy, GAMUT_P3_R, GAMUT_P3_G, GAMUT_P3_B, thickness);
        result.rgb = lerp(result.rgb, float3(0,1,0) * lineBright, e * 0.8);
    }
    if (ShowRec2020 > 0.5) {
        float e = GamutTriangle(xy, GAMUT_2020_R, GAMUT_2020_G, GAMUT_2020_B, thickness);
        result.rgb = lerp(result.rgb, float3(0,0.5,1) * lineBright, e * 0.8);
    }
    if (ShowMonitor > 0.5) {
        float2 mR = RedPrimary;
        float2 mG = GreenPrimary;
        float2 mB = BluePrimary;
        float e = GamutTriangle(xy, mR, mG, mB, thickness);
        result.rgb = lerp(result.rgb, float3(1, 0.8, 0) * lineBright, e * 0.9);
    }

    // D65 white point marker
    float dw = length(xy - D65_WHITE);
    if (dw < 0.008) result.rgb = float3(0.5, 0.5, 0.5) * Brightness;

    // Read scatter density from pre-computed histogram texture.
    // The histogram uses D65-centered coordinates:
    // u = (x - 0.3127) / 1.0 + 0.5, v = 0.5 - (y - 0.3290) / 1.0
    float histU = (xy.x - 0.3127) / 1.0 + 0.5;
    float histV = 0.5 - (xy.y - 0.3290) / 1.0;
    if (histU >= 0.0 && histU <= 1.0 && histV >= 0.0 && histV <= 1.0) {
        uint histW, histH;
        Histogram.GetDimensions(histW, histH);
        float4 histVal = Histogram.Load(int3(
            int(histU * float(histW)),
            int(histV * float(histH)), 0));
        float intensity = histVal.r;
        if (intensity > 0.0) {
            result.rgb += intensity * Brightness * 0.3 * float3(1, 1, 1);
        }
    }

    return result;
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"CIE Chromaticity Plot";
            desc.effectId = L"CIE Chromaticity Plot"; desc.effectVersion = 6;
            desc.category = L"Analysis";
            desc.subcategory = L"Scopes";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + ciePlotHLSL;
            desc.inputNames = { L"Histogram" };
            desc.parameters = {
                { L"ShowRec709",   L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"Hide", L"Show" } },
                { L"ShowP3",       L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"Hide", L"Show" } },
                { L"ShowRec2020",  L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"Hide", L"Show" } },
                { L"Brightness",   L"float", 2.0f,  0.1f, 10.0f, 0.1f },
                { L"DiagramSize",  L"float", kDefaultDiagramSize, 128.0f, 4096.0f, 64.0f },
                // Custom-primary triangle ("monitor" gamut). Bind these to
                // `Working Space.RedPrimary` etc. for monitor-matched plotting.
                { L"ShowMonitor",  L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"Hide", L"Show" } },
                { L"RedPrimary",   L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.64f, 0.33f }, 0.0f, 1.0f, 0.001f, {}, L"ShowMonitor == 1" },
                { L"GreenPrimary", L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.30f, 0.60f }, 0.0f, 1.0f, 0.001f, {}, L"ShowMonitor == 1" },
                { L"BluePrimary",  L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.15f, 0.06f }, 0.0f, 1.0f, 0.001f, {}, L"ShowMonitor == 1" },
            };
            m_effects.push_back(std::move(desc));
        }



        // ---- Gamut Source ----
        {
            static const std::string gamutSourceHLSL = R"HLSL(
// Gamut Source - generates all colors within a selected color gamut
// Fixed coordinate system centered on D65 white point. Scale fits Rec.2020
// so all three gamuts share the same spatial mapping.
//
// Gamut modes:
//   0 = Rec.709
//   1 = DCI-P3
//   2 = Rec.2020
//   3 = Custom  (uses RedPrimary/GreenPrimary/BluePrimary; bind to
//                Working Space.RedPrimary etc. for a monitor-matched source.)

cbuffer constants : register(b0) {
    float Gamut;
    float Luminance;    // nits (default 80.0, maps to scRGB 1.0)
    float OutputSize;   // pixels (default 1024)
    float2 RedPrimary;
    float2 GreenPrimary;
    float2 BluePrimary;
};

// Source generator: no input to sample, so what this shader wants IS the
// scene coordinate. Declared explicitly -- it previously got the same value
// by omitting SCENE_POSITION and letting it bind to a parameter labelled
// TEXCOORD0, which is the trap that displaced every input-sampling effect.
float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : SCENE_POSITION) : SV_TARGET
{
    float size = max(OutputSize, 128.0);

    // Read all cbuffer vars at top to keep DXC from optimizing them out.
    float gamut = Gamut;
    float2 cR = RedPrimary;
    float2 cG = GreenPrimary;
    float2 cB = BluePrimary;

    // Select gamut primaries
    float2 r, g, b;
    if (gamut > 2.5)     { r = cR; g = cG; b = cB; }
    else if (gamut > 1.5){ r = GAMUT_2020_R; g = GAMUT_2020_G; b = GAMUT_2020_B; }
    else if (gamut > 0.5){ r = GAMUT_P3_R;   g = GAMUT_P3_G;   b = GAMUT_P3_B; }
    else                 { r = GAMUT_709_R;  g = GAMUT_709_G;  b = GAMUT_709_B; }

    float2 center = D65_WHITE;
    float halfExtent = 0.50;

    float2 uv = uv0.xy / size;
    float2 xy;
    xy.x = center.x + (uv.x - 0.5) * 2.0 * halfExtent;
    xy.y = center.y - (uv.y - 0.5) * 2.0 * halfExtent;

    if (!PointInTriangle(xy, r, g, b))
        return float4(0, 0, 0, 1.0);

    float Y = Luminance / 80.0;
    float3 xyY_val = float3(xy.x, xy.y, Y);
    float3 xyz = xyYToXYZ(xyY_val);
    float3 rgb = XYZToScRGB(xyz);

    return float4(rgb, 1.0);
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Gamut Source";
            desc.effectId = L"Gamut Source"; desc.effectVersion = 4;
            desc.category = L"Source";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + gamutSourceHLSL;
            desc.inputNames = {};
            desc.parameters = {
                { L"Gamut",      L"float", 0.0f, 0.0f, 3.0f, 1.0f, { L"Rec.709", L"DCI-P3", L"Rec.2020", L"Custom" } },
                { L"Luminance",  L"float", 80.0f, 0.01f, 10000.0f, 10.0f },
                { L"OutputSize", L"float", kDefaultDiagramSize, 128.0f, 4096.0f, 64.0f },
                { L"RedPrimary",   L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.64f, 0.33f }, 0.0f, 1.0f, 0.001f, {}, L"Gamut == 3" },
                { L"GreenPrimary", L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.30f, 0.60f }, 0.0f, 1.0f, 0.001f, {}, L"Gamut == 3" },
                { L"BluePrimary",  L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.15f, 0.06f }, 0.0f, 1.0f, 0.001f, {}, L"Gamut == 3" },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Color Checker (Source) ----
        {
            static const std::string colorCheckerHLSL = R"HLSL(
// Macbeth ColorChecker - 24 reference patches in scRGB
// Source effect: no input required.

cbuffer constants : register(b0) {
    float PatchSize; // pixels per patch (default 64)
};

// 24 ColorChecker patches in linear sRGB (scRGB)
// Row 1: Dark Skin, Light Skin, Blue Sky, Foliage, Blue Flower, Bluish Green
// Row 2: Orange, Purplish Blue, Moderate Red, Purple, Yellow Green, Orange Yellow
// Row 3: Blue, Green, Red, Yellow, Magenta, Cyan
// Row 4: White, Neutral 8, Neutral 6.5, Neutral 5, Neutral 3.5, Black
static const float3 PATCHES[24] = {
    float3(0.0451, 0.0225, 0.0140),  // Dark Skin
    float3(0.3005, 0.1947, 0.1369),  // Light Skin
    float3(0.0600, 0.0962, 0.1622),  // Blue Sky
    float3(0.0300, 0.0410, 0.0130),  // Foliage
    float3(0.1118, 0.1018, 0.2218),  // Blue Flower
    float3(0.0940, 0.2560, 0.2180),  // Bluish Green
    float3(0.3270, 0.0935, 0.0100),  // Orange
    float3(0.0260, 0.0438, 0.2040),  // Purplish Blue
    float3(0.2180, 0.0360, 0.0310),  // Moderate Red
    float3(0.0280, 0.0130, 0.0510),  // Purple
    float3(0.1940, 0.2630, 0.0220),  // Yellow Green
    float3(0.3800, 0.1620, 0.0100),  // Orange Yellow
    float3(0.0100, 0.0180, 0.1470),  // Blue
    float3(0.0310, 0.1080, 0.0140),  // Green
    float3(0.1630, 0.0120, 0.0070),  // Red
    float3(0.4590, 0.3820, 0.0220),  // Yellow
    float3(0.2040, 0.0380, 0.1110),  // Magenta
    float3(0.0230, 0.1310, 0.2160),  // Cyan
    float3(0.8740, 0.8740, 0.8740),  // White
    float3(0.5100, 0.5100, 0.5100),  // Neutral 8
    float3(0.3040, 0.3040, 0.3040),  // Neutral 6.5
    float3(0.1610, 0.1610, 0.1610),  // Neutral 5
    float3(0.0680, 0.0680, 0.0680),  // Neutral 3.5
    float3(0.0210, 0.0210, 0.0210),  // Black
};

// Source generator: no input to sample, so what this shader wants IS the
// scene coordinate. Declared explicitly -- it previously got the same value
// by omitting SCENE_POSITION and letting it bind to a parameter labelled
// TEXCOORD0, which is the trap that displaced every input-sampling effect.
float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : SCENE_POSITION) : SV_TARGET
{
    float ps = max(PatchSize, 16.0);
    float2 pixPos = uv0.xy;
    uint col = (uint)(pixPos.x / ps);
    uint row = (uint)(pixPos.y / ps);

    if (col >= 6 || row >= 4)
        return float4(0.15, 0.15, 0.15, 1.0);

    uint idx = row * 6 + col;
    if (idx >= 24)
        return float4(0.15, 0.15, 0.15, 1.0);

    // Add thin border between patches
    float2 inPatch = fmod(pixPos, ps);
    if (inPatch.x < 1.0 || inPatch.y < 1.0)
        return float4(0.06, 0.06, 0.06, 1.0);

    return float4(PATCHES[idx], 1.0);
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Color Checker";
            desc.effectId = L"Color Checker"; desc.effectVersion = 2;
            desc.category = L"Source";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + colorCheckerHLSL;
            desc.inputNames = {};
            desc.parameters = {
                { L"PatchSize", L"float", 64.0f, 16.0f, 256.0f, 8.0f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Zone Plate (Source) ----
        {
            static const std::string zonePlateHLSL = R"HLSL(
// Zone Plate - circular resolution/aliasing test pattern
// Source effect: no input required.

cbuffer constants : register(b0) {
    float Frequency;   // default 0.5
    float PlateSize;   // pixels (default 1024)
};

// Source generator: no input to sample, so what this shader wants IS the
// scene coordinate. Declared explicitly -- it previously got the same value
// by omitting SCENE_POSITION and letting it bind to a parameter labelled
// TEXCOORD0, which is the trap that displaced every input-sampling effect.
float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : SCENE_POSITION) : SV_TARGET
{
    float size = max(PlateSize, 64.0);
    float2 center = float2(size * 0.5, size * 0.5);
    float2 p = uv0.xy - center;
    float r2 = dot(p, p);
    float v = 0.5 + 0.5 * cos(Frequency * r2 / size);
    return float4(v, v, v, 1.0);
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Zone Plate";
            desc.effectId = L"Zone Plate"; desc.effectVersion = 2;
            desc.category = L"Source";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + zonePlateHLSL;
            desc.inputNames = {};
            desc.parameters = {
                { L"Frequency", L"float", 0.5f, 0.01f, 5.0f, 0.01f },
                { L"PlateSize", L"float", kDefaultDiagramSize, 64.0f, 2048.0f, 64.0f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Gradient Generator (Source) ----
        {
            static const std::string gradientHLSL = R"HLSL(
// Gradient Generator - linear/radial gradients with HDR support
// Source effect: no input required.

cbuffer constants : register(b0) {
    uint GradientType;  // 0=Linear horizontal, 1=Linear vertical, 2=Radial
    float StartR;   // start color (scRGB)
    float StartG;
    float StartB;
    float EndR;     // end color (scRGB)
    float EndG;
    float EndB;
    float GradSize; // pixels (default 1024)
};

// Source generator: no input to sample, so what this shader wants IS the
// scene coordinate. Declared explicitly -- it previously got the same value
// by omitting SCENE_POSITION and letting it bind to a parameter labelled
// TEXCOORD0, which is the trap that displaced every input-sampling effect.
float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : SCENE_POSITION) : SV_TARGET
{
    float size = max(GradSize, 64.0);
    float t = 0;

    if (GradientType < 0.5)
        t = saturate(uv0.x / size);
    else if (GradientType < 1.5 && GradientType > 0.5)
        t = saturate(uv0.y / size);
    else {
        float2 center = float2(size * 0.5, size * 0.5);
        float r = length(uv0.xy - center) / (size * 0.5);
        t = saturate(r);
    }

    float3 startC = float3(StartR, StartG, StartB);
    float3 endC = float3(EndR, EndG, EndB);
    float3 color = lerp(startC, endC, t);
    return float4(color, 1.0);
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Gradient Generator";
            desc.effectId = L"Gradient Generator"; desc.effectVersion = 3;
            desc.category = L"Source";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + gradientHLSL;
            desc.inputNames = {};
            desc.parameters = {
                { L"GradientType", L"float", 0.0f, 0.0f, 2.0f, 1.0f, { L"Linear Horizontal", L"Linear Vertical", L"Radial" } },
                { L"StartR",       L"float", 0.0f, -1.0f, 125.0f, 0.01f },
                { L"StartG",       L"float", 0.0f, -1.0f, 125.0f, 0.01f },
                { L"StartB",       L"float", 0.0f, -1.0f, 125.0f, 0.01f },
                { L"EndR",         L"float", 1.0f, -1.0f, 125.0f, 0.01f },
                { L"EndG",         L"float", 1.0f, -1.0f, 125.0f, 0.01f },
                { L"EndB",         L"float", 1.0f, -1.0f, 125.0f, 0.01f },
                { L"GradSize",     L"float", kDefaultDiagramSize, 64.0f, 2048.0f, 64.0f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- HDR Test Pattern (Source) ----
        {
            static const std::string hdrTestHLSL = R"HLSL(
// HDR Test Pattern - standard patches at known luminance levels
// Source effect: no input required.

cbuffer constants : register(b0) {
    float PatternSize; // pixels (default 1024)
};

// Source generator: no input to sample, so what this shader wants IS the
// scene coordinate. Declared explicitly -- it previously got the same value
// by omitting SCENE_POSITION and letting it bind to a parameter labelled
// TEXCOORD0, which is the trap that displaced every input-sampling effect.
float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : SCENE_POSITION) : SV_TARGET
{
    float size = max(PatternSize, 256.0);
    float2 uv = uv0.xy / size;

    // 4x4 grid of patches at different luminance levels
    uint col = (uint)(uv.x * 4.0);
    uint row = (uint)(uv.y * 4.0);
    col = min(col, 3u);
    row = min(row, 3u);

    // Nit levels for each patch (scRGB: nits/80)
    // Row 0: 0.1, 1, 10, 80 nits
    // Row 1: 100, 200, 400, 1000 nits
    // Row 2: 2000, 4000, 10000, primaries
    // Row 3: R, G, B, grayscale ramp
    float nitsTable[12] = {
        0.1, 1.0, 10.0, 80.0,
        100.0, 200.0, 400.0, 1000.0,
        2000.0, 4000.0, 10000.0, 0.0
    };

    float3 color = float3(0, 0, 0);
    uint idx = row * 4 + col;

    if (idx < 11) {
        float scrgb = nitsTable[idx] / 80.0;
        color = float3(scrgb, scrgb, scrgb);
    } else if (idx == 11) {
        // Rec.709 red primary at 80 nits
        color = float3(1, 0, 0);
    } else if (idx == 12) {
        // Rec.709 green primary at 80 nits
        color = float3(0, 1, 0);
    } else if (idx == 13) {
        // Rec.709 blue primary at 80 nits
        color = float3(0, 0, 1);
    } else if (idx == 14) {
        // Yellow
        color = float3(1, 1, 0);
    } else {
        // Grayscale ramp within the patch
        float2 inPatch = frac(uv * 4.0);
        float ramp = inPatch.x;
        color = float3(ramp, ramp, ramp);
    }

    // Thin grid lines
    float2 inPatch = frac(uv * 4.0);
    if (inPatch.x < 0.01 || inPatch.y < 0.01)
        color = float3(0.15, 0.15, 0.15);

    return float4(color, 1.0);
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"HDR Test Pattern";
            desc.effectId = L"HDR Test Pattern"; desc.effectVersion = 2;
            desc.category = L"Source";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + hdrTestHLSL;
            desc.inputNames = {};
            desc.parameters = {
                { L"PatternSize", L"float", kDefaultDiagramSize, 256.0f, 2048.0f, 64.0f },
            };
            m_effects.push_back(std::move(desc));
        }


        // ---- Delta E Comparator ----
        {
            static const std::string deltaEHLSL = R"HLSL(
// Delta E Comparator -- D3D11 compute, per-pixel color difference between
// two inputs (Reference at t0, Test at t1). Supports CIE76, CIE94, CIEDE2000
// and dE ITP (BT.2124).
//
// Prefer ITP for anything HDR or wide-gamut: the three Lab metrics were fit
// to reflective samples under SDR viewing and degrade above ~100 nits and
// outside sRGB. One unit is ~1 JND in all four, so the numbers stay
// comparable when switching.

Texture2D<float4>   Reference   : register(t0);
Texture2D<float4>   Test        : register(t1);
RWTexture2D<float4> ImageOutput : register(u1);

cbuffer Constants : register(b0)
{
    uint  Width;
    uint  Height;
    uint  Method;        // 0 = CIE76, 1 = CIE94, 2 = CIEDE2000, 3 = dE ITP
    float Scale;         // visualization multiplier
    float MaxDeltaE;     // clamp for colormap (dE >= this = full red)
    uint  OutputMode;    // 0 = Heatmap (Turbo), 1 = Grayscale dE / MaxDeltaE
};

// CIE76: simple Euclidean distance in L*a*b*
float DeltaE76(float3 lab1, float3 lab2) {
    float3 d = lab1 - lab2;
    return sqrt(dot(d, d));
}

// CIE94 (graphic arts)
float DeltaE94(float3 lab1, float3 lab2) {
    float dL = lab1.x - lab2.x;
    float C1 = sqrt(lab1.y * lab1.y + lab1.z * lab1.z);
    float C2 = sqrt(lab2.y * lab2.y + lab2.z * lab2.z);
    float dC = C1 - C2;
    float da = lab1.y - lab2.y;
    float db = lab1.z - lab2.z;
    float dH2 = da * da + db * db - dC * dC;
    float dH = sqrt(max(dH2, 0.0));
    float SL = 1.0;
    float SC = 1.0 + 0.045 * C1;
    float SH = 1.0 + 0.015 * C1;
    float t1 = dL / SL;
    float t2 = dC / SC;
    float t3 = dH / SH;
    return sqrt(t1*t1 + t2*t2 + t3*t3);
}

// CIEDE2000
float DeltaE2000(float3 lab1, float3 lab2) {
    float L1 = lab1.x, a1 = lab1.y, b1 = lab1.z;
    float L2 = lab2.x, a2 = lab2.y, b2 = lab2.z;

    float Cab1 = sqrt(a1*a1 + b1*b1);
    float Cab2 = sqrt(a2*a2 + b2*b2);
    float Cab_avg = (Cab1 + Cab2) * 0.5;
    float Cab_avg7 = pow(Cab_avg, 7.0);
    float G = 0.5 * (1.0 - sqrt(Cab_avg7 / (Cab_avg7 + 6103515625.0))); // 25^7

    float a1p = a1 * (1.0 + G);
    float a2p = a2 * (1.0 + G);
    float C1p = sqrt(a1p*a1p + b1*b1);
    float C2p = sqrt(a2p*a2p + b2*b2);

    // Hue angles. atan2(0, 0) is implementation-defined and on some
    // drivers returns NaN — guard against (a,b)==(0,0) by treating C==0
    // as h==0 (the hue angle of an achromatic point is meaningless and
    // any consistent value works because the C-weighted hp_avg branch
    // below short-circuits to h1p+h2p).
    float h1p = (C1p < 1e-10) ? 0.0 : atan2(b1, a1p);
    if (h1p < 0.0) h1p += 6.28318530718;
    float h2p = (C2p < 1e-10) ? 0.0 : atan2(b2, a2p);
    if (h2p < 0.0) h2p += 6.28318530718;

    float dLp = L2 - L1;
    float dCp = C2p - C1p;

    float dhp;
    if (C1p * C2p < 1e-10) dhp = 0.0;
    else {
        dhp = h2p - h1p;
        if (dhp > 3.14159265359) dhp -= 6.28318530718;
        else if (dhp < -3.14159265359) dhp += 6.28318530718;
    }
    float dHp = 2.0 * sqrt(C1p * C2p) * sin(dhp * 0.5);

    float Lp_avg = (L1 + L2) * 0.5;
    float Cp_avg = (C1p + C2p) * 0.5;

    float hp_avg;
    if (C1p * C2p < 1e-10) hp_avg = h1p + h2p;
    else {
        hp_avg = (h1p + h2p) * 0.5;
        if (abs(h1p - h2p) > 3.14159265359) hp_avg += 3.14159265359;
    }

    float T = 1.0
        - 0.17 * cos(hp_avg - 0.52359877559)       // 30 deg
        + 0.24 * cos(2.0 * hp_avg)
        + 0.32 * cos(3.0 * hp_avg + 0.10471975512)  // 6 deg
        - 0.20 * cos(4.0 * hp_avg - 1.09955742876);  // 63 deg

    float Lp50sq = (Lp_avg - 50.0) * (Lp_avg - 50.0);
    float SL = 1.0 + 0.015 * Lp50sq / sqrt(20.0 + Lp50sq);
    float SC = 1.0 + 0.045 * Cp_avg;
    float SH = 1.0 + 0.015 * Cp_avg * T;

    float Cp_avg7 = pow(Cp_avg, 7.0);
    float RC = 2.0 * sqrt(Cp_avg7 / (Cp_avg7 + 6103515625.0));
    float hp_deg = hp_avg * 57.29577951; // rad to deg
    float dtheta = 30.0 * exp(-((hp_deg - 275.0) / 25.0) * ((hp_deg - 275.0) / 25.0));
    float RT = -sin(2.0 * dtheta * 0.01745329252) * RC;

    float t1 = dLp / SL;
    float t2 = dCp / SC;
    float t3 = dHp / SH;
    return sqrt(t1*t1 + t2*t2 + t3*t3 + RT * t2 * t3);
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= Width || dtid.y >= Height) return;

    float4 ref  = Reference.Load(int3(dtid.xy, 0));
    float4 test = Test.Load(int3(dtid.xy, 0));

    float dE;
    uint method = Method;
    if (method == 3)
    {
        // dE ITP works in PQ-encoded ICtCp, not Lab -- no XYZ->Lab hop.
        dE = DeltaEITPFromScRGB(ref.rgb, test.rgb);
    }
    else
    {
        float3 labRef  = ScRGBToLab(ref.rgb);
        float3 labTest = ScRGBToLab(test.rgb);
        if (method == 1)      dE = DeltaE94(labRef, labTest);
        else if (method == 2) dE = DeltaE2000(labRef, labTest);
        else                  dE = DeltaE76(labRef, labTest);
    }

    dE *= Scale;
    float mode = OutputMode;
    float t = saturate(dE / max(MaxDeltaE, 0.01));

    float4 outColor;
    if (mode > 0.5)
    {
        // Grayscale dE: a downstream Luminance Statistics node yields
        // live mean/p95/max dE values without needing CPU readback of
        // every pixel.
        outColor = float4(t, t, t, 1.0);
    }
    else
    {
        float3 color = TurboColormap(t) * smoothstep(0.0, 0.02, t);
        outColor = float4(color, 1.0);
    }
    ImageOutput[dtid.xy] = outColor;
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Delta E Comparator";
            desc.effectId = L"Delta E Comparator"; desc.effectVersion = 7;
            desc.category = L"Analysis";
            desc.subcategory = L"Comparison";
            desc.shaderType = Graph::CustomShaderType::D3D11ComputeShader;
            desc.hasImageOutput = true;
            desc.threadGroupX = 8;
            desc.threadGroupY = 8;
            desc.threadGroupZ = 1;
            desc.hlslSource = colorMath + deltaEHLSL;
            desc.inputNames = { L"Reference", L"Test" };
            desc.parameters = {
                // Default is dE ITP: this pipeline is HDR/WCG, where the three
                // Lab metrics are out of their fitted domain. Saved graphs keep
                // whatever Method they stored, so only new nodes pick it up.
                { L"Method",     L"float", 3.0f, 0.0f, 3.0f, 1.0f, { L"CIE76", L"CIE94", L"CIEDE2000", L"dE ITP (BT.2124)" } },
                { L"Scale",      L"float", 1.0f, 0.1f, 10.0f, 0.1f },
                { L"MaxDeltaE",  L"float", 1.0f, 0.1f, 100.0f, 0.1f },
                { L"OutputMode", L"float", 0.0f, 0.0f, 1.0f, 1.0f, { L"Heatmap", L"Grayscale dE" } },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- False Color Luminance Map ----
        {
            static const std::string falseColorHLSL = R"HLSL(
// False Color Luminance Map - maps nit ranges to distinct colors

cbuffer Constants : register(b0)
{
    float Opacity;  // Blend with original (1.0 = full false color)
};

Texture2D Source : register(t0);
SamplerState InputSampler : register(s0);

float3 FalseColor(float nits) {
    // Purple < 0.5 < Blue < 5 < Cyan < 20 < Green < 100 < Yellow < 400 < Orange < 1000 < Red < 4000 < White
    if (nits < 0.5)   return float3(0.2, 0.0, 0.4);    // Purple - crushed blacks
    if (nits < 5.0)   return float3(0.0, 0.0, 0.8);    // Blue - deep shadows
    if (nits < 20.0)  return float3(0.0, 0.6, 0.8);    // Cyan - shadow detail
    if (nits < 100.0) return float3(0.0, 0.7, 0.0);    // Green - mid-tones (SDR)
    if (nits < 400.0) return float3(0.9, 0.9, 0.0);    // Yellow - highlights
    if (nits < 1000.0) return float3(1.0, 0.5, 0.0);   // Orange - HDR specular
    if (nits < 4000.0) return float3(1.0, 0.0, 0.0);   // Red - bright HDR
    return float3(1.0, 1.0, 1.0);                       // White - peak HDR
}

float4 main(
    float4 pos      : SV_POSITION,
    float4 scenePos : SCENE_POSITION,
    float4 uv0      : TEXCOORD0
) : SV_Target
{
    // SCENE_POSITION is NOT optional -- D2D's pixel-shader input signature is
    // (SV_POSITION, SCENE_POSITION, TEXCOORD0..N), and omitting the middle one
    // binds the SCENE coordinate to the parameter named uv0 (see "D2D Custom
    // Effect Gotchas" in .github/copilot-instructions.md). uv0 is NORMALIZED once the signature
    // is right, so the input is read with Sample(), not Load(int3(...)).
    float4 color = Source.Sample(InputSampler, uv0.xy);
    float nits = ScRGBLuminanceNits(color.rgb);
    float3 fc = FalseColor(nits);
    float3 result = lerp(color.rgb, fc, Opacity);
    return float4(result, 1.0);
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Nit Map";
            desc.effectId = L"Nit Map"; desc.effectVersion = 2;
            desc.category = L"Analysis";
            desc.subcategory = L"Highlights";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + falseColorHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"Opacity", L"float", 1.0f, 0.0f, 1.0f, 0.05f },
            };
            m_effects.push_back(std::move(desc));
        }


        // ---- Gamut Volume Coverage ----
        {
            static const std::string gamutCoverageHLSL = R"HLSL(
// Gamut Volume Coverage - visualizes which gamut regions the source occupies.
// Migrated to D3D11 compute (Phase 8c perf): each thread group scatters all
// source pixels into a 64x64 chromaticity histogram in groupshared, then
// renders the 512x512 output diagram + triangle outline by reading the
// histogram. Total cost is O(srcW*srcH + outW*outH) -- ~8M ops on a 4K
// source -- vs the previous pixel-shader gather's O(srcSamples * outW*outH)
// which was ~17B ops at the same input.

// Multi-group (SHADERLAB_REDUCE_SCRATCH): SHADERLAB_REDUCE_GROUPS groups each
// scatter a share of the pixels into their groupshared bins, then add them
// into global counters in the scratch head; the last group to finish renders
// the diagram from the totals. Counts only, so global atomics are exact.

#include "shaderlab_params.hlsli"

Texture2D<float4>    Source : register(t0);
RWTexture2D<float4>  Output : register(u1);
SHADERLAB_REDUCE_SCRATCH

cbuffer Constants : register(b0)
{
    uint  Width;            // auto-injected by bridge
    uint  Height;
    float DiagramSize;      // output side length in pixels
    uint  TargetGamut;      // 0=sRGB, 1=DCI-P3, 2=BT.2020, 3=Custom
    float2 RedPrimary;
    float2 GreenPrimary;
    float2 BluePrimary;
};

// Triangle-edge antialiased line: returns 1 inside the line band, 0 outside.
float TriangleEdge(float2 p, float2 a, float2 b, float lineW) {
    float2 ab = b - a;
    float t = saturate(dot(p - a, ab) / dot(ab, ab));
    float d = length(p - a - ab * t);
    return smoothstep(lineW, 0.0, d);
}

// 64x64 chromaticity histogram. 4096 bins x 4 bytes x 2 buffers = 32 KB.
#define BINS 64
// Global counters in the cleared scratch head: word 0 is the reduction's
// completion counter, so the bins start at 16.
#define GLOBAL_IN   16
#define GLOBAL_OUT  (GLOBAL_IN + BINS * BINS)
groupshared uint gs_inGamut[BINS * BINS];
groupshared uint gs_outGamut[BINS * BINS];

[numthreads(32, 32, 1)]
void main(uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID)
{
    uint tid = GTid.x + GTid.y * 32;
    uint g = Gid.x;
    const uint totalThreads = 1024;

    uint outSize = max((uint)DiagramSize, 64u);

    // Read cbuffer (keep all GPU-resident on every code path so DXC
    // doesnt eliminate the slot).
    float gamutF = TargetGamut;
    float2 cR = RedPrimary;
    float2 cG = GreenPrimary;
    float2 cB = BluePrimary;

    // Resolve target gamut vertices once per group.
    float2 tR, tG, tB;
    uint gamut = (uint)gamutF;
    if (gamut == 1)      { tR = GAMUT_P3_R;   tG = GAMUT_P3_G;   tB = GAMUT_P3_B; }
    else if (gamut == 2) { tR = GAMUT_2020_R; tG = GAMUT_2020_G; tB = GAMUT_2020_B; }
    else if (gamut == 3) { tR = cR;           tG = cG;           tB = cB; }
    else                 { tR = GAMUT_709_R;  tG = GAMUT_709_G;  tB = GAMUT_709_B; }

    // Phase 1: zero groupshared.
    for (uint c = tid; c < BINS * BINS; c += totalThreads)
    {
        gs_inGamut[c]  = 0;
        gs_outGamut[c] = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    // Phase 2: scatter this group's share of the source pixels.
    uint totalPixels = Width * Height;
    for (uint pi = g * totalThreads + tid; pi < totalPixels;
         pi += totalThreads * SHADERLAB_REDUCE_GROUPS)
    {
        uint px = pi % Width;
        uint py = pi / Width;
        float4 s = Source[int2(px, py)];
        if (s.a < 0.001) continue;
        // Unclamped: negative scRGB components are wide-gamut chroma, and
        // clamping them collapses those pixels onto the sRGB hull -- which
        // is exactly the coverage this scatter exists to measure.
        float3 xyz = ScRGBToXYZ(s.rgb);
        float sum = xyz.x + xyz.y + xyz.z;
        if (sum < 1e-6) continue;
        float cieX = xyz.x / sum;
        float cieY = xyz.y / sum;

        // Map to bin coords matching the diagrams CIE x=[0,0.8], y=[0,0.9].
        float u = cieX / 0.8;
        float v = 1.0 - cieY / 0.9;
        if (u < 0 || u >= 1 || v < 0 || v >= 1) continue;

        uint bx = min((uint)(u * BINS), BINS - 1);
        uint by = min((uint)(v * BINS), BINS - 1);
        uint binIdx = by * BINS + bx;

        bool inGamut = PointInTriangle(float2(cieX, cieY), tR, tG, tB);
        if (inGamut) InterlockedAdd(gs_inGamut[binIdx],  1u);
        else         InterlockedAdd(gs_outGamut[binIdx], 1u);
    }
    GroupMemoryBarrierWithGroupSync();

    // Fold this group's bins into the global totals (non-empty bins only).
    for (uint fi = tid; fi < BINS * BINS; fi += totalThreads)
    {
        if (gs_inGamut[fi])  _SLScratch.InterlockedAdd((GLOBAL_IN  + fi) * 4, gs_inGamut[fi]);
        if (gs_outGamut[fi]) _SLScratch.InterlockedAdd((GLOBAL_OUT + fi) * 4, gs_outGamut[fi]);
    }

    if (!ShaderLabReduceIsLastGroup(tid))
        return;

    for (uint li = tid; li < BINS * BINS; li += totalThreads)
    {
        gs_inGamut[li]  = ShaderLabScratchLoad(GLOBAL_IN + li);
        gs_outGamut[li] = ShaderLabScratchLoad(GLOBAL_OUT + li);
    }
    GroupMemoryBarrierWithGroupSync();

    // Phase 3: render the 2D diagram. Each thread strides through output
    // pixels, writing bg + triangle outline + colored coverage dots.
    float lineW = 0.003;
    float dotIntensityScale = 0.05; // softens hot spots so a few hits dont saturate

    for (uint oi = tid; oi < outSize * outSize; oi += totalThreads)
    {
        uint ox = oi % outSize;
        uint oy = oi / outSize;
        float2 uvNorm = float2((ox + 0.5) / (float)outSize, (oy + 0.5) / (float)outSize);
        float2 cie_xy = float2(uvNorm.x * 0.8, (1.0 - uvNorm.y) * 0.9);

        float3 color = float3(0.05, 0.05, 0.05);
        float edge = TriangleEdge(cie_xy, tR, tG, lineW)
                   + TriangleEdge(cie_xy, tG, tB, lineW)
                   + TriangleEdge(cie_xy, tB, tR, lineW);
        color += float3(0.3, 0.3, 0.3) * saturate(edge);

        // Look up the bin this output pixel maps to.
        uint bx = min((uint)(uvNorm.x * BINS), BINS - 1);
        uint by = min((uint)(uvNorm.y * BINS), BINS - 1);
        uint binIdx = by * BINS + bx;
        uint inHits  = gs_inGamut[binIdx];
        uint outHits = gs_outGamut[binIdx];

        if (inHits > 0)
            color += float3(0.0, 0.8, 0.0) * saturate(float(inHits) * dotIntensityScale);
        if (outHits > 0)
            color += float3(1.0, 0.2, 0.0) * saturate(float(outHits) * dotIntensityScale);

        Output[int2(ox, oy)] = float4(color, 1.0);
    }
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Gamut Coverage";
            desc.effectId = L"Gamut Coverage"; desc.effectVersion = 7;
            desc.category = L"Analysis";
            desc.shaderType = Graph::CustomShaderType::D3D11ComputeShader;
            desc.hasImageOutput = true;
            desc.threadGroupX = 32;
            desc.threadGroupY = 32;
            desc.threadGroupZ = 1;
            desc.hlslSource = colorMath + gamutCoverageHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"DiagramSize",  L"float", kDefaultDiagramSize, 128.0f, 4096.0f, 64.0f },
                { L"TargetGamut", L"float", 0.0f, 0.0f, 3.0f, 1.0f, { L"sRGB", L"DCI-P3", L"BT.2020", L"Custom" } },
                { L"RedPrimary",   L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.64f, 0.33f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
                { L"GreenPrimary", L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.30f, 0.60f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
                { L"BluePrimary",  L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.15f, 0.06f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Gamut Map ----
        {
            static const std::string gamutMapHLSL = R"HLSL(
// Gamut Map - constrains input colors to a target gamut.
// Four modes:
//   0: Clip - clamp NEGATIVE target-space components to 0 (a chromaticity
//      clip; values above 1 are HDR range and are kept)
//   1: Nearest - project out-of-gamut CIE xy to nearest point on gamut triangle
//   2: Compress to White - move out-of-gamut xy toward D65 white until inside
//   3: Fit Gamut - uniformly scale all chromaticities to fit source gamut inside target

cbuffer Constants : register(b0)
{
    uint Mode;        // 0=Clip, 1=Nearest, 2=Compress, 3=Fit Gamut
    uint TargetGamut; // 0=sRGB, 1=DCI-P3, 2=BT.2020, 3=Custom
    float Strength;    // 0=bypass, 1=full mapping
    uint SourceGamut; // 0=sRGB, 1=DCI-P3, 2=BT.2020, 3=Custom
    float2 TargetRedPrimary;
    float2 TargetGreenPrimary;
    float2 TargetBluePrimary;
    float2 SourceRedPrimary;
    float2 SourceGreenPrimary;
    float2 SourceBluePrimary;
};

Texture2D InputTexture : register(t0);
SamplerState InputSampler : register(s0);

// Nearest point on line segment AB to point P
float2 NearestOnSegment(float2 p, float2 a, float2 b) {
    float2 ab = b - a;
    float t = saturate(dot(p - a, ab) / max(dot(ab, ab), 1e-10));
    return a + ab * t;
}

// Find the nearest point on triangle (a,b,c) boundary to point p
float2 NearestOnTriangle(float2 p, float2 a, float2 b, float2 c) {
    float2 p0 = NearestOnSegment(p, a, b);
    float2 p1 = NearestOnSegment(p, b, c);
    float2 p2 = NearestOnSegment(p, c, a);
    float d0 = dot(p - p0, p - p0);
    float d1 = dot(p - p1, p - p1);
    float d2 = dot(p - p2, p - p2);
    float2 best = p0;
    float bestD = d0;
    if (d1 < bestD) { best = p1; bestD = d1; }
    if (d2 < bestD) { best = p2; }
    return best;
}

// Line-segment intersection: find t where ray from P in direction dir crosses segment AB.
float RaySegmentIntersect(float2 p, float2 dir, float2 a, float2 b) {
    float2 ab = b - a;
    float denom = dir.x * ab.y - dir.y * ab.x;
    float result = -1.0;
    if (abs(denom) >= 1e-10)
    {
        float2 pa = a - p;
        float t = (pa.x * ab.y - pa.y * ab.x) / denom;
        float u = (pa.x * dir.y - pa.y * dir.x) / denom;
        if (t > 0.0 && u >= 0.0 && u <= 1.0)
            result = t;
    }
    return result;
}

// Move point toward white (D65) until it hits the gamut triangle boundary
float2 CompressToWhite(float2 p, float2 a, float2 b, float2 c) {
    float2 white = float2(0.3127, 0.3290);
    float2 dir = white - p;
    float tMin = 1e10;
    float t0 = RaySegmentIntersect(p, dir, a, b);
    float t1 = RaySegmentIntersect(p, dir, b, c);
    float t2 = RaySegmentIntersect(p, dir, c, a);
    if (t0 > 0 && t0 < tMin) tMin = t0;
    if (t1 > 0 && t1 < tMin) tMin = t1;
    if (t2 > 0 && t2 < tMin) tMin = t2;
    return (tMin < 1.0) ? p + dir * tMin : white;
}

// Compute how far a primary extends beyond the target boundary (ratio > 1 = outside)
float PrimaryExcursion(float2 primary, float2 white, float2 tR, float2 tG, float2 tB) {
    float2 dir = primary - white;
    float tMin = 1e10;
    float t0 = RaySegmentIntersect(white, dir, tR, tG);
    float t1 = RaySegmentIntersect(white, dir, tG, tB);
    float t2 = RaySegmentIntersect(white, dir, tB, tR);
    if (t0 > 0 && t0 < tMin) tMin = t0;
    if (t1 > 0 && t1 < tMin) tMin = t1;
    if (t2 > 0 && t2 < tMin) tMin = t2;
    // tMin is where ray hits boundary; primary is at t=1. Ratio = 1/tMin.
    return (tMin > 0 && tMin < 1e9) ? 1.0 / tMin : 1.0;
}

// Compute uniform scale factor to fit source gamut inside target gamut
float ComputeFitScale(float2 sR, float2 sG, float2 sB,
                      float2 tR, float2 tG, float2 tB) {
    float2 white = float2(0.3127, 0.3290);
    float maxExcursion = max(max(
        PrimaryExcursion(sR, white, tR, tG, tB),
        PrimaryExcursion(sG, white, tR, tG, tB)),
        PrimaryExcursion(sB, white, tR, tG, tB));
    return (maxExcursion > 1.0) ? 1.0 / maxExcursion : 1.0;
}

float4 main(
    float4 pos      : SV_POSITION,
    float4 posScene : SCENE_POSITION,
    float4 uv0      : TEXCOORD0
) : SV_Target
{
    float4 color = InputTexture.Sample(InputSampler, uv0.xy);
    if (Strength < 0.001) return color;

    // Near-black pixels have unreliable chromaticity — pass through unchanged.
    float lum = dot(max(color.rgb, 0.0), float3(0.2126, 0.7152, 0.0722));
    if (lum < 1e-5) return color;

    // Read all cbuffer vars at top so DXC keeps them resident.
    float targetF = TargetGamut;
    float sourceF = SourceGamut;
    float2 ctR = TargetRedPrimary;
    float2 ctG = TargetGreenPrimary;
    float2 ctB = TargetBluePrimary;
    float2 csR = SourceRedPrimary;
    float2 csG = SourceGreenPrimary;
    float2 csB = SourceBluePrimary;

    // Get target gamut primaries
    float2 gR, gG, gB;
    uint gamut = (uint)targetF;
    if (gamut == 1)      { gR = GAMUT_P3_R; gG = GAMUT_P3_G; gB = GAMUT_P3_B; }
    else if (gamut == 2) { gR = GAMUT_2020_R; gG = GAMUT_2020_G; gB = GAMUT_2020_B; }
    else if (gamut == 3) { gR = ctR; gG = ctG; gB = ctB; }
    else                 { gR = GAMUT_709_R; gG = GAMUT_709_G; gB = GAMUT_709_B; }

    uint mode = Mode;

    if (mode == 0)
    {
        // Clip mode: transform to target gamut RGB, clamp, transform back.
        // For sRGB (gamut 0), the pipeline is already in Rec.709, so just clamp.
        float3 rgb = color.rgb;

        if (gamut == 0) {
            // sRGB = Rec.709 primaries = scRGB primaries. Just clamp negatives.
            float3 clamped = max(rgb, 0.0);
            color.rgb = lerp(rgb, clamped, Strength);
        }
        else if (gamut == 1) {
            // scRGB -> XYZ -> P3 -> clamp -> XYZ -> scRGB
            float3 xyz = ScRGBToXYZ(rgb);
            float3 p3 = mul(XYZ_TO_P3D65, xyz);
            float3 clamped = max(p3, 0.0);
            float3 xyzBack = mul(P3D65_TO_XYZ, clamped);
            float3 result = XYZToScRGB(xyzBack);
            color.rgb = lerp(rgb, result, Strength);
        }
        else if (gamut == 3) {
            // Custom: the target's own primaries (D65 white, as the other
            // modes assume). Until effectVersion 6 this fell through to the
            // BT.2020 branch below, so a Custom target -- e.g. a panel's
            // primaries bound from Working Space -- silently clipped to
            // BT.2020 in Clip mode while every other mode honoured it.
            TargetXf t = MakeTargetXf(3, ctR, ctG, ctB, D65_WHITE);
            float3 clamped = max(ScRGBToTarget(t, rgb), 0.0);
            float3 result = TargetToScRGB(t, clamped);
            color.rgb = lerp(rgb, result, Strength);
        }
        else {
            // scRGB -> XYZ -> BT.2020 -> clamp -> XYZ -> scRGB
            float3 xyz = ScRGBToXYZ(rgb);
            float3 bt2020 = mul(XYZ_TO_REC2020, xyz);
            float3 clamped = max(bt2020, 0.0);
            float3 xyzBack = mul(REC2020_TO_XYZ, clamped);
            float3 result = XYZToScRGB(xyzBack);
            color.rgb = lerp(rgb, result, Strength);
        }
    }
    else if (mode == 3)
    {
        // Fit Gamut: uniformly scale all chromaticities toward D65 white.
        float2 sR, sG, sB;
        uint sg = SourceGamut;
        if (sg == 1)      { sR = GAMUT_P3_R; sG = GAMUT_P3_G; sB = GAMUT_P3_B; }
        else if (sg == 2) { sR = GAMUT_2020_R; sG = GAMUT_2020_G; sB = GAMUT_2020_B; }
        else if (sg == 3) { sR = csR; sG = csG; sB = csB; }
        else              { sR = GAMUT_709_R; sG = GAMUT_709_G; sB = GAMUT_709_B; }

        float scale = ComputeFitScale(sR, sG, sB, gR, gG, gB);
        if (scale >= 1.0) return color;

        float3 xyz = ScRGBToXYZ(color.rgb);
        float sum = xyz.x + xyz.y + xyz.z;
        if (sum < 1e-6) return color;

        float2 xy = float2(xyz.x / sum, xyz.y / sum);
        float Y = max(xyz.y, 0.0);
        float2 white = float2(0.3127, 0.3290);

        float s = lerp(1.0, scale, Strength);
        xy = white + (xy - white) * s;

        float X = (xy.y > 1e-6) ? xy.x * Y / xy.y : 0.0;
        float Z = (xy.y > 1e-6) ? (1.0 - xy.x - xy.y) * Y / xy.y : 0.0;
        color.rgb = XYZToScRGB(float3(X, Y, Z));
    }
    else
    {
        // Use raw scRGB (with negatives) for XYZ to preserve true chromaticity.
        float3 xyz = ScRGBToXYZ(color.rgb);
        float sum = xyz.x + xyz.y + xyz.z;
        if (sum < 1e-6) return color;

        float2 xy = float2(xyz.x / sum, xyz.y / sum);
        float Y = max(xyz.y, 0.0);  // Preserve luminance (clamp negative Y)

        // Check if inside gamut triangle
        bool inside = PointInTriangle(xy, gR, gG, gB);
        if (inside) return color;  // In-gamut: pass through unchanged

        float2 mapped;
        if (mode == 1)
            mapped = NearestOnTriangle(xy, gR, gG, gB);
        else
            mapped = CompressToWhite(xy, gR, gG, gB);

        xy = lerp(xy, mapped, Strength);

        // Reconstruct XYZ from mapped xy + original Y
        float X = (xy.y > 1e-6) ? xy.x * Y / xy.y : 0.0;
        float Z = (xy.y > 1e-6) ? (1.0 - xy.x - xy.y) * Y / xy.y : 0.0;
        color.rgb = XYZToScRGB(float3(X, Y, Z));
    }

    return color;
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Gamut Map";
            desc.effectId = L"Gamut Map"; desc.effectVersion = 6;
            desc.category = L"Analysis";
            desc.subcategory = L"Gamut Mapping";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + gamutMapHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"Mode",        L"float", 0.0f, 0.0f, 3.0f, 1.0f, { L"Clip", L"Nearest Point", L"Compress to White", L"Fit Gamut" } },
                { L"TargetGamut", L"float", 0.0f, 0.0f, 3.0f, 1.0f, { L"sRGB", L"DCI-P3", L"BT.2020", L"Custom" } },
                { L"Strength",    L"float", 1.0f, 0.0f, 1.0f, 0.05f },
                { L"SourceGamut", L"float", 2.0f, 0.0f, 3.0f, 1.0f, { L"sRGB", L"DCI-P3", L"BT.2020", L"Custom" }, L"Mode == 3" },
                // Custom-mode primaries. Bind to Working Space.RedPrimary etc.
                // Target primaries appear when TargetGamut == 3 (Custom).
                { L"TargetRedPrimary",   L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.64f, 0.33f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
                { L"TargetGreenPrimary", L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.30f, 0.60f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
                { L"TargetBluePrimary",  L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.15f, 0.06f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
                // Source primaries appear when Mode == 3 (Fit Gamut) AND
                // SourceGamut == 3 (Custom). Currently only one visibleWhen
                // condition is supported per parameter, so we gate on the
                // narrower SourceGamut == 3 — this is correct because the
                // SourceGamut parameter itself is gated on Mode == 3.
                { L"SourceRedPrimary",   L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.708f, 0.292f }, 0.0f, 1.0f, 0.001f, {}, L"SourceGamut == 3" },
                { L"SourceGreenPrimary", L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.170f, 0.797f }, 0.0f, 1.0f, 0.001f, {}, L"SourceGamut == 3" },
                { L"SourceBluePrimary",  L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.131f, 0.046f }, 0.0f, 1.0f, 0.001f, {}, L"SourceGamut == 3" },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- ICtCp Gamut Map ----
        {
            static const std::string perceptualGamutMapHLSL = R"HLSL(
// ICtCp Gamut Map - perceptual gamut mapping in BT.2100 ICtCp space.
// Samples the target gamut boundary as a polygon in the Ct/Cp plane
// at the pixel's intensity level, then maps out-of-gamut pixels.

cbuffer Constants : register(b0)
{
    uint Mode;          // 0=Nearest on Shell, 1=Compress to Neutral, 2=Fit to Shell, 3=Soft Compress
    uint TargetGamut;   // 0=sRGB, 1=DCI-P3, 2=BT.2020, 3=Custom
    float Strength;      // 0=bypass, 1=full
    uint SourceGamut;   // 0=sRGB, 1=DCI-P3, 2=BT.2020, 3=Custom
    float2 TargetRedPrimary;
    float2 TargetGreenPrimary;
    float2 TargetBluePrimary;
    float2 SourceRedPrimary;
    float2 SourceGreenPrimary;
    float2 SourceBluePrimary;
    float SoftThreshold; // mode 3: fraction of boundary radius where compression starts
    float SoftLimit;     // mode 3: source radius (x boundary) that maps onto the boundary
    float KneeHardness;  // mode 3: ACES p. 1=Reinhard, higher=harder corner
    // Host-computed (derived constants, ShaderLabEffects.cpp). Both depend on
    // the parameters alone, and rebuilding them per pixel is what made this
    // effect slow -- Fit to Shell measured 1.9 s per frame at 1 Mpx.
    //   FitScaleLut  mode 2: the fit scale at I = k/255, four per float4.
    //   TgtPolyLut   modes 0/1/3: the target boundary polygon at
    //                I = level/(POLY_LEVELS-1), two (Ct,Cp) vertices per float4.
    float4 FitScaleLut[64];
    float4 TgtPolyLut[1536];
};

Texture2D InputTexture : register(t0);
SamplerState InputSampler : register(s0);

#define NBP 48
#define POLY_LEVELS 64

float FitScaleAt(float I)
{
    float t = saturate(I) * 255.0;
    uint  k = min((uint)t, 254u);
    float f = t - (float)k;
    float a = FitScaleLut[k >> 2][k & 3];
    float b = FitScaleLut[(k + 1) >> 2][(k + 1) & 3];
    return lerp(a, b, f);
}

float2 PolyVertex(uint idx)
{
    float4 q = TgtPolyLut[idx >> 1];
    return (idx & 1) ? q.zw : q.xy;
}

// The target boundary at intensity I, interpolated between the two
// precomputed levels that bracket it.
void TargetBoundaryAt(float I, out float2 bnd[NBP])
{
    float t = saturate(I) * (POLY_LEVELS - 1);
    uint  k = min((uint)t, (uint)(POLY_LEVELS - 2));
    float f = t - (float)k;
    [unroll]
    for (uint i = 0; i < NBP; i++)
        bnd[i] = lerp(PolyVertex(k * NBP + i), PolyVertex((k + 1) * NBP + i), f);
}

void SampleBoundary(float2 gR, float2 gG, float2 gB, float iVal, out float2 bnd[NBP])
{
    float nits = PQ_EOTF(iVal);
    float Ys = max(nits / 80.0, 0.0001);
    uint ppe = NBP / 3;
    for (uint i = 0; i < NBP; i++)
    {
        float2 xy;
        uint e = i / ppe;
        float t = (float)(i % ppe) / (float)ppe;
        if (e == 0)      xy = lerp(gR, gG, t);
        else if (e == 1) xy = lerp(gG, gB, t);
        else             xy = lerp(gB, gR, t);
        float X = (xy.y > 1e-6) ? xy.x * Ys / xy.y : 0;
        float Z = (xy.y > 1e-6) ? (1.0 - xy.x - xy.y) * Ys / xy.y : 0;
        float3 ic = ScRGBToICtCp(XYZToScRGB(float3(X, Ys, Z)));
        bnd[i] = float2(ic.y, ic.z);
    }
}

bool PtInPoly(float2 p, float2 poly[NBP])
{
    bool inside = false;
    for (uint i = 0, j = NBP - 1; i < NBP; j = i++)
    {
        if (((poly[i].y > p.y) != (poly[j].y > p.y)) &&
            (p.x < (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x))
            inside = !inside;
    }
    return inside;
}

float2 NearestOnPoly(float2 p, float2 poly[NBP])
{
    float bestD2 = 1e10;
    float2 bestPt = p;
    for (uint i = 0; i < NBP; i++)
    {
        uint j = (i + 1) % NBP;
        float2 ab = poly[j] - poly[i];
        float t = saturate(dot(p - poly[i], ab) / max(dot(ab, ab), 1e-10));
        float2 proj = poly[i] + ab * t;
        float d2 = dot(p - proj, p - proj);
        if (d2 < bestD2) { bestD2 = d2; bestPt = proj; }
    }
    return bestPt;
}

float2 CompressNeutral(float2 p, float2 poly[NBP])
{
    float2 dir = float2(0,0) - p;
    float bestT = 1e10;
    for (uint i = 0; i < NBP; i++)
    {
        uint j = (i + 1) % NBP;
        float2 a = poly[i], b = poly[j];
        float2 ab = b - a;
        float denom = dir.x * ab.y - dir.y * ab.x;
        if (abs(denom) < 1e-10) continue;
        float2 pa = a - p;
        float t = (pa.x * ab.y - pa.y * ab.x) / denom;
        float u = (pa.x * dir.y - pa.y * dir.x) / denom;
        if (t > 0.0 && u >= 0.0 && u <= 1.0 && t < bestT)
            bestT = t;
    }
    float2 result = NearestOnPoly(p, poly);
    if (bestT < 1.0)
        result = p + dir * bestT;
    return result;
}

// Distance from the neutral axis (Ct=Cp=0) to the boundary polygon along
// direction `dir` (unit length). Returns 0 if the ray never hits an edge
// (degenerate polygon), which callers must guard.
float BoundaryRadius(float2 dir, float2 poly[NBP])
{
    float bestT = 1e10;
    for (uint i = 0; i < NBP; i++)
    {
        uint j = (i + 1) % NBP;
        float2 a = poly[i];
        float2 ab = poly[j] - a;
        float denom = dir.x * ab.y - dir.y * ab.x;
        if (abs(denom) < 1e-10) continue;
        float t = (a.x * ab.y - a.y * ab.x) / denom;
        float u = (a.x * dir.y - a.y * dir.x) / denom;
        if (t > 0.0 && u >= 0.0 && u <= 1.0 && t < bestT)
            bestT = t;
    }
    return (bestT < 1e9) ? bestT : 0.0;
}

// Compute uniform ICtCp scale factor: for each source boundary vertex,
// find how far it extends beyond the target boundary (ray from neutral).
float ComputeICtCpFitScale(float2 srcBnd[NBP], float2 tgtBnd[NBP])
{
    float maxRatio = 1.0;
    for (uint i = 0; i < NBP; i++)
    {
        float2 p = srcBnd[i];
        float distSrc = length(p);
        if (distSrc < 1e-8) continue;

        // Ray from neutral (0,0) through source boundary point
        float2 dir = p;
        float bestT = 1e10;
        for (uint j = 0; j < NBP; j++)
        {
            uint k = (j + 1) % NBP;
            float2 a = tgtBnd[j], b = tgtBnd[k];
            float2 ab = b - a;
            float denom = dir.x * ab.y - dir.y * ab.x;
            if (abs(denom) < 1e-10) continue;
            float2 pa = a; // a - origin(0,0)
            float t = (pa.x * ab.y - pa.y * ab.x) / denom;
            float u = (pa.x * dir.y - pa.y * dir.x) / denom;
            if (t > 0.0 && u >= 0.0 && u <= 1.0 && t < bestT)
                bestT = t;
        }
        // Target boundary is at t*dir from neutral; source vertex is at 1.0*dir.
        // Ratio = 1/bestT: if bestT < 1, source extends beyond target.
        if (bestT > 0 && bestT < 1e9)
            maxRatio = max(maxRatio, 1.0 / bestT);
    }
    return (maxRatio > 1.0) ? 1.0 / maxRatio : 1.0;
}

float4 main(
    float4 pos : SV_POSITION, float4 ps : SCENE_POSITION, float4 uv0 : TEXCOORD0
) : SV_Target
{
    float4 color = InputTexture.Sample(InputSampler, uv0.xy);
    if (Strength < 0.001) return color;

    // Near-black pixels have unreliable chromaticity — pass through unchanged.
    float lum = dot(max(color.rgb, 0.0), float3(0.2126, 0.7152, 0.0722));
    if (lum < 1e-5) return color;

    // Read all cbuffer vars at top so DXC keeps them resident.
    float targetF = TargetGamut;
    float sourceF = SourceGamut;
    float2 ctR = TargetRedPrimary;
    float2 ctG = TargetGreenPrimary;
    float2 ctB = TargetBluePrimary;
    float2 csR = SourceRedPrimary;
    float2 csG = SourceGreenPrimary;
    float2 csB = SourceBluePrimary;
    float softT = SoftThreshold;
    float softL = SoftLimit;
    float softP = KneeHardness;

    float2 gR, gG, gB;
    uint g = (uint)targetF;
    if (g == 1)      { gR = GAMUT_P3_R; gG = GAMUT_P3_G; gB = GAMUT_P3_B; }
    else if (g == 2) { gR = GAMUT_2020_R; gG = GAMUT_2020_G; gB = GAMUT_2020_B; }
    else if (g == 3) { gR = ctR; gG = ctG; gB = ctB; }
    else             { gR = GAMUT_709_R; gG = GAMUT_709_G; gB = GAMUT_709_B; }

    // Use CIE xy triangle test for reliable in/out-of-gamut detection,
    // then do the actual mapping in ICtCp for perceptual quality.
    // Unclamped: clamping negatives first projects the colour onto the
    // sRGB gamut surface, which makes every wide-gamut pixel test as
    // *inside* the target and defeats the detection entirely. XYZ stays
    // non-negative for real colours, so the xyzSum guard below still holds.
    float3 xyz = ScRGBToXYZ(color.rgb);
    float xyzSum = xyz.x + xyz.y + xyz.z;
    if (xyzSum < 1e-6) return color;
    float2 cieXY = float2(xyz.x / xyzSum, xyz.y / xyzSum);

    uint mode = Mode;

    if (mode == 2)
    {
        // Fit to Shell: uniformly scale all Ct/Cp toward neutral axis.
        float2 sR, sG, sB;
        uint sg = SourceGamut;
        if (sg == 1)      { sR = GAMUT_P3_R; sG = GAMUT_P3_G; sB = GAMUT_P3_B; }
        else if (sg == 2) { sR = GAMUT_2020_R; sG = GAMUT_2020_G; sB = GAMUT_2020_B; }
        else if (sg == 3) { sR = csR; sG = csG; sB = csB; }
        else              { sR = GAMUT_709_R; sG = GAMUT_709_G; sB = GAMUT_709_B; }

        float3 ictcp = ScRGBToICtCp(color.rgb);
        float origY = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));

        // The fit scale depends only on I (and the gamuts), so it is read from
        // the host-built table instead of sampling two 48-point boundaries
        // and running a 48x48 ray test here.
        float scale = FitScaleAt(ictcp.x);
        if (scale >= 1.0) return color;

        float2 ctcp = float2(ictcp.y, ictcp.z);
        float s = lerp(1.0, scale, Strength);
        ctcp *= s;

        float3 mappedRGB = ICtCpToScRGB(float3(ictcp.x, ctcp.x, ctcp.y));
        float mappedY = dot(max(mappedRGB, 0.0), float3(0.2126, 0.7152, 0.0722));
        if (mappedY > 1e-6)
            mappedRGB *= origY / mappedY;
        color.rgb = mappedRGB;
    }
    else if (mode == 3)
    {
        // Soft Compress: unlike modes 0/1 this also touches *in-gamut*
        // pixels whose chroma radius exceeds SoftThreshold x boundary,
        // buying smooth gradients across the boundary at the cost of
        // slightly desaturating legal near-boundary colors.
        float3 ictcp = ScRGBToICtCp(color.rgb);
        float2 ctcp = float2(ictcp.y, ictcp.z);
        float r = length(ctcp);
        // 1e-3, not 1e-6. This guard exists to skip the boundary walk for
        // achromatic pixels, but a neutral's residual chroma after the scRGB
        // -> ICtCp round trip is 3.4e-6 to 2.9e-5 -- above 1e-6 -- so the
        // early-out never fired and every neutral pixel paid a 48-point
        // polygon walk with a ray intersection per segment. The boundary
        // radius is O(0.1), so 1e-3 is still ~1% of it: nothing that could be
        // meaningfully compressed is skipped. Verified bit-exact across
        // neutral, near-neutral, in-gamut saturated and wide-gamut (negative
        // component) probes.
        if (r > 1e-3)
        {
            float origY = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));
            float2 bnd[NBP];
            TargetBoundaryAt(ictcp.x, bnd);
            float2 dir = ctcp / r;
            float B = BoundaryRadius(dir, bnd);
            if (B > 1e-6)
            {
                float d = r / B;
                float dNew = SoftCompressDistance(d, softT, softL, softP);
                float2 mapped = dir * (dNew * B);
                ctcp = lerp(ctcp, mapped, Strength);
                float3 mappedRGB = ICtCpToScRGB(float3(ictcp.x, ctcp.x, ctcp.y));
                float mappedY = dot(max(mappedRGB, 0.0), float3(0.2126, 0.7152, 0.0722));
                if (mappedY > 1e-6)
                    mappedRGB *= origY / mappedY;
                color.rgb = mappedRGB;
            }
        }
    }
    else
    {
        // Modes 0 and 1: per-pixel nearest/compress (only out-of-gamut pixels).
        // Use CIE xy detection with slight inset to avoid boundary jitter.
        // Inset the triangle slightly toward white so boundary pixels consistently
        // classify as out-of-gamut rather than randomly flickering.
        float2 white = float2(0.3127, 0.3290);
        float inset = 0.002;
        float2 iR = lerp(gR, white, inset);
        float2 iG = lerp(gG, white, inset);
        float2 iB = lerp(gB, white, inset);
        bool insideGamut = PointInTriangle(cieXY, iR, iG, iB);
        if (!insideGamut)
        {
            float3 ictcp = ScRGBToICtCp(color.rgb);
            float2 ctcp = float2(ictcp.y, ictcp.z);
            float origY = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));

            float2 bnd[NBP];
            TargetBoundaryAt(ictcp.x, bnd);

            float2 mapped = (mode == 1)
                ? CompressNeutral(ctcp, bnd)
                : NearestOnPoly(ctcp, bnd);
            ctcp = lerp(ctcp, mapped, Strength);
            float3 mappedRGB = ICtCpToScRGB(float3(ictcp.x, ctcp.x, ctcp.y));
            float mappedY = dot(max(mappedRGB, 0.0), float3(0.2126, 0.7152, 0.0722));
            if (mappedY > 1e-6)
                mappedRGB *= origY / mappedY;
            color.rgb = mappedRGB;
        }
    }
    return color;
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"ICtCp Gamut Map";
            desc.effectId = L"ICtCp Gamut Map"; desc.effectVersion = 13;
            desc.category = L"Analysis";
            desc.subcategory = L"Gamut Mapping";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + perceptualGamutMapHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"Mode",        L"float", 0.0f, 0.0f, 3.0f, 1.0f, { L"Nearest on Shell", L"Compress to Neutral", L"Fit to Shell", L"Soft Compress" } },
                { L"TargetGamut", L"float", 0.0f, 0.0f, 3.0f, 1.0f, { L"sRGB", L"DCI-P3", L"BT.2020", L"Custom" } },
                { L"Strength",    L"float", 1.0f, 0.0f, 1.0f, 0.05f },
                { L"SourceGamut", L"float", 2.0f, 0.0f, 3.0f, 1.0f, { L"sRGB", L"DCI-P3", L"BT.2020", L"Custom" }, L"Mode == 2" },
                { L"SoftThreshold", L"float", 0.75f, 0.0f, 0.99f, 0.01f, {}, L"Mode == 3" },
                { L"SoftLimit",     L"float", 1.5f,  1.01f, 4.0f, 0.01f, {}, L"Mode == 3" },
                { L"KneeHardness",  L"float", 1.2f,  1.0f,  4.0f, 0.05f, {}, L"Mode == 3" },
                { L"TargetRedPrimary",   L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.64f, 0.33f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
                { L"TargetGreenPrimary", L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.30f, 0.60f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
                { L"TargetBluePrimary",  L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.15f, 0.06f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
                { L"SourceRedPrimary",   L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.708f, 0.292f }, 0.0f, 1.0f, 0.001f, {}, L"SourceGamut == 3" },
                { L"SourceGreenPrimary", L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.170f, 0.797f }, 0.0f, 1.0f, 0.001f, {}, L"SourceGamut == 3" },
                { L"SourceBluePrimary",  L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.131f, 0.046f }, 0.0f, 1.0f, 0.001f, {}, L"SourceGamut == 3" },
            };
            desc.deriveConstants = MakeIctcpGamutMapDerived();
            m_effects.push_back(std::move(desc));
        }

        // ---- ICtCp Boundary Viewer ----
        {
            static const std::string ictcpBoundaryHLSL = R"HLSL(
// ICtCp Boundary Viewer - visualizes the gamut boundary in ICtCp Ct/Cp space
// at multiple intensity (I) levels.

cbuffer Constants : register(b0)
{
    float DiagramSize;
    uint TargetGamut;
    float Intensity;   // Which I level to highlight (PQ domain, 0-1)
    float2 RedPrimary;
    float2 GreenPrimary;
    float2 BluePrimary;
    // Host-computed (derived constants, ShaderLabEffects.cpp): the boundary
    // polygon at each of the 6 drawn I levels -- 0.15, 0.3, 0.45, 0.6, 0.75
    // and Intensity -- 24 vertices each, two (Ct,Cp) pairs per float4. They
    // depend on the parameters alone; building them here cost 144 colour
    // conversions per output pixel.
    float4 BndPts[72];
};

Texture2D InputTexture : register(t0);
SamplerState InputSampler : register(s0);

#define NBP 24

float2 BndVertex(uint idx)
{
    float4 q = BndPts[idx >> 1];
    return (idx & 1) ? q.zw : q.xy;
}

float4 main(
    float4 pos : SV_POSITION, float4 ps : SCENE_POSITION, float4 uv0 : TEXCOORD0
) : SV_Target
{
    float size = max(DiagramSize, 256.0);
    float2 ctcp = (uv0.xy - 0.5) * 1.0;

    // Read all cbuffer vars at top so DXC keeps them resident.
    float gamutF = TargetGamut;
    float2 cR = RedPrimary;
    float2 cG = GreenPrimary;
    float2 cB = BluePrimary;

    float2 gR, gG, gB;
    uint g = (uint)gamutF;
    if (g == 1)      { gR = GAMUT_P3_R; gG = GAMUT_P3_G; gB = GAMUT_P3_B; }
    else if (g == 2) { gR = GAMUT_2020_R; gG = GAMUT_2020_G; gB = GAMUT_2020_B; }
    else if (g == 3) { gR = cR; gG = cG; gB = cB; }
    else             { gR = GAMUT_709_R; gG = GAMUT_709_G; gB = GAMUT_709_B; }

    float3 color = float3(0.01, 0.01, 0.01);
    float lineW = 1.5 / size;
    float dotR = 3.0 / size;

    // Boundaries at 5 fixed I levels + the user-selected one (host-built).
    float3 lColors[6] = {
        float3(0.15, 0.05, 0.05),
        float3(0.15, 0.1, 0.05),
        float3(0.05, 0.15, 0.05),
        float3(0.05, 0.1, 0.15),
        float3(0.1, 0.05, 0.15),
        float3(0.4, 0.4, 0.0)   // User-selected level in yellow
    };

    for (uint lv = 0; lv < 6; lv++)
    {
        for (uint i = 0; i < NBP; i++)
        {
            float2 a = BndVertex(lv * NBP + i);
            float2 b = BndVertex(lv * NBP + (i + 1) % NBP);
            float2 ab = b - a;
            float t = saturate(dot(ctcp - a, ab) / max(dot(ab, ab), 1e-10));
            float d = length(ctcp - a - ab * t);
            if (d < lineW)
                color = max(color, lColors[lv] * (1.0 - d / lineW));

            if (length(ctcp - a) < dotR)
                color = max(color, lColors[lv] * 1.5);
        }
    }

    // Axes
    if (abs(ctcp.x) < 0.4 / size || abs(ctcp.y) < 0.4 / size)
        color = max(color, float3(0.08, 0.08, 0.08));

    // Center dot (neutral)
    if (length(ctcp) < dotR * 1.5)
        color = float3(0.3, 0.3, 0.3);

    return float4(color, 1.0);
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"ICtCp Boundary";
            desc.effectId = L"ICtCp Boundary"; desc.effectVersion = 5;
            desc.category = L"Analysis";
            desc.subcategory = L"Gamut Mapping";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + ictcpBoundaryHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"DiagramSize", L"float", kDefaultDiagramSize, 128.0f, 2048.0f, 64.0f },
                { L"TargetGamut", L"float", 0.0f, 0.0f, 3.0f, 1.0f, { L"sRGB", L"DCI-P3", L"BT.2020", L"Custom" } },
                { L"Intensity",   L"float", 0.5f, 0.05f, 0.95f, 0.05f },
                { L"RedPrimary",   L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.64f, 0.33f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
                { L"GreenPrimary", L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.30f, 0.60f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
                { L"BluePrimary",  L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.15f, 0.06f }, 0.0f, 1.0f, 0.001f, {}, L"TargetGamut == 3" },
            };
            desc.deriveConstants = MakeIctcpBoundaryDerived();
            m_effects.push_back(std::move(desc));
        }

        // ---- ICtCp Gamut Boundary LUT ----
        // Generator for a tone mapper's gamut-boundary lookup input. Give it
        // the same SdrWhiteNits and TargetGamut as its consumer (bind both to
        // the same source): a consumer should refuse a table built for a
        // different peak or gamut and fall back to the exact search.
        {
            static const std::string gamutLutMainHLSL = R"HLSL(
// ICtCp Gamut Boundary LUT - D3D11 Compute Shader (generator)
//
// Precomputes B(I, hue) = CubeBoundaryRadius(I, dir(hue), peak, target) into a
// GAMUT_LUT_W x GAMUT_LUT_H table, read by a consumer on a lookup input.
//
// Why this pays: a gamut-compressing tone mapper runs that search dozens of
// times per out-of-gamut pixel, and on real wide-gamut HDR content the
// searches dominate its cost. B depends on (I, hue, peak, target gamut) and on NOTHING in the image,
// so it can be built once and reused for every frame until the display's SDR
// white or the target gamut changes -- 32K entries instead of millions of
// pixels. One table is ONE target: an sRGB and a P3 consumer each want
// their own LUT node (a rebuild is ~0.04 ms of GPU, so the graph is the cache).
//
// A generator: no image input. The host feeds a 1x1 placeholder at t0 only to
// satisfy the compute path, and since that placeholder is never dirty this
// re-dispatches only when SdrWhiteNits or the target moves.
Texture2D<float4>   Source      : register(t0);   // placeholder, unused
RWTexture2D<float4> ImageOutput : register(u1);

cbuffer constants : register(b0)
{
    uint  Width;          // auto-injected from input #0 (the 1x1 placeholder)
    uint  Height;
    uint  OutputWidth;    // hidden default == GAMUT_LUT_W
    uint  OutputHeight;   // hidden default == GAMUT_LUT_H
    float SdrWhiteNits;   // MUST match the consumer's, or it refuses the table
    uint   TargetGamut;   // likewise -- same meaning as the consumer's
    float2 RedPrimary;    // Custom only
    float2 GreenPrimary;
    float2 BluePrimary;
    float2 WhitePoint;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    // Every cbuffer member is read before any branch: the compiler strips
    // members not referenced on all paths (see CLAUDE.md). But the bounds
    // come from GAMUT_LUT_W/H -- the constants the READER indexes with --
    // not from the packed OutputWidth/Height. Whether a texel gets written
    // must not depend on a cbuffer value: the runner clears this texture
    // before every dispatch, so a dispatch that skips its writes leaves a
    // blank table behind. With the bounds fixed, the only cbuffer-dependent
    // output is the stamp, which the consumer checks anyway.
    uint  sizeTouch = OutputWidth + OutputHeight + Width + Height;
    float W         = max(SdrWhiteNits, 1.0);
    TargetXf tg     = MakeTargetXf(TargetGamut, RedPrimary, GreenPrimary, BluePrimary, WhitePoint);
    if (id.x >= GAMUT_LUT_W || id.y >= GAMUT_LUT_H) return;

    float peakCh  = W / 80.0;
    float peakOut = NitsToI(W);
    float hue = ((id.x + 0.5) / GAMUT_LUT_W) * 6.28318530718 - 3.14159265359;
    float I   = GamutLutRowToI(id.y / float(GAMUT_LUT_H - 1), peakOut);
    float B   = CubeBoundaryRadius(I, float2(cos(hue), sin(hue)), peakCh, tg);

    // .g / .b / .a are the stamp a consumer checks before trusting this:
    // .a is the target's fingerprint, computed by the same MakeTargetXf the
    // consumer runs, so a table for another gamut is refused.
    // sizeTouch * 0 keeps the size members referenced without affecting it.
    ImageOutput[id.xy] = float4(B, peakCh + sizeTouch * 0.0, peakOut, tg.fingerprint);
}
)HLSL";
            ShaderLabEffectDescriptor desc;
            desc.name = L"ICtCp Gamut Boundary LUT";
            desc.effectId = L"ICtCp Gamut Boundary LUT"; desc.effectVersion = 2;
            desc.category = L"Analysis";
            desc.subcategory = L"Tone Mapping";
            desc.shaderType = Graph::CustomShaderType::D3D11ComputeShader;
            desc.hasImageOutput = true;
            // 8x8, so the evaluator tiles the dispatch across the table rather
            // than running it as a single group.
            desc.threadGroupX = 8;
            desc.threadGroupY = 8;
            desc.threadGroupZ = 1;
            desc.hlslSource = colorMath + GamutBoundaryHLSL() + gamutLutMainHLSL;
            desc.inputNames = {};   // a generator
            desc.parameters = {
                { L"SdrWhiteNits", L"float", 200.0f, 80.0f, 1000.0f, 1.0f },
                // Target gamut. Custom takes primaries + white point -- bind
                // them to Working Space.RedPrimary / GreenPrimary / BluePrimary
                // / WhitePoint to fit a specific panel. A non-D65 white is
                // reached by Bradford adaptation (see MakeTargetXf).
                { L"TargetGamut",  L"float", 0.0f, 0.0f, 3.0f, 1.0f, { L"sRGB", L"Display P3", L"BT.2020", L"Custom" } },
                { L"RedPrimary",   L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.64f,   0.33f   }, 0.0f, 1.0f, 0.0001f, {}, L"TargetGamut == 3" },
                { L"GreenPrimary", L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.30f,   0.60f   }, 0.0f, 1.0f, 0.0001f, {}, L"TargetGamut == 3" },
                { L"BluePrimary",  L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.15f,   0.06f   }, 0.0f, 1.0f, 0.0001f, {}, L"TargetGamut == 3" },
                { L"WhitePoint",   L"float2", winrt::Windows::Foundation::Numerics::float2{ 0.3127f, 0.3290f }, 0.0f, 1.0f, 0.0001f, {}, L"TargetGamut == 3" },
            };
            // Fixed, not user-editable: a consumer indexes the table with
            // the same constants, so a resized table would be read wrongly.
            desc.hiddenDefaults = {
                { L"OutputWidth",  Graph::PropertyValue{ kGamutLutW } },
                { L"OutputHeight", Graph::PropertyValue{ kGamutLutH } },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- ICtCp Round-Trip Validator ----
        // Diagnostic effect: passes input through scRGB -> ICtCp -> scRGB
        // and outputs |out - in| * Gain. A correct implementation renders
        // black (modulo FP16 precision); any non-trivial color means the
        // ICtCp matrices or PQ helpers are wrong, and downstream tone-
        // mapping effects are untrustworthy.
        {
            static const std::string ictcpRoundTripHLSL = R"HLSL(
// ICtCp Round-Trip Validator
Texture2D Source : register(t0);
SamplerState Sampler : register(s0);

cbuffer constants : register(b0) {
    float Gain;       // default 1000
};

float4 main(
    float4 pos      : SV_POSITION,
    float4 scenePos : SCENE_POSITION,
    float4 uv0      : TEXCOORD0) : SV_Target
{
    // This one mattered more than most: with the old two-parameter signature
    // the Load() read out of bounds and returned 0, so diff = abs(back - 0)*Gain
    // came out BLACK -- which this effect defines as "round trip is correct".
    // The suite's one free correctness oracle was reporting a pass while
    // reading transparent padding.
    float4 color = Source.Sample(Sampler, uv0.xy);
    float3 ictcp = ScRGBToICtCp(color.rgb);
    float3 back  = ICtCpToScRGB(ictcp);
    float3 diff  = abs(back - color.rgb) * Gain;
    return float4(diff, 1.0);
}
)HLSL";
            ShaderLabEffectDescriptor desc;
            desc.name = L"ICtCp Round-Trip Validator";
            desc.effectId = L"ICtCp Round-Trip Validator"; desc.effectVersion = 4;
            desc.category = L"Analysis";
            desc.subcategory = L"Tone Mapping";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + ictcpRoundTripHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"Gain", L"float", 1000.0f, 1.0f, 100000.0f, 10.0f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- ICtCp Tone Map (HDR -> SDR) ----
        // Compresses I-channel only via Reinhard, leaving Ct/Cp untouched
        // so HUE is preserved by construction. Saturation may shift slightly
        // when ToneLift > 0 because lifting I without rescaling chroma
        // reduces apparent colorfulness. Both peaks are in nits and converted
        // to I-coordinates (PQ values) before the curve is applied.
        //
        // ToneLift adds a configurable mid-bump on top of the compression so
        // dark/mid scenes can be brightened toward "D2D HDR Tone Map"-style
        // output. ToneLift = 0 is pure peak-only compression (preserves HDR
        // creative intent). ToneLift ~= 0.6 approximates D2D HDR Tone Map's
        // dark-end lift on typical HDR content.
        {
            static const std::string ictcpToneMapHLSL = R"HLSL(
// ICtCp Tone Map (HDR -> SDR), I-channel Reinhard with optional mid-bump.
// Migrated to D3D11 compute (Phase 8) so it can consume upstream
// IEngineComputeOutput SRVs directly via the SHADERLAB_GPU_BUFFER macros
// when SourcePeakNits / TargetPeakNits are bound from a Luminance
// Statistics or similar producer.
#include "shaderlab_params.hlsli"

// Inputs / outputs (bridge-provided):
//   t0 -> input image (FP32 RGBA, scRGB linear)
//   u1 -> output image (RWTexture2D<float4>, FP32 RGBA, scRGB linear)
// Plus auto-injected Width/Height in the cbuffer + GPU-bindable params
// at t1 (SourcePeakNits) and t2 (TargetPeakNits) when GPU mode is on.
Texture2D<float4>        Source : register(t0);
RWTexture2D<float4>      ImageOutput : register(u1);

SHADERLAB_GPU_BUFFER(SourcePeakNits, t1)
SHADERLAB_GPU_BUFFER(TargetPeakNits, t2)

cbuffer constants : register(b0) {
    uint   Width;
    uint   Height;
    SHADERLAB_PARAM(float, SourcePeakNits)        // typical 1000-10000
    SHADERLAB_PARAM(float, TargetPeakNits)        // SDR target peak (e.g. 80, 203)
    float  Strength;                              // 0..1 lerp from identity to compressed+lifted
    float  ToneLift;                              // 0..1 mid-tone lift
    float  KneeNits;                              // identity below this; 0 = compress from black
};

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= Width || dtid.y >= Height) return;

    SHADERLAB_LOAD_PARAM(float, SourcePeakNits)
    SHADERLAB_LOAD_PARAM(float, TargetPeakNits)

    float4 color = Source.Load(int3(dtid.xy, 0));
    float3 ictcp = ScRGBToICtCp(color.rgb);

    float peakIn  = NitsToI(SourcePeakNits);
    float peakOut = NitsToI(TargetPeakNits);

    // Optional knee (BT.2390-EETF-style): content at or below KneeNits
    // passes through UNCHANGED; only [knee, SourcePeak] compresses into
    // [knee, TargetPeak]. This is the mixed-content-safe shape for the
    // primary screenshot workflow — an HDR capture of a mixed SDR/HDR
    // desktop converts to SDR with the SDR windows kept at their
    // presentation brightness while true-HDR highlights roll off into
    // the remaining headroom. The shifted Reinhard has slope 1 at the
    // knee (C1-continuous). KneeNits = 0 reproduces the classic
    // compress-from-black curve exactly.
    float kneeI = NitsToI(clamp(KneeNits, 0.0, TargetPeakNits * 0.999));
    float compressed;
    if (ictcp.x <= kneeI || peakIn <= peakOut)
    {
        compressed = ictcp.x;
    }
    else
    {
        compressed = kneeI + ReinhardCompressI(
            ictcp.x - kneeI, peakIn - kneeI, peakOut - kneeI);
    }

    // Anchored polynomial lift in I-space, applied AFTER compression.
    // Curve: f(x) = x + a*x*(1-x), evaluated in normalized [0, peakOut]
    // space then scaled back to I units. Properties:
    //   f(0)   = 0           (preserves true black)
    //   f(1)   = 1           (preserves peakOut anchor)
    //   f'(0)  = 1 + a       (controlled finite toe slope; no shadow blow-up)
    //   max bump occurs near x = 0.5 (mid-tone region), magnitude = a/4
    float a = saturate(ToneLift);
    float invPeakOut = 1.0 / max(peakOut, 1e-6);
    float xn = saturate(compressed * invPeakOut);
    float lifted = (xn + a * xn * (1.0 - xn)) * peakOut;

    ictcp.x = lerp(ictcp.x, lifted, saturate(Strength));

    float3 outRgb = ICtCpToScRGB(ictcp);
    ImageOutput[dtid.xy] = float4(outRgb, color.a);
}
)HLSL";
            ShaderLabEffectDescriptor desc;
            desc.name = L"ICtCp Tone Map (HDR -> SDR)";
            desc.effectId = L"ICtCp Tone Map"; desc.effectVersion = 14;
            desc.category = L"Analysis";
            desc.subcategory = L"Tone Mapping";
            desc.shaderType = Graph::CustomShaderType::D3D11ComputeShader;
            desc.hasImageOutput = true;
            desc.threadGroupX = 8;
            desc.threadGroupY = 8;
            desc.threadGroupZ = 1;
            desc.hlslSource = colorMath + ictcpToneMapHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                // SourcePeakNits + TargetPeakNits remain gpuBindable.
                // Now in D3D11 compute, the host can route the upstream
                // SRV directly into the consumer's t-slot via the
                // bridge's SetGpuBinding -- no CPU readback round-trip.
                Graph::ParameterDefinition{ L"SourcePeakNits", L"float", 1000.0f, 100.0f, 10000.0f, 50.0f, {}, L"", true },
                Graph::ParameterDefinition{ L"TargetPeakNits", L"float",  203.0f,  80.0f,   500.0f,  1.0f, {}, L"", true },
                Graph::ParameterDefinition{ L"Strength",       L"float",    1.0f,   0.0f,     1.0f, 0.05f },
                Graph::ParameterDefinition{ L"ToneLift",       L"float",    0.0f,   0.0f,     1.0f, 0.05f },
                Graph::ParameterDefinition{ L"KneeNits",       L"float",    0.0f,   0.0f,   500.0f,  1.0f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- ICtCp Inverse Tone Map (SDR -> HDR) ----
        // Inverse Reinhard on I; expands SDR-anchored content into the
        // HDR peak. Mirror of ICtCp Tone Map; Ct/Cp unchanged.
        // D3D11 compute -- SourcePeakNits + TargetPeakNits gpuBindable.
        {
            static const std::string ictcpInverseToneMapHLSL = R"HLSL(
// ICtCp Inverse Tone Map (SDR -> HDR) -- D3D11 compute, I-channel inverse Reinhard.
#include "shaderlab_params.hlsli"

Texture2D<float4>        Source      : register(t0);
RWTexture2D<float4>      ImageOutput : register(u1);

SHADERLAB_GPU_BUFFER(SourcePeakNits, t1)
SHADERLAB_GPU_BUFFER(TargetPeakNits, t2)

cbuffer constants : register(b0) {
    uint   Width;
    uint   Height;
    SHADERLAB_PARAM(float, SourcePeakNits)        // SDR source peak (e.g. 80, 203)
    SHADERLAB_PARAM(float, TargetPeakNits)        // typical 1000-10000
    float  Strength;                              // 0..1 lerp from identity to expanded
    float  DiffuseWhiteNits;                      // shadow/mid anchor (HDR paper white)
    float  KneeNits;                              // identity below this; 0 = expand from black
};

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= Width || dtid.y >= Height) return;

    SHADERLAB_LOAD_PARAM(float, SourcePeakNits)
    SHADERLAB_LOAD_PARAM(float, TargetPeakNits)

    float4 color = Source.Load(int3(dtid.xy, 0));
    float3 ictcp = ScRGBToICtCp(color.rgb);

    // Inverse of ReinhardCompressI(x, peakIn=HDR, peakOut=SDR):
    // for SDR -> HDR expansion we feed the *compressed* (SDR) range as
    // peakOut and the *expanded* (HDR) range as peakIn. Input I lives in
    // [0, sdrI] (SDR range); the helper returns I in [0, hdrI] (HDR).
    float sdrI = NitsToI(SourcePeakNits);
    float hdrI = NitsToI(TargetPeakNits);

    // Optional knee: content at or below KneeNits passes through
    // UNCHANGED; only the range above expands toward the target peak.
    // This is the mixed-content-safe shape (BT.2390-style): an image
    // whose "SDR chunk" is already presentation-referenced (e.g. white
    // level boosted, or a mixed SDR/HDR composite) keeps that region
    // intact instead of blowing everything above SourcePeakNits to the
    // peak. The shifted inverse-Reinhard has slope 1 at the knee, so
    // the curve is C1-continuous there. KneeNits = 0 reproduces the
    // classic expand-from-black curve exactly.
    float kneeI = NitsToI(clamp(KneeNits, 0.0, SourcePeakNits * 0.999));
    float expanded;
    if (ictcp.x <= kneeI || hdrI <= sdrI)
    {
        expanded = ictcp.x;
    }
    else
    {
        expanded = kneeI + ReinhardExpandI(
            ictcp.x - kneeI, hdrI - kneeI, sdrI - kneeI);
    }

    // Shadow/mid anchor: the pure inverse-Reinhard has slope 1 at black
    // in I-space, so shadows keep their SDR nit levels while the rest of
    // the picture expands -- perceptually crushed blacks. Let the low end
    // instead scale like an SDR presentation at DiffuseWhiteNits paper
    // white (nits x D/S, the BT.2446-style lift), and let the expansion
    // curve take over wherever it exceeds that. Skipped when a knee is
    // set -- the knee's contract is bit-exact identity below it, which
    // a lift would violate.
    if (KneeNits < 1.0)
    {
        float diffuseScale = max(DiffuseWhiteNits, 1.0) / max(SourcePeakNits, 1.0);
        float lifted = NitsToI(IToNits(ictcp.x) * diffuseScale);
        expanded = max(expanded, lifted);
    }
    expanded = min(expanded, hdrI);

    ictcp.x = lerp(ictcp.x, expanded, saturate(Strength));

    float3 outRgb = ICtCpToScRGB(ictcp);
    ImageOutput[dtid.xy] = float4(outRgb, color.a);
}
)HLSL";
            ShaderLabEffectDescriptor desc;
            desc.name = L"ICtCp Inverse Tone Map (SDR -> HDR)";
            desc.effectId = L"ICtCp Inverse Tone Map"; desc.effectVersion = 14;
            desc.category = L"Analysis";
            desc.subcategory = L"Tone Mapping";
            desc.shaderType = Graph::CustomShaderType::D3D11ComputeShader;
            desc.hasImageOutput = true;
            desc.threadGroupX = 8;
            desc.threadGroupY = 8;
            desc.threadGroupZ = 1;
            desc.hlslSource = colorMath + ictcpInverseToneMapHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                Graph::ParameterDefinition{ L"SourcePeakNits", L"float",  203.0f,   80.0f,   500.0f,  1.0f, {}, L"", true },
                Graph::ParameterDefinition{ L"TargetPeakNits", L"float", 1000.0f,  100.0f, 10000.0f, 50.0f, {}, L"", true },
                Graph::ParameterDefinition{ L"Strength",       L"float",    1.0f,    0.0f,     1.0f, 0.05f },
                Graph::ParameterDefinition{ L"DiffuseWhiteNits", L"float",  203.0f,   80.0f,   400.0f, 1.0f },
                Graph::ParameterDefinition{ L"KneeNits",       L"float",    0.0f,    0.0f,   500.0f,  1.0f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- ICtCp Saturation ----
        // Scale Ct/Cp uniformly. Hue and luminance are unchanged because
        // the (Ct, Cp) chromaticity vector is just multiplied; this is
        // the reason ICtCp exists. Saturation = 1 is identity, > 1
        // boosts, < 1 desaturates, 0 -> grayscale.
        {
            static const std::string ictcpSaturationHLSL = R"HLSL(
// ICtCp Saturation — uniform Ct/Cp scale (hue + luminance preserved)
Texture2D Source : register(t0);
SamplerState Sampler : register(s0);

cbuffer constants : register(b0) {
    float Saturation;   // 1.0 = identity, 0.0 = grayscale, >1 = boosted
};

float4 main(
    float4 pos      : SV_POSITION,
    float4 scenePos : SCENE_POSITION,
    float4 uv0      : TEXCOORD0) : SV_Target
{
    float4 color = Source.Sample(Sampler, uv0.xy);
    float3 ictcp = ScRGBToICtCp(color.rgb);
    ictcp.y *= Saturation;
    ictcp.z *= Saturation;
    float3 outRgb = ICtCpToScRGB(ictcp);
    return float4(outRgb, color.a);
}
)HLSL";
            ShaderLabEffectDescriptor desc;
            desc.name = L"ICtCp Saturation";
            desc.effectId = L"ICtCp Saturation"; desc.effectVersion = 3;
            desc.category = L"Analysis";
            desc.subcategory = L"Tone Mapping";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + ictcpSaturationHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"Saturation", L"float", 1.0f, 0.0f, 4.0f, 0.05f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- ICtCp Highlight Desaturation ----
        // Smooth desaturation as I rises — the standard "filmic
        // highlight rolloff" trick. Below KneeNits the image is
        // unchanged; between KneeNits and PeakNits the saturation
        // multiplier ramps from 1 toward (1 - Amount); above PeakNits
        // the saturation is fully attenuated. Pairs naturally with
        // ICtCp Tone Map: tone-map first, then desat the highlights.
        // D3D11 compute -- KneeNits + PeakNits gpuBindable (typical
        // bindings: LumStats.P95 -> KneeNits, LumStats.Max -> PeakNits).
        {
            static const std::string ictcpHighlightDesatHLSL = R"HLSL(
// ICtCp Highlight Desaturation -- D3D11 compute, smooth Ct/Cp rolloff vs. I.
#include "shaderlab_params.hlsli"

Texture2D<float4>        Source      : register(t0);
RWTexture2D<float4>      ImageOutput : register(u1);

SHADERLAB_GPU_BUFFER(KneeNits, t1)
SHADERLAB_GPU_BUFFER(PeakNits, t2)

cbuffer constants : register(b0) {
    uint  Width;
    uint  Height;
    SHADERLAB_PARAM(float, KneeNits)    // start of the rolloff (e.g. 200)
    SHADERLAB_PARAM(float, PeakNits)    // end of the rolloff   (e.g. 1000)
    float Amount;                       // [0..1] how far to desaturate at peak
};

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= Width || dtid.y >= Height) return;

    SHADERLAB_LOAD_PARAM(float, KneeNits)
    SHADERLAB_LOAD_PARAM(float, PeakNits)

    float4 color = Source.Load(int3(dtid.xy, 0));
    float3 ictcp = ScRGBToICtCp(color.rgb);

    // Map I (PQ) back to nits so the user's parameters mean what they
    // say even though the I axis is non-linear.
    float nits = IToNits(ictcp.x);
    float t = saturate((nits - KneeNits) / max(PeakNits - KneeNits, 1e-3));
    float scale = 1.0 - saturate(Amount) * smoothstep(0.0, 1.0, t);

    ictcp.y *= scale;
    ictcp.z *= scale;
    float3 outRgb = ICtCpToScRGB(ictcp);
    ImageOutput[dtid.xy] = float4(outRgb, color.a);
}
)HLSL";
            ShaderLabEffectDescriptor desc;
            desc.name = L"ICtCp Highlight Desaturation";
            desc.effectId = L"ICtCp Highlight Desaturation"; desc.effectVersion = 5;
            desc.category = L"Analysis";
            desc.subcategory = L"Tone Mapping";
            desc.shaderType = Graph::CustomShaderType::D3D11ComputeShader;
            desc.hasImageOutput = true;
            desc.threadGroupX = 8;
            desc.threadGroupY = 8;
            desc.threadGroupZ = 1;
            desc.hlslSource = colorMath + ictcpHighlightDesatHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                Graph::ParameterDefinition{ L"KneeNits", L"float",  200.0f, 10.0f,  5000.0f, 10.0f, {}, L"", true },
                Graph::ParameterDefinition{ L"PeakNits", L"float", 1000.0f, 50.0f, 10000.0f, 50.0f, {}, L"", true },
                { L"Amount",       L"float",    1.0f,  0.0f,     1.0f, 0.05f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Split Comparison ----
        {
            static const std::string splitCompareHLSL = R"HLSL(
// Split Comparison - oriented wipe between two inputs.
// Works with arbitrary `Angle` (degrees) and `SplitPosition` (0..1).
//
// The wipe boundary is a line through the image perpendicular to the
// direction (cos(Angle), sin(Angle)). SplitPosition sweeps the line
// across the image: 0 = entire image is B, 1 = entire image is A.
// At 0.5 the line passes through the image center.
//
// Coordinates are normalized so that "1.0" of travel corresponds to
// the full extent of the image *along the wipe direction* -- i.e.
// |cos(Angle)| * W + |sin(Angle)| * H. This makes the wipe sweep the
// entire image regardless of angle: at SplitPosition=0 nothing of A
// shows; at 1 nothing of B shows; in between you get a clean
// diagonal line.
//
// LineWidth controls the dividing line thickness in pixels.
//
// Both inputs are read at the output coordinate. The effect runs with
// D2D1_PIXEL_OPTIONS_TRIVIAL_SAMPLING, whose contract is that a pixel
// shader reads its inputs 1:1 with the output; an input smaller than the
// union output rect therefore covers only its own region and reads black
// elsewhere. Put a Scale node upstream to compare branches of differing
// size.

Texture2D ImageA : register(t0);
Texture2D ImageB : register(t1);
SamplerState InputSampler : register(s0);

cbuffer Constants : register(b0)
{
    float SplitPosition;   // 0..1 sweep along wipe direction
    float LineWidth;       // dividing line thickness in pixels
    float Angle;           // degrees; 0 = horizontal wipe (vertical line),
                           // 90 = vertical wipe (horizontal line),
                           // 45 = top-left-to-bottom-right diagonal
    float OutputW;         // host-injected: union of input *content* widths
    float OutputH;         // host-injected: union of input *content* heights
};

float4 main(
    float4 pos      : SV_POSITION,
    float4 scenePos : SCENE_POSITION,
    float4 uv0      : TEXCOORD0,
    float4 uv1      : TEXCOORD1) : SV_TARGET
{
    // THE two-input displacement bug, and the reason it looked like a
    // two-input problem rather than a signature problem. D2D gives each input
    // its OWN texel coordinate -- TEXCOORD0 for input 0, TEXCOORD1 for input 1
    // -- after SV_POSITION and SCENE_POSITION. This shader declared neither
    // SCENE_POSITION nor TEXCOORD1, so the single parameter it called uv0
    // received the SCENE coordinate and was then used to Load() BOTH inputs.
    // Whenever an input's content did not fill D2D's intermediate allocation,
    // that coordinate was offset by -(intermediate - content)/2 and both
    // images sampled from the wrong place.
    //
    // Use the host-supplied output dimensions, not GetDimensions(), since D2D
    // pads input textures to atlas allocation sizes (e.g. 4096x4096 when the
    // actual output rect is 3840x2160).
    float W = max(OutputW, 1.0);
    float H = max(OutputH, 1.0);

    // Each input sampled with its own normalized coordinate.
    float4 a = ImageA.Sample(InputSampler, uv0.xy);
    float4 b = ImageB.Sample(InputSampler, uv1.xy);

    // Direction vector along which we project pixel positions.
    float radians = Angle * 3.14159265 / 180.0;
    float2 dir = float2(cos(radians), sin(radians));

    // Project the pixel coord (relative to image center) onto dir.
    // SplitPosition = 0..1 sweeps the wipe across the full extent of
    // the image *along* the dir vector, with 0.5 always pivoting on
    // the geometric image center regardless of angle.
    // scenePos, not uv0: the wipe is defined in pixels, and uv0 is normalized.
    float2 p = scenePos.xy - float2(W * 0.5, H * 0.5);
    float projPx  = dot(p, dir);
    float halfMax = 0.5 * (abs(dir.x) * W + abs(dir.y) * H);
    float projNorm = saturate(projPx / max(halfMax * 2.0, 1.0) + 0.5);

    float threshold = SplitPosition;
    float dist = abs(projNorm - threshold) * (halfMax * 2.0);

    // Dividing line.
    float halfLine = max(LineWidth * 0.5, 0.5);
    if (dist < halfLine)
        return float4(1, 1, 1, 1);

    if (projNorm < threshold)
        return a;
    return b;
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Split Comparison";
            desc.effectId = L"Split Comparison"; desc.effectVersion = 8;
            desc.category = L"Analysis";
            desc.subcategory = L"Comparison";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + splitCompareHLSL;
            desc.inputNames = { L"ImageA", L"ImageB" };
            desc.parameters = {
                { L"SplitPosition", L"float",   0.5f,    0.0f,   1.0f,  0.01f },
                { L"LineWidth",     L"float",   2.0f,    0.0f,  10.0f,  0.5f },
                { L"Angle",         L"float",   0.0f, -360.0f, 360.0f,  1.0f },
                // Hidden: host writes the actual output-rect dimensions
                // every frame (see GraphEvaluator's pixel-shader eval).
                // The per-input ImageAW/AH/BW/BH pair dropped in v7 along
                // with the atlas-compensating Sample() path.
                Graph::ParameterDefinition{ L"OutputW", L"float", 1.0f, 1.0f, 16384.0f, 1.0f, {}, L"", true },
                Graph::ParameterDefinition{ L"OutputH", L"float", 1.0f, 1.0f, 16384.0f, 1.0f, {}, L"", true },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Image Statistics ----
        // D3D11 compute shader with stride-based reduction + 256-bin histogram.
        // Built-in path uses GpuReduction (RWBuffer<uint>); user-modified path
        // uses D3D11ComputeRunner (RWStructuredBuffer<float4>) via Effect Designer.
        {
            static const std::string channelStatsHLSL = R"HLSL(
// Channel Statistics - D3D11 Compute Shader
// Population stats for a single colour channel (R/G/B/A). For
// per-luminance stats use Luminance Statistics; for chromaticity stats
// use Chromaticity Statistics.
//
// Multi-group reduction (SHADERLAB_REDUCE_SCRATCH): SHADERLAB_REDUCE_GROUPS
// groups of 32x32 threads each take every G-th row, reduce into groupshared,
// and write a partial block to scratch; the last group to finish folds the
// partials and computes median/P95 from the merged 256-bin histogram. It
// used to be ONE group walking the whole frame on a single CU.
//
// Result[0..6]: Min, Max, Mean, Median, P95, Samples, Nonzero%

#include "shaderlab_params.hlsli"

Texture2D<float4> Source : register(t0);
RWStructuredBuffer<float4> Result : register(u0);
SHADERLAB_REDUCE_SCRATCH

cbuffer Constants : register(b0)
{
    uint Width;
    uint Height;
    uint Channel;      // 0=R, 1=G, 2=B, 3=A
    uint NonzeroOnly;  // 0=all, 1=nonzero only
};

#define GROUP_SIZE 32
#define THREAD_COUNT (GROUP_SIZE * GROUP_SIZE)
#define HIST_BINS 256
#define HIST_MAX 100.0

// Scratch words: per-group partial blocks, then per-group histograms.
#define PART_STRIDE 8
#define PART_BASE   SHADERLAB_REDUCE_CLEARED
#define HIST_BASE   (PART_BASE + SHADERLAB_REDUCE_GROUPS * PART_STRIDE)

groupshared float gs_min[THREAD_COUNT];
groupshared float gs_max[THREAD_COUNT];
groupshared float gs_sum[THREAD_COUNT];
groupshared uint  gs_count[THREAD_COUNT];
groupshared uint  gs_total[THREAD_COUNT];
groupshared uint  gs_nonzero[THREAD_COUNT];
groupshared uint  gs_hist[HIST_BINS];

float GetValue(float4 pix, uint ch)
{
    if (ch == 0) return pix.r;
    if (ch == 1) return pix.g;
    if (ch == 2) return pix.b;
    return pix.a;
}

void ReduceShared(uint tid)
{
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = THREAD_COUNT / 2; stride > 0; stride >>= 1)
    {
        if (tid < stride)
        {
            gs_min[tid] = min(gs_min[tid], gs_min[tid + stride]);
            gs_max[tid] = max(gs_max[tid], gs_max[tid + stride]);
            gs_sum[tid] += gs_sum[tid + stride];
            gs_count[tid] += gs_count[tid + stride];
            gs_total[tid] += gs_total[tid + stride];
            gs_nonzero[tid] += gs_nonzero[tid + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

[numthreads(GROUP_SIZE, GROUP_SIZE, 1)]
void main(uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID)
{
    uint tid = GTid.x + GTid.y * GROUP_SIZE;
    uint g = Gid.x;

    for (uint bi = tid; bi < HIST_BINS; bi += THREAD_COUNT)
        gs_hist[bi] = 0;
    GroupMemoryBarrierWithGroupSync();

    float tMin = 1e30;
    float tMax = -1e30;
    float tSum = 0;
    uint  tCount = 0;
    uint  tTotal = 0;
    uint  tNonzero = 0;

    for (uint y = g; y < Height; y += SHADERLAB_REDUCE_GROUPS)
    {
        for (uint x = tid; x < Width; x += THREAD_COUNT)
        {
            float4 pix = Source.Load(int3(x, y, 0));
            float v = GetValue(pix, Channel);

            tTotal++;
            bool nz = abs(v) > 0.0001;
            if (nz) tNonzero++;
            if (NonzeroOnly == 1 && !nz) continue;

            tMin = min(tMin, v);
            tMax = max(tMax, v);
            tSum += v;
            tCount++;

            float normalized = saturate(v / HIST_MAX);
            uint bin = min((uint)(normalized * 255.0), 255u);
            InterlockedAdd(gs_hist[bin], 1u);
        }
    }

    gs_min[tid] = tMin;
    gs_max[tid] = tMax;
    gs_sum[tid] = tSum;
    gs_count[tid] = tCount;
    gs_total[tid] = tTotal;
    gs_nonzero[tid] = tNonzero;
    ReduceShared(tid);

    // This group's partial block + histogram.
    uint pb = PART_BASE + g * PART_STRIDE;
    if (tid == 0)
    {
        ShaderLabScratchStore(pb + 0, asuint(gs_min[0]));
        ShaderLabScratchStore(pb + 1, asuint(gs_max[0]));
        ShaderLabScratchStore(pb + 2, asuint(gs_sum[0]));
        ShaderLabScratchStore(pb + 3, gs_count[0]);
        ShaderLabScratchStore(pb + 4, gs_total[0]);
        ShaderLabScratchStore(pb + 5, gs_nonzero[0]);
    }
    for (uint hb = tid; hb < HIST_BINS; hb += THREAD_COUNT)
        ShaderLabScratchStore(HIST_BASE + g * HIST_BINS + hb, gs_hist[hb]);

    if (!ShaderLabReduceIsLastGroup(tid))
        return;

    // Last group: fold every group's partials.
    if (tid < SHADERLAB_REDUCE_GROUPS)
    {
        uint b2 = PART_BASE + tid * PART_STRIDE;
        gs_min[tid]     = asfloat(ShaderLabScratchLoad(b2 + 0));
        gs_max[tid]     = asfloat(ShaderLabScratchLoad(b2 + 1));
        gs_sum[tid]     = asfloat(ShaderLabScratchLoad(b2 + 2));
        gs_count[tid]   = ShaderLabScratchLoad(b2 + 3);
        gs_total[tid]   = ShaderLabScratchLoad(b2 + 4);
        gs_nonzero[tid] = ShaderLabScratchLoad(b2 + 5);
    }
    else
    {
        gs_min[tid] = 1e30; gs_max[tid] = -1e30; gs_sum[tid] = 0;
        gs_count[tid] = 0; gs_total[tid] = 0; gs_nonzero[tid] = 0;
    }
    for (uint fb = tid; fb < HIST_BINS; fb += THREAD_COUNT)
    {
        uint s = 0;
        for (uint gg = 0; gg < SHADERLAB_REDUCE_GROUPS; ++gg)
            s += ShaderLabScratchLoad(HIST_BASE + gg * HIST_BINS + fb);
        gs_hist[fb] = s;
    }
    ReduceShared(tid);

    if (tid == 0)
    {
        float fMin = gs_min[0];
        float fMax = gs_max[0];
        uint  totalSamples = gs_count[0];
        uint  totalPixels = gs_total[0];
        uint  nonzeroPixels = gs_nonzero[0];
        float fMean = (totalSamples > 0) ? (gs_sum[0] / float(totalSamples)) : 0.0;

        float fMedian = 0.0;
        float fP95 = 0.0;
        uint medianTarget = totalSamples / 2;
        uint p95Target = (uint)(totalSamples * 0.95);
        uint cumulative = 0;
        bool foundMedian = false;
        bool foundP95 = false;

        for (uint b = 0; b < HIST_BINS; b++)
        {
            cumulative += gs_hist[b];
            float binValue = (float(b) + 0.5) / 255.0 * HIST_MAX;
            if (!foundMedian && cumulative >= medianTarget) { fMedian = binValue; foundMedian = true; }
            if (!foundP95    && cumulative >= p95Target)    { fP95 = binValue;    foundP95 = true; }
        }

        float nonzeroPct = (totalPixels > 0)
            ? float(nonzeroPixels) / float(totalPixels) : 0.0;

        Result[0] = float4(fMin, 0, 0, 0);
        Result[1] = float4(fMax, 0, 0, 0);
        Result[2] = float4(fMean, 0, 0, 0);
        Result[3] = float4(fMedian, 0, 0, 0);
        Result[4] = float4(fP95, 0, 0, 0);
        Result[5] = float4(float(totalSamples), 0, 0, 0);
        Result[6] = float4(nonzeroPct, 0, 0, 0);
    }
}
)HLSL";
            ShaderLabEffectDescriptor desc;
            desc.name = L"Channel Statistics";
            desc.effectId = L"Channel Statistics"; desc.effectVersion = 2;
            desc.category = L"Analysis";
            desc.subcategory = L"Statistics";
            desc.shaderType = Graph::CustomShaderType::D3D11ComputeShader;
            desc.hlslSource = channelStatsHLSL;
            desc.dataOnly = true;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"Channel",     L"float", 0.0f, 0.0f, 3.0f, 1.0f, { L"Red", L"Green", L"Blue", L"Alpha" } },
                { L"NonzeroOnly", L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"All Pixels", L"Nonzero Only" } },
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Min",      Graph::AnalysisFieldType::Float },
                { L"Max",      Graph::AnalysisFieldType::Float },
                { L"Mean",     Graph::AnalysisFieldType::Float },
                { L"Median",   Graph::AnalysisFieldType::Float },
                { L"P95",      Graph::AnalysisFieldType::Float },
                { L"Samples",  Graph::AnalysisFieldType::Float },
                { L"Nonzero%", Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Luminance Statistics ----
        // Population stats on BT.709 luminance, with HDR-aware extras
        // for adaptive tone mapping. Histogram is log-spaced (8 decades
        // covering ~0.001..10000 nits) so percentiles stay accurate at
        // the bright end where adaptive tone mapping cares the most.
        {
            static const std::string luminanceStatsHLSL = R"HLSL(
// Luminance Statistics - D3D11 Compute Shader
// Outputs (in nits when Units = 1, normalized otherwise):
//   Min, Max, Mean, Median, P95, P99, AvgLog, ClippedFraction, Samples,
//   WhiteErrorBudget
//
// WhiteErrorBudget = min(ClippedFraction * BudgetScale, BudgetMax), computed
// HERE rather than in a downstream Numeric Expression node. That is a
// performance decision, not a tidiness one: Numeric Expression evaluates on
// the CPU, so binding it to ClippedFraction forces this node's analysis buffer
// through a GPU->CPU Map() every frame -- measured at 10.2 ms of CPU stall per
// frame at 5.2 Mpx, because the Map waits on the GPU and destroys CPU/GPU
// overlap. Producing the value on the GPU removes that consumer entirely.
//
// Units enum:
//   0 = Normalized (scRGB Y, where 1.0 = SDR white = 80 nits)
//   1 = Nits (Y * 80)
//
// ClippedFraction: fraction of pixels whose luminance >= ClipNits.

// Multi-group reduction (SHADERLAB_REDUCE_SCRATCH): SHADERLAB_REDUCE_GROUPS
// groups each take every G-th row and write a partial block; the last group
// to finish folds them. It used to be ONE 32x32 group walking the whole frame
// on a single CU -- 1-6 ms at 4K for what is a memory-bound scan.

#include "shaderlab_params.hlsli"

Texture2D<float4> Source : register(t0);
RWStructuredBuffer<float4> Result : register(u0);
SHADERLAB_REDUCE_SCRATCH

cbuffer Constants : register(b0)
{
    uint Width;
    uint Height;
    uint Units;        // 0 = Normalized, 1 = Nits
    float ClipNits;
    float BudgetScale; // dE ITP of white error to spend per unit clipped
    float BudgetMax;   // ceiling, matching WhiteErrorBudget's own max
};

#define GROUP_SIZE 32
#define THREAD_COUNT (GROUP_SIZE * GROUP_SIZE)
#define HIST_BINS 256

// Log-spaced histogram covers 8 decades: 1e-2 .. 1e6 nits.
// nits = 10^(LOG_MIN + (bin / HIST_BINS) * LOG_RANGE)
#define LOG_MIN   -2.0
#define LOG_RANGE 8.0

// Scratch words: per-group partial blocks, then per-group histograms.
#define PART_STRIDE 8
#define PART_BASE   SHADERLAB_REDUCE_CLEARED
#define HIST_BASE   (PART_BASE + SHADERLAB_REDUCE_GROUPS * PART_STRIDE)

groupshared float gs_min[THREAD_COUNT];
groupshared float gs_max[THREAD_COUNT];
groupshared float gs_sum[THREAD_COUNT];
groupshared float gs_logsum[THREAD_COUNT];
groupshared uint  gs_count[THREAD_COUNT];
groupshared uint  gs_clipped[THREAD_COUNT];
groupshared uint  gs_hist[HIST_BINS];

void ReduceShared(uint tid)
{
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = THREAD_COUNT / 2; stride > 0; stride >>= 1)
    {
        if (tid < stride)
        {
            gs_min[tid] = min(gs_min[tid], gs_min[tid + stride]);
            gs_max[tid] = max(gs_max[tid], gs_max[tid + stride]);
            gs_sum[tid] += gs_sum[tid + stride];
            gs_logsum[tid] += gs_logsum[tid + stride];
            gs_count[tid] += gs_count[tid + stride];
            gs_clipped[tid] += gs_clipped[tid + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

[numthreads(GROUP_SIZE, GROUP_SIZE, 1)]
void main(uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID)
{
    uint tid = GTid.x + GTid.y * GROUP_SIZE;
    uint g = Gid.x;

    for (uint bi = tid; bi < HIST_BINS; bi += THREAD_COUNT)
        gs_hist[bi] = 0;
    GroupMemoryBarrierWithGroupSync();

    // Resolve clip threshold (always in nits internally).
    float clipNitsResolved = ClipNits;

    float tMin = 1e30;
    float tMax = -1e30;
    float tSum = 0;       // sum of nit values
    float tLogSum = 0;    // sum of log10(nits) for AvgLog
    uint  tCount = 0;
    uint  tClipped = 0;

    for (uint y = g; y < Height; y += SHADERLAB_REDUCE_GROUPS)
    {
        for (uint x = tid; x < Width; x += THREAD_COUNT)
        {
            float4 pix = Source.Load(int3(x, y, 0));
            float Y = max(0.0, dot(pix.rgb, float3(0.2126, 0.7152, 0.0722)));
            float nits = Y * 80.0;

            tCount++;
            tMin = min(tMin, nits);
            tMax = max(tMax, nits);
            tSum += nits;
            float lv = log10(max(nits, 1e-4));
            tLogSum += lv;
            if (nits >= clipNitsResolved) tClipped++;

            float t = (lv - LOG_MIN) / LOG_RANGE;
            uint bin = clamp((uint)(saturate(t) * (HIST_BINS - 1)), 0u, (uint)(HIST_BINS - 1));
            InterlockedAdd(gs_hist[bin], 1u);
        }
    }

    gs_min[tid] = tMin;
    gs_max[tid] = tMax;
    gs_sum[tid] = tSum;
    gs_logsum[tid] = tLogSum;
    gs_count[tid] = tCount;
    gs_clipped[tid] = tClipped;
    ReduceShared(tid);

    uint pb = PART_BASE + g * PART_STRIDE;
    if (tid == 0)
    {
        ShaderLabScratchStore(pb + 0, asuint(gs_min[0]));
        ShaderLabScratchStore(pb + 1, asuint(gs_max[0]));
        ShaderLabScratchStore(pb + 2, asuint(gs_sum[0]));
        ShaderLabScratchStore(pb + 3, asuint(gs_logsum[0]));
        ShaderLabScratchStore(pb + 4, gs_count[0]);
        ShaderLabScratchStore(pb + 5, gs_clipped[0]);
    }
    for (uint hb = tid; hb < HIST_BINS; hb += THREAD_COUNT)
        ShaderLabScratchStore(HIST_BASE + g * HIST_BINS + hb, gs_hist[hb]);

    if (!ShaderLabReduceIsLastGroup(tid))
        return;

    // Last group: fold every group's partials.
    if (tid < SHADERLAB_REDUCE_GROUPS)
    {
        uint b2 = PART_BASE + tid * PART_STRIDE;
        gs_min[tid]     = asfloat(ShaderLabScratchLoad(b2 + 0));
        gs_max[tid]     = asfloat(ShaderLabScratchLoad(b2 + 1));
        gs_sum[tid]     = asfloat(ShaderLabScratchLoad(b2 + 2));
        gs_logsum[tid]  = asfloat(ShaderLabScratchLoad(b2 + 3));
        gs_count[tid]   = ShaderLabScratchLoad(b2 + 4);
        gs_clipped[tid] = ShaderLabScratchLoad(b2 + 5);
    }
    else
    {
        gs_min[tid] = 1e30; gs_max[tid] = -1e30; gs_sum[tid] = 0;
        gs_logsum[tid] = 0; gs_count[tid] = 0; gs_clipped[tid] = 0;
    }
    for (uint fb = tid; fb < HIST_BINS; fb += THREAD_COUNT)
    {
        uint s = 0;
        for (uint gg = 0; gg < SHADERLAB_REDUCE_GROUPS; ++gg)
            s += ShaderLabScratchLoad(HIST_BASE + gg * HIST_BINS + fb);
        gs_hist[fb] = s;
    }
    ReduceShared(tid);

    if (tid == 0)
    {
        uint  totalSamples = gs_count[0];
        float fMin = gs_min[0];
        float fMax = gs_max[0];
        float fMean   = (totalSamples > 0) ? (gs_sum[0]    / float(totalSamples)) : 0.0;
        float fAvgLogLog = (totalSamples > 0) ? (gs_logsum[0] / float(totalSamples)) : 0.0;
        float fAvgLog = pow(10.0, fAvgLogLog);  // geometric mean in nits

        // Histogram CDF -> percentiles (in log-domain bin centres).
        float fMedian = 0.0;
        float fP95 = 0.0;
        float fP99 = 0.0;
        uint medianTarget = totalSamples / 2;
        uint p95Target = (uint)(totalSamples * 0.95);
        uint p99Target = (uint)(totalSamples * 0.99);
        uint cumulative = 0;
        bool foundMedian = false, foundP95 = false, foundP99 = false;

        for (uint b = 0; b < HIST_BINS; b++)
        {
            cumulative += gs_hist[b];
            float binLog = LOG_MIN + (float(b) + 0.5) / float(HIST_BINS) * LOG_RANGE;
            float binNits = pow(10.0, binLog);
            if (!foundMedian && cumulative >= medianTarget) { fMedian = binNits; foundMedian = true; }
            if (!foundP95    && cumulative >= p95Target)    { fP95 = binNits;    foundP95 = true; }
            if (!foundP99    && cumulative >= p99Target)    { fP99 = binNits;    foundP99 = true; }
        }

        float clippedFrac = (totalSamples > 0)
            ? float(gs_clipped[0]) / float(totalSamples) : 0.0;

        // Convert to normalized (scRGB Y) if requested.
        float scale = (Units == 1) ? 1.0 : (1.0 / 80.0);

        Result[0] = float4(fMin    * scale, 0, 0, 0);
        Result[1] = float4(fMax    * scale, 0, 0, 0);
        Result[2] = float4(fMean   * scale, 0, 0, 0);
        Result[3] = float4(fMedian * scale, 0, 0, 0);
        Result[4] = float4(fP95    * scale, 0, 0, 0);
        Result[5] = float4(fP99    * scale, 0, 0, 0);
        Result[6] = float4(fAvgLog * scale, 0, 0, 0);
        Result[7] = float4(clippedFrac, 0, 0, 0);
        Result[8] = float4(float(totalSamples), 0, 0, 0);
        // Adaptive white-error budget (a dE ITP allowance that grows with
        // the clipped fraction), ready to bind straight into a consumer's
        // gpu-bindable parameter with nothing CPU-side in between. Clamped here so a consumer cannot be handed a value
        // outside the parameter's declared range.
        Result[9] = float4(
            clamp(min(clippedFrac * BudgetScale, BudgetMax), 0.0, BudgetMax),
            0, 0, 0);
    }
}
)HLSL";
            ShaderLabEffectDescriptor desc;
            desc.name = L"Luminance Statistics";
            desc.effectId = L"Luminance Statistics"; desc.effectVersion = 4;
            desc.category = L"Analysis";
            desc.subcategory = L"Statistics";
            desc.shaderType = Graph::CustomShaderType::D3D11ComputeShader;
            desc.hlslSource = luminanceStatsHLSL;
            desc.dataOnly = true;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"Units",      L"float", 1.0f,    0.0f, 1.0f,    1.0f, { L"Normalized", L"Nits" } },
                { L"ClipNits",   L"float", 203.0f, 10.0f, 10000.0f, 1.0f },
                { L"BudgetScale", L"float", 15.0f,  0.0f, 100.0f,   0.5f },
                { L"BudgetMax",   L"float", 20.0f,  0.0f, 100.0f,   0.5f },
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Min",             Graph::AnalysisFieldType::Float },
                { L"Max",             Graph::AnalysisFieldType::Float },
                { L"Mean",            Graph::AnalysisFieldType::Float },
                { L"Median",          Graph::AnalysisFieldType::Float },
                { L"P95",             Graph::AnalysisFieldType::Float },
                { L"P99",             Graph::AnalysisFieldType::Float },
                { L"AvgLog",          Graph::AnalysisFieldType::Float },
                { L"ClippedFraction", Graph::AnalysisFieldType::Float },
                { L"Samples",         Graph::AnalysisFieldType::Float },
                { L"WhiteErrorBudget", Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Chromaticity Statistics ----
        // ICtCp-domain stats: average and peak chroma, mean hue. Free in
        // ICtCp and meaningful precisely because Ct/Cp distance is
        // perceptually uniform — so MeanChroma corresponds to "how
        // saturated the image looks" in a way no RGB-domain stat does.
        {
            static const std::string chromaStatsHLSL = R"HLSL(
// Chromaticity Statistics - D3D11 Compute Shader
// All stats computed in BT.2100 ICtCp.
// Result fields:
//   [0] MeanCt
//   [1] MeanCp
//   [2] MeanChroma   = mean of length(Ct, Cp)
//   [3] MaxChroma
//   [4] MeanHueDeg   = degrees from atan2(Cp, Ct), wrapped 0..360
//   [5] Samples

// Multi-group reduction (SHADERLAB_REDUCE_SCRATCH); see Luminance Statistics.

#include "shaderlab_params.hlsli"

Texture2D<float4> Source : register(t0);
RWStructuredBuffer<float4> Result : register(u0);
SHADERLAB_REDUCE_SCRATCH

cbuffer Constants : register(b0)
{
    uint Width;
    uint Height;
};

#define GROUP_SIZE 32
#define THREAD_COUNT (GROUP_SIZE * GROUP_SIZE)
#define PART_STRIDE 8
#define PART_BASE   SHADERLAB_REDUCE_CLEARED

groupshared float gs_ct[THREAD_COUNT];
groupshared float gs_cp[THREAD_COUNT];
groupshared float gs_chroma[THREAD_COUNT];
groupshared float gs_chromaMax[THREAD_COUNT];
groupshared float gs_huex[THREAD_COUNT];   // accumulator for mean-of-angles via unit vectors
groupshared float gs_huey[THREAD_COUNT];
groupshared uint  gs_count[THREAD_COUNT];

void ReduceShared(uint tid)
{
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = THREAD_COUNT / 2; stride > 0; stride >>= 1)
    {
        if (tid < stride)
        {
            gs_ct[tid] += gs_ct[tid + stride];
            gs_cp[tid] += gs_cp[tid + stride];
            gs_chroma[tid] += gs_chroma[tid + stride];
            gs_chromaMax[tid] = max(gs_chromaMax[tid], gs_chromaMax[tid + stride]);
            gs_huex[tid] += gs_huex[tid + stride];
            gs_huey[tid] += gs_huey[tid + stride];
            gs_count[tid] += gs_count[tid + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

[numthreads(GROUP_SIZE, GROUP_SIZE, 1)]
void main(uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID)
{
    uint tid = GTid.x + GTid.y * GROUP_SIZE;
    uint g = Gid.x;

    float tCt = 0, tCp = 0;
    float tChroma = 0;
    float tChromaMax = 0;
    float tHx = 0, tHy = 0;
    uint  tCount = 0;

    for (uint y = g; y < Height; y += SHADERLAB_REDUCE_GROUPS)
    {
        for (uint x = tid; x < Width; x += THREAD_COUNT)
        {
            float4 pix = Source.Load(int3(x, y, 0));
            float3 ictcp = ScRGBToICtCp(pix.rgb);
            float ct = ictcp.y;
            float cp = ictcp.z;
            float chroma = sqrt(ct * ct + cp * cp);

            tCt += ct;
            tCp += cp;
            tChroma += chroma;
            tChromaMax = max(tChromaMax, chroma);
            // Mean of angles via unit-vector sum (the only correct way).
            if (chroma > 1e-6) {
                tHx += ct / chroma;
                tHy += cp / chroma;
            }
            tCount++;
        }
    }

    gs_ct[tid] = tCt;
    gs_cp[tid] = tCp;
    gs_chroma[tid] = tChroma;
    gs_chromaMax[tid] = tChromaMax;
    gs_huex[tid] = tHx;
    gs_huey[tid] = tHy;
    gs_count[tid] = tCount;
    ReduceShared(tid);

    uint pb = PART_BASE + g * PART_STRIDE;
    if (tid == 0)
    {
        ShaderLabScratchStore(pb + 0, asuint(gs_ct[0]));
        ShaderLabScratchStore(pb + 1, asuint(gs_cp[0]));
        ShaderLabScratchStore(pb + 2, asuint(gs_chroma[0]));
        ShaderLabScratchStore(pb + 3, asuint(gs_chromaMax[0]));
        ShaderLabScratchStore(pb + 4, asuint(gs_huex[0]));
        ShaderLabScratchStore(pb + 5, asuint(gs_huey[0]));
        ShaderLabScratchStore(pb + 6, gs_count[0]);
    }

    if (!ShaderLabReduceIsLastGroup(tid))
        return;

    if (tid < SHADERLAB_REDUCE_GROUPS)
    {
        uint b2 = PART_BASE + tid * PART_STRIDE;
        gs_ct[tid]        = asfloat(ShaderLabScratchLoad(b2 + 0));
        gs_cp[tid]        = asfloat(ShaderLabScratchLoad(b2 + 1));
        gs_chroma[tid]    = asfloat(ShaderLabScratchLoad(b2 + 2));
        gs_chromaMax[tid] = asfloat(ShaderLabScratchLoad(b2 + 3));
        gs_huex[tid]      = asfloat(ShaderLabScratchLoad(b2 + 4));
        gs_huey[tid]      = asfloat(ShaderLabScratchLoad(b2 + 5));
        gs_count[tid]     = ShaderLabScratchLoad(b2 + 6);
    }
    else
    {
        gs_ct[tid] = 0; gs_cp[tid] = 0; gs_chroma[tid] = 0; gs_chromaMax[tid] = 0;
        gs_huex[tid] = 0; gs_huey[tid] = 0; gs_count[tid] = 0;
    }
    ReduceShared(tid);

    if (tid == 0)
    {
        uint  N = gs_count[0];
        float invN = (N > 0) ? (1.0 / float(N)) : 0.0;
        float meanCt = gs_ct[0] * invN;
        float meanCp = gs_cp[0] * invN;
        float meanChroma = gs_chroma[0] * invN;
        float maxChroma = gs_chromaMax[0];

        float angle = atan2(gs_huey[0], gs_huex[0]);  // radians, [-pi, pi]
        float deg = degrees(angle);
        if (deg < 0.0) deg += 360.0;

        Result[0] = float4(meanCt, 0, 0, 0);
        Result[1] = float4(meanCp, 0, 0, 0);
        Result[2] = float4(meanChroma, 0, 0, 0);
        Result[3] = float4(maxChroma, 0, 0, 0);
        Result[4] = float4(deg, 0, 0, 0);
        Result[5] = float4(float(N), 0, 0, 0);
    }
}
)HLSL";
            ShaderLabEffectDescriptor desc;
            desc.name = L"Chromaticity Statistics";
            desc.effectId = L"Chromaticity Statistics"; desc.effectVersion = 2;
            desc.category = L"Analysis";
            desc.subcategory = L"Statistics";
            desc.shaderType = Graph::CustomShaderType::D3D11ComputeShader;
            desc.hlslSource = colorMath + chromaStatsHLSL;
            desc.dataOnly = true;
            desc.inputNames = { L"Source" };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"MeanCt",     Graph::AnalysisFieldType::Float },
                { L"MeanCp",     Graph::AnalysisFieldType::Float },
                { L"MeanChroma", Graph::AnalysisFieldType::Float },
                { L"MaxChroma",  Graph::AnalysisFieldType::Float },
                { L"MeanHueDeg", Graph::AnalysisFieldType::Float },
                { L"Samples",    Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Parameter: Float Slider ----
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Float Parameter";
            desc.effectId = L"Float Parameter"; desc.effectVersion = 1;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            // No HLSL — evaluator handles parameter nodes directly.
            desc.parameters = {
                { L"Value", L"float", 0.5f, 0.0f, 1.0f, 0.01f },
                { L"Min",   L"float", 0.0f, -10000.0f, 10000.0f, 0.1f },
                { L"Max",   L"float", 1.0f, -10000.0f, 10000.0f, 0.1f },
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Value", Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Parameter: Integer Slider ----
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Integer Parameter";
            desc.effectId = L"Integer Parameter"; desc.effectVersion = 1;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.parameters = {
                { L"Value", L"float", 0.0f, 0.0f, 10.0f, 1.0f },
                { L"Min",   L"float", 0.0f, -10000.0f, 10000.0f, 1.0f },
                { L"Max",   L"float", 10.0f, -10000.0f, 10000.0f, 1.0f },
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Value", Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Parameter: Toggle ----
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Toggle Parameter";
            desc.effectId = L"Toggle Parameter"; desc.effectVersion = 1;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.parameters = {
                { L"Value", L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"Off", L"On" } },
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Value", Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Parameter: Gamut Selector ----
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Gamut Parameter";
            desc.effectId = L"Gamut Parameter"; desc.effectVersion = 2;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.parameters = {
                { L"Value", L"float", 0.0f, 0.0f, 2.0f, 1.0f, { L"sRGB", L"DCI-P3", L"BT.2020" } },
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Value", Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Parameter: Clock ----
        // Time-based animation source. Outputs elapsed Time (seconds) and
        // Progress (0→1 normalized). Has on-node Play/Pause and seek slider.
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Clock";
            desc.effectId = L"Clock"; desc.effectVersion = 2;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.parameters = {
                { L"AutoDuration", L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"Off", L"On" } },
                { L"StartTime", L"float", 0.0f, 0.0f, 3600.0f, 0.1f },
                { L"StopTime",  L"float", 10.0f, 0.0f, 3600.0f, 0.1f },
                { L"Speed",     L"float", 1.0f, -10.0f, 10.0f, 0.1f },
                { L"Loop",      L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"Off", L"On" } },
                // How often the clock actually EMITS, in Hz. 0 = every frame,
                // which is what it always did. Above 0, Time and Progress are
                // quantised to 1/rate and consumers are dirtied only when that
                // quantised value changes -- so the rate controls how often
                // downstream work is invalidated, not merely what number is
                // reported. That distinction is the point: a clock that
                // dirties every frame makes every downstream node re-run every
                // frame regardless of how slowly its value is moving.
                { L"UpdateRate", L"float", 0.0f, 0.0f, 240.0f, 1.0f },
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Time",     Graph::AnalysisFieldType::Float },
                { L"Progress", Graph::AnalysisFieldType::Float },
            };
            // Mark as clock node for special render loop handling.
            desc.isClock = true;
            m_effects.push_back(std::move(desc));
        }

        // ---- Numeric Expression Parameter Node ----
        // A single configurable math node that evaluates a user-provided
        // expression with up to 5 named scalar inputs. Replaces the older
        // Add/Subtract/Multiply/Divide/Min/Max nodes — all of those are
        // expressible as a one-line formula here (e.g. "max(A,B)", "A*B+C").
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Numeric Expression";
            desc.effectId = L"Math Expression"; desc.effectVersion = 1;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.parameters = {
                { L"Expression", L"string", std::wstring(L"A"), 0.0f, 0.0f, 0.0f },
                { L"A", L"float", 0.0f, -100000.0f, 100000.0f, 0.1f },
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Result", Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Random Parameter Node ----
        // Takes a single float Seed and produces a deterministic, well-mixed
        // float in [0, 1]. The output is a pure function of the seed value:
        // whenever the upstream value changes (e.g. driven by a Clock or
        // Numeric Expression), a fresh random number is produced; identical
        // seeds always reproduce the same output, which keeps graphs and
        // pixel-trace results deterministic.
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Random";
            desc.effectId = L"Random"; desc.effectVersion = 1;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.parameters = {
                { L"Seed", L"float", 0.0f, -1000000.0f, 1000000.0f, 0.01f },
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Result", Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- ShaderLab Scale (Resampling) ----
        // D3D11 compute scale effect with filter algorithms beyond what D2D's
        // CLSID_D2D1Scale natively offers (Lanczos-3, Mitchell-Netravali,
        // Catmull-Rom, Box / area, Gaussian). Useful as a hand-inserted
        // resolution cap in heavy graphs: insert between Source and the
        // visual branch and the entire downstream chain renders at the
        // smaller resolution.
        //
        // Output dimensions are explicitly OutputWidth + OutputHeight. To
        // drive them from source dimensions, add an Image Info node on the
        // same input and route ImageInfo.Width / .Height through Numeric
        // Expression nodes (e.g. expr "A * 0.5") into Scale.OutputWidth /
        // .OutputHeight.
        //
        // OutputWidth = 0 or OutputHeight = 0 falls back to the source's
        // bounds (no scaling).
        {
            static const std::string scaleHLSL = R"HLSL(
// ShaderLab Scale -- D3D11 compute resampler with multi-tap filters.
// Output pixel (x,y) reads a windowed neighborhood around the corresponding
// source location and combines the samples with the chosen kernel.

Texture2D<float4>   Source      : register(t0);
RWTexture2D<float4> ImageOutput : register(u1);

cbuffer constants : register(b0) {
    uint  Width;          // source dims (auto-injected from input #0)
    uint  Height;
    uint  OutputWidth;    // 0 = pass-through (= source Width)
    uint  OutputHeight;   // 0 = pass-through (= source Height)
    uint  FilterMode;     // 0=Bilinear 1=CatmullRom 2=Mitchell 3=Lanczos3 4=Box 5=Gaussian
};

// ---- Filter kernels (1D weights; separable for 2D) ---------------------

float W_bilinear(float t) { return max(0.0, 1.0 - abs(t)); }

float W_catmullrom(float t) {
    t = abs(t);
    if (t < 1.0) return  1.5*t*t*t - 2.5*t*t + 1.0;
    if (t < 2.0) return -0.5*t*t*t + 2.5*t*t - 4.0*t + 2.0;
    return 0.0;
}

// Mitchell-Netravali B=1/3, C=1/3 -- balanced sharpness/ringing default.
float W_mitchell(float t) {
    t = abs(t);
    const float B = 1.0/3.0, C = 1.0/3.0;
    if (t < 1.0) {
        return ((12.0 - 9.0*B - 6.0*C)*t*t*t
              + (-18.0 + 12.0*B + 6.0*C)*t*t
              + (6.0 - 2.0*B)) / 6.0;
    }
    if (t < 2.0) {
        return ((-B - 6.0*C)*t*t*t
              + (6.0*B + 30.0*C)*t*t
              + (-12.0*B - 48.0*C)*t
              + (8.0*B + 24.0*C)) / 6.0;
    }
    return 0.0;
}

float W_lanczos3(float t) {
    t = abs(t);
    if (t < 1e-5) return 1.0;
    if (t >= 3.0) return 0.0;
    float pt = 3.14159265 * t;
    return 3.0 * sin(pt) * sin(pt / 3.0) / (pt * pt);
}

float W_box(float t, float halfWidth) {
    return abs(t) <= halfWidth ? 1.0 : 0.0;
}

float W_gauss(float t) {
    // sigma = 0.5 -- tight kernel. Effective radius widens with downscale
    // ratio via the support multiplier in main().
    return exp(-(t*t) * 2.0);   // = exp(-t^2 / (2*0.5^2))
}

float Kernel1D(uint mode, float t) {
    if      (mode == 0) return W_bilinear(t);
    else if (mode == 1) return W_catmullrom(t);
    else if (mode == 2) return W_mitchell(t);
    else if (mode == 3) return W_lanczos3(t);
    else if (mode == 4) return W_box(t, 0.5);
    return W_gauss(t);
}

// ---- Separable filtering, one dispatch -------------------------------------
// The kernel is separable (w = wx * wy), so each 8x8 output tile filters the
// SOURCE rows it needs horizontally into groupshared, then filters those
// vertically. Rows are streamed through a small buffer CHUNK rows at a time,
// so any scale ratio works with one code path. Each 1-D weight is evaluated
// once per tile column / row instead of once per tap, and each source texel
// is read ~once per tile column instead of once per overlapping output tap:
// a Lanczos-3 4x downscale went from 625 taps + 2,500 sin per output pixel to
// ~25. The sum is the same mathematically (sum_y wy * sum_x wx * S) but in a
// different order, so results differ from the old 2-D loop by rounding only.
//
// Groupshared is kept small on purpose: an 8 KB buffer measured an 8x8 tile
// upscale at 2x the time of the plain per-pixel loop, from occupancy alone.
#define TILE      8
#define MAX_R     12
#define MAX_TAPS  (2 * MAX_R + 1)
#define CHUNK     16
groupshared float4 gsH[CHUNK * TILE];
groupshared float  gsWx[TILE * MAX_TAPS];
groupshared float  gsWy[TILE * MAX_TAPS];

[numthreads(TILE, TILE, 1)]
void main(uint3 dtid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    // Pass-through when OutputWidth/Height not set: act as identity.
    uint outW = (OutputWidth  > 0) ? OutputWidth  : Width;
    uint outH = (OutputHeight > 0) ? OutputHeight : Height;
    const bool active = dtid.x < outW && dtid.y < outH;

    float2 outSize   = float2(outW, outH);
    float2 srcSize   = float2(Width, Height);
    float2 ratio     = srcSize / outSize;             // src per dst pixel

    // For downscale (ratio > 1) we widen the kernel by the ratio so the
    // filter integrates over the *destination footprint* in source pixels;
    // otherwise samples alias. For upscale (ratio < 1) we keep support = 1.
    float2 support = max(ratio, float2(1.0, 1.0));

    // Per-mode base radius in kernel domain (half-width of the kernel).
    uint mode = FilterMode;
    float baseRadius = 1.0;
    if      (mode == 1) baseRadius = 2.0;   // Catmull-Rom
    else if (mode == 2) baseRadius = 2.0;   // Mitchell
    else if (mode == 3) baseRadius = 3.0;   // Lanczos-3
    else if (mode == 4) baseRadius = 0.5;   // Box (half-width)
    else if (mode == 5) baseRadius = 2.0;   // Gaussian (truncated)

    // Effective radius in source pixels after widening for downscale.
    float2 effR = baseRadius * support;
    int2   r    = int2(ceil(effR));
    r = clamp(r, int2(1, 1), int2(MAX_R, MAX_R));   // safety cap
    const int nx = 2 * r.x + 1;
    const int ny = 2 * r.y + 1;

    const uint tid = gtid.y * TILE + gtid.x;
    const uint ox0 = gid.x * TILE;
    const uint oy0 = gid.y * TILE;

    // 1-D weights: one set per tile column (horizontal) and row (vertical),
    // each at the edge-clamped tap position, exactly as the 2-D loop had.
    for (uint i = tid; i < (uint)(TILE * nx); i += TILE * TILE)
    {
        uint c = i / (uint)nx, k = i % (uint)nx;
        float scx = (ox0 + c + 0.5) * ratio.x - 0.5;
        int   sx  = clamp(int(floor(scx)) + int(k) - r.x, 0, int(Width) - 1);
        gsWx[c * MAX_TAPS + k] = Kernel1D(mode, (float(sx) + 0.5 - scx) / support.x);
    }
    for (uint j = tid; j < (uint)(TILE * ny); j += TILE * TILE)
    {
        uint c = j / (uint)ny, k = j % (uint)ny;
        float scy = (oy0 + c + 0.5) * ratio.y - 0.5;
        int   sy  = clamp(int(floor(scy)) + int(k) - r.y, 0, int(Height) - 1);
        gsWy[c * MAX_TAPS + k] = Kernel1D(mode, (float(sy) + 0.5 - scy) / support.y);
    }
    GroupMemoryBarrierWithGroupSync();

    // Source rows this tile spans, before edge clamping.
    const int rowLo = int(floor((oy0 + 0.5) * ratio.y - 0.5)) - r.y;
    // This thread's first vertical tap.
    const int myLo = int(floor((dtid.y + 0.5) * ratio.y - 0.5)) - r.y;
    // Trip count from uniforms alone, so every barrier is in uniform flow:
    // a tile spans at most (TILE-1)*ratio + 1 centre rows, plus the taps.
    const int maxRows = int(ceil((TILE - 1) * ratio.y)) + 1 + ny;
    const int chunks  = (maxRows + CHUNK - 1) / CHUNK;

    float4 sum = float4(0, 0, 0, 0);
    [loop]
    for (int ch = 0; ch < chunks; ++ch)
    {
        const int base = rowLo + ch * CHUNK;
        // Horizontal pass: CHUNK source rows x every tile column.
        for (uint h = tid; h < CHUNK * TILE; h += TILE * TILE)
        {
            uint rr = h / TILE, c = h % TILE;
            int   sy  = clamp(base + int(rr), 0, int(Height) - 1);
            float scx = (ox0 + c + 0.5) * ratio.x - 0.5;
            int   icx = int(floor(scx));
            float4 acc = float4(0, 0, 0, 0);
            [loop]
            for (int k = 0; k < nx; ++k)
            {
                int sx = clamp(icx + k - r.x, 0, int(Width) - 1);
                acc += Source.Load(int3(sx, sy, 0)) * gsWx[c * MAX_TAPS + k];
            }
            gsH[rr * TILE + c] = acc;
        }
        GroupMemoryBarrierWithGroupSync();

        // Vertical pass: this thread's taps that fall in the chunk.
        const int k0 = max(0, base - myLo);
        const int k1 = min(ny - 1, base + CHUNK - 1 - myLo);
        [loop]
        for (int k = k0; k <= k1; ++k)
            sum += gsH[(myLo + k - base) * TILE + gtid.x] * gsWy[gtid.y * MAX_TAPS + k];
        GroupMemoryBarrierWithGroupSync();
    }

    if (!active) return;

    float wxSum = 0.0, wySum = 0.0;
    for (int kx = 0; kx < nx; ++kx) wxSum += gsWx[gtid.x * MAX_TAPS + kx];
    for (int ky = 0; ky < ny; ++ky) wySum += gsWy[gtid.y * MAX_TAPS + ky];
    float wsum = wxSum * wySum;

    ImageOutput[dtid.xy] = (wsum > 1e-6) ? (sum / wsum) : float4(0, 0, 0, 0);
}
)HLSL";
            ShaderLabEffectDescriptor desc;
            desc.name = L"Scale";
            desc.effectId = L"ShaderLab Scale"; desc.effectVersion = 2;
            desc.category = L"Color";
            desc.subcategory = L"Resampling";
            desc.shaderType = Graph::CustomShaderType::D3D11ComputeShader;
            desc.hasImageOutput = true;
            desc.threadGroupX = 8;
            desc.threadGroupY = 8;
            desc.threadGroupZ = 1;
            desc.hlslSource = scaleHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                // OutputWidth / OutputHeight are the primary controls.
                // 0 means "pass through at source dims". To drive them
                // from source dimensions, bind from an Image Info node
                // (see catalog) through a Numeric Expression.
                { L"OutputWidth",  L"uint", 0.0f, 0.0f, 16384.0f, 1.0f },
                { L"OutputHeight", L"uint", 0.0f, 0.0f, 16384.0f, 1.0f },
                { L"FilterMode",   L"float", 3.0f, 0.0f, 5.0f, 1.0f,
                    { L"Bilinear", L"Catmull-Rom Bicubic", L"Mitchell-Netravali",
                      L"Lanczos-3", L"Box (Area)", L"Gaussian" } },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Image Info ----
        // Analysis-only compute that exposes its source's dimensions and a
        // few derived quantities as typed analysis fields. The runner
        // auto-injects Width / Height into the cbuffer based on the input
        // texture; this shader just copies them into the result buffer
        // alongside two convenience derivations.
        //
        // Composes naturally with Numeric Expression to drive parameters
        // that should track the source -- e.g. ImageInfo.Width through
        // "A * 0.5" into Scale.OutputWidth for a half-resolution pass.
        {
            static const std::string imageInfoHLSL = R"HLSL(
// Image Info -- D3D11 compute, exposes source dims as analysis fields.

Texture2D<float4>          Source : register(t0);
RWStructuredBuffer<float4> Result : register(u0);

cbuffer constants : register(b0) {
    uint Width;
    uint Height;
};

[numthreads(1, 1, 1)]
void main()
{
    float w = float(Width);
    float h = float(Height);
    float aspect = (h > 0.5) ? (w / h) : 0.0;
    float pixels = w * h;
    // Each analysis field maps to its own Result[i].x slot.
    Result[0] = float4(w,      0, 0, 0);   // Width
    Result[1] = float4(h,      0, 0, 0);   // Height
    Result[2] = float4(aspect, 0, 0, 0);   // AspectRatio
    Result[3] = float4(pixels, 0, 0, 0);   // PixelCount
}
)HLSL";
            ShaderLabEffectDescriptor desc;
            desc.name = L"Image Info";
            desc.effectId = L"Image Info"; desc.effectVersion = 1;
            desc.category = L"Analysis";
            desc.subcategory = L"Statistics";
            desc.shaderType = Graph::CustomShaderType::D3D11ComputeShader;
            desc.dataOnly = true;
            desc.hasImageOutput = false;
            desc.threadGroupX = 1;
            desc.threadGroupY = 1;
            desc.threadGroupZ = 1;
            desc.hlslSource = imageInfoHLSL;
            desc.inputNames = { L"Source" };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Width",       Graph::AnalysisFieldType::Float },
                { L"Height",      Graph::AnalysisFieldType::Float },
                { L"AspectRatio", Graph::AnalysisFieldType::Float },
                { L"PixelCount",  Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Working Space Parameter Node ----
        // A first-class, host-driven parameter node that mirrors the active
        // display profile (live or simulated) as 14 typed analysis output
        // fields. Lets graphs wire other nodes (tone mapper, OOG, etc.) to
        // the live working space via the property-binding system instead of
        // duplicating per-effect "follow the display" toggles. Updated by
        // MainWindow::UpdateWorkingSpaceNodes() whenever the display profile
        // changes; values are written into node->properties keyed by field
        // name and unpacked into the analysis output by the regular
        // parameter-node branch in GraphEvaluator.
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Working Space";
            desc.effectId = L"Working Space"; desc.effectVersion = 1;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            // No parameters — values are entirely host-driven. Properties
            // panel will be empty for this node, exactly the desired UX.
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                // Display mode (Float, encoded as 0=SDR, 1=WCG/ACM, 2=HDR).
                { L"ActiveColorMode",   Graph::AnalysisFieldType::Float },
                // Capability + user-toggle flags (Float-as-bool, 0/1).
                { L"HdrSupported",      Graph::AnalysisFieldType::Float },
                { L"HdrUserEnabled",    Graph::AnalysisFieldType::Float },
                { L"WcgSupported",      Graph::AnalysisFieldType::Float },
                { L"WcgUserEnabled",    Graph::AnalysisFieldType::Float },
                { L"IsSimulated",       Graph::AnalysisFieldType::Float },
                // Luminance levels in nits.
                { L"SdrWhiteNits",      Graph::AnalysisFieldType::Float },
                { L"PeakNits",          Graph::AnalysisFieldType::Float },
                { L"MinNits",           Graph::AnalysisFieldType::Float },
                { L"MaxFullFrameNits",  Graph::AnalysisFieldType::Float },
                // Color primaries as CIE xy (Float2).
                { L"RedPrimary",        Graph::AnalysisFieldType::Float2 },
                { L"GreenPrimary",      Graph::AnalysisFieldType::Float2 },
                { L"BluePrimary",       Graph::AnalysisFieldType::Float2 },
                { L"WhitePoint",        Graph::AnalysisFieldType::Float2 },
            };
            // Bootstrap defaults — sRGB SDR. Lets the node show meaningful
            // values immediately on creation, even before MainWindow's
            // UpdateWorkingSpaceNodes() has had a chance to run.
            using winrt::Windows::Foundation::Numerics::float2;
            desc.hiddenDefaults = {
                { L"ActiveColorMode",   Graph::PropertyValue{ 0.0f } },
                { L"HdrSupported",      Graph::PropertyValue{ 0.0f } },
                { L"HdrUserEnabled",    Graph::PropertyValue{ 0.0f } },
                { L"WcgSupported",      Graph::PropertyValue{ 0.0f } },
                { L"WcgUserEnabled",    Graph::PropertyValue{ 0.0f } },
                { L"IsSimulated",       Graph::PropertyValue{ 0.0f } },
                { L"SdrWhiteNits",      Graph::PropertyValue{ 80.0f } },
                { L"PeakNits",          Graph::PropertyValue{ 80.0f } },
                { L"MinNits",           Graph::PropertyValue{ 0.5f } },
                { L"MaxFullFrameNits",  Graph::PropertyValue{ 80.0f } },
                { L"RedPrimary",        Graph::PropertyValue{ float2{ 0.640f, 0.330f } } },
                { L"GreenPrimary",      Graph::PropertyValue{ float2{ 0.300f, 0.600f } } },
                { L"BluePrimary",       Graph::PropertyValue{ float2{ 0.150f, 0.060f } } },
                { L"WhitePoint",        Graph::PropertyValue{ float2{ 0.3127f, 0.3290f } } },
            };
            m_effects.push_back(std::move(desc));
        }
    }
}
