#include "pch_engine.h"
#include "ShaderLabEffects.h"

// Shared HLSL color-math library prepended to every ShaderLab effect
// shader at compile time, and to every Tests/Math/* test bench kernel.
// Extracted from ShaderLabEffects.cpp at commit f25537b (Phase 5 partial)
// to keep the ShaderLab effects descriptor file focused on effect data.

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

// PQ with a signed extension, mirroring the curve through the origin the
// same way LabF does for CIE Lab. PQ itself is only defined for
// non-negative light, but scRGB expresses wide-gamut colors as negative
// Rec.709 components -- a BT.2020 green is (-0.87, +1.0, +0.06)-ish. A
// hard clamp there is not a safety net, it is an sRGB gamut clip applied
// before any colour science runs. Mirroring keeps the excursion
// representable so the round trip is lossless.
float PQ_InvEOTF_Signed(float L) {
    float v = PQ_InvEOTF(abs(L));
    return (L < 0.0) ? -v : v;
}

float PQ_EOTF_Signed(float N) {
    float v = PQ_EOTF(abs(N));
    return (N < 0.0) ? -v : v;
}

// scRGB -> ICtCp
float3 ScRGBToICtCp(float3 rgb) {
    // scRGB (1.0 = 80 nits) -> absolute luminance XYZ. No clamp: negative
    // components carry wide-gamut chroma, and the LMS mixing below is
    // non-negative for every physically realizable colour anyway (the
    // BT.2124 cone primaries enclose the visible locus), so the signed PQ
    // only engages on genuinely out-of-locus or below-black input.
    float3 xyz = ScRGBToXYZ(rgb);
    // Scale to absolute nits for PQ (XYZ Y=1 = 80 nits in scRGB)
    xyz *= 80.0;
    float3 lms = mul(XYZ_TO_LMS_ICTCP, xyz);
    // PQ encode each LMS component (input in nits, output [-1,1])
    float3 pqLms = float3(
        PQ_InvEOTF_Signed(lms.x),
        PQ_InvEOTF_Signed(lms.y),
        PQ_InvEOTF_Signed(lms.z));
    return mul(PQLMS_TO_ICTCP, pqLms);
}

// ICtCp -> scRGB
float3 ICtCpToScRGB(float3 ictcp) {
    float3 pqLms = mul(ICTCP_TO_PQLMS, ictcp);
    // Magnitude clamp: PQ_EOTF's rational form goes singular past |V| = 1
    // (the denominator crosses zero) and yields NaN/Inf, which callers can
    // reach by moving I without rescaling Ct/Cp. Clamp the magnitude and
    // keep the sign so wide-gamut excursions survive.
    pqLms = clamp(pqLms, -1.0, 1.0);
    // PQ decode to nits
    float3 lms = float3(
        PQ_EOTF_Signed(pqLms.x),
        PQ_EOTF_Signed(pqLms.y),
        PQ_EOTF_Signed(pqLms.z));
    float3 xyz = mul(LMS_TO_XYZ_ICTCP, lms);
    // Scale back from nits to scRGB (80 nits = 1.0)
    xyz /= 80.0;
    return XYZToScRGB(xyz);
}

// ---- Delta E ITP (ITU-R BT.2124) ----
//
// The HDR/WCG colour-difference metric. CIE Lab's dE76/94/2000 were derived
// from reflective samples under SDR viewing and lose meaning above roughly
// 100 nits and outside sRGB -- exactly where this pipeline operates -- so
// dE ITP is the correct ruler for tone-mapping and gamut work here.
//
//   dE_ITP = 720 * sqrt( dI^2 + dT^2 + dP^2 ),  T = 0.5 * Ct,  P = Cp
//
// The 0.5 on Ct converts BT.2100 ICtCp into the "ITP" difference space
// (Ct's range is twice Cp's); the 720 scales one unit to approximately one
// JND, so it is directly comparable to a dE2000 of 1.
//
// BT.2124 is defined on PQ-encoded ICtCp, which is what ScRGBToICtCp
// produces. Takes ICtCp triples, not scRGB -- convert first.
float DeltaEITP(float3 ictcp1, float3 ictcp2) {
    float dI = ictcp1.x - ictcp2.x;
    float dT = 0.5 * (ictcp1.y - ictcp2.y);
    float dP = ictcp1.z - ictcp2.z;
    return 720.0 * sqrt(dI * dI + dT * dT + dP * dP);
}

// Convenience: dE ITP straight from two scRGB colours.
float DeltaEITPFromScRGB(float3 rgb1, float3 rgb2) {
    return DeltaEITP(ScRGBToICtCp(rgb1), ScRGBToICtCp(rgb2));
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

// ---- I-channel (PQ-encoded nits) helpers for ICtCp tone mapping ----
// In BT.2100 ICtCp, I is a weighted PQ-encoded sum of LMS. For
// chromaticity-preserving operations (compress / expand only I, leave
// Ct/Cp), it is standard to treat I as if it were PQ(neutral_nits) and
// design the curve in nits-via-PQ space. This is what BT.2390 does.

// Convert a nit value to its corresponding I coordinate.
float NitsToI(float nits) {
    return PQ_InvEOTF(max(nits, 0.0));
}

// Convert an I coordinate back to nits.
float IToNits(float I) {
    return PQ_EOTF(I);
}

// Reinhard compression on I, expressed in I-space directly. Anchored
// Möbius: maps 0 -> 0 and peakIn_I -> peakOut_I exactly, with f'(0)=1
// (linear at the low end) and smooth rolloff near peakIn. Both peaks
// are I coordinates (PQ values). For HDR -> SDR pass peakIn = HDR_I,
// peakOut = SDR_I. Inputs above peakIn_I are clamped so the curve
// can't walk past its anchor onto the rising branch beyond peakIn.
float ReinhardCompressI(float I, float peakIn_I, float peakOut_I) {
    float pp = peakIn_I * peakOut_I;
    if (pp <= 1e-12) return 0.0;
    float Ic = clamp(I, 0.0, peakIn_I);
    float denom = pp + Ic * (peakIn_I - peakOut_I);
    return Ic * pp / max(denom, 1e-12);
}

// Inverse of ReinhardCompressI: given an I value in [0, peakOut_I]
// returns the I in [0, peakIn_I] that would compress to it. Same
// peakIn/peakOut convention as ReinhardCompressI: peakIn is the
// *uncompressed* range, peakOut is the *compressed* range. For
// SDR -> HDR expansion callers pass peakIn = HDR_I, peakOut = SDR_I.
// Inputs above peakOut_I are clamped so the curve saturates at peakIn
// rather than racing toward the asymptote.
float ReinhardExpandI(float I, float peakIn_I, float peakOut_I) {
    float pp = peakIn_I * peakOut_I;
    if (pp <= 1e-12) return 0.0;
    float Ic = clamp(I, 0.0, peakOut_I);
    float denom = pp - Ic * (peakIn_I - peakOut_I);
    return Ic * pp / max(denom, 1e-12);
}

// Soft gamut-distance compression (1D). `d` is a pixel's chroma radius
// normalized so the gamut boundary sits at 1.0 (d < 1 in-gamut, d > 1
// out). Returns the remapped radius. Contract:
//   - d <= threshold        -> returned unchanged (identity zone)
//   - d == limit            -> maps exactly to 1.0 (the boundary)
//   - monotone increasing, C1 at d == threshold (slope 1 where the
//     curve meets the identity segment, so gradients don't kink)
//   - d > limit             -> may exceed 1.0 slightly (ACES-style;
//     callers pick `limit` to cover their expected source range)
// threshold in [0, 1): where compression starts, e.g. 0.75.
// limit > 1: the source radius that lands exactly on the boundary.
// power >= 1: knee hardness. 1 reduces exactly to Reinhard; higher
//   values track identity longer and turn harder near the boundary
//   (less desaturation of legal colors, more crowding of illegal ones).
//   ACES RGC ships 1.2.
// ---- 8-bit display-referred output helpers -----------------------------
// These exist for the screenshot path, where the handback is 8bpc sRGB and
// we therefore own the quantizer. The above-white band survives the tone
// curve inside a very small number of codes (a 0.7*W knee leaves roughly 33
// of 256 after the sRGB OETF), so quantizing without dither bands visibly
// in exactly the smooth HDR gradients the feature exists to preserve.

// Interleaved Gradient Noise (Jimenez 2014). Cheap, deterministic, and
// spectrally much better behaved than a hash-based white noise, which makes
// it a reasonable dither source when a blue-noise texture isn't available.
// Expects integer pixel coordinates; returns [0, 1).
float InterleavedGradientNoise(float2 p) {
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// Triangular-PDF dither, [-1, 1] LSB. The sum of two independent uniforms
// decorrelates the quantization error from the signal; plain uniform dither
// leaves a residual pattern modulated by the signal itself.
float TriangularDither(float2 p) {
    float n1 = InterleavedGradientNoise(p);
    float n2 = InterleavedGradientNoise(p + 5.588238);
    return n1 + n2 - 1.0;
}

// Quantize an already-encoded [0,1] value to `levels` steps with dither.
// `strength` scales the dither in LSBs (1.0 = standard TPDF, 0 = none).
float3 DitherQuantize(float3 encoded, float2 p, float levels, float strength) {
    float maxCode = max(levels - 1.0, 1.0);
    float3 d = TriangularDither(p) * 0.5 * strength;
    return saturate(round(saturate(encoded) * maxCode + d) / maxCode);
}

float SoftCompressDistance(float d, float threshold, float limit, float power) {
    float t = clamp(threshold, 0.0, 0.99);
    float l = max(limit, 1.01);
    float p = clamp(power, 1.0, 8.0);
    if (d <= t) return d;
    // ACES-RGC-style power curve y = t + x / (1 + (x/s)^p)^(1/p), with
    // the scale s solved from the anchor f(l - t) == 1 - t, so d == l
    // lands exactly on the boundary. f'(0) == 1 keeps the join C1.
    float x = d - t;
    float s = (l - t) / pow(pow((l - t) / (1.0 - t), p) - 1.0, 1.0 / p);
    return t + x / pow(1.0 + pow(x / s, p), 1.0 / p);
}
)HLSL";

    SHADERLAB_API const std::string& GetColorMathHLSL()
    {
        return s_colorMathHLSL;
    }

}
