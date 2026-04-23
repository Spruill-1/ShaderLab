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
    uint  ColormapMode; // 0=Turbo, 1=Inferno
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
// Out-of-Gamut Highlight
Texture2D Source : register(t0);

cbuffer constants : register(b0) {
    uint  TargetGamut;     // 0=Rec.709, 1=P3, 2=Rec.2020, 3=Monitor
    float OverlayR;        // default 1.0
    float OverlayG;        // default 0.0
    float OverlayB;        // default 1.0 (magenta)
    float OverlayStrength; // default 0.7
    // Monitor gamut primaries (injected when TargetGamut=3)
    float MonitorRedX;
    float MonitorRedY;
    float MonitorGreenX;
    float MonitorGreenY;
    float MonitorBlueX;
    float MonitorBlueY;
};

bool IsOutOfGamut(float3 rgb, uint gamut) {
    float3 xyz = ScRGBToXYZ(rgb);
    float3 xyY = XYZToxyY(xyz);
    float2 p = xyY.xy;

    float2 r, g, b;
    if (gamut == 0) { r = GAMUT_709_R;  g = GAMUT_709_G;  b = GAMUT_709_B; }
    else if (gamut == 1) { r = GAMUT_P3_R;   g = GAMUT_P3_G;   b = GAMUT_P3_B; }
    else if (gamut == 2) { r = GAMUT_2020_R; g = GAMUT_2020_G; b = GAMUT_2020_B; }
    else { r = float2(MonitorRedX, MonitorRedY); g = float2(MonitorGreenX, MonitorGreenY); b = float2(MonitorBlueX, MonitorBlueY); }

    return !PointInTriangle(p, r, g, b);
}

float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float4 color = Source.Load(int3(uv0.xy, 0));
    if (color.a < 0.001) return color;

    bool oog = IsOutOfGamut(color.rgb, TargetGamut);
    if (oog) {
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
        node.type = (desc.shaderType == CustomShaderType::ComputeShader)
            ? NodeType::ComputeShader : NodeType::PixelShader;
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
        CoCreateGuid(&def.shaderGuid);

        // Set up input pins from input names.
        for (uint32_t i = 0; i < desc.inputNames.size(); ++i)
            node.inputPins.push_back({ std::format(L"I{}", i), i });

        // Set default property values from parameter definitions.
        for (const auto& param : desc.parameters)
            node.properties[param.name] = param.defaultValue;

        node.customEffect = std::move(def);
        return node;
    }

    void ShaderLabEffects::RegisterAll()
    {
        const auto& colorMath = GetColorMathHLSL();

        // ---- Luminance Heatmap ----
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Luminance Heatmap";
            desc.category = L"Analysis";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + s_luminanceHeatmapHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"MinNits",      L"float", 0.0f,    0.0f, 10000.0f, 1.0f },
                { L"MaxNits",      L"float", 10000.0f, 0.0f, 10000.0f, 100.0f },
                { L"ColormapMode", L"uint",  uint32_t(0), 0.0f, 1.0f, 1.0f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Out-of-Gamut Highlight ----
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Out-of-Gamut Highlight";
            desc.category = L"Analysis";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + s_outOfGamutHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"TargetGamut",     L"uint",  uint32_t(0), 0.0f, 3.0f, 1.0f },
                { L"OverlayR",        L"float", 1.0f,  0.0f, 1.0f, 0.01f },
                { L"OverlayG",        L"float", 0.0f,  0.0f, 1.0f, 0.01f },
                { L"OverlayB",        L"float", 1.0f,  0.0f, 1.0f, 0.01f },
                { L"OverlayStrength", L"float", 0.7f,  0.0f, 1.0f, 0.01f },
                { L"MonitorRedX",     L"float", 0.64f, 0.0f, 1.0f, 0.001f },
                { L"MonitorRedY",     L"float", 0.33f, 0.0f, 1.0f, 0.001f },
                { L"MonitorGreenX",   L"float", 0.30f, 0.0f, 1.0f, 0.001f },
                { L"MonitorGreenY",   L"float", 0.60f, 0.0f, 1.0f, 0.001f },
                { L"MonitorBlueX",    L"float", 0.15f, 0.0f, 1.0f, 0.001f },
                { L"MonitorBlueY",    L"float", 0.06f, 0.0f, 1.0f, 0.001f },
            };
            m_effects.push_back(std::move(desc));
        }

        // Future effects will be added here as they're implemented.

        // ---- CIE Chromaticity Plot ----
        {
            static const std::string ciePlotHLSL = R"HLSL(
// CIE 1931 xy Chromaticity Diagram
// Renders the spectral locus horseshoe with gamut triangle overlays.
// When an input image is connected, each pixel's chromaticity is
// scattered onto the diagram as a bright dot.
Texture2D Source : register(t0);

cbuffer constants : register(b0) {
    uint ShowRec709;    // 1=show gamut triangle
    uint ShowP3;        // 1=show gamut triangle
    uint ShowRec2020;   // 1=show gamut triangle
    float Brightness;   // scatter dot brightness (default 2.0)
    float DiagramSize;  // output size in pixels (default 512)
};

// CIE 1931 2-degree observer spectral locus (sampled at 5nm intervals, 380-700nm)
// Stored as xy pairs. Approximate with a polynomial for GPU efficiency.
float2 SpectralLocusXY(float nm) {
    // Attempt to sample across the spectral locus approximately
    float t = saturate((nm - 380.0) / 320.0);
    // x coordinate along the locus
    float x = 0.17 + t * (0.73 - 0.17);
    if (t < 0.3) x = 0.17 + t * 2.0 * 0.1;
    // This is a rough approximation - the real locus is complex
    // Use parametric curves fitted to CIE 1931 data:
    float x2 = t * t;
    float x3 = x2 * t;
    x = 0.17 + 0.24*t + 1.67*x2 - 2.48*x3 + 1.04*x2*x2;
    float y = 0.005 + 2.4*t - 6.3*x2 + 8.1*x3 - 3.8*x2*x2;
    y = max(y, 0.005);
    return float2(saturate(x), saturate(y));
}

// Check if xy coordinate is inside the visible gamut (approximate)
bool IsVisibleGamut(float2 xy) {
    // Very rough test: inside the horseshoe
    if (xy.x < 0.0 || xy.y < 0.0 || xy.x + xy.y > 1.0) return false;
    if (xy.y < 0.01) return false;
    // Simple bounding: above the purple line and below the locus
    float purpleY = lerp(0.005, 0.33, (xy.x - 0.17) / (0.73 - 0.17));
    return xy.y > purpleY * 0.3;
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

    float4 result = float4(0.15, 0.15, 0.15, 1.0); // dark background

    // Render the visible gamut region with approximate spectral colors
    float3 xyY = float3(xy.x, xy.y, 0.5);
    float3 xyz = xyYToXYZ(xyY);
    float3 rgb = XYZToScRGB(xyz);
    // Normalize to visible range and clamp
    float maxC = max(max(rgb.r, rgb.g), max(rgb.b, 0.001));
    rgb = rgb / maxC * 0.5; // dim background

    if (rgb.r >= -0.01 && rgb.g >= -0.01 && rgb.b >= -0.01 && xy.y > 0.01) {
        rgb = max(rgb, 0.0);
        result.rgb = rgb;
    }

    // Gamut triangles
    float thickness = 0.003;
    if (ShowRec709 > 0) {
        float e = GamutTriangle(xy, GAMUT_709_R, GAMUT_709_G, GAMUT_709_B, thickness);
        result.rgb = lerp(result.rgb, float3(1,1,1), e * 0.8);
    }
    if (ShowP3 > 0) {
        float e = GamutTriangle(xy, GAMUT_P3_R, GAMUT_P3_G, GAMUT_P3_B, thickness);
        result.rgb = lerp(result.rgb, float3(0,1,0), e * 0.8);
    }
    if (ShowRec2020 > 0) {
        float e = GamutTriangle(xy, GAMUT_2020_R, GAMUT_2020_G, GAMUT_2020_B, thickness);
        result.rgb = lerp(result.rgb, float3(0,0.5,1), e * 0.8);
    }

    // D65 white point marker
    float dw = length(xy - D65_WHITE);
    if (dw < 0.008) result.rgb = float3(1, 1, 1);

    // Scatter source image pixels onto the diagram
    uint srcW, srcH;
    Source.GetDimensions(srcW, srcH);
    if (srcW > 0 && srcH > 0) {
        // Sample source pixels and check if they map near this xy position
        // (reverse scatter: for each diagram pixel, check if any source pixel maps here)
        // This is approximate - sample a grid of source pixels
        float bestDist = 1e10;
        float3 bestColor = float3(0,0,0);

        uint stepX = max(1, srcW / 64);
        uint stepY = max(1, srcH / 64);
        for (uint sy = 0; sy < srcH; sy += stepY) {
            for (uint sx = 0; sx < srcW; sx += stepX) {
                float4 srcPix = Source.Load(int3(sx, sy, 0));
                if (srcPix.a < 0.001) continue;
                float3 pxyz = ScRGBToXYZ(srcPix.rgb);
                float2 pxy = XYZToxyY(pxyz).xy;
                float d = length(pxy - xy);
                if (d < bestDist) {
                    bestDist = d;
                    bestColor = srcPix.rgb;
                }
            }
        }

        if (bestDist < 0.01) {
            float intensity = saturate(1.0 - bestDist / 0.01) * Brightness;
            result.rgb += intensity * float3(1, 1, 1);
        }
    }

    return result;
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"CIE Chromaticity Plot";
            desc.category = L"Analysis";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + ciePlotHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"ShowRec709",   L"uint",  uint32_t(1), 0.0f, 1.0f, 1.0f },
                { L"ShowP3",       L"uint",  uint32_t(1), 0.0f, 1.0f, 1.0f },
                { L"ShowRec2020",  L"uint",  uint32_t(1), 0.0f, 1.0f, 1.0f },
                { L"Brightness",   L"float", 2.0f,  0.1f, 10.0f, 0.1f },
                { L"DiagramSize",  L"float", 512.0f, 128.0f, 2048.0f, 64.0f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Waveform Monitor ----
        {
            static const std::string waveformHLSL = R"HLSL(
// Waveform Monitor - video-style RGB waveform display
// X-axis = horizontal position in source, Y-axis = value (0-1 or nits)
Texture2D Source : register(t0);

cbuffer constants : register(b0) {
    float MaxValue;     // default 1.0 (scRGB units)
    uint  ShowR;        // 1=show red channel
    uint  ShowG;        // 1=show green channel
    uint  ShowB;        // 1=show blue channel
    float WaveformH;    // height of waveform in pixels (default 256)
};

float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : TEXCOORD0) : SV_TARGET
{
    uint srcW, srcH;
    Source.GetDimensions(srcW, srcH);
    if (srcW == 0 || srcH == 0)
        return float4(0.15, 0.15, 0.15, 1.0);

    float2 pixPos = uv0.xy;
    float height = max(WaveformH, 64.0);

    // Map x to source column
    uint srcX = (uint)(pixPos.x / max(1.0, (float)srcW) * srcW);
    srcX = min(srcX, srcW - 1);

    // Map y to value (inverted: top = max, bottom = 0)
    float valueAtY = (1.0 - pixPos.y / height) * MaxValue;

    float4 result = float4(0.15, 0.15, 0.15, 1.0);

    // Scan the source column and accumulate hits
    float rHit = 0, gHit = 0, bHit = 0;
    uint step = max(1, srcH / 256);
    for (uint sy = 0; sy < srcH; sy += step) {
        float4 pix = Source.Load(int3(srcX, sy, 0));
        float thickness = MaxValue / height * 2.0;

        if (ShowR > 0 && abs(pix.r - valueAtY) < thickness) rHit += 0.1;
        if (ShowG > 0 && abs(pix.g - valueAtY) < thickness) gHit += 0.1;
        if (ShowB > 0 && abs(pix.b - valueAtY) < thickness) bHit += 0.1;
    }

    result.r += saturate(rHit) * 0.8;
    result.g += saturate(gHit) * 0.8;
    result.b += saturate(bHit) * 0.8;

    // Grid lines at 0, 0.25, 0.5, 0.75, 1.0
    for (float v = 0; v <= MaxValue; v += MaxValue * 0.25) {
        float gridY = (1.0 - v / MaxValue) * height;
        if (abs(pixPos.y - gridY) < 0.5)
            result.rgb += 0.15;
    }

    return result;
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Waveform Monitor";
            desc.category = L"Analysis";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + waveformHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"MaxValue",   L"float", 1.0f,  0.01f, 125.0f, 0.1f },
                { L"ShowR",      L"uint",  uint32_t(1), 0.0f, 1.0f, 1.0f },
                { L"ShowG",      L"uint",  uint32_t(1), 0.0f, 1.0f, 1.0f },
                { L"ShowB",      L"uint",  uint32_t(1), 0.0f, 1.0f, 1.0f },
                { L"WaveformH",  L"float", 256.0f, 64.0f, 1024.0f, 32.0f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Color Gamut Chart (Source) ----
        {
            static const std::string gamutChartHLSL = R"HLSL(
// Color Gamut Chart - renders CIE horseshoe with filled gamut regions
// Source effect: generates its own image, no input needed.

cbuffer constants : register(b0) {
    uint FillRec709;   // 1=fill with color
    uint FillP3;       // 1=fill with color
    uint FillRec2020;  // 1=fill with color
    float OutputSize;  // pixels (default 512)
};

float4 main(
    float4 pos : SV_POSITION,
    float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float size = max(OutputSize, 128.0);
    float2 xy = float2(uv0.x / size * 0.8, (1.0 - uv0.y / size) * 0.9);

    float4 result = float4(0.18, 0.18, 0.18, 1.0);

    // Fill gamut regions with their actual colors
    float3 xyY = float3(xy.x, xy.y, 0.4);
    float3 xyz = xyYToXYZ(xyY);
    float3 rgb = XYZToScRGB(xyz);

    bool in709  = PointInTriangle(xy, GAMUT_709_R, GAMUT_709_G, GAMUT_709_B);
    bool inP3   = PointInTriangle(xy, GAMUT_P3_R, GAMUT_P3_G, GAMUT_P3_B);
    bool in2020 = PointInTriangle(xy, GAMUT_2020_R, GAMUT_2020_G, GAMUT_2020_B);

    if (in2020 && FillRec2020 > 0) {
        float3 normalized = max(rgb, 0.0);
        float m = max(max(normalized.r, normalized.g), max(normalized.b, 0.001));
        result.rgb = normalized / m * 0.4;
    }
    if (inP3 && FillP3 > 0) {
        float3 normalized = max(rgb, 0.0);
        float m = max(max(normalized.r, normalized.g), max(normalized.b, 0.001));
        result.rgb = normalized / m * 0.55;
    }
    if (in709 && FillRec709 > 0) {
        float3 normalized = max(rgb, 0.0);
        float m = max(max(normalized.r, normalized.g), max(normalized.b, 0.001));
        result.rgb = normalized / m * 0.7;
    }

    // Gamut triangle outlines
    float thickness = 0.003;
    if (FillRec709 > 0) {
        float e = 0;
        // Simple line segments
        float2 pts709[3] = { GAMUT_709_R, GAMUT_709_G, GAMUT_709_B };
        [unroll] for (uint i = 0; i < 3; i++) {
            float2 a = pts709[i], b = pts709[(i+1)%3];
            float2 ab = b - a;
            float t = saturate(dot(xy - a, ab) / dot(ab, ab));
            float d = length(xy - (a + t * ab));
            e = max(e, smoothstep(thickness, 0.0, d));
        }
        result.rgb = lerp(result.rgb, float3(1,1,1), e * 0.9);
    }
    if (FillP3 > 0) {
        float e = 0;
        float2 ptsP3[3] = { GAMUT_P3_R, GAMUT_P3_G, GAMUT_P3_B };
        [unroll] for (uint i = 0; i < 3; i++) {
            float2 a = ptsP3[i], b = ptsP3[(i+1)%3];
            float2 ab = b - a;
            float t = saturate(dot(xy - a, ab) / dot(ab, ab));
            float d = length(xy - (a + t * ab));
            e = max(e, smoothstep(thickness, 0.0, d));
        }
        result.rgb = lerp(result.rgb, float3(0,1,0), e * 0.9);
    }
    if (FillRec2020 > 0) {
        float e = 0;
        float2 pts2020[3] = { GAMUT_2020_R, GAMUT_2020_G, GAMUT_2020_B };
        [unroll] for (uint i = 0; i < 3; i++) {
            float2 a = pts2020[i], b = pts2020[(i+1)%3];
            float2 ab = b - a;
            float t = saturate(dot(xy - a, ab) / dot(ab, ab));
            float d = length(xy - (a + t * ab));
            e = max(e, smoothstep(thickness, 0.0, d));
        }
        result.rgb = lerp(result.rgb, float3(0,0.5,1), e * 0.9);
    }

    // D65 white point
    if (length(xy - D65_WHITE) < 0.008)
        result.rgb = float3(1, 1, 1);

    return result;
}
)HLSL";

            ShaderLabEffectDescriptor desc;
            desc.name = L"Color Gamut Chart";
            desc.category = L"Source";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + gamutChartHLSL;
            desc.inputNames = {};
            desc.parameters = {
                { L"FillRec709",  L"uint",  uint32_t(1), 0.0f, 1.0f, 1.0f },
                { L"FillP3",      L"uint",  uint32_t(1), 0.0f, 1.0f, 1.0f },
                { L"FillRec2020", L"uint",  uint32_t(1), 0.0f, 1.0f, 1.0f },
                { L"OutputSize",  L"float", 512.0f, 128.0f, 2048.0f, 64.0f },
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
            desc.category = L"Source";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + zonePlateHLSL;
            desc.inputNames = {};
            desc.parameters = {
                { L"Frequency", L"float", 0.5f, 0.01f, 5.0f, 0.01f },
                { L"PlateSize", L"float", 512.0f, 64.0f, 2048.0f, 64.0f },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Gradient Generator (Source) ----
        {
            static const std::string gradientHLSL = R"HLSL(
// Gradient Generator - linear/radial gradients with HDR support
// Source effect: no input required.

cbuffer constants : register(b0) {
    uint  GradientType;  // 0=Linear horizontal, 1=Linear vertical, 2=Radial
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

    if (GradientType == 0)
        t = saturate(uv0.x / size);
    else if (GradientType == 1)
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
            desc.category = L"Source";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + gradientHLSL;
            desc.inputNames = {};
            desc.parameters = {
                { L"GradientType", L"uint",  uint32_t(0), 0.0f, 2.0f, 1.0f },
                { L"StartR",       L"float", 0.0f, -1.0f, 125.0f, 0.01f },
                { L"StartG",       L"float", 0.0f, -1.0f, 125.0f, 0.01f },
                { L"StartB",       L"float", 0.0f, -1.0f, 125.0f, 0.01f },
                { L"EndR",         L"float", 1.0f, -1.0f, 125.0f, 0.01f },
                { L"EndG",         L"float", 1.0f, -1.0f, 125.0f, 0.01f },
                { L"EndB",         L"float", 1.0f, -1.0f, 125.0f, 0.01f },
                { L"GradSize",     L"float", 512.0f, 64.0f, 2048.0f, 64.0f },
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
            desc.category = L"Source";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + hdrTestHLSL;
            desc.inputNames = {};
            desc.parameters = {
                { L"PatternSize", L"float", 512.0f, 256.0f, 2048.0f, 64.0f },
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
            desc.category = L"Analysis";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.hlslSource = colorMath + vectorscopeHLSL;
            desc.inputNames = { L"Source" };
            desc.parameters = {
                { L"ScopeSize",  L"float", 256.0f, 64.0f, 1024.0f, 32.0f },
                { L"Intensity",  L"float", 1.0f,   0.1f, 5.0f, 0.1f },
            };
            m_effects.push_back(std::move(desc));
        }
    }
}
