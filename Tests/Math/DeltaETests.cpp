#include "pch_engine.h"
#include "../ShaderTestBench.h"
#include "../TestCommon.h"

#include <cstdio>
#include <string>

namespace ShaderLab::Tests
{
    // The DeltaE76 / DeltaE94 / DeltaE2000 helpers live inside the Delta E
    // Comparator's pixel shader, not in the shared GetColorMathHLSL().
    // Until they migrate to the shared library, prepend them at test time
    // so the bench can exercise them in isolation.
    static const std::string kDeltaEHelpers = R"HLSL(
float DeltaE76(float3 lab1, float3 lab2) {
    float3 d = lab1 - lab2;
    return sqrt(dot(d, d));
}

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

float DeltaE2000(float3 lab1, float3 lab2) {
    float L1 = lab1.x, a1 = lab1.y, b1 = lab1.z;
    float L2 = lab2.x, a2 = lab2.y, b2 = lab2.z;
    float Cab1 = sqrt(a1*a1 + b1*b1);
    float Cab2 = sqrt(a2*a2 + b2*b2);
    float Cab_avg = (Cab1 + Cab2) * 0.5;
    float Cab_avg7 = pow(Cab_avg, 7.0);
    float G = 0.5 * (1.0 - sqrt(Cab_avg7 / (Cab_avg7 + 6103515625.0)));
    float a1p = a1 * (1.0 + G);
    float a2p = a2 * (1.0 + G);
    float C1p = sqrt(a1p*a1p + b1*b1);
    float C2p = sqrt(a2p*a2p + b2*b2);
    float h1p = (C1p < 1e-10) ? 0.0 : atan2(b1, a1p); if (h1p < 0.0) h1p += 6.28318530718;
    float h2p = (C2p < 1e-10) ? 0.0 : atan2(b2, a2p); if (h2p < 0.0) h2p += 6.28318530718;
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
        - 0.17 * cos(hp_avg - 0.52359877559)
        + 0.24 * cos(2.0 * hp_avg)
        + 0.32 * cos(3.0 * hp_avg + 0.10471975512)
        - 0.20 * cos(4.0 * hp_avg - 1.09955742876);
    float Lp50sq = (Lp_avg - 50.0) * (Lp_avg - 50.0);
    float SL = 1.0 + 0.015 * Lp50sq / sqrt(20.0 + Lp50sq);
    float SC = 1.0 + 0.045 * Cp_avg;
    float SH = 1.0 + 0.015 * Cp_avg * T;
    float Cp_avg7 = pow(Cp_avg, 7.0);
    float RC = 2.0 * sqrt(Cp_avg7 / (Cp_avg7 + 6103515625.0));
    float hp_deg = hp_avg * 57.29577951;
    float dtheta = 30.0 * exp(-((hp_deg - 275.0) / 25.0) * ((hp_deg - 275.0) / 25.0));
    float RT = -sin(2.0 * dtheta * 0.01745329252) * RC;
    float t1 = dLp / SL;
    float t2 = dCp / SC;
    float t3 = dHp / SH;
    return sqrt(t1*t1 + t2*t2 + t3*t3 + RT * t2 * t3);
}

)HLSL";

    void TestDeltaE(ShaderTestBench& bench)
    {
        std::printf("\n=== Math: Delta E ===\n");

        // ---- Identity & symmetry ------------------------------------------
        {
            auto r = bench.Run(R"(
                float3 lab = float3(50.0, 10.0, -5.0);
                float d76 = DeltaE76(lab, lab);
                float d94 = DeltaE94(lab, lab);
                float d2000 = DeltaE2000(lab, lab);
                Result[0] = float4(d76, d94, d2000, 0);
            )", 1, kDeltaEHelpers);
            TEST("DeltaE76(c, c) == 0",
                !r.empty() && Near(r[0].x, 0.0f, 1e-5f));
            TEST("DeltaE94(c, c) == 0",
                !r.empty() && Near(r[0].y, 0.0f, 1e-5f));
            TEST("DeltaE2000(c, c) == 0",
                !r.empty() && Near(r[0].z, 0.0f, 1e-5f));
        }
        {
            auto r = bench.Run(R"(
                float3 a = float3(50.0, 10.0, -5.0);
                float3 b = float3(45.0, -3.0, 8.0);
                float dab = DeltaE76(a, b);
                float dba = DeltaE76(b, a);
                Result[0] = float4(dab, dba, abs(dab - dba), 0);
            )", 1, kDeltaEHelpers);
            TEST("DeltaE76 is symmetric: dE(a,b) == dE(b,a)",
                !r.empty() && Near(r[0].z, 0.0f, 1e-4f));
        }

        // ---- DeltaE76 algebra ---------------------------------------------
        // Pure Euclidean L*a*b* distance — easy to compute by hand.
        // (50,0,0) <-> (40,3,-4): dL=-10, da=3, db=-4. dE = sqrt(100+9+16)=sqrt(125)~=11.180.
        {
            auto r = bench.Run(R"(
                float d = DeltaE76(float3(50, 0, 0), float3(40, 3, -4));
                Result[0] = float4(d, 0, 0, 0);
            )", 1, kDeltaEHelpers);
            TEST("DeltaE76((50,0,0),(40,3,-4)) ~= 11.180 (sqrt(125))",
                !r.empty() && Near(r[0].x, 11.180f, 1e-2f));
        }

        // ---- Delta E ITP (BT.2124) ----------------------------------------
        // DeltaEITP / DeltaEITPFromScRGB live in the SHARED colour-math
        // library (unlike the Lab metrics above), so the bench gets them
        // without kDeltaEHelpers.
        {
            auto r = bench.Run(R"(
                float3 c = float3(0.5, 0.02, -0.03);
                float3 a = float3(0.5, 0.02, -0.03);
                float3 b = float3(0.4, -0.01, 0.05);
                float ident = DeltaEITP(c, c);
                float dab = DeltaEITP(a, b);
                float dba = DeltaEITP(b, a);
                Result[0] = float4(ident, dab, dba, abs(dab - dba));
            )", 1);
            TEST("DeltaEITP(c, c) == 0", !r.empty() && Near(r[0].x, 0.0f, 1e-6f));
            TEST("DeltaEITP is symmetric", !r.empty() && Near(r[0].w, 0.0f, 1e-5f));
        }
        {
            // The 720 scale factor, isolated: a pure I difference of 0.001
            // must read 0.72 by definition of the metric.
            auto r = bench.Run(R"(
                float d = DeltaEITP(float3(0.0, 0.0, 0.0), float3(0.001, 0.0, 0.0));
                Result[0] = float4(d, 0, 0, 0);
            )", 1);
            TEST("DeltaEITP scale: dI=0.001 -> 0.72", !r.empty() && Near(r[0].x, 0.72f, 1e-4f));
        }
        {
            // BT.2124's defining asymmetry: T = 0.5*Ct while P = Cp, so an
            // identical numeric step in Cp counts DOUBLE the same step in Ct.
            // Dropping the 0.5 (or applying it to Cp) is the easy bug.
            auto r = bench.Run(R"(
                float dCt = DeltaEITP(float3(0,0,0), float3(0, 0.01, 0));
                float dCp = DeltaEITP(float3(0,0,0), float3(0, 0, 0.01));
                Result[0] = float4(dCt, dCp, dCp / max(dCt, 1e-9), 0);
            )", 1);
            TEST("DeltaEITP weights Ct at half of Cp (ratio == 2)",
                !r.empty() && Near(r[0].z, 2.0f, 1e-4f));
            TEST("DeltaEITP Ct step: 0.01 -> 3.6",
                !r.empty() && Near(r[0].x, 3.6f, 1e-3f));
        }
        {
            // Grounds the scale against a metric that IS valid in the SDR
            // sRGB domain: for a small near-neutral step both are in their
            // fitted range and ~1 unit = ~1 JND, so they must agree within a
            // small factor. This is what would catch a wrong constant
            // (720 vs 100 vs 1) that the pure-formula checks accept happily.
            auto r = bench.Run(R"(
                float3 rgbA = float3(0.5, 0.5, 0.5);
                float3 rgbB = float3(0.52, 0.5, 0.5);
                float itp  = DeltaEITPFromScRGB(rgbA, rgbB);
                float lab  = DeltaE2000(ScRGBToLab(rgbA), ScRGBToLab(rgbB));
                Result[0] = float4(itp, lab, itp / max(lab, 1e-6), 0);
            )", 1, kDeltaEHelpers);
            bool sane = !r.empty() && r[0].x > 0.0f && r[0].y > 0.0f &&
                        r[0].z > 0.2f && r[0].z < 5.0f;
            if (!r.empty())
                std::printf("  dE ITP %.3f vs dE2000 %.3f (ratio %.2f) on a small SDR step\n",
                            r[0].x, r[0].y, r[0].z);
            TEST("DeltaEITP magnitude agrees with dE2000 within 5x on a small SDR step", sane);
        }
        {
            // Monotonic in separation, and meaningful where the Lab metrics
            // are out of their domain: bright HDR neutrals.
            // (scRGB 1.0 = 80 nits, so 12.5 = 1000 nits.)
            auto r = bench.Run(R"(
                float3 a = float3(12.5, 12.5, 12.5);
                float3 b = float3(13.75, 13.75, 13.75);
                float3 c = float3(25.0, 25.0, 25.0);
                float dNear = DeltaEITPFromScRGB(a, b);
                float dFar  = DeltaEITPFromScRGB(a, c);
                Result[0] = float4(dNear, dFar, dFar - dNear, 0);
            )", 1);
            TEST("DeltaEITP is non-zero at HDR levels (1000 vs 1100 nits)",
                !r.empty() && r[0].x > 0.5f);
            TEST("DeltaEITP grows with separation at HDR levels",
                !r.empty() && r[0].z > 0.0f);
        }

        // ---- DeltaE2000 Sharma reference pairs ----------------------------
        // From Sharma, Wu, Dalal: "The CIEDE2000 Color-Difference Formula:
        // Implementation Notes, Supplementary Test Data, and Mathematical
        // Observations" (Color Research and Application, 2005, table 1).
        // Use a few well-spread pairs across the L/C/h space to exercise
        // every branch of the formula (G correction, hue wrap, T term,
        // RT rotation in the blue region).
        //
        // Pair 6 (50,-1,2) vs (50,0,0) was previously broken: atan2(0,0)
        // on (a2=0,b2=0) produced NaN that propagated through hp_avg.
        // Fixed by guarding h2p with C2p<1e-10 (Phase 5 companion fix to
        // bug `p2-bug-de2000-nan`). Effect version bumped 3 -> 4.
        struct Pair { float l1, a1, b1, l2, a2, b2, expected; };
        const Pair pairs[] = {
            { 50.0000f,  2.6772f, -79.7751f, 50.0000f, 0.0000f, -82.7485f, 2.0425f },
            { 50.0000f, -1.0000f,   2.0000f, 50.0000f, 0.0000f,   0.0000f, 2.3669f },
            { 50.0000f,  2.5000f,   0.0000f, 50.0000f, 0.0000f,  -2.5000f, 4.3065f },
            { 60.2574f, -34.0099f, 36.2677f, 60.4626f, -34.1751f, 39.4387f, 1.2644f },
            { 22.7233f,  20.0904f, -46.6940f, 23.0331f, 14.9730f, -42.5619f, 2.0373f },
        };
        for (const auto& p : pairs)
        {
            char body[512];
            std::snprintf(body, sizeof(body),
                "float d = DeltaE2000(float3(%.4f,%.4f,%.4f), float3(%.4f,%.4f,%.4f));"
                "Result[0] = float4(d, 0, 0, 0);",
                p.l1, p.a1, p.b1, p.l2, p.a2, p.b2);
            auto r = bench.Run(std::string(body), 1, kDeltaEHelpers);
            char name[256];
            std::snprintf(name, sizeof(name),
                "DeltaE2000 Sharma pair (%.2f, %.2f, %.2f) vs (%.2f, %.2f, %.2f) ~= %.4f",
                p.l1, p.a1, p.b1, p.l2, p.a2, p.b2, p.expected);
            TEST(name, !r.empty() && Near(r[0].x, p.expected, 5e-3f));
        }
    }
}
