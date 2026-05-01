#include "pch.h"
#include "ShaderLabEffects.h"

namespace ShaderLab::Effects
{
    // -----------------------------------------------------------------------
    // Shared color math HLSL (prepended to all ShaderLab shaders)
    // -----------------------------------------------------------------------

    static const std::string s_colorMathHLSL = R"HLSL(
// ---- ShaderLab Color Math Library ----

// scRGB: linear Rec.709 primaries, 1.0 = 80 nits SDR white
// Pipeline operates in FP16 scRGB throughout.

// sRGB EOTF (decode gamma)
float3 SRGBToLinear(float3 c) {
    return float3(
        c.r <= 0.04045 ? c.r / 12.92 : pow((c.r + 0.055) / 1.055, 2.4),
        c.g <= 0.04045 ? c.g / 12.92 : pow((c.g + 0.055) / 1.055, 2.4),
        c.b <= 0.04045 ? c.b / 12.92 : pow((c.b + 0.055) / 1.055, 2.4));
}

// sRGB inverse EOTF (encode gamma)
float3 LinearToSRGB(float3 c) {
    return float3(
        c.r <= 0.0031308 ? c.r * 12.92 : 1.055 * pow(c.r, 1.0/2.4) - 0.055,
        c.g <= 0.0031308 ? c.g * 12.92 : 1.055 * pow(c.g, 1.0/2.4) - 0.055,
        c.b <= 0.0031308 ? c.b * 12.92 : 1.055 * pow(c.b, 1.0/2.4) - 0.055);
}

// scRGB (Rec.709 linear) -> CIE XYZ (D65)
static const float3x3 REC709_TO_XYZ = float3x3(
    0.4123908, 0.3575843, 0.1804808,
    0.2126390, 0.7151687, 0.0721923,
    0.0193308, 0.1191950, 0.9505322
);

// CIE XYZ (D65) -> scRGB (Rec.709 linear)
static const float3x3 XYZ_TO_REC709 = float3x3(
     3.2409699, -1.5373832, -0.4986108,
    -0.9692436,  1.8759675,  0.0415551,
     0.0556301, -0.2039770,  1.0569715
);

// scRGB -> CIE XYZ
float3 ScRGBToXYZ(float3 rgb) {
    return mul(REC709_TO_XYZ, rgb);
}

// CIE XYZ -> scRGB
float3 XYZToScRGB(float3 xyz) {
    return mul(XYZ_TO_REC709, xyz);
}

// CIE XYZ -> CIE xyY
float3 XYZToxyY(float3 xyz) {
    float sum = xyz.x + xyz.y + xyz.z;
    float2 xy = (sum < 1e-10) ? float2(0.3127, 0.3290) : float2(xyz.x / sum, xyz.y / sum);
    return float3(xy, xyz.y);
}

// CIE xyY -> CIE XYZ
float3 xyYToXYZ(float3 xyY) {
    float X = (xyY.y < 1e-10) ? 0.0 : xyY.x * xyY.z / xyY.y;
    float Z = (xyY.y < 1e-10) ? 0.0 : (1.0 - xyY.x - xyY.y) * xyY.z / xyY.y;
    return float3(X, xyY.z, Z);
}

// Luminance in nits from scRGB (1.0 scRGB = 80 nits)
float ScRGBToNits(float3 rgb) {
    return dot(rgb, float3(0.2126390, 0.7151687, 0.0721923)) * 80.0;
}

// Luminance in nits from scRGB (Y component, handles negative values)
float ScRGBLuminanceNits(float3 rgb) {
    return max(0.0, dot(rgb, float3(0.2126390, 0.7151687, 0.0721923))) * 80.0;
}

// PQ (ST.2084) EOTF: PQ signal [0,1] -> linear nits [0,10000]
float PQ_EOTF(float N) {
    float Np = pow(max(N, 0.0), 1.0 / 78.84375);
    float num = max(Np - 0.8359375, 0.0);
    float den = 18.8515625 - 18.6875 * Np;
    return 10000.0 * pow(num / max(den, 1e-10), 1.0 / 0.1593017578125);
}

// PQ inverse EOTF: linear nits [0,10000] -> PQ signal [0,1]
float PQ_InvEOTF(float L) {
    float Lp = pow(max(L, 0.0) / 10000.0, 0.1593017578125);
    float num = 0.8359375 + 18.8515625 * Lp;
    float den = 1.0 + 18.6875 * Lp;
    return pow(num / den, 78.84375);
}

// Rec.2020 linear -> CIE XYZ
static const float3x3 REC2020_TO_XYZ = float3x3(
    0.6369580, 0.1446169, 0.1688810,
    0.2627002, 0.6779981, 0.0593017,
    0.0000000, 0.0280727, 1.0609851
);

// CIE XYZ -> Rec.2020 linear
static const float3x3 XYZ_TO_REC2020 = float3x3(
     1.7166512, -0.3556708, -0.2533663,
    -0.6666844,  1.6164812,  0.0157685,
     0.0176399, -0.0427706,  0.9421031
);

// DCI-P3 (D65) linear -> CIE XYZ
static const float3x3 P3D65_TO_XYZ = float3x3(
    0.4865709, 0.2656677, 0.1982173,
    0.2289746, 0.6917385, 0.0792869,
    0.0000000, 0.0451134, 1.0439444
);

// CIE XYZ -> DCI-P3 (D65) linear
static const float3x3 XYZ_TO_P3D65 = float3x3(
     2.4934969, -0.9313836, -0.4027108,
    -0.8294890,  1.7626641,  0.0236247,
     0.0358458, -0.0761724,  0.9568845
);

// Gamut primaries in CIE xy coordinates
// Rec.709/sRGB
static const float2 GAMUT_709_R  = float2(0.64, 0.33);
static const float2 GAMUT_709_G  = float2(0.30, 0.60);
static const float2 GAMUT_709_B  = float2(0.15, 0.06);

// DCI-P3 (D65)
static const float2 GAMUT_P3_R   = float2(0.680, 0.320);
static const float2 GAMUT_P3_G   = float2(0.265, 0.690);
static const float2 GAMUT_P3_B   = float2(0.150, 0.060);

// Rec.2020
static const float2 GAMUT_2020_R = float2(0.708, 0.292);
static const float2 GAMUT_2020_G = float2(0.170, 0.797);
static const float2 GAMUT_2020_B = float2(0.131, 0.046);

// D65 white point
static const float2 D65_WHITE    = float2(0.3127, 0.3290);

// Check if point p is inside triangle (a, b, c) using barycentric coordinates
bool PointInTriangle(float2 p, float2 a, float2 b, float2 c) {
    float2 v0 = c - a, v1 = b - a, v2 = p - a;
    float d00 = dot(v0, v0);
    float d01 = dot(v0, v1);
    float d02 = dot(v0, v2);
    float d11 = dot(v1, v1);
    float d12 = dot(v1, v2);
    float inv = 1.0 / (d00 * d11 - d01 * d01);
    float u = (d11 * d02 - d01 * d12) * inv;
    float v = (d00 * d12 - d01 * d02) * inv;
    return (u >= 0) && (v >= 0) && (u + v <= 1.0);
}

// Turbo colormap approximation (for luminance heatmaps)
float3 TurboColormap(float t) {
    t = saturate(t);
    float r = saturate(0.13572138 + t * (4.61539260 + t * (-42.66032258 + t * (132.13108234 + t * (-152.94239396 + t * 59.28637943)))));
    float g = saturate(0.09140261 + t * (2.19418839 + t * (4.84296658 + t * (-14.18503333 + t * (4.27729857 + t * 2.82956604)))));
    float b = saturate(0.10667330 + t * (12.64194608 + t * (-60.58204836 + t * (110.36276771 + t * (-89.90310912 + t * 27.34824973)))));
    return float3(r, g, b);
}

// D65 reference white in XYZ
static const float3 D65_XYZ = float3(0.95047, 1.00000, 1.08883);

// CIE Lab helper
float LabF(float t) {
    // Signed extension: handle negative XYZ values from out-of-gamut scRGB.
    float at = abs(t);
    float ft = (at > 0.008856) ? pow(at, 1.0/3.0) : (7.787 * at + 16.0/116.0);
    return (t < 0.0) ? -ft : ft;
}

// CIE XYZ -> CIE L*a*b* (D65)
float3 XYZToLab(float3 xyz) {
    float fx = LabF(xyz.x / D65_XYZ.x);
    float fy = LabF(xyz.y / D65_XYZ.y);
    float fz = LabF(xyz.z / D65_XYZ.z);
    float L = 116.0 * fy - 16.0;
    float a = 500.0 * (fx - fy);
    float b = 200.0 * (fy - fz);
    return float3(L, a, b);
}

// scRGB -> CIE L*a*b*
float3 ScRGBToLab(float3 rgb) {
    return XYZToLab(ScRGBToXYZ(rgb));
}

// ---- ICtCp (BT.2100) ----
// Pipeline: scRGB -> XYZ -> LMS (BT.2124 cross-talk) -> PQ encode -> ICtCp

// XYZ to LMS (BT.2124 / Hunt-Pointer-Estevez with cross-talk)
static const float3x3 XYZ_TO_LMS_ICTCP = float3x3(
     0.3592832, 0.6976051, -0.0358916,
    -0.1920808, 1.1004768,  0.0753741,
     0.0070797, 0.0748262,  0.8433009
);

// LMS to XYZ (inverse)
static const float3x3 LMS_TO_XYZ_ICTCP = float3x3(
     2.0701800, -1.3264569,  0.2066510,
     0.3649882,  0.6805541, -0.0453723,
    -0.0496570, -0.0492033,  1.1880720
);

// PQ-encoded LMS to ICtCp
static const float3x3 PQLMS_TO_ICTCP = float3x3(
    2048.0/4096.0,  2048.0/4096.0,     0.0/4096.0,
    6610.0/4096.0, -13613.0/4096.0,  7003.0/4096.0,
   17933.0/4096.0, -17390.0/4096.0,  -543.0/4096.0
);

// ICtCp to PQ-encoded LMS (inverse)
static const float3x3 ICTCP_TO_PQLMS = float3x3(
    1.0,  0.008609037,  0.111029625,
    1.0, -0.008609037, -0.111029625,
    1.0,  0.560031336, -0.320627175
);

// scRGB -> ICtCp
float3 ScRGBToICtCp(float3 rgb) {
    // scRGB (1.0 = 80 nits) -> absolute luminance XYZ
    float3 xyz = ScRGBToXYZ(max(rgb, 0.0));
    // Scale to absolute nits for PQ (XYZ Y=1 = 80 nits in scRGB)
    xyz *= 80.0;
    float3 lms = mul(XYZ_TO_LMS_ICTCP, xyz);
    lms = max(lms, 0.0);
    // PQ encode each LMS component (input in nits, output [0,1])
    float3 pqLms = float3(
        PQ_InvEOTF(lms.x),
        PQ_InvEOTF(lms.y),
        PQ_InvEOTF(lms.z));
    return mul(PQLMS_TO_ICTCP, pqLms);
}

// ICtCp -> scRGB
float3 ICtCpToScRGB(float3 ictcp) {
    float3 pqLms = mul(ICTCP_TO_PQLMS, ictcp);
    // PQ decode to nits
    float3 lms = float3(
        PQ_EOTF(pqLms.x),
        PQ_EOTF(pqLms.y),
        PQ_EOTF(pqLms.z));
    float3 xyz = mul(LMS_TO_XYZ_ICTCP, lms);
    // Scale back from nits to scRGB (80 nits = 1.0)
    xyz /= 80.0;
    return XYZToScRGB(xyz);
}

// OKLab: linear sRGB -> OKLab
float3 LinearToOKLab(float3 c) {
    float l = 0.4122214708 * c.r + 0.5363325363 * c.g + 0.0514459929 * c.b;
    float m = 0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b;
    float s = 0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b;
    float l_ = pow(max(l, 0.0), 1.0/3.0);
    float m_ = pow(max(m, 0.0), 1.0/3.0);
    float s_ = pow(max(s, 0.0), 1.0/3.0);
    return float3(
        0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
        1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
        0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_
    );
}
)HLSL";

    const std::string& GetColorMathHLSL()
    {
        return s_colorMathHLSL;
    }

    // -----------------------------------------------------------------------
    // Effect HLSL sources
    // -----------------------------------------------------------------------

    static const std::string s_luminanceHeatmapHLSL = R"HLSL(
// Luminance Heatmap - false-color visualization of luminance
Texture2D Source : register(t0);

cbuffer constants : register(b0) {
    float MinNits;     // default 0.0
    float MaxNits;     // default 10000.0
    float ColormapMode; // 0=Turbo, 1=Inferno
};

float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float4 color = Source.Load(int3(uv0.xy, 0));
    float nits = ScRGBLuminanceNits(color.rgb);
    float t = saturate((nits - MinNits) / max(MaxNits - MinNits, 0.001));
    float3 mapped = TurboColormap(t);
    // Turbo colormap outputs perceptual 0-1 values.
    // Keep as-is in scRGB (1.0 = 80 nits SDR white) for visible display.
    return float4(mapped, color.a);
}
)HLSL";

    static const std::string s_outOfGamutHLSL = R"HLSL(
// Gamut Highlight
// TargetGamut modes:
//   0 = Current Monitor (primaries injected by host)
//   1 = Rec.709
//   2 = DCI-P3
//   3 = Rec.2020
//   4 = Preview Mode (primaries follow Display dropdown)
Texture2D Source : register(t0);

cbuffer constants : register(b0) {
    float TargetGamut;
    float OverlayR;
    float OverlayG;
    float OverlayB;
    float OverlayStrength;
    float Mode;
    // Primaries for modes 0 and 4 (auto-injected, not user-visible)
    float PrimRedX_hidden;
    float PrimRedY_hidden;
    float PrimGreenX_hidden;
    float PrimGreenY_hidden;
    float PrimBlueX_hidden;
    float PrimBlueY_hidden;
};

float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float4 color = Source.Load(int3(uv0.xy, 0));
    if (color.a < 0.001) return color;

    // All modes use the same primaries-based approach.
    // For Rec.709/P3/2020, the evaluator could inject known primaries,
    // but we use matrix conversion for those (more accurate).
    // The Prim* variables are always referenced to prevent HLSL optimization.
    float3 xyz = ScRGBToXYZ(color.rgb);
    float3 targetRGB = color.rgb;

    // Force the compiler to keep Prim* in the cbuffer by always reading them.
    float2 mr = float2(PrimRedX_hidden, PrimRedY_hidden);
    float2 mg = float2(PrimGreenX_hidden, PrimGreenY_hidden);
    float2 mb = float2(PrimBlueX_hidden, PrimBlueY_hidden);

    if (TargetGamut > 0.5 && TargetGamut < 1.5) {
        targetRGB = color.rgb; // Rec.709
    } else if (TargetGamut > 1.5 && TargetGamut < 2.5) {
        targetRGB = mul(XYZ_TO_P3D65, xyz);
    } else if (TargetGamut > 2.5 && TargetGamut < 3.5) {
        targetRGB = mul(XYZ_TO_REC2020, xyz);
    } else {
        // Current Monitor (0) or Preview Mode (4): chromaticity triangle test
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

    // -----------------------------------------------------------------------
    // Registry
    // -----------------------------------------------------------------------

    ShaderLabEffects& ShaderLabEffects::Instance()
    {
        static ShaderLabEffects instance;
        return instance;
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

    void ShaderLabEffects::RegisterAll()
    {
        const auto& colorMath = GetColorMathHLSL();

        // ---- Luminance Heatmap ----
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Luminance Heatmap";
            desc.effectId = L"Luminance Heatmap"; desc.effectVersion = 1;
            desc.category = L"Analysis";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + s_luminanceHeatmapHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"MinNits",      L"float", 0.0f,    0.0f, 10000.0f, 1.0f },
                { L"MaxNits",      L"float", 10000.0f, 0.0f, 10000.0f, 100.0f },
                { L"ColormapMode", L"float", 0.0f, 0.0f, 1.0f, 1.0f, { L"Turbo", L"Inferno" } },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Gamut Highlight ----
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Gamut Highlight";
            desc.effectId = L"Gamut Highlight"; desc.effectVersion = 1;
            desc.category = L"Analysis";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + s_outOfGamutHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"TargetGamut",     L"float", 0.0f, 0.0f, 4.0f, 1.0f, { L"Current Monitor", L"Rec.709", L"DCI-P3", L"Rec.2020", L"Preview Mode" } },
                { L"OverlayR",        L"float", 1.0f,  0.0f, 1.0f, 0.01f },
                { L"OverlayG",        L"float", 0.0f,  0.0f, 1.0f, 0.01f },
                { L"OverlayB",        L"float", 1.0f,  0.0f, 1.0f, 0.01f },
                { L"OverlayStrength", L"float", 0.7f,  0.0f, 1.0f, 0.01f },
                { L"Mode",            L"float", 0.0f,  0.0f, 1.0f, 1.0f, { L"Out-of-Gamut", L"In-Gamut" } },
            };
            // Hidden cbuffer properties: primaries auto-injected by host.
            desc.hiddenDefaults = {
                { L"PrimRedX_hidden",   0.64f }, { L"PrimRedY_hidden",   0.33f },
                { L"PrimGreenX_hidden", 0.30f }, { L"PrimGreenY_hidden", 0.60f },
                { L"PrimBlueX_hidden",  0.15f }, { L"PrimBlueY_hidden",  0.06f },
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

Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Output : register(u0);

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
groupshared uint gs_counts[TILE_SIZE * TILE_SIZE];
groupshared float gs_lumSum[TILE_SIZE * TILE_SIZE];
groupshared uint gs_maxCount;

[numthreads(32, 32, 1)]
void main(uint3 GTid : SV_GroupThreadID) {
    uint tid = GTid.x + GTid.y * 32;
    uint totalThreads = 1024;
    uint totalPixels = Width * Height;
    uint outSize = max(OutputSize, 64u);

    // Clear output texture.
    for (uint ci = tid; ci < outSize * outSize; ci += totalThreads) {
        Output[int2(ci % outSize, ci / outSize)] = float4(0, 0, 0, 0);
    }

    // Clear groupshared tile.
    for (uint ti = tid; ti < TILE_SIZE * TILE_SIZE; ti += totalThreads) {
        gs_counts[ti] = 0;
        gs_lumSum[ti] = 0.0;
    }
    GroupMemoryBarrierWithGroupSync();

    // Process all source pixels, scatter into groupshared tile.
    // The tile covers the full output range at reduced resolution.
    for (uint pi = tid; pi < totalPixels; pi += totalThreads) {
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

    // Find max count for log normalization.
    if (tid == 0) gs_maxCount = 1;
    GroupMemoryBarrierWithGroupSync();
    for (uint mi = tid; mi < TILE_SIZE * TILE_SIZE; mi += totalThreads) {
        InterlockedMax(gs_maxCount, gs_counts[mi]);
    }
    GroupMemoryBarrierWithGroupSync();

    float logMax = log2(float(gs_maxCount) + 1.0);

    // Write groupshared tile to output texture, upscaling to full resolution.
    // Each tile bin maps to a block of (outSize/TILE_SIZE) x (outSize/TILE_SIZE) pixels.
    float scale = float(outSize) / float(TILE_SIZE);
    for (uint oi = tid; oi < outSize * outSize; oi += totalThreads) {
        uint ox = oi % outSize;
        uint oy = oi / outSize;
        // Map output pixel back to tile bin.
        uint tx = min((uint)(float(ox) / scale), TILE_SIZE - 1);
        uint ty = min((uint)(float(oy) / scale), TILE_SIZE - 1);
        uint idx = ty * TILE_SIZE + tx;
        uint cnt = gs_counts[idx];
        if (cnt > 0) {
            float logDensity = log2(float(cnt) + 1.0) / logMax;
            float avgLum = gs_lumSum[idx] / float(cnt);
            Output[int2(ox, oy)] = float4(logDensity, avgLum, 0, 1.0);
        }
    }
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"CIE Histogram";
            desc.effectId = L"CIE Histogram"; desc.effectVersion = 1;
            desc.category = L"Analysis";
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
    uint ShowMonitor;   // 1=show current monitor gamut triangle
    float MonRedX_hidden;      // Monitor primary red x (auto-injected)
    float MonRedY_hidden;
    float MonGreenX_hidden;
    float MonGreenY_hidden;
    float MonBlueX_hidden;
    float MonBlueY_hidden;
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

float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : TEXCOORD0) : SV_TARGET
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
        float2 mR = float2(MonRedX_hidden, MonRedY_hidden);
        float2 mG = float2(MonGreenX_hidden, MonGreenY_hidden);
        float2 mB = float2(MonBlueX_hidden, MonBlueY_hidden);
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
            desc.effectId = L"CIE Chromaticity Plot"; desc.effectVersion = 4;
            desc.category = L"Analysis";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + ciePlotHLSL;
            desc.inputNames = { L"Histogram" };
            desc.parameters = {
                { L"ShowRec709",   L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"Hide", L"Show" } },
                { L"ShowP3",       L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"Hide", L"Show" } },
                { L"ShowRec2020",  L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"Hide", L"Show" } },
                { L"Brightness",   L"float", 2.0f,  0.1f, 10.0f, 0.1f },
                { L"DiagramSize",  L"float", 1024.0f, 128.0f, 4096.0f, 64.0f },
                { L"ShowMonitor",  L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"Hide", L"Show" } },
            };
            desc.hiddenDefaults = {
                { L"MonRedX_hidden",   0.64f }, { L"MonRedY_hidden",   0.33f },
                { L"MonGreenX_hidden", 0.30f }, { L"MonGreenY_hidden", 0.60f },
                { L"MonBlueX_hidden",  0.15f }, { L"MonBlueY_hidden",  0.06f },
            };
            m_effects.push_back(std::move(desc));
        }



        // ---- Gamut Source ----
        {
            static const std::string gamutSourceHLSL = R"HLSL(
// Gamut Source - generates all colors within a selected color gamut
// Fixed coordinate system centered on D65 white point. Scale fits Rec.2020
// so all three gamuts share the same spatial mapping.

cbuffer constants : register(b0) {
    float Gamut;        // 0=Rec.709, 1=DCI-P3, 2=Rec.2020, 3=Working Space
    float Luminance;    // nits (default 80.0, maps to scRGB 1.0)
    float OutputSize;   // pixels (default 1024)
    float WsRedX_hidden;   float WsRedY_hidden;
    float WsGreenX_hidden; float WsGreenY_hidden;
    float WsBlueX_hidden;  float WsBlueY_hidden;
};

float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float size = max(OutputSize, 128.0);

    // Select gamut primaries
    float2 r, g, b;
    if (Gamut > 2.5)     { r = float2(WsRedX_hidden, WsRedY_hidden);
                           g = float2(WsGreenX_hidden, WsGreenY_hidden);
                           b = float2(WsBlueX_hidden, WsBlueY_hidden); }
    else if (Gamut > 1.5){ r = GAMUT_2020_R; g = GAMUT_2020_G; b = GAMUT_2020_B; }
    else if (Gamut > 0.5){ r = GAMUT_P3_R;   g = GAMUT_P3_G;   b = GAMUT_P3_B; }
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
            desc.effectId = L"Gamut Source"; desc.effectVersion = 2;
            desc.category = L"Source";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + gamutSourceHLSL;
            desc.inputNames = {};
            desc.parameters = {
                { L"Gamut",      L"float", 0.0f, 0.0f, 3.0f, 1.0f, { L"Rec.709", L"DCI-P3", L"Rec.2020", L"Working Space" } },
                { L"Luminance",  L"float", 80.0f, 0.01f, 10000.0f, 10.0f },
                { L"OutputSize", L"float", 1024.0f, 128.0f, 4096.0f, 64.0f },
            };
            desc.hiddenDefaults = {
                { L"WsRedX_hidden",   0.64f }, { L"WsRedY_hidden",   0.33f },
                { L"WsGreenX_hidden", 0.30f }, { L"WsGreenY_hidden", 0.60f },
                { L"WsBlueX_hidden",  0.15f }, { L"WsBlueY_hidden",  0.06f },
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

float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : TEXCOORD0) : SV_TARGET
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
            desc.effectId = L"Color Checker"; desc.effectVersion = 1;
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
    float PlateSize;   // pixels (default 512)
};

float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : TEXCOORD0) : SV_TARGET
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
            desc.effectId = L"Zone Plate"; desc.effectVersion = 1;
            desc.category = L"Source";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + zonePlateHLSL;
            desc.inputNames = {};
            desc.parameters = {
                { L"Frequency", L"float", 0.5f, 0.01f, 5.0f, 0.01f },
                { L"PlateSize", L"float", 1024.0f, 64.0f, 2048.0f, 64.0f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Gradient Generator (Source) ----
        {
            static const std::string gradientHLSL = R"HLSL(
// Gradient Generator - linear/radial gradients with HDR support
// Source effect: no input required.

cbuffer constants : register(b0) {
    float GradientType;  // 0=Linear horizontal, 1=Linear vertical, 2=Radial
    float StartR;   // start color (scRGB)
    float StartG;
    float StartB;
    float EndR;     // end color (scRGB)
    float EndG;
    float EndB;
    float GradSize; // pixels (default 512)
};

float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : TEXCOORD0) : SV_TARGET
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
            desc.effectId = L"Gradient Generator"; desc.effectVersion = 1;
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
                { L"GradSize",     L"float", 1024.0f, 64.0f, 2048.0f, 64.0f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- HDR Test Pattern (Source) ----
        {
            static const std::string hdrTestHLSL = R"HLSL(
// HDR Test Pattern - standard patches at known luminance levels
// Source effect: no input required.

cbuffer constants : register(b0) {
    float PatternSize; // pixels (default 512)
};

float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : TEXCOORD0) : SV_TARGET
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
            desc.effectId = L"HDR Test Pattern"; desc.effectVersion = 1;
            desc.category = L"Source";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + hdrTestHLSL;
            desc.inputNames = {};
            desc.parameters = {
                { L"PatternSize", L"float", 1024.0f, 256.0f, 2048.0f, 64.0f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Vectorscope (Analysis) ----
        {
            static const std::string vectorscopeHLSL = R"HLSL(
// Vectorscope - chrominance distribution on a circular plot
Texture2D Source : register(t0);

cbuffer constants : register(b0) {
    float ScopeSize;    // pixels (default 256)
    float Intensity;    // dot brightness (default 1.0)
};

float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float size = max(ScopeSize, 64.0);
    float2 center = float2(size * 0.5, size * 0.5);
    float2 p = (uv0.xy - center) / (size * 0.5); // -1 to 1

    float4 result = float4(0.15, 0.15, 0.15, 1.0);

    // Draw circular border
    float r = length(p);
    if (abs(r - 1.0) < 0.01)
        result.rgb = float3(0.3, 0.3, 0.3);

    // Cross-hairs
    if ((abs(p.x) < 0.003 || abs(p.y) < 0.003) && r < 1.0)
        result.rgb = float3(0.2, 0.2, 0.2);

    // Scatter source pixels: use Cb/Cr as position
    uint srcW, srcH;
    Source.GetDimensions(srcW, srcH);
    if (srcW > 0 && srcH > 0) {
        float bestDist = 1e10;
        uint stepX = max(1, srcW / 64);
        uint stepY = max(1, srcH / 64);
        for (uint sy = 0; sy < srcH; sy += stepY) {
            for (uint sx = 0; sx < srcW; sx += stepX) {
                float4 pix = Source.Load(int3(sx, sy, 0));
                if (pix.a < 0.001) continue;
                // YCbCr: Cb = B-Y, Cr = R-Y (simplified)
                float Y = dot(pix.rgb, float3(0.2126, 0.7152, 0.0722));
                float Cb = (pix.b - Y) * 0.9;
                float Cr = (pix.r - Y) * 0.9;
                float2 sPos = float2(Cr, -Cb); // map to scope space
                float d = length(sPos - p);
                bestDist = min(bestDist, d);
            }
        }
        if (bestDist < 0.02) {
            float hit = saturate(1.0 - bestDist / 0.02) * Intensity;
            result.rgb += hit * float3(0.8, 0.8, 0.8);
        }
    }

    return result;
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Vectorscope";
            desc.effectId = L"Vectorscope"; desc.effectVersion = 1;
            desc.category = L"Analysis";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + vectorscopeHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"ScopeSize",  L"float", 1024.0f, 64.0f, 2048.0f, 32.0f },
                { L"Intensity",  L"float", 1.0f,   0.1f, 5.0f, 0.1f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Delta E Comparator ----
        {
            static const std::string deltaEHLSL = R"HLSL(
// Delta E Comparator - per-pixel color difference between two inputs
// Supports: CIE76, CIE94, CIEDE2000

cbuffer Constants : register(b0)
{
    float Method;    // 0 = CIE76, 1 = CIE94, 2 = CIEDE2000
    float Scale;     // Multiplier for visualization (higher = more sensitive)
    float MaxDeltaE; // Clamp for colormap (dE at this value = full red)
};

Texture2D InputTexture : register(t0);   // Reference
Texture2D InputTexture1 : register(t1);  // Test
SamplerState InputSampler : register(s0);

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

    float h1p = atan2(b1, a1p);
    if (h1p < 0.0) h1p += 6.28318530718;
    float h2p = atan2(b2, a2p);
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

float4 main(
    float4 pos      : SV_POSITION,
    float4 posScene : SCENE_POSITION,
    float4 uv0      : TEXCOORD0,
    float4 uv1      : TEXCOORD1
) : SV_Target
{
    float4 ref  = InputTexture.Sample(InputSampler, uv0.xy);
    float4 test = InputTexture1.Sample(InputSampler, uv1.xy);

    float3 labRef  = ScRGBToLab(ref.rgb);
    float3 labTest = ScRGBToLab(test.rgb);

    float dE;
    uint method = (uint)Method;
    if (method == 1)      dE = DeltaE94(labRef, labTest);
    else if (method == 2) dE = DeltaE2000(labRef, labTest);
    else                  dE = DeltaE76(labRef, labTest);

    dE *= Scale;

    // Map to colormap: 0 = black (exact match), MaxDeltaE = full red.
    float t = saturate(dE / max(MaxDeltaE, 0.01));
    float3 color = TurboColormap(t) * smoothstep(0.0, 0.02, t);

    return float4(color, 1.0);
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Delta E Comparator";
            desc.effectId = L"Delta E Comparator"; desc.effectVersion = 2;
            desc.category = L"Analysis";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + deltaEHLSL;
            desc.inputNames = { L"Reference", L"Test" };
            desc.parameters = {
                { L"Method",    L"float", 2.0f, 0.0f, 2.0f, 1.0f, { L"CIE76", L"CIE94", L"CIEDE2000" } },
                { L"Scale",     L"float", 1.0f, 0.1f, 10.0f, 0.1f },
                { L"MaxDeltaE", L"float", 1.0f, 0.1f, 100.0f, 0.1f },
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

Texture2D InputTexture : register(t0);
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
    float4 posScene : SCENE_POSITION,
    float4 uv0      : TEXCOORD0
) : SV_Target
{
    float4 color = InputTexture.Sample(InputSampler, uv0.xy);
    float nits = ScRGBLuminanceNits(color.rgb);
    float3 fc = FalseColor(nits);
    float3 result = lerp(color.rgb, fc, Opacity);
    return float4(result, 1.0);
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Nit Map";
            desc.effectId = L"Nit Map"; desc.effectVersion = 1;
            desc.category = L"Analysis";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + falseColorHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"Opacity", L"float", 1.0f, 0.0f, 1.0f, 0.05f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Waveform Monitor ----
        {
            static const std::string waveformHLSL = R"HLSL(
// Waveform Monitor - RGB parade or luminance waveform
// For each output pixel (x, y), samples the source column x and counts
// how many source pixels have a value near the level represented by y.

cbuffer Constants : register(b0)
{
    float WaveformSize;  // Output height in pixels
    float Mode;          // 0 = Luma, 1 = RGB Parade, 2 = RGB Overlay
    float Gain;          // Brightness gain for the waveform traces
    float MaxNits;       // Max nits for the Y axis (default 1000)
};

Texture2D InputTexture : register(t0);
SamplerState InputSampler : register(s0);

float4 main(
    float4 pos      : SV_POSITION,
    float4 posScene : SCENE_POSITION,
    float4 uv0      : TEXCOORD0
) : SV_Target
{
    float2 dims;
    InputTexture.GetDimensions(dims.x, dims.y);
    if (dims.x < 1 || dims.y < 1)
        return float4(0, 0, 0, 1);

    float outW, outH;
    uint mode = (uint)Mode;

    // Output size: width matches source, height = WaveformSize
    outW = dims.x;
    if (mode == 1) outW = dims.x * 3.0; // RGB parade: 3x width
    outH = WaveformSize;

    // Current pixel in output space
    float px = uv0.x * outW;
    float py = uv0.y * outH;

    // Y axis: 0 at bottom, maxNits at top
    float level = (1.0 - py / outH);  // 0 at bottom, 1 at top

    // Determine which source column and channel(s) to sample
    float srcCol;
    int channelMask = 7; // all RGB
    if (mode == 1) {
        // RGB Parade: left third = R, middle = G, right = B
        float third = outW / 3.0;
        if (px < third)      { srcCol = px; channelMask = 1; }
        else if (px < 2*third) { srcCol = px - third; channelMask = 2; }
        else                  { srcCol = px - 2*third; channelMask = 4; }
    } else {
        srcCol = px;
    }

    float u = (srcCol + 0.5) / dims.x;

    // Sample source column and accumulate hits
    float hitR = 0, hitG = 0, hitB = 0, hitL = 0;
    float binSize = MaxNits / outH * 2.0; // Tolerance per bin (2 pixels)

    uint samples = min((uint)dims.y, 512); // Cap for performance
    float step = dims.y / (float)samples;

    for (uint i = 0; i < samples; i++)
    {
        float v = ((float)i * step + 0.5) / dims.y;
        float4 s = InputTexture.SampleLevel(InputSampler, float2(u, v), 0);

        float nitsR = max(0, s.r) * 80.0;
        float nitsG = max(0, s.g) * 80.0;
        float nitsB = max(0, s.b) * 80.0;
        float nitsL = ScRGBLuminanceNits(s.rgb);

        float targetNits = level * MaxNits;

        if (mode == 0) { // Luma
            if (abs(nitsL - targetNits) < binSize) hitL += 1.0;
        } else { // RGB
            if (abs(nitsR - targetNits) < binSize) hitR += 1.0;
            if (abs(nitsG - targetNits) < binSize) hitG += 1.0;
            if (abs(nitsB - targetNits) < binSize) hitB += 1.0;
        }
    }

    float scale = Gain / (float)samples * 40.0;
    float3 color;

    if (mode == 0) {
        float lum = hitL * scale;
        color = float3(lum, lum, lum);
    } else if (mode == 1) {
        // Parade: show only the relevant channel
        if (channelMask == 1)      color = float3(hitR * scale, 0, 0);
        else if (channelMask == 2) color = float3(0, hitG * scale, 0);
        else                       color = float3(0, 0, hitB * scale);
    } else {
        // Overlay: all channels on top of each other
        color = float3(hitR * scale, hitG * scale, hitB * scale);
    }

    // Draw horizontal reference lines at key nit levels
    float lineAlpha = 0.0;
    float nitsAtY = level * MaxNits;
    float lineThick = MaxNits / outH * 0.5;
    if (abs(nitsAtY - 80.0) < lineThick)   lineAlpha = 0.15; // SDR white
    if (abs(nitsAtY - 203.0) < lineThick)  lineAlpha = 0.15; // HDR reference
    if (abs(nitsAtY - 1000.0) < lineThick) lineAlpha = 0.15; // 1000 nits

    color += float3(lineAlpha, lineAlpha, lineAlpha);

    // Dark background
    float bg = 0.02;
    color = max(color, float3(bg, bg, bg));

    return float4(color, 1.0);
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Waveform Monitor";
            desc.effectId = L"Waveform Monitor"; desc.effectVersion = 2;
            desc.category = L"Analysis";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + waveformHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"WaveformSize", L"float", 512.0f, 128.0f, 2048.0f, 64.0f },
                { L"Mode",     L"float", 1.0f, 0.0f, 2.0f, 1.0f, { L"Luminance", L"RGB Parade", L"RGB Overlay" } },
                { L"Gain",     L"float", 1.0f, 0.1f, 10.0f, 0.1f },
                { L"MaxNits",  L"float", 1000.0f, 80.0f, 10000.0f, 100.0f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Gamut Volume Coverage ----
        {
            static const std::string gamutCoverageHLSL = R"HLSL(
// Gamut Volume Coverage - visualizes which gamut regions are occupied
// Shows a CIE xy diagram with dots for occupied chromaticities

cbuffer Constants : register(b0)
{
    float DiagramSize; // output size in pixels
    float TargetGamut; // 0 = sRGB, 1 = DCI-P3, 2 = BT.2020, 3 = Working Space
    float WsRedX_hidden;   float WsRedY_hidden;
    float WsGreenX_hidden; float WsGreenY_hidden;
    float WsBlueX_hidden;  float WsBlueY_hidden;
};

Texture2D InputTexture : register(t0);
SamplerState InputSampler : register(s0);

// Draw triangle outline
float TriangleEdge(float2 p, float2 a, float2 b, float lineW) {
    float2 ab = b - a;
    float t = saturate(dot(p - a, ab) / dot(ab, ab));
    float d = length(p - a - ab * t);
    return smoothstep(lineW, 0.0, d);
}

float4 main(
    float4 pos      : SV_POSITION,
    float4 posScene : SCENE_POSITION,
    float4 uv0      : TEXCOORD0
) : SV_Target
{
    float2 dims;
    InputTexture.GetDimensions(dims.x, dims.y);

    float size = max(DiagramSize, 256.0);
    float2 uv = uv0.xy * size;

    // CIE xy mapping: x=[0,0.8], y=[0,0.9] (same as CIE plot)
    float2 cie_xy = float2(uv.x / size * 0.8, (1.0 - uv.y / size) * 0.9);

    float3 bg = float3(0.05, 0.05, 0.05);

    // Draw target gamut triangle
    float2 tR, tG, tB;
    uint gamut = (uint)TargetGamut;
    if (gamut == 1)      { tR = GAMUT_P3_R; tG = GAMUT_P3_G; tB = GAMUT_P3_B; }
    else if (gamut == 2) { tR = GAMUT_2020_R; tG = GAMUT_2020_G; tB = GAMUT_2020_B; }
    else if (gamut == 3) { tR = float2(WsRedX_hidden, WsRedY_hidden); tG = float2(WsGreenX_hidden, WsGreenY_hidden); tB = float2(WsBlueX_hidden, WsBlueY_hidden); }
    else                 { tR = GAMUT_709_R; tG = GAMUT_709_G; tB = GAMUT_709_B; }

    float lineW = 0.003;
    float edge = TriangleEdge(cie_xy, tR, tG, lineW)
               + TriangleEdge(cie_xy, tG, tB, lineW)
               + TriangleEdge(cie_xy, tB, tR, lineW);
    bg += float3(0.3, 0.3, 0.3) * saturate(edge);

    // Sample source image and scatter chromaticities
    float hitInGamut = 0;
    float hitOutGamut = 0;
    uint sampleCount = min((uint)(dims.x * dims.y), 65536u);
    uint sqrtSamples = (uint)ceil(sqrt((float)sampleCount));
    float stepX = dims.x / (float)sqrtSamples;
    float stepY = dims.y / (float)sqrtSamples;

    float dotRadius = 0.004;

    for (uint sy = 0; sy < sqrtSamples; sy++)
    {
        for (uint sx = 0; sx < sqrtSamples; sx++)
        {
            float2 suv = float2((sx * stepX + 0.5) / dims.x, (sy * stepY + 0.5) / dims.y);
            float4 s = InputTexture.SampleLevel(InputSampler, suv, 0);
            float3 xyz = ScRGBToXYZ(max(s.rgb, 0.0));
            float sum = xyz.x + xyz.y + xyz.z;
            if (sum < 1e-6) continue;
            float2 sxy = float2(xyz.x / sum, xyz.y / sum);

            float d = length(cie_xy - sxy);
            if (d < dotRadius)
            {
                bool inGamut = PointInTriangle(sxy, tR, tG, tB);
                if (inGamut) hitInGamut += 1.0;
                else hitOutGamut += 1.0;
            }
        }
    }

    float3 color = bg;
    if (hitInGamut > 0)
        color += float3(0.0, 0.8, 0.0) * min(hitInGamut * 0.5, 1.0);
    if (hitOutGamut > 0)
        color += float3(1.0, 0.2, 0.0) * min(hitOutGamut * 0.5, 1.0);

    return float4(color, 1.0);
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Gamut Coverage";
            desc.effectId = L"Gamut Coverage"; desc.effectVersion = 2;
            desc.category = L"Analysis";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + gamutCoverageHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"DiagramSize",  L"float", 1024.0f, 128.0f, 4096.0f, 64.0f },
                { L"TargetGamut", L"float", 0.0f, 0.0f, 3.0f, 1.0f, { L"sRGB", L"DCI-P3", L"BT.2020", L"Working Space" } },
            };
            desc.hiddenDefaults = {
                { L"WsRedX_hidden", 0.64f }, { L"WsRedY_hidden", 0.33f },
                { L"WsGreenX_hidden", 0.30f }, { L"WsGreenY_hidden", 0.60f },
                { L"WsBlueX_hidden", 0.15f }, { L"WsBlueY_hidden", 0.06f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Gamut Map ----
        {
            static const std::string gamutMapHLSL = R"HLSL(
// Gamut Map - constrains input colors to a target gamut.
// Four modes:
//   0: Clip - hard clamp RGB components to [0,1] in target space
//   1: Nearest - project out-of-gamut CIE xy to nearest point on gamut triangle
//   2: Compress to White - move out-of-gamut xy toward D65 white until inside
//   3: Fit Gamut - uniformly scale all chromaticities to fit source gamut inside target

cbuffer Constants : register(b0)
{
    float Mode;        // 0=Clip, 1=Nearest, 2=Compress, 3=Fit Gamut
    float TargetGamut; // 0=sRGB, 1=DCI-P3, 2=BT.2020, 3=Working Space
    float Strength;    // 0=bypass, 1=full mapping
    float SourceGamut; // 0=sRGB, 1=DCI-P3, 2=BT.2020, 3=Working Space
    float WsRedX_hidden;   float WsRedY_hidden;
    float WsGreenX_hidden; float WsGreenY_hidden;
    float WsBlueX_hidden;  float WsBlueY_hidden;
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

    // Get target gamut primaries
    float2 gR, gG, gB;
    uint gamut = (uint)TargetGamut;
    if (gamut == 1)      { gR = GAMUT_P3_R; gG = GAMUT_P3_G; gB = GAMUT_P3_B; }
    else if (gamut == 2) { gR = GAMUT_2020_R; gG = GAMUT_2020_G; gB = GAMUT_2020_B; }
    else if (gamut == 3) { gR = float2(WsRedX_hidden, WsRedY_hidden); gG = float2(WsGreenX_hidden, WsGreenY_hidden); gB = float2(WsBlueX_hidden, WsBlueY_hidden); }
    else                 { gR = GAMUT_709_R; gG = GAMUT_709_G; gB = GAMUT_709_B; }

    uint mode = (uint)Mode;

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
        uint sg = (uint)SourceGamut;
        if (sg == 1)      { sR = GAMUT_P3_R; sG = GAMUT_P3_G; sB = GAMUT_P3_B; }
        else if (sg == 2) { sR = GAMUT_2020_R; sG = GAMUT_2020_G; sB = GAMUT_2020_B; }
        else if (sg == 3) { sR = float2(WsRedX_hidden, WsRedY_hidden); sG = float2(WsGreenX_hidden, WsGreenY_hidden); sB = float2(WsBlueX_hidden, WsBlueY_hidden); }
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
            desc.effectId = L"Gamut Map"; desc.effectVersion = 3;
            desc.category = L"Analysis";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + gamutMapHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"Mode",        L"float", 0.0f, 0.0f, 3.0f, 1.0f, { L"Clip", L"Nearest Point", L"Compress to White", L"Fit Gamut" } },
                { L"TargetGamut", L"float", 0.0f, 0.0f, 3.0f, 1.0f, { L"sRGB", L"DCI-P3", L"BT.2020", L"Working Space" } },
                { L"Strength",    L"float", 1.0f, 0.0f, 1.0f, 0.05f },
                { L"SourceGamut", L"float", 2.0f, 0.0f, 3.0f, 1.0f, { L"sRGB", L"DCI-P3", L"BT.2020", L"Working Space" }, L"Mode == 3" },
            };
            desc.hiddenDefaults = {
                { L"WsRedX_hidden", 0.64f }, { L"WsRedY_hidden", 0.33f },
                { L"WsGreenX_hidden", 0.30f }, { L"WsGreenY_hidden", 0.60f },
                { L"WsBlueX_hidden", 0.15f }, { L"WsBlueY_hidden", 0.06f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Perceptual Gamut Map (ICtCp) ----
        {
            static const std::string perceptualGamutMapHLSL = R"HLSL(
// Perceptual Gamut Map - gamut mapping in ICtCp space.
// Samples the target gamut boundary as a polygon in the Ct/Cp plane
// at the pixel's intensity level, then maps out-of-gamut pixels.

cbuffer Constants : register(b0)
{
    float Mode;          // 0=Nearest on Shell, 1=Compress to Neutral, 2=Fit to Shell
    float TargetGamut;   // 0=sRGB, 1=DCI-P3, 2=BT.2020, 3=Working Space
    float Strength;      // 0=bypass, 1=full
    float SourceGamut;   // 0=sRGB, 1=DCI-P3, 2=BT.2020, 3=Working Space
    float WsRedX_hidden;   float WsRedY_hidden;
    float WsGreenX_hidden; float WsGreenY_hidden;
    float WsBlueX_hidden;  float WsBlueY_hidden;
};

Texture2D InputTexture : register(t0);
SamplerState InputSampler : register(s0);

#define NBP 48

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

    float2 gR, gG, gB;
    uint g = (uint)TargetGamut;
    if (g == 1)      { gR = GAMUT_P3_R; gG = GAMUT_P3_G; gB = GAMUT_P3_B; }
    else if (g == 2) { gR = GAMUT_2020_R; gG = GAMUT_2020_G; gB = GAMUT_2020_B; }
    else if (g == 3) { gR = float2(WsRedX_hidden, WsRedY_hidden); gG = float2(WsGreenX_hidden, WsGreenY_hidden); gB = float2(WsBlueX_hidden, WsBlueY_hidden); }
    else             { gR = GAMUT_709_R; gG = GAMUT_709_G; gB = GAMUT_709_B; }

    // Use CIE xy triangle test for reliable in/out-of-gamut detection,
    // then do the actual mapping in ICtCp for perceptual quality.
    float3 xyz = ScRGBToXYZ(max(color.rgb, 0.0));
    float xyzSum = xyz.x + xyz.y + xyz.z;
    if (xyzSum < 1e-6) return color;
    float2 cieXY = float2(xyz.x / xyzSum, xyz.y / xyzSum);

    uint mode = (uint)Mode;

    if (mode == 2)
    {
        // Fit to Shell: uniformly scale all Ct/Cp toward neutral axis.
        float2 sR, sG, sB;
        uint sg = (uint)SourceGamut;
        if (sg == 1)      { sR = GAMUT_P3_R; sG = GAMUT_P3_G; sB = GAMUT_P3_B; }
        else if (sg == 2) { sR = GAMUT_2020_R; sG = GAMUT_2020_G; sB = GAMUT_2020_B; }
        else if (sg == 3) { sR = float2(WsRedX_hidden, WsRedY_hidden); sG = float2(WsGreenX_hidden, WsGreenY_hidden); sB = float2(WsBlueX_hidden, WsBlueY_hidden); }
        else              { sR = GAMUT_709_R; sG = GAMUT_709_G; sB = GAMUT_709_B; }

        float3 ictcp = ScRGBToICtCp(max(color.rgb, 0.0));
        float origY = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));

        // Sample both source and target boundaries at this I level
        float2 srcBnd[NBP], tgtBnd[NBP];
        SampleBoundary(sR, sG, sB, ictcp.x, srcBnd);
        SampleBoundary(gR, gG, gB, ictcp.x, tgtBnd);

        float scale = ComputeICtCpFitScale(srcBnd, tgtBnd);
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
            float3 ictcp = ScRGBToICtCp(max(color.rgb, 0.0));
            float2 ctcp = float2(ictcp.y, ictcp.z);
            float origY = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));

            float2 bnd[NBP];
            SampleBoundary(gR, gG, gB, ictcp.x, bnd);

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
            desc.name = L"Perceptual Gamut Map";
            desc.effectId = L"Perceptual Gamut Map"; desc.effectVersion = 7;
            desc.category = L"Analysis";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + perceptualGamutMapHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"Mode",        L"float", 0.0f, 0.0f, 2.0f, 1.0f, { L"Nearest on Shell", L"Compress to Neutral", L"Fit to Shell" } },
                { L"TargetGamut", L"float", 0.0f, 0.0f, 3.0f, 1.0f, { L"sRGB", L"DCI-P3", L"BT.2020", L"Working Space" } },
                { L"Strength",    L"float", 1.0f, 0.0f, 1.0f, 0.05f },
                { L"SourceGamut", L"float", 2.0f, 0.0f, 3.0f, 1.0f, { L"sRGB", L"DCI-P3", L"BT.2020", L"Working Space" }, L"Mode == 2" },
            };
            desc.hiddenDefaults = {
                { L"WsRedX_hidden", 0.64f }, { L"WsRedY_hidden", 0.33f },
                { L"WsGreenX_hidden", 0.30f }, { L"WsGreenY_hidden", 0.60f },
                { L"WsBlueX_hidden", 0.15f }, { L"WsBlueY_hidden", 0.06f },
            };
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
    float TargetGamut;
    float Intensity;   // Which I level to highlight (PQ domain, 0-1)
    float WsRedX_hidden;   float WsRedY_hidden;
    float WsGreenX_hidden; float WsGreenY_hidden;
    float WsBlueX_hidden;  float WsBlueY_hidden;
};

Texture2D InputTexture : register(t0);
SamplerState InputSampler : register(s0);

#define NBP 24

void SampleBnd(float2 gR, float2 gG, float2 gB, float iVal, out float2 bnd[NBP])
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

float4 main(
    float4 pos : SV_POSITION, float4 ps : SCENE_POSITION, float4 uv0 : TEXCOORD0
) : SV_Target
{
    float size = max(DiagramSize, 256.0);
    float2 ctcp = (uv0.xy - 0.5) * 1.0;

    float2 gR, gG, gB;
    uint g = (uint)TargetGamut;
    if (g == 1)      { gR = GAMUT_P3_R; gG = GAMUT_P3_G; gB = GAMUT_P3_B; }
    else if (g == 2) { gR = GAMUT_2020_R; gG = GAMUT_2020_G; gB = GAMUT_2020_B; }
    else if (g == 3) { gR = float2(WsRedX_hidden, WsRedY_hidden); gG = float2(WsGreenX_hidden, WsGreenY_hidden); gB = float2(WsBlueX_hidden, WsBlueY_hidden); }
    else             { gR = GAMUT_709_R; gG = GAMUT_709_G; gB = GAMUT_709_B; }

    float3 color = float3(0.01, 0.01, 0.01);
    float lineW = 1.5 / size;
    float dotR = 3.0 / size;

    // Draw boundaries at 5 fixed I levels + the user-selected one
    float iLevels[6] = { 0.15, 0.3, 0.45, 0.6, 0.75, Intensity };
    float3 lColors[6] = {
        float3(0.15, 0.05, 0.05),
        float3(0.15, 0.1, 0.05),
        float3(0.05, 0.15, 0.05),
        float3(0.05, 0.1, 0.15),
        float3(0.1, 0.05, 0.15),
        float3(0.4, 0.4, 0.0)   // User-selected level in yellow
    };

    for (int lv = 0; lv < 6; lv++)
    {
        float2 bnd[NBP];
        SampleBnd(gR, gG, gB, iLevels[lv], bnd);

        for (uint i = 0; i < NBP; i++)
        {
            uint j = (i + 1) % NBP;
            float2 ab = bnd[j] - bnd[i];
            float t = saturate(dot(ctcp - bnd[i], ab) / max(dot(ab, ab), 1e-10));
            float d = length(ctcp - bnd[i] - ab * t);
            if (d < lineW)
                color = max(color, lColors[lv] * (1.0 - d / lineW));

            if (length(ctcp - bnd[i]) < dotR)
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
            desc.effectId = L"ICtCp Boundary"; desc.effectVersion = 2;
            desc.category = L"Analysis";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + ictcpBoundaryHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"DiagramSize", L"float", 1024.0f, 128.0f, 2048.0f, 64.0f },
                { L"TargetGamut", L"float", 0.0f, 0.0f, 3.0f, 1.0f, { L"sRGB", L"DCI-P3", L"BT.2020", L"Working Space" } },
                { L"Intensity",   L"float", 0.5f, 0.05f, 0.95f, 0.05f },
            };
            desc.hiddenDefaults = {
                { L"WsRedX_hidden", 0.64f }, { L"WsRedY_hidden", 0.33f },
                { L"WsGreenX_hidden", 0.30f }, { L"WsGreenY_hidden", 0.60f },
                { L"WsBlueX_hidden", 0.15f }, { L"WsBlueY_hidden", 0.06f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Split Comparison ----
        {
            static const std::string splitCompareHLSL = R"HLSL(
// Split Comparison — side-by-side wipe between two inputs.
// SplitPosition [0..1]: 0 = full Image B, 1 = full Image A.
// A thin dividing line is drawn at the split boundary.

Texture2D ImageA : register(t0);
Texture2D ImageB : register(t1);

cbuffer Constants : register(b0)
{
    float SplitPosition;
    float LineWidth;
    float Orientation;
};

float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float4 a = ImageA.Load(int3(uv0.xy, 0));
    float4 b = ImageB.Load(int3(uv0.xy, 0));

    uint w, h;
    ImageA.GetDimensions(w, h);

    float coord = uv0.x;
    float size = float(w);
    if (Orientation > 0.5)
    {
        coord = uv0.y;
        size = float(h);
    }

    float splitPx = SplitPosition * size;
    float dist = abs(coord - splitPx);

    // Dividing line.
    float halfLine = max(LineWidth * 0.5, 0.5);
    if (dist < halfLine)
        return float4(1, 1, 1, 1);

    // Left/top = Image A, right/bottom = Image B.
    if (coord < splitPx)
        return a;
    return b;
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Split Comparison";
            desc.effectId = L"Split Comparison"; desc.effectVersion = 1;
            desc.category = L"Analysis";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + splitCompareHLSL;
            desc.inputNames = { L"ImageA", L"ImageB" };
            desc.parameters = {
                { L"SplitPosition", L"float", 0.5f, 0.0f, 1.0f, 0.01f },
                { L"LineWidth",     L"float", 2.0f, 0.0f, 10.0f, 0.5f },
                { L"Orientation",   L"float", 0.0f, 0.0f, 1.0f, 1.0f, { L"Horizontal", L"Vertical" } },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Image Statistics ----
        // D3D11 compute shader with stride-based reduction + 256-bin histogram.
        // Built-in path uses GpuReduction (RWBuffer<uint>); user-modified path
        // uses D3D11ComputeRunner (RWStructuredBuffer<float4>) via Effect Designer.
        {
            static const std::string imageStatsHLSL = R"HLSL(
// Image Statistics - D3D11 Compute Shader
// 32x32 = 1024 threads, dispatched as (1,1,1).
// Each thread strides across the full image, reduces in groupshared,
// then thread 0 computes median/P95 from a 256-bin histogram CDF.
//
// Output: RWStructuredBuffer<float4> — one float4 per analysis field.
//   Result[0].x = Min
//   Result[1].x = Max
//   Result[2].x = Mean
//   Result[3].x = Median  (from histogram CDF)
//   Result[4].x = P95     (from histogram CDF)
//   Result[5].x = Samples
//   Result[6].x = Nonzero%
//
// Width and Height are auto-injected at cbuffer offset 0.
// Channel and NonzeroOnly are user parameters at offset 8.

Texture2D<float4> Source : register(t0);
RWStructuredBuffer<float4> Result : register(u0);

cbuffer Constants : register(b0)
{
    uint Width;
    uint Height;
    uint Channel;      // 0=luminance, 1=R, 2=G, 3=B, 4=A
    uint NonzeroOnly;  // 0=all, 1=nonzero only
};

#define GROUP_SIZE 32
#define THREAD_COUNT (GROUP_SIZE * GROUP_SIZE)
#define HIST_BINS 256
#define HIST_MAX 100.0

groupshared float gs_min[THREAD_COUNT];
groupshared float gs_max[THREAD_COUNT];
groupshared float gs_sum[THREAD_COUNT];
groupshared uint  gs_count[THREAD_COUNT];
groupshared uint  gs_total[THREAD_COUNT];
groupshared uint  gs_nonzero[THREAD_COUNT];
groupshared uint  gs_hist[HIST_BINS];

float GetValue(float4 pix, uint ch)
{
    float result = 0.2126 * pix.r + 0.7152 * pix.g + 0.0722 * pix.b;
    if (ch == 1) result = pix.r;
    else if (ch == 2) result = pix.g;
    else if (ch == 3) result = pix.b;
    else if (ch == 4) result = pix.a;
    return result;
}

[numthreads(GROUP_SIZE, GROUP_SIZE, 1)]
void main(uint3 GTid : SV_GroupThreadID)
{
    uint tid = GTid.x + GTid.y * GROUP_SIZE;

    // Clear shared histogram.
    for (uint bi = tid; bi < HIST_BINS; bi += THREAD_COUNT)
        gs_hist[bi] = 0;
    GroupMemoryBarrierWithGroupSync();

    float tMin = 1e30;
    float tMax = -1e30;
    float tSum = 0;
    uint  tCount = 0;
    uint  tTotal = 0;
    uint  tNonzero = 0;

    // Stride across entire image.
    for (uint y = GTid.y; y < Height; y += GROUP_SIZE)
    {
        for (uint x = GTid.x; x < Width; x += GROUP_SIZE)
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

            // Histogram bin.
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

    GroupMemoryBarrierWithGroupSync();

    // Parallel reduction.
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

    // Thread 0 computes final stats and writes results.
    if (tid == 0)
    {
        float fMin = gs_min[0];
        float fMax = gs_max[0];
        uint  totalSamples = gs_count[0];
        uint  totalPixels = gs_total[0];
        uint  nonzeroPixels = gs_nonzero[0];
        float fMean = (totalSamples > 0) ? (gs_sum[0] / float(totalSamples)) : 0.0;

        // Compute median and P95 from histogram CDF.
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
            if (!foundMedian && cumulative >= medianTarget)
            {
                fMedian = binValue;
                foundMedian = true;
            }
            if (!foundP95 && cumulative >= p95Target)
            {
                fP95 = binValue;
                foundP95 = true;
            }
        }

        float nonzeroPct = (totalPixels > 0)
            ? float(nonzeroPixels) / float(totalPixels)
            : 0.0;

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
            desc.name = L"Image Statistics";
            desc.effectId = L"Image Statistics"; desc.effectVersion = 7;
            desc.category = L"Analysis";
            desc.shaderType = Graph::CustomShaderType::D3D11ComputeShader;
            desc.hlslSource = imageStatsHLSL;
            desc.dataOnly = true;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"Channel",     L"float", 0.0f, 0.0f, 4.0f, 1.0f, { L"Luminance", L"Red", L"Green", L"Blue", L"Alpha" } },
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
            desc.effectId = L"Gamut Parameter"; desc.effectVersion = 1;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.parameters = {
                { L"Value", L"float", 0.0f, 0.0f, 3.0f, 1.0f, { L"sRGB", L"DCI-P3", L"BT.2020", L"Working Space" } },
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
            desc.effectId = L"Clock"; desc.effectVersion = 1;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.parameters = {
                { L"AutoDuration", L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"Off", L"On" } },
                { L"StartTime", L"float", 0.0f, 0.0f, 3600.0f, 0.1f },
                { L"StopTime",  L"float", 10.0f, 0.0f, 3600.0f, 0.1f },
                { L"Speed",     L"float", 1.0f, -10.0f, 10.0f, 0.1f },
                { L"Loop",      L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"Off", L"On" } },
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

        // ---- Math Parameter Nodes ----
        // Each takes two float inputs (A, B) and outputs Result.
        struct MathOp { std::wstring name; std::wstring id; };
        MathOp mathOps[] = {
            { L"Max",      L"Math Max" },
            { L"Min",      L"Math Min" },
            { L"Add",      L"Math Add" },
            { L"Subtract", L"Math Subtract" },
            { L"Multiply", L"Math Multiply" },
            { L"Divide",   L"Math Divide" },
        };
        for (const auto& op : mathOps)
        {
            ShaderLabEffectDescriptor desc;
            desc.name = op.name;
            desc.effectId = op.id; desc.effectVersion = 1;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.parameters = {
                { L"A", L"float", 0.0f, -100000.0f, 100000.0f, 0.1f },
                { L"B", L"float", 0.0f, -100000.0f, 100000.0f, 0.1f },
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Result", Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }
    }
}
