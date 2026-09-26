#include "pch_engine.h"
#include "TestCommon.h"
#include "ShaderTestBench.h"
#include "Graph/EffectGraph.h"
#include "Rendering/GraphEvaluator.h"
#include "Rendering/PixelReadback.h"
#include "Rendering/VideoExport.h"
#include "Effects/SourceNodeFactory.h"
#include "Effects/ShaderLabEffects.h"
#include "Effects/EffectRegistry.h"
#include "Effects/ShaderCompiler.h"
#include "Effects/BytecodeCache.h"
#include "Effects/ImageLoader.h"
#include "Effects/Performance.h"
#include "Effects/ShaderLabParamsHlsl.h"
#include "Effects/CustomPixelShaderEffect.h"
#include "Effects/CustomComputeShaderEffect.h"
#include "Rendering/RenderThreadDispatcher.h"
#include "Graph/GraphUiSnapshot.h"
#include "Engine/Mcp/McpRouter.h"
#include "Engine/Mcp/McpJsonRpc.h"
#include "Engine/Mcp/McpToolCatalog.h"
#include "Engine/Mcp/McpFrame.h"
#include "Engine/Mcp/McpCrypto.h"
#include "Engine/Mcp/McpPeerIdentity.h"
#include "Engine/Mcp/McpChannel.h"
#include "Engine/Mcp/McpSessionClient.h"

#include <atomic>
#include <thread>

#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

// ============================================================================
// ShaderLab Test Runner — standalone console app sharing the engine code.
// Usage: ShaderLabTests.exe [--adapter warp|default]
// Exit code = number of failures (0 = all passed).
// ============================================================================

// Forward decl from Tests/Math/*.cpp.
namespace ShaderLab::Tests
{
    void TestTransferFunctions(ShaderTestBench& bench);
    void TestColorMatrices(ShaderTestBench& bench);
    void TestMobiusReinhard(ShaderTestBench& bench);
    void TestDeltaE(ShaderTestBench& bench);
    void TestGamut(ShaderTestBench& bench);
}

namespace
{
    // TEST() and g_passed/g_failed live in TestCommon.h so multiple TUs
    // (Tests/Math/*.cpp) can share the same summary counters. Pull them
    // into the local anonymous namespace via using-declarations so the
    // existing test bodies in this file stay unchanged.
    using ShaderLab::Tests::TEST;
    using ShaderLab::Tests::g_passed;
    using ShaderLab::Tests::g_failed;

    // Shared device resources.
    winrt::com_ptr<ID3D11Device5> g_d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext4> g_d3dContext;
    winrt::com_ptr<ID2D1Factory7> g_d2dFactory;
    winrt::com_ptr<ID2D1DeviceContext5> g_dc;

    // Phase 7 spike discovered: cachedOutput is only valid while the
    // GraphEvaluator that produced it is alive. The evaluator owns the
    // ID2D1Effect cache; effects own their output ID2D1Image; node
    // pointers are non-owning. Keep this evaluator alive for the
    // lifetime of any test that wants to use cachedOutput downstream
    // (DrawImage, pixel readback, etc).
    ShaderLab::Rendering::GraphEvaluator g_evaluator;

    void Evaluate(ShaderLab::Graph::EffectGraph& graph, ShaderLab::Effects::SourceNodeFactory& sf)
    {
        graph.MarkAllDirty();
        for (auto& node : const_cast<std::vector<ShaderLab::Graph::EffectNode>&>(graph.Nodes()))
        {
            if (node.type == ShaderLab::Graph::NodeType::Source)
            {
                try { sf.PrepareSourceNode(node, g_dc.get(), 0.0, g_d3dDevice.get(), g_d3dContext.get()); }
                catch (...) {}
            }
        }
        g_evaluator.Evaluate(graph, g_dc.get());
        g_evaluator.Evaluate(graph, g_dc.get()); // second pass for new effects
    }

    bool HasOutput(const ShaderLab::Graph::EffectNode& node)
    {
        return node.cachedOutput != nullptr;
    }

    // ========================================================================
    // Test categories
    // ========================================================================

    void TestGraphOperations()
    {
        printf("\n=== Graph Operations ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();

        ShaderLab::Graph::EffectGraph g;

        // Add/remove.
        auto node = ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source"));
        auto id1 = g.AddNode(std::move(node));
        TEST("AddNode", g.FindNode(id1) != nullptr);
        TEST("NodeCount", g.Nodes().size() == 1);
        g.RemoveNode(id1);
        TEST("RemoveNode", g.FindNode(id1) == nullptr && g.Nodes().empty());

        // Connect/disconnect.
        auto src = ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source"));
        auto blur = ShaderLab::Effects::EffectRegistry::Instance().CreateNode(
            *ShaderLab::Effects::EffectRegistry::Instance().FindByName(L"Gaussian Blur"));
        auto srcId = g.AddNode(std::move(src));
        auto blurId = g.AddNode(std::move(blur));
        TEST("Connect", g.Connect(srcId, 0, blurId, 0));
        TEST("EdgeExists", g.GetInputEdges(blurId).size() == 1);
        g.Disconnect(srcId, 0, blurId, 0);
        TEST("Disconnect", g.GetInputEdges(blurId).empty());

        // Topo sort.
        g.Connect(srcId, 0, blurId, 0);
        auto topo = g.TopologicalSort();
        TEST("TopoSort", topo.size() == 2 && topo[0] == srcId);

        // Cycle detection.
        TEST("DetectsCycle", g.WouldCreateCycle(blurId, srcId));
        TEST("AllowsExisting", !g.WouldCreateCycle(srcId, blurId));
    }

    void TestSerialization()
    {
        printf("\n=== Serialization ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();

        ShaderLab::Graph::EffectGraph g;
        auto src = ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source"));
        auto blur = ShaderLab::Effects::EffectRegistry::Instance().CreateNode(
            *ShaderLab::Effects::EffectRegistry::Instance().FindByName(L"Gaussian Blur"));
        auto srcId = g.AddNode(std::move(src));
        auto blurId = g.AddNode(std::move(blur));
        g.Connect(srcId, 0, blurId, 0);
        g.FindNode(blurId)->properties[L"StandardDeviation"] = 3.0f;

        auto json = g.ToJson();
        TEST("SerializeNotEmpty", !json.empty());

        auto g2 = ShaderLab::Graph::EffectGraph::FromJson(json);
        TEST("DeserializeNodeCount", g2.Nodes().size() == 2);
        TEST("DeserializeEdges", !g2.Edges().empty());

        auto* blurLoaded = g2.FindNode(blurId);
        bool propOk = false;
        if (blurLoaded) {
            auto it = blurLoaded->properties.find(L"StandardDeviation");
            if (it != blurLoaded->properties.end()) {
                auto* fv = std::get_if<float>(&it->second);
                propOk = fv && std::abs(*fv - 3.0f) < 0.01f;
            }
        }
        TEST("PropertyPreserved", propOk);
    }

    // Documentation drift guard.
    //
    // The ShaderLab effect count is quoted in six places outside the code
    // (docs/effects/builtin-catalog.md, docs/README.md,
    // docs/development/project-structure.md, docs/architecture/engine-host-split.md,
    // and twice in .github/copilot-instructions.md). Those quotes drifted to
    // 33 and 35 while the registry held 36. Adding or removing an effect
    // should fail here, as a reminder to update the catalog table and the
    // counts alongside it -- not silently desync the docs again.
    // Nothing in the suite could see a coordinate-semantics defect: the catalog
    // test checks counts and ids, and the render tests assert only
    // HasOutput() && runtimeError.empty() -- which a blank diagram and an
    // all-black identity both satisfy. 306/306 passed with a blank ICtCp
    // Boundary and two -128px shaders live in the same DLL. This is the cheap
    // structural check that would have caught all three, and it needs no GPU.
    void TestPixelShaderSignatures()
    {
        printf("\n=== Pixel Shader Signatures (D2D contract guard) ===\n");

        // D2D's pixel-shader input signature is
        //   (SV_POSITION, SCENE_POSITION, TEXCOORD0..N)
        // with one TEXCOORD per input. Omitting SCENE_POSITION shifts the
        // binding so a parameter labelled TEXCOORD0 receives the SCENE
        // coordinate; declaring it twice is equally wrong and fxc accepts it
        // silently. Both are invisible at the default source size.
        const auto& all = ShaderLab::Effects::ShaderLabEffects::Instance().All();
        std::wstring missingScene, duplicateScene, sharedTexcoord, badOrder;
        size_t checked = 0;

        for (const auto& e : all)
        {
            if (e.shaderType != ShaderLab::Graph::CustomShaderType::PixelShader) continue;
            if (e.hlslSource.empty()) continue;

            // Look only at the entry point's parameter list.
            const size_t mainPos = e.hlslSource.rfind("float4 main(");
            if (mainPos == std::string::npos) continue;
            const size_t close = e.hlslSource.find(')', mainPos);
            if (close == std::string::npos) continue;
            const std::string sig = e.hlslSource.substr(mainPos, close - mainPos);
            ++checked;

            auto countOf = [&](const char* needle) {
                size_t n = 0, at = 0;
                while ((at = sig.find(needle, at)) != std::string::npos) { ++n; at += 1; }
                return n;
            };
            const size_t scene = countOf("SCENE_POSITION");

            // Collect the DISTINCT TEXCOORD indices rather than counting the
            // substring. A plain count lets TEXCOORD0-declared-twice satisfy a
            // two-input shader, and lets {0,2} or {1} pass with no index 0.
            std::set<int> texIdx;
            for (size_t at = 0; (at = sig.find("TEXCOORD", at)) != std::string::npos; ++at)
            {
                size_t d = at + 8, v = 0; bool any = false;
                while (d < sig.size() && isdigit(static_cast<unsigned char>(sig[d])))
                { v = v * 10 + static_cast<size_t>(sig[d] - '0'); ++d; any = true; }
                if (any) texIdx.insert(static_cast<int>(v));
            }

            // D2D binds POSITIONALLY, so order matters as much as presence: a
            // shader declaring (SV_POSITION, TEXCOORD0, SCENE_POSITION) passes
            // every presence check and still receives the scene coordinate in
            // uv0. Require SCENE_POSITION to appear before the first TEXCOORD.
            const size_t scenePos = sig.find("SCENE_POSITION");
            const size_t firstTex = sig.find("TEXCOORD");
            if (!texIdx.empty() && scene > 0 && scenePos > firstTex)
                badOrder += e.name + L" ";

            if (!texIdx.empty() && scene == 0) missingScene   += e.name + L" ";
            if (scene > 1)                     duplicateScene += e.name + L" ";

            // Indices must be exactly {0 .. inputs-1}: one per input, no gaps,
            // no duplicates, starting at 0.
            if (!e.inputNames.empty())
            {
                std::set<int> want;
                for (size_t k = 0; k < e.inputNames.size(); ++k) want.insert(static_cast<int>(k));
                // CIE Chromaticity Plot legitimately reads no TEXCOORD: it takes
                // the scene coordinate for diagram geometry and derives its own
                // histogram texel coords via GetDimensions(). Recorded rather
                // than silently tolerated -- see ShaderLabEffects.cpp.
                const bool knownException = (e.name == L"CIE Chromaticity Plot");
                if (!knownException && texIdx != want) sharedTexcoord += e.name + L" ";
            }
        }

        if (!missingScene.empty())
            printf("  missing SCENE_POSITION: %ls\n", missingScene.c_str());
        if (!duplicateScene.empty())
            printf("  duplicate SCENE_POSITION: %ls\n", duplicateScene.c_str());
        if (!sharedTexcoord.empty())
            printf("  TEXCOORD indices not {0..inputs-1}: %ls\n", sharedTexcoord.c_str());
        if (!badOrder.empty())
            printf("  SCENE_POSITION after TEXCOORD: %ls\n", badOrder.c_str());

        // Pin the scan size the way TestEffectCatalogCount pins the catalog:
        // `checked > 0` cannot notice the extraction silently shrinking, which
        // would turn this guard into a no-op that still reports PASS.
        constexpr size_t kExpectedPixelShadersScanned = 14;
        if (checked != kExpectedPixelShadersScanned)
            printf("  scanned %zu pixel shaders, expected %zu -- if you added or removed one, "
                   "update kExpectedPixelShadersScanned; if not, the main() extraction broke.\n",
                   checked, kExpectedPixelShadersScanned);
        TEST("pixel-shader scan covered the expected count",
             checked == kExpectedPixelShadersScanned);
        TEST("every TEXCOORD-taking pixel shader declares SCENE_POSITION", missingScene.empty());
        TEST("no pixel shader declares SCENE_POSITION twice", duplicateScene.empty());
        TEST("TEXCOORD indices are exactly {0..inputs-1}", sharedTexcoord.empty());
        TEST("SCENE_POSITION precedes TEXCOORD (D2D binds positionally)", badOrder.empty());
    }

    void TestEffectCatalogCount()
    {
        printf("\n=== Effect Catalog Count (doc drift guard) ===\n");

        // Bump this together with the catalog table + the counts listed above.
        constexpr size_t kExpectedShaderLabEffects = 36;

        const auto& all = ShaderLab::Effects::ShaderLabEffects::Instance().All();
        if (all.size() != kExpectedShaderLabEffects)
        {
            printf("  registry holds %zu effects, expected %zu -- update "
                   "docs/effects/builtin-catalog.md and the counts in "
                   "docs/README.md, docs/development/project-structure.md, "
                   "docs/architecture/engine-host-split.md and "
                   ".github/copilot-instructions.md, then bump "
                   "kExpectedShaderLabEffects.\n",
                   all.size(), kExpectedShaderLabEffects);
        }
        TEST("ShaderLab effect count matches the documented catalog",
             all.size() == kExpectedShaderLabEffects);

        // effectId is the stable identity saved in graphs; a duplicate would
        // make effectVersion upgrades ambiguous on load.
        std::set<std::wstring> ids;
        bool unique = true;
        for (const auto& e : all)
            if (!ids.insert(e.effectId).second) unique = false;
        TEST("every effectId is unique", unique);
    }

    void TestSourceEffects()
    {
        printf("\n=== Source Effects ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();

        const wchar_t* names[] = {
            L"Gamut Source", L"Color Checker", L"Zone Plate",
            L"Gradient Generator", L"HDR Test Pattern"
        };
        for (const auto* name : names)
        {
            std::string testName = "Source_";
            for (const wchar_t* p = name; *p; ++p) testName += static_cast<char>(*p);

            auto* desc = registry.FindByName(name);
            if (!desc) { TEST("FindDescriptor", false); continue; }

            ShaderLab::Graph::EffectGraph g;
            ShaderLab::Effects::SourceNodeFactory sf;
            auto node = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            auto id = g.AddNode(std::move(node));
            Evaluate(g, sf);

            auto* n = g.FindNode(id);
            bool ok = n && HasOutput(*n) && n->runtimeError.empty();
            TEST(testName.c_str(), ok);
        }
    }

    // Regression: a D3D11-compute node consuming ANOTHER D3D11-compute node
    // used the pre-render captured during Evaluate -- i.e. before
    // ProcessDeferredCompute had dispatched the producer -- and therefore read
    // the PREVIOUS evaluation's pixels. The symptom was a clean one-mutation
    // lag: sweeping an upstream parameter reported the value from the step
    // before, silently, with entirely plausible numbers and no error anywhere.
    // That makes every compute-fed measurement wrong in a way no single
    // reading can reveal -- Delta E Comparator and all three Statistics nodes
    // are D3D11 compute, so the measurement instruments were affected too.
    //
    // Dispatches ONCE per sweep on purpose. The two-dispatch `runTwoPass`
    // below would mask the lag by giving the consumer a second chance to
    // catch up; headless renders with a single ProcessDeferredCompute per
    // frame, which is the path that actually broke.
    void TestComputeChainFreshness()
    {
        printf("\n=== Compute Chain Freshness ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();

        auto* srcDesc  = registry.FindByName(L"Gamut Source");
        auto* prodDesc = registry.FindByName(L"ICtCp Tone Map (HDR -> SDR)");
        auto* consDesc = registry.FindByName(L"ICtCp Highlight Desaturation");
        if (!srcDesc || !prodDesc || !consDesc)
        {
            TEST("ComputeChain_FindDescriptors", false);
            return;
        }

        ShaderLab::Graph::EffectGraph g;
        ShaderLab::Effects::SourceNodeFactory sf;
        auto srcId  = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*srcDesc));
        auto prodId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*prodDesc));
        auto consId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*consDesc));
        g.Connect(srcId, 0, prodId, 0);
        g.Connect(prodId, 0, consId, 0);

        // Gamut Source renders black outside its gamut triangle, so sample the
        // D65 centre of the default 1024px output -- a corner reads 0 and tone
        // maps to 0 at every peak, which would make the sweep look static for
        // reasons that have nothing to do with the bug under test. 400 nits
        // puts the sample well above the tone curve's knee so the parameter
        // actually bites.
        g.FindNode(srcId)->properties[L"Luminance"] = 400.0f;
        constexpr int32_t kProbeX = 512, kProbeY = 512;

        // Exactly the shape headless runEval and MainWindow::RenderFrameToOffscreen use:
        // ProcessDeferredCompute inside the draw session, then the frozen
        // sweep that clears the dirty flags the dispatch just set (without it
        // the readback below returns NotReady). Frozen so this in-session pass
        // cannot re-queue a compute node and nest a BeginDraw.
        auto dispatchOnce = [&]() {
            g_dc->SetTarget(nullptr);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            if (g.HasDirtyNodes())
            {
                g_evaluator.SetDeferredComputeFrozen(true);
                g_evaluator.Evaluate(g, g_dc.get());
                g_evaluator.SetDeferredComputeFrozen(false);
            }
            g_dc->EndDraw();
        };

        // Warm-up: new D2D custom effects need two evaluation passes before
        // their output is correct, so settle the chain before measuring.
        Evaluate(g, sf); dispatchOnce();
        Evaluate(g, sf); dispatchOnce();

        auto sweep = [&](float targetPeak) -> float {
            g.FindNode(prodId)->properties[L"TargetPeakNits"] = targetPeak;
            Evaluate(g, sf);
            dispatchOnce();
            auto r = ShaderLab::Rendering::ReadPixelRegion(g, consId, kProbeX, kProbeY, 1, 1, g_dc.get());
            if (r.status != ShaderLab::Rendering::ReadPixelRegionStatus::Success ||
                r.pixels.size() < 4)
            {
                printf("      readback failed: status=%d pixels=%zu cachedOutput=%s\n",
                    static_cast<int>(r.status), r.pixels.size(),
                    g.FindNode(consId)->cachedOutput ? "set" : "null");
                return -1.0f;
            }
            return r.pixels[1];   // green channel
        };

        const float a1 = sweep(203.0f);
        const float b  = sweep(100.0f);
        const float a2 = sweep(203.0f);

        const bool readOk = a1 >= 0.0f && b >= 0.0f && a2 >= 0.0f;
        TEST("ComputeChain_ReadbackSucceeded", readOk);
        // Guard the value assertions on the readback: without this, a failed
        // read returns -1 from every sweep and the equality check below passes
        // vacuously -- a false pass that would hide the very bug it pins.
        // The consumer must respond to its producer's parameter at all. With
        // the lag, the 100 read returns the 203 result and these are equal.
        TEST("ComputeChain_DownstreamTracksUpstream", readOk && std::fabs(a1 - b) > 1e-3f);
        // Returning to the first setting must reproduce the first value. With
        // the lag, this second 203 read returns the 100 result instead.
        TEST("ComputeChain_NoStaleLag", readOk && std::fabs(a1 - a2) < 1e-4f);

        // --- and the same thing ONE HOP FURTHER OUT --------------------------
        // The direct compute->compute case above passed while
        // compute -> PIXEL SHADER -> compute still lagged, because the middle
        // node is not in the deferred set even though its D2D output resolves
        // through a producer that has not been dispatched yet. Checking only
        // the immediate producer was not enough; the guard has to cover
        // everything DOWNSTREAM of this pass's dispatches. Pin both shapes so a
        // future narrowing of that set fails here rather than in a measurement.
        auto* midDesc = registry.FindByName(L"Nit Map");
        if (!midDesc) { TEST("ComputeChain2_FindDescriptor", false); return; }

        // m_effectCache is keyed on node id ALONE with no type check, and every
        // EffectGraph starts its ids at 1 -- so without this the second graph
        // inherits the first graph's cached effects for the same ids and the
        // test may not be exercising the nodes it names.
        g_evaluator.ReleaseCache();
        ShaderLab::Graph::EffectGraph g2;
        ShaderLab::Effects::SourceNodeFactory sf2;
        auto srcId2  = g2.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*srcDesc));
        auto prodId2 = g2.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*prodDesc));
        auto midId2  = g2.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*midDesc));
        auto consId2 = g2.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*consDesc));
        g2.Connect(srcId2, 0, prodId2, 0);
        g2.Connect(prodId2, 0, midId2, 0);
        g2.Connect(midId2, 0, consId2, 0);
        g2.FindNode(srcId2)->properties[L"Luminance"] = 400.0f;
        g2.FindNode(midId2)->properties[L"Opacity"] = 0.0f;   // pass-through

        auto dispatchOnce2 = [&]() {
            g_dc->SetTarget(nullptr);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g2, g_dc.get());
            if (g2.HasDirtyNodes())
            {
                g_evaluator.SetDeferredComputeFrozen(true);
                g_evaluator.Evaluate(g2, g_dc.get());
                g_evaluator.SetDeferredComputeFrozen(false);
            }
            g_dc->EndDraw();
        };
        Evaluate(g2, sf2); dispatchOnce2();
        Evaluate(g2, sf2); dispatchOnce2();

        auto sweep2 = [&](float targetPeak) -> float {
            g2.FindNode(prodId2)->properties[L"TargetPeakNits"] = targetPeak;
            Evaluate(g2, sf2);
            dispatchOnce2();
            auto r = ShaderLab::Rendering::ReadPixelRegion(g2, consId2, kProbeX, kProbeY, 1, 1, g_dc.get());
            return (r.status == ShaderLab::Rendering::ReadPixelRegionStatus::Success &&
                    r.pixels.size() >= 4) ? r.pixels[1] : -1.0f;
        };
        const float c1 = sweep2(203.0f);
        const float c2 = sweep2(100.0f);
        const float c3 = sweep2(203.0f);
        const bool readOk2 = c1 >= 0.0f && c2 >= 0.0f && c3 >= 0.0f;
        TEST("ComputeChain2Hop_ReadbackSucceeded", readOk2);
        TEST("ComputeChain2Hop_TracksUpstream", readOk2 && std::fabs(c1 - c2) > 1e-3f);
        TEST("ComputeChain2Hop_NoStaleLag", readOk2 && std::fabs(c1 - c3) < 1e-4f);
    }

    void TestAnalysisEffects()
    {
        printf("\n=== Analysis Effects ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        // Wave Monitor + similar viewers migrated to D3D11 compute (Phase
        // 8c) need the BeginDraw + ProcessDeferredCompute + double-eval
        // dance to actually produce output. Apply it uniformly so this
        // test works for both pixel-shader and compute-bridge effects.
        auto runTwoPass = [&](ShaderLab::Graph::EffectGraph& g,
                              ShaderLab::Effects::SourceNodeFactory& sf) {
            g_evaluator.ReleaseCache();
            Evaluate(g, sf);
            g_dc->SetTarget(nullptr);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();
            Evaluate(g, sf);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();
        };

        const wchar_t* names[] = {
            L"Luminance Heatmap", L"Gamut Highlight", L"Nit Map"
        };
        for (const auto* name : names)
        {
            auto* desc = registry.FindByName(name);
            if (!desc) { TEST("FindDescriptor", false); continue; }

            ShaderLab::Graph::EffectGraph g;
            ShaderLab::Effects::SourceNodeFactory sf;
            auto srcNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"Gamut Source"));
            auto fxNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            auto srcId = g.AddNode(std::move(srcNode));
            auto fxId = g.AddNode(std::move(fxNode));
            g.Connect(srcId, 0, fxId, 0);
            runTwoPass(g, sf);

            auto* n = g.FindNode(fxId);
            bool ok = n && HasOutput(*n) && n->runtimeError.empty();
            std::string testName = "Analysis_";
            for (const wchar_t* p = name; *p; ++p) testName += static_cast<char>(*p);
            TEST(testName.c_str(), ok);
        }

        {
            auto* desc = registry.FindByName(L"Split Comparison");
            if (!desc) { TEST("FindDescriptor", false); return; }

            ShaderLab::Graph::EffectGraph g;
            ShaderLab::Effects::SourceNodeFactory sf;
            auto src1 = ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"Gamut Source"));
            auto src2 = ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"Color Checker"));
            auto fxNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            auto src1Id = g.AddNode(std::move(src1));
            auto src2Id = g.AddNode(std::move(src2));
            auto fxId = g.AddNode(std::move(fxNode));
            g.Connect(src1Id, 0, fxId, 0);
            g.Connect(src2Id, 0, fxId, 1);
            runTwoPass(g, sf);

            auto* n = g.FindNode(fxId);
            TEST("Analysis_Split Comparison", n && HasOutput(*n) && n->runtimeError.empty());
        }
    }

    void TestBuiltInD2DEffects()
    {
        printf("\n=== Built-in D2D Effects ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto& d2dReg = ShaderLab::Effects::EffectRegistry::Instance();

        const wchar_t* names[] = {
            L"Gaussian Blur", L"Brightness", L"Contrast",
            L"Grayscale", L"Invert", L"Saturation",
            L"Hue Rotation", L"Exposure", L"Sharpen",
            L"Edge Detection", L"Crop"
        };
        for (const auto* name : names)
        {
            auto* desc = d2dReg.FindByName(name);
            if (!desc) { TEST("FindD2DEffect", false); continue; }

            ShaderLab::Graph::EffectGraph g;
            ShaderLab::Effects::SourceNodeFactory sf;
            auto srcNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"Gamut Source"));
            auto fxNode = d2dReg.CreateNode(*desc);
            auto srcId = g.AddNode(std::move(srcNode));
            auto fxId = g.AddNode(std::move(fxNode));
            g.Connect(srcId, 0, fxId, 0);
            Evaluate(g, sf);

            auto* n = g.FindNode(fxId);
            bool ok = n && HasOutput(*n);
            std::string testName = "D2D_";
            for (const wchar_t* p = name; *p; ++p) testName += static_cast<char>(*p);
            TEST(testName.c_str(), ok);
        }
    }

    void TestPropertyBindings()
    {
        printf("\n=== Property Bindings ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto& d2dReg = ShaderLab::Effects::EffectRegistry::Instance();

        ShaderLab::Graph::EffectGraph g;
        ShaderLab::Effects::SourceNodeFactory sf;

        auto srcNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *registry.FindByName(L"Gamut Source"));
        auto paramNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *registry.FindByName(L"Float Parameter"));
        auto blurNode = d2dReg.CreateNode(*d2dReg.FindByName(L"Gaussian Blur"));

        auto srcId = g.AddNode(std::move(srcNode));
        auto paramId = g.AddNode(std::move(paramNode));
        auto blurId = g.AddNode(std::move(blurNode));
        g.Connect(srcId, 0, blurId, 0);

        g.FindNode(paramId)->properties[L"Value"] = 5.0f;
        auto err = g.BindProperty(blurId, L"StandardDeviation", paramId, L"Value", 0);
        TEST("BindProperty", err.empty());

        Evaluate(g, sf);
        auto* blur = g.FindNode(blurId);
        auto sdIt = blur->properties.find(L"StandardDeviation");
        bool sdOk = false;
        if (sdIt != blur->properties.end()) {
            auto* fv = std::get_if<float>(&sdIt->second);
            sdOk = fv && *fv >= 4.5f && *fv <= 5.5f;
        }
        TEST("BindingPropagates", sdOk);
    }

    void TestMathNodes()
    {
        printf("\n=== Numeric Expression Node ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();

        // Effect descriptor exists.
        auto* desc = registry.FindByName(L"Numeric Expression");
        TEST("DescriptorExists", desc != nullptr);
        if (!desc) return;
        TEST("StableEffectId", desc->effectId == L"Math Expression");

        auto baseNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
        TEST("DefaultHasOnlyA",
            baseNode.properties.count(L"A") == 1 &&
            baseNode.properties.count(L"B") == 0 &&
            baseNode.properties.count(L"Expression") == 1);

        // Each entry: expression + (name,value) pairs to set + expected result.
        struct Case {
            const wchar_t* name;
            const wchar_t* expression;
            std::vector<std::pair<const wchar_t*, float>> inputs;
            float expected;
        };
        Case cases[] = {
            { L"Identity",         L"A",                 {{L"A", 7.5f}},                            7.5f },
            { L"AddTwoInputs",     L"A + B",             {{L"A", 3.0f},{L"B", 4.0f}},               7.0f },
            { L"SubtractMultiply", L"(A - B) * C",       {{L"A", 10.0f},{L"B", 4.0f},{L"C", 2.0f}}, 12.0f },
            { L"MaxOfFour",        L"max(A, B, C, D)",   {{L"A", 1.0f},{L"B", 9.0f},{L"C", 3.0f},{L"D", 7.0f}}, 9.0f },
            { L"FiveInputAvg",     L"(A + B + C + D + E) / 5",
                                   {{L"A", 1.0f},{L"B", 2.0f},{L"C", 3.0f},{L"D", 4.0f},{L"E", 5.0f}}, 3.0f },
            { L"SinPi",            L"sin(pi)",           {{L"A", 0.0f}},                            0.0f },
            { L"Conditional",      L"if(A > B, A, B)",   {{L"A", 5.0f},{L"B", 3.0f}},               5.0f },
        };

        for (const auto& c : cases)
        {
            ShaderLab::Graph::EffectGraph g;
            ShaderLab::Effects::SourceNodeFactory sf;
            auto node = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            // Add as many inputs as required (default has only A).
            for (const auto& [pname, pval] : c.inputs)
            {
                if (node.properties.find(pname) == node.properties.end())
                {
                    ShaderLab::Graph::ParameterDefinition pd;
                    pd.name = pname; pd.typeName = L"float"; pd.defaultValue = 0.0f;
                    pd.minValue = -100000.0f; pd.maxValue = 100000.0f; pd.step = 0.1f;
                    node.customEffect->parameters.push_back(std::move(pd));
                    node.properties[pname] = 0.0f;
                }
                node.properties[pname] = pval;
            }
            node.properties[L"Expression"] = std::wstring(c.expression);

            auto id = g.AddNode(std::move(node));
            Evaluate(g, sf);

            auto* mn = g.FindNode(id);
            bool ok = false;
            float got = 0.0f;
            if (mn) {
                for (const auto& f : mn->analysisOutput.fields) {
                    if (f.name == L"Result") { got = f.components[0]; break; }
                }
                ok = std::abs(got - c.expected) < 0.01f && mn->runtimeError.empty();
            }
            std::string testName = "Eval_";
            for (const wchar_t* p = c.name; *p; ++p) testName += static_cast<char>(*p);
            TEST(testName.c_str(), ok);
        }

        // Parse error path.
        {
            ShaderLab::Graph::EffectGraph g;
            ShaderLab::Effects::SourceNodeFactory sf;
            auto node = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            node.properties[L"Expression"] = std::wstring(L"A + + +");
            auto id = g.AddNode(std::move(node));
            Evaluate(g, sf);
            auto* mn = g.FindNode(id);
            TEST("ParseErrorReported", mn && !mn->runtimeError.empty());
        }

        // Add/remove inputs at the graph level (mirrors the UI flow).
        {
            ShaderLab::Graph::EffectGraph g;
            auto node = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            auto id = g.AddNode(std::move(node));
            auto* n = g.FindNode(id);

            auto addInput = [&](const wchar_t* name) {
                ShaderLab::Graph::ParameterDefinition pd;
                pd.name = name; pd.typeName = L"float"; pd.defaultValue = 0.0f;
                pd.minValue = -100000.0f; pd.maxValue = 100000.0f; pd.step = 0.1f;
                n->customEffect->parameters.push_back(std::move(pd));
                n->properties[name] = 0.0f;
            };
            addInput(L"B");
            addInput(L"C");
            TEST("AddInputs",
                n->properties.count(L"B") == 1 && n->properties.count(L"C") == 1);

            // Remove B.
            std::erase_if(n->customEffect->parameters,
                [](const auto& p) { return p.name == L"B"; });
            n->properties.erase(L"B");
            g.UnbindProperty(id, L"B");
            TEST("RemoveInput",
                n->properties.count(L"B") == 0 &&
                n->properties.count(L"A") == 1 && n->properties.count(L"C") == 1);
        }

        // JSON round-trip with custom inputs.
        {
            ShaderLab::Graph::EffectGraph g;
            auto node = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            // Add B and C, set values + expression.
            for (const wchar_t* name : { L"B", L"C" })
            {
                ShaderLab::Graph::ParameterDefinition pd;
                pd.name = name; pd.typeName = L"float"; pd.defaultValue = 0.0f;
                pd.minValue = -100000.0f; pd.maxValue = 100000.0f; pd.step = 0.1f;
                node.customEffect->parameters.push_back(std::move(pd));
                node.properties[name] = 0.0f;
            }
            node.properties[L"A"] = 1.0f;
            node.properties[L"B"] = 2.0f;
            node.properties[L"C"] = 3.0f;
            node.properties[L"Expression"] = std::wstring(L"A + B * C");
            auto id = g.AddNode(std::move(node));

            auto json = g.ToJson();
            auto g2 = ShaderLab::Graph::EffectGraph::FromJson(json);

            ShaderLab::Effects::SourceNodeFactory sf;
            Evaluate(g2, sf);
            auto* mn = g2.FindNode(id);
            float got = 0.0f;
            if (mn) {
                for (const auto& f : mn->analysisOutput.fields)
                    if (f.name == L"Result") { got = f.components[0]; break; }
            }
            TEST("JsonRoundTripInputsAndExpression",
                mn && std::abs(got - 7.0f) < 0.01f); // 1 + 2*3 = 7
        }
    }


    void TestClockNode()
    {
        printf("\n=== Clock Node ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();

        auto* desc = registry.FindByName(L"Clock");
        TEST("ClockExists", desc != nullptr);
        if (desc) {
            auto node = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            TEST("IsClock", node.isClock);
            TEST("HasAutoDuration", node.properties.count(L"AutoDuration") > 0);
            TEST("HasTimeOutput", !node.customEffect->analysisFields.empty() &&
                node.customEffect->analysisFields[0].name == L"Time");
        }
    }

    void TestShaderCompilation()
    {
        printf("\n=== Shader Compilation ===\n");

        // NOTE: the inline HLSL in this test is a COMPILE/CACHE-KEY fixture,
        // not a signature example. It is never rasterised, so its
        // two-parameter main() is harmless here -- but do not copy it. The
        // real contract is (SV_POSITION, SCENE_POSITION, TEXCOORD0..N), one
        // TEXCOORD per input; TestPixelShaderSignatures enforces it over the
        // shipped catalog.
        std::string validPS = R"(
Texture2D Source : register(t0);
float4 main(float4 pos : SV_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET
{ return Source.Load(int3(uv0.xy, 0)); }
)";
        auto result = ShaderLab::Effects::ShaderCompiler::CompileFromString(
            validPS, "test.hlsl", "main", "ps_5_0");
        TEST("ValidPixelShader", result.succeeded);

        auto bad = ShaderLab::Effects::ShaderCompiler::CompileFromString(
            "not hlsl!", "bad.hlsl", "main", "ps_5_0");
        TEST("InvalidShaderFails", !bad.succeeded);

        std::string validCS = R"(
RWTexture2D<float4> output : register(u0);
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) { output[id.xy] = float4(1,0,0,1); }
)";
        auto csResult = ShaderLab::Effects::ShaderCompiler::CompileFromString(
            validCS, "test.hlsl", "main", "cs_5_0");
        TEST("ValidComputeShader", csResult.succeeded);

        // ---- lane 3: analysis values as a GPU-resident texture --------------
        // A compute-analysis node publishes Result[] three ways; lane 3 is the
        // 1 x N RGBA32F texture, the only GPU-resident route a Direct2D PIXEL
        // shader can consume. D2D maps effect INPUTS to t0, t1, ... and will
        // not bind an arbitrary SRV, and CreateResourceTexture takes CPU bytes
        // -- so it would reintroduce the readback the lane exists to remove.
        //
        // Two properties worth pinning: the lane is OPT-IN (no request means
        // no texture, no copy shader, no dispatch), and when requested the
        // texels match the structured buffer exactly. A copy that quietly
        // reordered or dropped a field would surface far downstream as a
        // shader reading the wrong statistic.
        {
            using namespace ShaderLab::Rendering;
            D3D11ComputeRunner runner;
            runner.Initialize(g_d3dDevice.get());

            const char* kProducer = R"(
RWStructuredBuffer<float4> Result : register(u0);
cbuffer Constants : register(b0) { uint Width; uint Height; };
[numthreads(1,1,1)]
void main(uint3 t : SV_DispatchThreadID) {
    Result[0] = float4(11, 0, 0, 0);
    Result[1] = float4(22, 0, 0, 0);
    Result[2] = float4(33, 0, 0, 0);
    Result[3] = float4(44, 0, 0, 0);
}
)";
            bool compiled = runner.CompileShader(kProducer);
            TEST("Lane3_ProducerCompiles", compiled);

            if (compiled)
            {
                TEST("Lane3_OptInByDefault", !runner.AnalysisTextureRequested());
                winrt::com_ptr<ID3D11Texture2D> none;
                TEST("Lane3_NoTextureBeforeRequest",
                    runner.GetAnalysisTexture(none.put()) == E_NOT_VALID_STATE);

                TEST("Lane3_RequestSucceeds", runner.RequestAnalysisTexture() == S_OK);

                // Dispatch needs a real input at t0 -- it reads Width/Height
                // off it to auto-inject the constants, and bails on null.
                D3D11_TEXTURE2D_DESC id{};
                id.Width = 4; id.Height = 4; id.MipLevels = 1; id.ArraySize = 1;
                id.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                id.SampleDesc.Count = 1;
                id.Usage = D3D11_USAGE_DEFAULT;
                id.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                winrt::com_ptr<ID3D11Texture2D> inTex;
                g_d3dDevice->CreateTexture2D(&id, nullptr, inTex.put());

                // The readback here belongs to the TEST, not to the lane --
                // lane 3 itself never touches the CPU.
                std::vector<float> vals = runner.Dispatch(inTex.get(), {}, 4);
                TEST("Lane3_DispatchProducedFields", vals.size() >= 16);

                winrt::com_ptr<ID3D11Texture2D> tex;
                HRESULT hrTex = runner.GetAnalysisTexture(tex.put());
                TEST("Lane3_TextureAvailableAfterDispatch", hrTex == S_OK && tex);

                bool shapeOk = false, texelsOk = false;
                if (tex)
                {
                    D3D11_TEXTURE2D_DESC td{};
                    tex->GetDesc(&td);
                    shapeOk = (td.Height == 1 && td.Width == 4 &&
                               td.Format == DXGI_FORMAT_R32G32B32A32_FLOAT);

                    D3D11_TEXTURE2D_DESC sd = td;
                    sd.Usage = D3D11_USAGE_STAGING;
                    sd.BindFlags = 0;
                    sd.MiscFlags = 0;
                    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    winrt::com_ptr<ID3D11Texture2D> staging;
                    if (SUCCEEDED(g_d3dDevice->CreateTexture2D(&sd, nullptr, staging.put())))
                    {
                        g_d3dContext->CopyResource(staging.get(), tex.get());
                        D3D11_MAPPED_SUBRESOURCE m{};
                        if (SUCCEEDED(g_d3dContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m)))
                        {
                            auto* row = static_cast<const float*>(m.pData);
                            texelsOk = (row[0] == 11.0f && row[4] == 22.0f &&
                                        row[8] == 33.0f && row[12] == 44.0f);
                            g_d3dContext->Unmap(staging.get(), 0);
                        }
                    }
                }
                TEST("Lane3_TextureShape (1 row, one texel per float4 slot)", shapeOk);
                TEST("Lane3_TexelsMatchStructuredBuffer", texelsOk);

                // Lane 3 must not have disturbed lane 1 (SRV) or lane 2 (CPU).
                TEST("Lane3_DoesNotDisturbCpuValues",
                    vals.size() >= 16 && vals[0] == 11.0f && vals[12] == 44.0f);
                TEST("Lane3_DoesNotDisturbSrv", runner.GetResultSRV() != nullptr);
            }
        }

        // ---- shaderlab_params.hlsli include + macro mode validation ---------
        // Phase 8: a shader that uses SHADERLAB_PARAM should compile in
        // both cbuffer mode (_SLPARAM_X_GPU=0) and GPU-bound mode
        // (_SLPARAM_X_GPU=1). We don't reflect the resulting bytecode
        // here -- the goal is just to prove the include resolver +
        // macro injection wire all the way through D3DCompile without
        // a syntax error in the macro file itself.
        std::string macroPS = R"(
#include "shaderlab_params.hlsli"
Texture2D Source : register(t0);
SHADERLAB_GPU_BUFFER(Exposure, t1)
cbuffer Constants : register(b0) {
    SHADERLAB_PARAM(float, Exposure)
    float Padding;
};
float4 main(float4 pos : SV_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET {
    SHADERLAB_LOAD_PARAM(float, Exposure)
    return Source.Load(int3(uv0.xy, 0)) * Exposure;
}
)";
        // Default mode: parameter lives in cbuffer.
        auto cbufferMode = ShaderLab::Effects::ShaderCompiler::CompileFromString(
            macroPS, "test_param_cbuffer.hlsl", "main", "ps_5_0",
            { { "_SLPARAM_Exposure_GPU", "0" } });
        TEST("ShaderLabParamsHlsli_CbufferMode", cbufferMode.succeeded);

        // GPU-bound mode: parameter comes from StructuredBuffer<float4>.
        auto gpuMode = ShaderLab::Effects::ShaderCompiler::CompileFromString(
            macroPS, "test_param_gpu.hlsl", "main", "ps_5_0",
            { { "_SLPARAM_Exposure_GPU", "1" } });
        TEST("ShaderLabParamsHlsli_GpuMode", gpuMode.succeeded);

        // Texture-smuggled mode: the value arrives as a Texture2D effect
        // INPUT rather than a StructuredBuffer. This is the only GPU-resident
        // route available to a Direct2D PIXEL shader -- D2D maps a transform's
        // inputs to t0, t1, ... and will not bind an arbitrary SRV, while
        // ID2D1EffectContext::CreateResourceTexture takes `const BYTE*` CPU
        // memory and so would reintroduce the very readback the binding
        // exists to remove. A ps_5_0 compile is the meaningful check: the
        // StructuredBuffer form above compiles for cs_5_0 too, so it cannot
        // prove the pixel-shader path works.
        auto texMode = ShaderLab::Effects::ShaderCompiler::CompileFromString(
            macroPS, "test_param_tex.hlsl", "main", "ps_5_0",
            { { "_SLPARAM_Exposure_GPU", "2" } });
        TEST("ShaderLabParamsHlsli_TextureMode (pixel-shader GPU binding)",
            texMode.succeeded);

        // All three modes must compile from the SAME source text. If an
        // author had to write the shader differently per mode, the host could
        // not switch binding strategy without re-authoring the effect.
        TEST("ShaderLabParamsHlsli_AllModesShareOneSource",
            cbufferMode.succeeded && gpuMode.succeeded && texMode.succeeded);

        // The direct-access helpers must also compile in a pixel shader --
        // they are the escape hatch for an effect that wants a whole analysis
        // row rather than one parameter at a time.
        {
            std::string helperPS = R"(
#include "shaderlab_params.hlsli"
Texture2D Source : register(t0);
Texture2D<float4> Analysis : register(t1);
float4 main(float4 pos : SV_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET {
    float  maxNits = ShaderLabAnalysisField(Analysis, 1);
    float4 row     = ShaderLabAnalysisTexel(Analysis, 7);
    return Source.Load(int3(uv0.xy, 0)) * maxNits + row;
}
)";
            auto helperMode = ShaderLab::Effects::ShaderCompiler::CompileFromString(
                helperPS, "test_analysis_helpers.hlsl", "main", "ps_5_0", {});
            TEST("ShaderLabParamsHlsli_AnalysisHelpers", helperMode.succeeded);
        }

        // ---- ICtCp Gamut Boundary LUT: a generator the host can drive ------
        {
            using namespace ShaderLab::Effects;
            const auto* desc = ShaderLabEffects::Instance().FindById(L"ICtCp Gamut Boundary LUT");
            TEST("GamutLut_DescriptorExists", desc != nullptr);
            if (desc)
            {
                // A generator: no image inputs, an image output, and a group
                // small enough that the evaluator TILES the dispatch rather
                // than running it as one group (>= 256 threads means (1,1,1)).
                TEST("GamutLut_IsGenerator", desc->inputNames.empty() && desc->hasImageOutput);
                TEST("GamutLut_TiledDispatch",
                     desc->threadGroupX == 8 && desc->threadGroupY == 8 &&
                     desc->threadGroupX * desc->threadGroupY < 256);

                // The size is fixed because the reader indexes with the same
                // constants; it has to be in hiddenDefaults for the evaluator's
                // explicit-size path to allocate the output at all.
                auto dim = [&](const wchar_t* key) -> uint32_t {
                    auto it = desc->hiddenDefaults.find(key);
                    if (it == desc->hiddenDefaults.end()) return 0;
                    if (auto* u = std::get_if<uint32_t>(&it->second)) return *u;
                    return 0;
                };
                const uint32_t w = dim(L"OutputWidth"), h = dim(L"OutputHeight");
                TEST("GamutLut_FixedSizeHidden", w > 0 && h > 1);
                TEST("GamutLut_WidthIsPowerOfTwo", w > 0 && (w & (w - 1)) == 0);

                std::string src(desc->hlslSource.begin(), desc->hlslSource.end());
                // Geometry stamped into the HLSL must agree with the hidden size.
                TEST("GamutLut_HlslGeometryMatches",
                     src.find("#define GAMUT_LUT_W " + std::to_string(w)) != std::string::npos &&
                     src.find("#define GAMUT_LUT_H " + std::to_string(h)) != std::string::npos);
                // Same boundary search the tone mapper runs -- one copy.
                TEST("GamutLut_SharesBoundarySearch",
                     src.find("float CubeBoundaryRadius(") != std::string::npos);

                auto compiled = ShaderCompiler::CompileFromString(
                    src, "gamut_lut.hlsl", "main", "cs_5_0", {});
                if (!compiled.succeeded && compiled.errors)
                    printf("  [info] compile errors: %.900s\n",
                        static_cast<const char*>(compiled.errors->GetBufferPointer()));
                TEST("GamutLut_Compiles", compiled.succeeded);
                if (compiled.succeeded)
                {
                    auto refl = ShaderCompiler::Reflect(compiled.bytecode.get());
                    auto has = [&](const wchar_t* n) {
                        for (const auto& cb : refl.constantBuffers)
                            for (const auto& v : cb.variables)
                                if (v.name == n) return true;
                        return false;
                    };
                    // The host packs these by name: the runner's auto-injected
                    // source dims, the hidden size, and the one real parameter.
                    TEST("GamutLut_CbufferHasOutputDims", has(L"OutputWidth") && has(L"OutputHeight"));
                    TEST("GamutLut_CbufferHasSdrWhiteNits", has(L"SdrWhiteNits"));
                }
            }
        }

        // ---- ICtCp Gamut Map cbuffer layout (Soft Compress regression) -----
        // The Soft Compress params are the first cbuffer payload past byte
        // 64 in any ShaderLab effect. Compile the real registry descriptor
        // exactly as graph-load does and assert the reflected layout, so a
        // packing/reflection size bug can't silently zero them again.
        {
            using namespace ShaderLab::Effects;
            const auto* desc = ShaderLabEffects::Instance().FindById(L"ICtCp Gamut Map");
            TEST("ICtCpGamutMap_DescriptorExists", desc != nullptr);
            if (desc)
            {
                std::string src(desc->hlslSource.begin(), desc->hlslSource.end());
                auto compiled = ShaderCompiler::CompileFromString(
                    src, "ictcp_gamut_map.hlsl", "main", "ps_5_0");
                TEST("ICtCpGamutMap_Compiles", compiled.succeeded);
                if (compiled.succeeded)
                {
                    auto refl = ShaderCompiler::Reflect(compiled.bytecode.get());
                    TEST("ICtCpGamutMap_HasCbuffer", !refl.constantBuffers.empty());
                    if (!refl.constantBuffers.empty())
                    {
                        const auto& cb = refl.constantBuffers[0];
                        printf("  [info] cbuffer '%ls' sizeBytes=%u vars=%zu\n",
                            cb.name.c_str(), cb.sizeBytes, cb.variables.size());
                        auto findVar = [&](const wchar_t* n) -> const ShaderVariable* {
                            for (const auto& v : cb.variables)
                                if (v.name == n) return &v;
                            return nullptr;
                        };
                        const auto* st = findVar(L"SoftThreshold");
                        const auto* sl = findVar(L"SoftLimit");
                        const auto* kh = findVar(L"KneeHardness");
                        for (const auto& v : cb.variables)
                            printf("  [info] var %ls offset=%u size=%u\n",
                                v.name.c_str(), v.offset, v.size);
                        // The parameter block still ends at byte 80; the two
                        // host-built tables (derived constants) follow it:
                        // 64 + 1536 float4.
                        const auto* fit  = findVar(L"FitScaleLut");
                        const auto* poly = findVar(L"TgtPolyLut");
                        TEST("ICtCpGamutMap_FitScaleLutAt80", fit && fit->offset == 80 && fit->size == 64 * 16);
                        TEST("ICtCpGamutMap_TgtPolyLutFollows", poly && poly->offset == 80 + 64 * 16 && poly->size == 1536 * 16);
                        TEST("ICtCpGamutMap_CbufferSize", cb.sizeBytes == 80 + (64 + 1536) * 16);
                        TEST("ICtCpGamutMap_SoftThresholdAt64", st && st->offset == 64);
                        TEST("ICtCpGamutMap_SoftLimitAt68",     sl && sl->offset == 68);
                        TEST("ICtCpGamutMap_KneeHardnessAt72",  kh && kh->offset == 72);
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------------------
    // BytecodeCache (Phase 8)
    // ------------------------------------------------------------------------

    void TestBytecodeCache()
    {
        printf("\n=== BytecodeCache ===\n");
        using namespace ShaderLab::Effects;
        auto& cache = BytecodeCache::Instance();
        cache.Clear();

        // NOTE: the inline HLSL in this test is a COMPILE/CACHE-KEY fixture,
        // not a signature example. It is never rasterised, so its
        // two-parameter main() is harmless here -- but do not copy it. The
        // real contract is (SV_POSITION, SCENE_POSITION, TEXCOORD0..N), one
        // TEXCOORD per input; TestPixelShaderSignatures enforces it over the
        // shipped catalog.
        std::string hlsl = R"(
Texture2D Source : register(t0);
float4 main(float4 pos : SV_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET
{ return Source.Load(int3(uv0.xy, 0)); }
)";
        std::string canonical = CanonicalizeHlslSource(hlsl);

        BytecodeCompileRequest req;
        req.key.sourceHash         = HashCanonicalSource(canonical);
        req.key.paramSignatureHash = HashParamSignature({});
        req.key.includeLibraryHash = IncludeLibraryHash();
        req.key.macroBitset        = 0;
        req.key.entryPoint         = "main";
        req.key.target             = "ps_5_0";
        req.metadata.effectId      = L"BytecodeCacheTest";
        req.metadata.version       = 1;
        req.hlslSource             = canonical;

        // First call -> miss -> inline compile -> Ready.
        auto r1 = cache.GetOrCompile(req);
        TEST("BytecodeCache_FirstCompileReady", r1.status == BytecodeStatus::Ready);
        TEST("BytecodeCache_FirstCompileBytecodeNonEmpty", !r1.bytecode.empty());
        TEST("BytecodeCache_FirstCompileNotFromCache", !r1.fromCache);

        // Second call with identical key -> cache hit, identical bytecode.
        auto r2 = cache.GetOrCompile(req);
        TEST("BytecodeCache_SecondCompileReady", r2.status == BytecodeStatus::Ready);
        TEST("BytecodeCache_SecondCompileFromCache", r2.fromCache);
        TEST("BytecodeCache_SecondCompileBytecodeMatches",
             r2.bytecode.size() == r1.bytecode.size() &&
             memcmp(r2.bytecode.data(), r1.bytecode.data(), r1.bytecode.size()) == 0);

        // Failed entry caches its diagnostic and doesn't recompile.
        BytecodeCompileRequest bad = req;
        bad.hlslSource             = "broken!";
        bad.key.sourceHash         = HashCanonicalSource(bad.hlslSource);
        auto bad1 = cache.GetOrCompile(bad);
        TEST("BytecodeCache_BadCompileFailed", bad1.status == BytecodeStatus::Failed);
        TEST("BytecodeCache_BadCompileHasDiagnostic", !bad1.errorMessage.empty());

        auto bad2 = cache.GetOrCompile(bad);
        TEST("BytecodeCache_BadCompileFromCacheOnRetry", bad2.fromCache);

        // Macro bitset participates in identity: same source + different
        // macro bits should produce a fresh entry, not reuse the
        // baseline.
        std::string macroPS = R"(
#include "shaderlab_params.hlsli"
Texture2D Source : register(t0);
SHADERLAB_GPU_BUFFER(Exposure, t1)
cbuffer Constants : register(b0) {
    SHADERLAB_PARAM(float, Exposure)
    float Padding;
};
float4 main(float4 pos : SV_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET {
    SHADERLAB_LOAD_PARAM(float, Exposure)
    return Source.Load(int3(uv0.xy, 0)) * Exposure;
}
)";
        std::string macroCanonical = CanonicalizeHlslSource(macroPS);
        BytecodeCompileRequest mBase;
        mBase.key.sourceHash         = HashCanonicalSource(macroCanonical);
        mBase.key.paramSignatureHash = HashParamSignature({ "Exposure" });
        mBase.key.includeLibraryHash = IncludeLibraryHash();
        mBase.key.macroBitset        = 0;
        mBase.key.entryPoint         = "main";
        mBase.key.target             = "ps_5_0";
        mBase.hlslSource             = macroCanonical;
        mBase.gpuBindableParamNames  = { "Exposure" };

        auto cb = cache.GetOrCompile(mBase);
        TEST("BytecodeCache_MacroCbufferOK", cb.status == BytecodeStatus::Ready);

        BytecodeCompileRequest mGpu = mBase;
        mGpu.key.macroBitset = 1;
        auto gp = cache.GetOrCompile(mGpu);
        TEST("BytecodeCache_MacroGpuOK", gp.status == BytecodeStatus::Ready);
        TEST("BytecodeCache_MacroVariantsDistinct",
             cb.bytecode.size() != gp.bytecode.size() ||
             memcmp(cb.bytecode.data(), gp.bytecode.data(), cb.bytecode.size()) != 0);

        // PrecompileCommonShapes enqueues N+1 variants; verify they
        // become Ready after a brief wait.
        cache.Clear();
        BytecodeCacheMetadata meta;
        meta.effectId = L"PrecompileTest";
        meta.version  = 1;
        cache.PrecompileCommonShapes(meta, macroCanonical, "main", "ps_5_0", { "Exposure" });

        // Wait up to 5s for both variants. Drives both the worker
        // queue and (via timeoutMs) the inline-fallback path.
        auto waitFor = [&](uint32_t bitset)
        {
            BytecodeCompileRequest r;
            r.key.sourceHash         = HashCanonicalSource(macroCanonical);
            r.key.paramSignatureHash = HashParamSignature({ "Exposure" });
            r.key.includeLibraryHash = IncludeLibraryHash();
            r.key.macroBitset        = bitset;
            r.key.entryPoint         = "main";
            r.key.target             = "ps_5_0";
            r.hlslSource             = macroCanonical;
            r.gpuBindableParamNames  = { "Exposure" };
            return cache.GetOrCompile(r, 5000);
        };
        auto wb = waitFor(0);
        auto wg = waitFor(1);
        TEST("BytecodeCache_PrecompileBaselineReady", wb.status == BytecodeStatus::Ready);
        TEST("BytecodeCache_PrecompileGpuVariantReady", wg.status == BytecodeStatus::Ready);

        cache.Clear();

        // ---- Disk persistence (Phase 8 p8-cache-disk) -------------------
        // Configure a temp disk root, compile, verify file written,
        // then clear in-memory and verify GetOrCompile re-reads from
        // disk (not from a fresh D3DCompile).
        wchar_t tmpPath[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tmpPath);
        std::wstring diskRoot = std::wstring(tmpPath) + L"shaderlab_bytecode_cache_test";
        cache.SetDiskCacheRoot(diskRoot);
        // Wipe any leftover state from a previous test run -- we want
        // a known-empty disk + memory state for the assertions below.
        cache.ClearDisk();
        cache.Clear();

        BytecodeCompileRequest diskReq;
        diskReq.key.sourceHash         = HashCanonicalSource(canonical);
        diskReq.key.paramSignatureHash = HashParamSignature({});
        diskReq.key.includeLibraryHash = IncludeLibraryHash();
        diskReq.key.macroBitset        = 0;
        diskReq.key.entryPoint         = "main";
        diskReq.key.target             = "ps_5_0";
        diskReq.metadata.effectId      = L"DiskPersistTest";
        diskReq.metadata.version       = 1;
        diskReq.hlslSource             = canonical;

        auto d1 = cache.GetOrCompile(diskReq);
        TEST("BytecodeCache_DiskCompileReady", d1.status == BytecodeStatus::Ready);
        TEST("BytecodeCache_DiskCompileNotFromCache", !d1.fromCache);

        // Verify a .cso file landed under the expected hierarchy.
        std::wstring expectedDir = diskRoot + L"\\DiskPersistTest\\1";
        WIN32_FIND_DATAW fd{};
        HANDLE h = ::FindFirstFileW((expectedDir + L"\\*.cso").c_str(), &fd);
        bool csoExists = (h != INVALID_HANDLE_VALUE);
        if (csoExists) ::FindClose(h);
        TEST("BytecodeCache_DiskFileWritten", csoExists);

        // Clear the in-memory cache; subsequent GetOrCompile must
        // hydrate from disk, not recompile.
        cache.Clear();
        size_t inlineBefore = cache.GetStats().inlineCompiles;
        auto d2 = cache.GetOrCompile(diskReq);
        size_t inlineAfter = cache.GetStats().inlineCompiles;
        TEST("BytecodeCache_DiskRehydrateReady", d2.status == BytecodeStatus::Ready);
        TEST("BytecodeCache_DiskRehydrateFromCache", d2.fromCache);
        TEST("BytecodeCache_DiskRehydrateNoNewCompile", inlineAfter == inlineBefore);
        TEST("BytecodeCache_DiskRehydrateBytecodeMatches",
             d2.bytecode.size() == d1.bytecode.size() &&
             memcmp(d2.bytecode.data(), d1.bytecode.data(), d1.bytecode.size()) == 0);

        // Reaper: ClearDisk wipes everything.
        auto reapStats = cache.ClearDisk();
        TEST("BytecodeCache_ClearDiskFreedAtLeastOne", reapStats.filesDeleted >= 1);

        // Cleanup: disable disk cache so subsequent tests don't pollute.
        cache.SetDiskCacheRoot(L"");
        cache.Clear();
    }

    void TestEffectChain()
    {
        printf("\n=== Effect Chain ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto& d2dReg = ShaderLab::Effects::EffectRegistry::Instance();

        ShaderLab::Graph::EffectGraph g;
        ShaderLab::Effects::SourceNodeFactory sf;

        auto srcNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source"));
        auto blurNode = d2dReg.CreateNode(*d2dReg.FindByName(L"Gaussian Blur"));
        auto invertNode = d2dReg.CreateNode(*d2dReg.FindByName(L"Invert"));

        auto srcId = g.AddNode(std::move(srcNode));
        auto blurId = g.AddNode(std::move(blurNode));
        auto invertId = g.AddNode(std::move(invertNode));
        g.Connect(srcId, 0, blurId, 0);
        g.Connect(blurId, 0, invertId, 0);

        Evaluate(g, sf);
        auto* inv = g.FindNode(invertId);
        TEST("ThreeNodeChain", inv && HasOutput(*inv));
    }

    // Direct exercise of Luminance Statistics (D3D11 compute path) so we can
    // tell whether a Mean=0 readback is a Luminance Statistics-specific bug
    // or something the headless host introduces.
    void TestLuminanceStatistics()
    {
        printf("\n=== Luminance Statistics (D3D11 compute) ===\n");
        // g_evaluator is a global; node IDs (1, 2, ...) collide with
        // prior tests' graphs, so flush its caches before building a
        // fresh graph or CreateOrGetEffect will cache-hit a stale
        // effect from a different test.
        g_evaluator.ReleaseCache();
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto* lumDesc = registry.FindByName(L"Luminance Statistics");
        if (!lumDesc) { TEST("LumStatsDescriptorFound", false); return; }

        ShaderLab::Graph::EffectGraph g;
        ShaderLab::Effects::SourceNodeFactory sf;
        auto srcNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *registry.FindByName(L"Gamut Source"));
        auto statsNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*lumDesc);
        auto srcId = g.AddNode(std::move(srcNode));
        auto statsId = g.AddNode(std::move(statsNode));
        g.Connect(srcId, 0, statsId, 0);
        Evaluate(g, sf);
        // Wrap ProcessDeferredCompute in a BeginDraw/EndDraw because
        // DispatchUserD3D11Compute calls dc->DrawImage internally, and
        // outside an active draw session that DrawImage is a silent
        // no-op (the compute shader reads a black input texture).
        // Mirrors MainWindow::RenderFrame's structure.
        // D2D custom effects need TWO evaluation passes for newly
        // created effects -- first creates/initializes, second
        // produces correct output.
        g_dc->SetTarget(nullptr);
        g_dc->BeginDraw();
        g_evaluator.ProcessDeferredCompute(g, g_dc.get());
        g_dc->EndDraw();
        Evaluate(g, sf);
        g_dc->BeginDraw();
        g_evaluator.ProcessDeferredCompute(g, g_dc.get());
        g_dc->EndDraw();

        auto* n = g.FindNode(statsId);
        TEST("LumStats_NodeExists", n != nullptr);
        if (!n) return;
        TEST("LumStats_AnalysisFieldsPopulated", !n->analysisOutput.fields.empty());

        float mean = 0.0f, maxVal = 0.0f;
        for (const auto& f : n->analysisOutput.fields) {
            if (f.name == L"Mean") mean = f.components[0];
            if (f.name == L"Max")  maxVal = f.components[0];
        }
        // Gamut Source at Luminance=80 covers part of the 64x64 image with
        // gamut-shaped fill. Max should reach ~80 nits; Mean should be a
        // small but positive fraction of that. Both being 0 is the
        // "DrawImage outside draw session" failure mode.
        TEST("LumStats_MaxIsPositive", maxVal > 0.0f);
        TEST("LumStats_MeanIsPositive", mean > 0.0f);
        if (mean == 0.0f || maxVal == 0.0f) {
            printf("    (debug) Mean=%.4f Max=%.4f -- D3D11 dispatch likely not reading source\n",
                mean, maxVal);
        }
    }

    // ----- Phase 8: GPU-binding routing through the bridge -----------------
    //
    // Verifies the feature-flag-on path:
    //   Gamut Source -> Luminance Statistics -> ICtCp Tone Map (D3D11
    //   compute, post-migration) with TargetPeakNits bound to Mean.
    // Asserts:
    //   1. ICtCp produces a non-trivial output (> 0 pixels).
    //   2. Performance::GpuBindingDetections() increments.
    //   3. With the flag off, the same chain still works (CPU readback).
    //   4. With the flag on but no upstream binding, ICtCp still works
    //      (graceful baseline path).
    // A node BOUND to an analysis node must see that node's new values in
    // the same frame. Nothing reaches it by edge -- Luminance Statistics has
    // no image output -- so before the fix it evaluated with whatever the
    // binding held before the dispatch, and on a static graph kept it.
    void TestVideoExportHelpers()
    {
        printf("\n=== Video export helpers ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto* clockDesc = registry.FindByName(L"Clock");
        if (!clockDesc) { TEST("VideoExport_ClockDescriptor", false); return; }

        ShaderLab::Graph::EffectGraph g;
        auto cid = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*clockDesc));
        auto setP = [&](const wchar_t* k, float v) { g.FindNode(cid)->properties[k] = v; };
        auto timeAt = [&](double t) { ShaderLab::Rendering::SetClocksToTime(g, t); return g.FindNode(cid)->clockTime; };

        // Defaults: Start 0, Stop 10, Speed 1, Loop on.
        TEST("VideoExport_ClockLoopWraps", std::abs(timeAt(12.5) - 2.5) < 1e-9);
        setP(L"Loop", 0.0f);
        TEST("VideoExport_ClockNoLoopClamps", std::abs(timeAt(12.0) - 10.0) < 1e-9);
        setP(L"Loop", 1.0f); setP(L"Speed", 2.0f);
        TEST("VideoExport_ClockSpeedScales", std::abs(timeAt(3.0) - 6.0) < 1e-9 && std::abs(timeAt(6.0) - 2.0) < 1e-9);
        TEST("VideoExport_LongestClockUsesSpeed", std::abs(ShaderLab::Rendering::LongestClockDuration(g) - 5.0) < 1e-9);
        setP(L"Speed", -1.0f);
        TEST("VideoExport_ClockNegativeSpeedLoops", std::abs(timeAt(2.5) - 7.5) < 1e-9);

        // Regression: a graph LOADED from JSON must have working Clocks.
        // isClock is not serialized, and only the GUI's file-open path used to
        // re-derive it -- headless, MCP /graph/load and the adapter-switch
        // reload all produced Clocks that never advanced.
        auto loaded = ShaderLab::Graph::EffectGraph::FromJson(g.ToJson());
        ShaderLab::Effects::ShaderLabEffects::RestoreRuntimeFlags(loaded);
        bool clockFlagged = false;
        for (const auto& n : loaded.Nodes()) if (n.isClock) clockFlagged = true;
        TEST("VideoExport_LoadedClockIsClock", clockFlagged);
        TEST("VideoExport_LoadedClockSteps", ShaderLab::Rendering::SetClocksToTime(loaded, 1.0) == 1);

        std::wstring err;
        auto path = ShaderLab::Rendering::FindFfmpeg(L"C:\\definitely\\not\\here\\ffmpeg.exe", err);
        TEST("VideoExport_MissingFfmpegFailsWithInstructions",
             path.empty() && err.find(L"not found") != std::wstring::npos && err.find(L"PATH") != std::wstring::npos);
    }

    // Tests\fixtures\<name>, found by walking up from the test executable
    // (<Platform>\<Config>\ShaderLabTests\ShaderLabTests.exe). Empty if absent.
    std::wstring FindFixture(const wchar_t* name)
    {
        wchar_t buf[MAX_PATH]{};
        ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
        std::filesystem::path dir = std::filesystem::path(buf).parent_path();
        for (int i = 0; i < 6 && !dir.empty(); ++i)
        {
            auto p = dir / L"Tests" / L"fixtures" / name;
            if (std::filesystem::exists(p)) return p.wstring();
            dir = dir.parent_path();
        }
        return {};
    }

    // Video source: a frame-accurate seek, no re-seek once satisfied, and the
    // zero-copy (hardware decode) upload producing exactly what the CPU
    // Lock2D path does. On WARP there is no hardware decoder, so both
    // providers take the CPU path and the comparison is trivially equal --
    // the zero-copy half needs a hardware run (--adapter default).
    void TestVideoSourceSeekAndZeroCopy()
    {
        printf("\n=== Video source: frame-accurate seek + zero-copy upload ===\n");
        struct Clip { const wchar_t* file; double frame; const char* tag; };
        const Clip clips[] = {
            { L"video_hdr10_hevc_30f.mp4", 17.5, "HEVC Main10 (P010)" },
            { L"video_sdr_h264_30f.mp4",   17.5, "H.264 High, B-frames (NV12)" },
        };
        auto readback = [&](ID3D11Texture2D* tex, std::vector<uint8_t>& out) -> bool
        {
            if (!tex) return false;
            D3D11_TEXTURE2D_DESC d{};
            tex->GetDesc(&d);
            d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.MiscFlags = 0;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            winrt::com_ptr<ID3D11Texture2D> staging;
            if (FAILED(g_d3dDevice->CreateTexture2D(&d, nullptr, staging.put()))) return false;
            g_d3dContext->CopyResource(staging.get(), tex);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(g_d3dContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m))) return false;
            const size_t row = static_cast<size_t>(d.Width) * 8;   // RGBA16F
            out.resize(row * d.Height);
            for (UINT y = 0; y < d.Height; ++y)
                std::memcpy(out.data() + y * row, static_cast<const uint8_t*>(m.pData) + y * m.RowPitch, row);
            g_d3dContext->Unmap(staging.get(), 0);
            return true;
        };
        for (const auto& clip : clips)
        {
            const std::wstring path = FindFixture(clip.file);
            if (path.empty())
            {
                // Generated, not checked in -- and CI has no ffmpeg. Neither a
                // pass nor a fail: say so loudly instead.
                printf("  [SKIP] %ls not found -- run Tests\\fixtures\\MakeVideoFixtures.ps1\n", clip.file);
                continue;
            }

            ShaderLab::Effects::VideoSourceProvider zc, cpu;
            cpu.SetZeroCopyEnabled(false);
            const bool opened =
                zc.Open(path, g_dc.get(), g_d3dDevice.get(), g_d3dContext.get()) &&
                cpu.Open(path, g_dc.get(), g_d3dDevice.get(), g_d3dContext.get());
            TEST("Video_Opens", opened);
            if (!opened) continue;

            const double fps = zc.FrameRate() > 0 ? zc.FrameRate() : 30.0;
            const double target = clip.frame / fps;   // mid-frame, far from the only keyframe
            // Open decodes the first frame synchronously; consume it so the
            // upload below is the seek's own frame.
            zc.UploadIfReady(g_dc.get());
            cpu.UploadIfReady(g_dc.get());
            const uint64_t dz = zc.DecodeCount(), dcpu = cpu.DecodeCount();
            bool uploadedZc = false, uploadedCpu = false;
            zc.Seek(target);
            cpu.Seek(target);
            for (int i = 0; i < 600 && !(uploadedZc && uploadedCpu); ++i)
            {
                if (!uploadedZc && zc.DecodeCount() > dz)    uploadedZc  = zc.UploadIfReady(g_dc.get());
                if (!uploadedCpu && cpu.DecodeCount() > dcpu) uploadedCpu = cpu.UploadIfReady(g_dc.get());
                if (!(uploadedZc && uploadedCpu)) ::Sleep(5);
            }
            TEST("Video_SeekDeliversAFrame", uploadedZc && uploadedCpu);
            if (!(uploadedZc && uploadedCpu))
                printf("    (debug) decodes zc %llu->%llu cpu %llu->%llu, err '%ls'\n",
                    (unsigned long long)dz, (unsigned long long)zc.DecodeCount(),
                    (unsigned long long)dcpu, (unsigned long long)cpu.DecodeCount(),
                    zc.LastError().c_str());

            // The frame shown must be the one covering the target, not the
            // keyframe MF lands on.
            const double pos = zc.CurrentPosition();
            const double expected = std::floor(clip.frame) / fps;
            printf("  [info] %s: %.2f fps, target %.4f s -> frame at %.4f s (expected %.4f), zero-copy %s\n",
                clip.tag, fps, target, pos, expected, zc.LastUploadWasZeroCopy() ? "yes" : "no");
            TEST("Video_SeekIsFrameAccurate", std::abs(pos - expected) < 0.25 / fps);
            TEST("Video_SeekTargetRemembered", zc.LastSeekTarget() == target);

            std::vector<uint8_t> a, b;
            const bool readA = readback(zc.OutputTexture(), a);
            const bool readB = readback(cpu.OutputTexture(), b);
            TEST("Video_ZeroCopyMatchesCpuPath", readA && readB && a == b);
            if (readA && readB && a != b && a.size() == b.size())
            {
                const size_t row = static_cast<size_t>(zc.FrameWidth()) * 8;
                size_t firstRow = SIZE_MAX, rowsDiff = 0;
                for (size_t y = 0; y * row < a.size(); ++y)
                    if (std::memcmp(a.data() + y * row, b.data() + y * row, row) != 0)
                    { ++rowsDiff; if (firstRow == SIZE_MAX) firstRow = y; }
                printf("    (debug) %ux%u: %zu rows differ, first %zu\n",
                    zc.FrameWidth(), zc.FrameHeight(), rowsDiff, firstRow);
            }
            // Informational: decode + upload wall time per frame, both paths.
            auto timeFrames = [&](ShaderLab::Effects::VideoSourceProvider& p, int frames) -> double
            {
                p.Seek(0.0);
                auto t0 = std::chrono::steady_clock::now();
                int got = 0;
                while (got < frames &&
                       std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20))
                {
                    if (p.UploadIfReady(g_dc.get())) { ++got; p.RequestNextFrame(); }
                    else std::this_thread::yield();
                }
                g_d3dContext->Flush();
                const double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                return got ? ms / got : 0.0;
            };
            const double msZc = timeFrames(zc, 20);
            const double msCpu = timeFrames(cpu, 20);
            printf("  [info] %s %ux%u: %.2f ms/frame zero-copy vs %.2f ms/frame CPU path\n",
                clip.tag, zc.FrameWidth(), zc.FrameHeight(), msZc, msCpu);
            zc.Close();
            cpu.Close();
        }
    }

    // Regressions for the 2026-09 performance pass. Each pins a behaviour a
    // fix introduced, so a later change cannot quietly undo it.
    void TestPerfPassRegressions()
    {
        printf("\n=== Performance pass regressions ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto& d2dReg   = ShaderLab::Effects::EffectRegistry::Instance();

        // One frame in the shape every host uses.
        auto frame = [&](ShaderLab::Graph::EffectGraph& g)
        {
            g_dc->SetTarget(nullptr);
            g_evaluator.Evaluate(g, g_dc.get());
            if (g.HasDirtyNodes())
                g_evaluator.Evaluate(g, g_dc.get());
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            if (g.HasDirtyNodes())
            {
                g_evaluator.SetDeferredComputeFrozen(true);
                g_evaluator.Evaluate(g, g_dc.get());
                g_evaluator.SetDeferredComputeFrozen(false);
            }
            g_dc->EndDraw();
        };
        auto field = [](const ShaderLab::Graph::EffectNode* n, const wchar_t* name) -> float
        {
            if (!n) return -1.0f;
            for (const auto& f : n->analysisOutput.fields)
                if (f.name == name) return f.components[0];
            return -1.0f;
        };

        // (1) The reduction contract is written twice -- C++ constants and
        // the HLSL defines -- and must agree or groups go missing silently.
        {
            const std::string lib(ShaderLab::Effects::GetShaderLabParamsHLSL());
            auto defineValue = [&](const std::string& name) -> long long
            {
                const std::string key = "#define " + name + " ";
                auto p = lib.find(key);
                if (p == std::string::npos) return -1;
                return std::strtoll(lib.c_str() + p + key.size(), nullptr, 10);
            };
            TEST("Perf_ReduceGroupsMatchHlsl",
                 defineValue("SHADERLAB_REDUCE_GROUPS") == ShaderLab::Effects::kReduceGroups);
            TEST("Perf_ReduceScratchMatchHlsl",
                 defineValue("SHADERLAB_REDUCE_SCRATCH_UINTS") == ShaderLab::Effects::kReduceScratchUints);
            TEST("Perf_ReduceClearedMatchHlsl",
                 defineValue("SHADERLAB_REDUCE_CLEARED") == ShaderLab::Effects::kReduceScratchClearedUints);
        }

        // (2) Multi-group statistics count every pixel exactly once, including
        // an image wider than one group's 1024 threads and with FEWER rows
        // than there are groups (most groups get no rows at all).
        {
            g_evaluator.ReleaseCache();
            ShaderLab::Graph::EffectGraph g;
            auto gradId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"Gradient Generator")));
            auto cropId = g.AddNode(d2dReg.CreateNode(*d2dReg.FindByName(L"Crop")));
            auto statsId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"Luminance Statistics")));
            g.FindNode(gradId)->properties[L"GradSize"] = 2048.0f;
            g.FindNode(cropId)->properties[L"Rect"] =
                winrt::Windows::Foundation::Numerics::float4{ 0.0f, 0.0f, 2048.0f, 37.0f };
            g.Connect(gradId, 0, cropId, 0);
            g.Connect(cropId, 0, statsId, 0);
            g.MarkAllDirty();
            frame(g);
            g.MarkAllDirty();
            frame(g);
            auto* sn = g.FindNode(statsId);
            const float samples = field(sn, L"Samples");
            const float mx = field(sn, L"Max");
            const float mean = field(sn, L"Mean");
            TEST("Perf_MultiGroupStatsCountEveryPixelOnce", samples == 2048.0f * 37.0f);
            TEST("Perf_MultiGroupStatsMaxIsGradientEnd", mx > 79.0f && mx <= 80.01f);
            TEST("Perf_MultiGroupStatsMeanIsGradientMidpoint", std::abs(mean - 40.0f) < 0.5f);
            if (samples != 2048.0f * 37.0f || std::abs(mean - 40.0f) >= 0.5f)
                printf("    (debug) Samples=%.0f Max=%.4f Mean=%.4f\n", samples, mx, mean);
        }

        // (3) An unbounded input (Flood) into a compute node is refused with a
        // message, instead of an 8192^2 FP32 (1 GiB) pre-render.
        {
            g_evaluator.ReleaseCache();
            ShaderLab::Effects::SourceNodeFactory sf;
            ShaderLab::Graph::EffectGraph g;
            auto floodId = g.AddNode(ShaderLab::Effects::SourceNodeFactory::CreateFloodSourceNode(
                winrt::Windows::Foundation::Numerics::float4{ 1.0f, 1.0f, 1.0f, 1.0f }));
            auto statsId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"Luminance Statistics")));
            g.Connect(floodId, 0, statsId, 0);
            for (auto& n : const_cast<std::vector<ShaderLab::Graph::EffectNode>&>(g.Nodes()))
                if (n.type == ShaderLab::Graph::NodeType::Source)
                    sf.PrepareSourceNode(n, g_dc.get(), 0.0, g_d3dDevice.get(), g_d3dContext.get());
            g.MarkAllDirty();
            frame(g);
            auto* sn = g.FindNode(statsId);
            TEST("Perf_UnboundedComputeInputIsRefused",
                 sn && sn->runtimeError.find(L"unbounded") != std::wstring::npos);
        }

        // (4) A compute consumer bound to a compute producer dispatches ONCE
        // per cycle even when the host evaluates twice. The queue flag is
        // sticky until ProcessDeferredCompute, so the second pass used to
        // re-queue the consumer: 3 dispatches for 2 nodes.
        {
            g_evaluator.ReleaseCache();
            const bool prevGpu = ShaderLab::Performance::IsGpuBindingsEnabled();
            ShaderLab::Performance::SetGpuBindingsEnabled(true);
            ShaderLab::Graph::EffectGraph g;
            auto srcId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"Gamut Source")));
            auto statsId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"Luminance Statistics")));
            auto tmId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"ICtCp Tone Map (HDR -> SDR)")));
            g.Connect(srcId, 0, statsId, 0);
            g.Connect(srcId, 0, tmId, 0);
            g.BindProperty(tmId, L"SourcePeakNits", statsId, L"Max", 0);
            for (int i = 0; i < 3; ++i) { g.MarkAllDirty(); frame(g); }

            g.MarkAllDirty();
            g_dc->SetTarget(nullptr);
            g_evaluator.Evaluate(g, g_dc.get());
            g_evaluator.Evaluate(g, g_dc.get());   // unconditional, like the old headless host
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();
            TEST("Perf_BoundComputeConsumerDispatchedOnce", g_evaluator.DispatchesLastFrame() == 2);
            if (g_evaluator.DispatchesLastFrame() != 2)
                printf("    (debug) dispatches=%u\n", g_evaluator.DispatchesLastFrame());
            ShaderLab::Performance::SetGpuBindingsEnabled(prevGpu);
        }

        // (4b) Targeted dirtying replaced MarkAllDirty at every edit site, so
        // the graph primitives must dirty exactly what they change, and a
        // change made while a node is not needed must survive until it is.
        {
            ShaderLab::Graph::EffectGraph g;
            auto mk = [&](const wchar_t* n) {
                return g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(n)));
            };
            auto a = mk(L"Gamut Source"), b = mk(L"Gamut Source");
            auto sa = mk(L"Luminance Statistics"), sb = mk(L"Luminance Statistics");
            auto tm = mk(L"ICtCp Round-Trip Validator");
            g.Connect(a, 0, sa, 0);
            g.Connect(b, 0, sb, 0);
            g.Connect(a, 0, tm, 0);
            auto clean = [&] { for (auto& n : const_cast<std::vector<ShaderLab::Graph::EffectNode>&>(g.Nodes())) n.dirty = false; };
            auto dirty = [&](uint32_t id) { return g.FindNode(id)->dirty; };

            clean();
            g.BindProperty(tm, L"Gain", sa, L"Max", 0);
            TEST("Dirty_BindMarksOnlyConsumer", dirty(tm) && !dirty(a) && !dirty(b) && !dirty(sa) && !dirty(sb));

            clean();
            g.RemoveNode(sa);   // consumed by tm through the binding
            TEST("Dirty_RemoveNodeDirtiesBindingConsumer", dirty(tm) && !dirty(b) && !dirty(sb));

            clean();
            g.RemoveNode(a);    // consumed by tm through an edge
            TEST("Dirty_RemoveNodeDirtiesEdgeConsumer", dirty(tm) && !dirty(b) && !dirty(sb));

            clean();
            g.FindNode(sb)->needed = false;
            g.FindNode(sb)->dirty = true;
            TEST("Dirty_UnneededDoesNotCountAsDirty", !g.HasDirtyNodes());
            g_evaluator.ReleaseCache();
            g_dc->SetTarget(nullptr);
            g_evaluator.Evaluate(g, g_dc.get());
            TEST("Dirty_UnneededKeepsPendingChange", dirty(sb));
            g.FindNode(sb)->needed = true;
            TEST("Dirty_PendingChangeSurfacesWhenNeeded", g.HasDirtyNodes());
            g_evaluator.ReleaseCache();
        }

        // (5) Async readback: the values land a frame or two later, equal to
        // what a blocking read returns, and the evaluator reports the pending
        // copy so a host keeps rendering until it does.
        {
            auto build = [&](ShaderLab::Graph::EffectGraph& g) -> uint32_t
            {
                auto srcId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
                    *registry.FindByName(L"Gamut Source")));
                auto statsId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
                    *registry.FindByName(L"Luminance Statistics")));
                g.Connect(srcId, 0, statsId, 0);
                return statsId;
            };

            g_evaluator.ReleaseCache();
            ShaderLab::Graph::EffectGraph gb;
            auto sb = build(gb);
            for (int i = 0; i < 2; ++i) { gb.MarkAllDirty(); frame(gb); }
            const float blockingMax = field(gb.FindNode(sb), L"Max");

            g_evaluator.ReleaseCache();
            ShaderLab::Graph::EffectGraph ga;
            auto sa = build(ga);
            ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(true);
            ShaderLab::Performance::SetAsyncAnalysisReadbackEnabled(true);
            g_evaluator.SetCpuAnalysisInterest({ sa });
            for (int i = 0; i < 2; ++i) { ga.MarkAllDirty(); frame(ga); }
            // Let pending copies land: frames with nothing dirty still poll.
            bool sawPending = g_evaluator.HasPendingReadbacks();
            for (int i = 0; i < 200 && g_evaluator.HasPendingReadbacks(); ++i)
            {
                ::Sleep(1);
                frame(ga);
            }
            const float asyncMax = field(ga.FindNode(sa), L"Max");
            TEST("Perf_AsyncReadbackWasQueued", sawPending);
            TEST("Perf_AsyncReadbackDrains", !g_evaluator.HasPendingReadbacks());
            TEST("Perf_AsyncReadbackMatchesBlocking",
                 blockingMax > 0.0f && asyncMax == blockingMax);
            if (asyncMax != blockingMax)
                printf("    (debug) blocking Max=%.4f async Max=%.4f\n", blockingMax, asyncMax);
            ShaderLab::Performance::SetAsyncAnalysisReadbackEnabled(false);
            ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(false);
            g_evaluator.ClearCpuAnalysisInterest();
        }
        g_evaluator.ReleaseCache();
    }

    // Lane 3, consumer side, end to end: an analysis value reaching a PIXEL
    // shader as a GPU texture. No public built-in pixel shader has a
    // gpu-bindable parameter, so this defines a tiny one here. It also
    // declares a lookup input, pinning the reserved-pin layout the evaluator
    // and the shader must agree on:
    //     pin 0  Source (image)   pin 1  Table (lookup)   pin 2  Level (gpu-bindable)
    // The shader outputs Level in every channel, so the pixel IS the bound value.
    void TestLane3PixelConsumer()
    {
        printf("\n=== Lane 3: analysis value into a pixel shader as a texture ===\n");
        g_evaluator.ReleaseCache();
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();

        ShaderLab::Effects::ShaderLabEffectDescriptor d;
        d.name = L"Lane3 Test Consumer";
        d.effectId = L"Lane3 Test Consumer"; d.effectVersion = 1;
        d.shaderType = ShaderLab::Graph::CustomShaderType::PixelShader;
        d.inputNames = { L"Source", L"Table" };
        d.lookupInputCount = 1;
        d.hlslSource = R"HLSL(
#include "shaderlab_params.hlsli"
Texture2D Source : register(t0);
Texture2D Table  : register(t1);
SHADERLAB_GPU_BUFFER(Level, t2)
SamplerState InputSampler : register(s0);
cbuffer Constants : register(b0)
{
    SHADERLAB_PARAM(float, Level)
};
float4 main(float4 pos : SV_POSITION, float4 scenePos : SCENE_POSITION,
            float4 uv0 : TEXCOORD0, float4 uv1 : TEXCOORD1, float4 uv2 : TEXCOORD2) : SV_TARGET
{
    SHADERLAB_LOAD_PARAM(float, Level)
    float a = Source.Sample(InputSampler, uv0.xy).a;
    return float4(Level, Level, Level, a);
}
)HLSL";
        d.parameters = {
            ShaderLab::Graph::ParameterDefinition{ L"Level", L"float", 0.0f, 0.0f, 10000.0f, 1.0f, {}, L"", true },
        };

        auto run = [&](bool gpu) -> std::pair<float, float>
        {
            g_evaluator.ReleaseCache();
            ShaderLab::Performance::SetGpuBindingsEnabled(gpu);
            ShaderLab::Graph::EffectGraph g;
            auto src   = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source")));
            auto stats = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Luminance Statistics")));
            auto cons  = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(d));
            g.Connect(src, 0, stats, 0);
            g.Connect(src, 0, cons, 0);             // Table left unwired: the placeholder fills it
            g.BindProperty(cons, L"Level", stats, L"Max", 0);
            // Texture mode engages only once the producer has published its
            // texture, so give it a few frames, as a live graph would.
            for (int i = 0; i < 4; ++i)
            {
                g.MarkAllDirty();
                g_dc->SetTarget(nullptr);
                g_evaluator.Evaluate(g, g_dc.get());
                if (g.HasDirtyNodes()) g_evaluator.Evaluate(g, g_dc.get());
                g_dc->BeginDraw();
                g_evaluator.ProcessDeferredCompute(g, g_dc.get());
                if (g.HasDirtyNodes())
                {
                    g_evaluator.SetDeferredComputeFrozen(true);
                    g_evaluator.Evaluate(g, g_dc.get());
                    g_evaluator.SetDeferredComputeFrozen(false);
                }
                g_dc->EndDraw();
            }
            float maxNits = -1.0f;
            for (const auto& f : g.FindNode(stats)->analysisOutput.fields)
                if (f.name == L"Max") maxNits = f.components[0];
            auto r = ShaderLab::Rendering::ReadPixelRegion(g, cons, 8, 8, 1, 1, g_dc.get());
            const float px = (r.status == ShaderLab::Rendering::ReadPixelRegionStatus::Success && r.pixels.size() >= 4)
                ? r.pixels[0] : -1.0f;
            auto* cn = g.FindNode(cons);
            if (!cn->runtimeError.empty()) printf("    (debug) consumer error: %ls\n", cn->runtimeError.c_str());
            return { px, maxNits };
        };

        const bool prevGpu = ShaderLab::Performance::IsGpuBindingsEnabled();
        const uint64_t detBefore = ShaderLab::Performance::GpuBindingDetections();
        auto [gpuPx, gpuMax] = run(true);
        const uint64_t detAfter = ShaderLab::Performance::GpuBindingDetections();
        auto [cpuPx, cpuMax] = run(false);
        ShaderLab::Performance::SetGpuBindingsEnabled(prevGpu);
        g_evaluator.ReleaseCache();

        printf("  [info] Max=%.4f  pixel via texture=%.4f  via cbuffer=%.4f\n", gpuMax, gpuPx, cpuPx);
        TEST("Lane3Consumer_ProducerMeasuredSomething", gpuMax > 1.0f);
        TEST("Lane3Consumer_GpuBindingWasUsed", detAfter > detBefore);
        TEST("Lane3Consumer_TextureDeliversTheValue", std::abs(gpuPx - gpuMax) <= 1e-3f * gpuMax);
        TEST("Lane3Consumer_CbufferPathAgrees", std::abs(cpuPx - cpuMax) <= 1e-3f * cpuMax && std::abs(cpuPx - gpuPx) <= 1e-3f * gpuMax);
    }

    void TestBoundConsumerFreshSameFrame()
    {
        printf("\n=== Bound consumer sees analysis values the same frame ===\n");
        g_evaluator.ReleaseCache();
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto* srcDesc   = registry.FindByName(L"Gamut Source");
        auto* statsDesc = registry.FindByName(L"Luminance Statistics");
        auto* tmDesc    = registry.FindByName(L"ICtCp Round-Trip Validator");
        if (!srcDesc || !statsDesc || !tmDesc) { TEST("BoundFresh_DescriptorsFound", false); return; }

        ShaderLab::Effects::SourceNodeFactory sf;
        ShaderLab::Graph::EffectGraph g;
        auto srcId   = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*srcDesc));
        auto statsId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*statsDesc));
        auto tmId    = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*tmDesc));
        g.Connect(srcId, 0, statsId, 0);
        g.Connect(srcId, 0, tmId, 0);
        g.BindProperty(tmId, L"Gain", statsId, L"Max", 0);
        ShaderLab::Performance::SetGpuBindingsEnabled(false);   // CPU path: the value lands in properties

        // ONE frame, in the shape every host uses (MainWindow::RenderFrameToOffscreen,
        // headless RunRender): evaluate outside the session, dispatch inside it,
        // then the frozen sweep for whatever the dispatch dirtied.
        Evaluate(g, sf);
        g_dc->SetTarget(nullptr);
        g_dc->BeginDraw();
        g_evaluator.ProcessDeferredCompute(g, g_dc.get());
        if (g.HasDirtyNodes())
        {
            g_evaluator.SetDeferredComputeFrozen(true);
            g_evaluator.Evaluate(g, g_dc.get());
            g_evaluator.SetDeferredComputeFrozen(false);
        }
        g_dc->EndDraw();

        float statsMax = -1.0f;
        if (auto* sn = g.FindNode(statsId))
            for (const auto& f : sn->analysisOutput.fields)
                if (f.name == L"Max" && !f.components.empty()) statsMax = f.components[0];
        float tmPeak = -1.0f;
        if (auto* tn = g.FindNode(tmId))
            if (auto it = tn->properties.find(L"Gain"); it != tn->properties.end())
                if (auto* f = std::get_if<float>(&it->second)) tmPeak = *f;

        // Instrument check first: the measured Max must differ from the
        // parameter's default (1000), or "equal" below would prove nothing.
        TEST("BoundFresh_StatsProducedAValue", statsMax > 0.0f && std::abs(statsMax - 1000.0f) > 1.0f);
        TEST("BoundFresh_ConsumerHasThisFramesValue",
             statsMax > 0.0f && std::abs(tmPeak - statsMax) <= 1e-3f * statsMax);
    }

    void TestGpuBindingRouting()
    {
        printf("\n=== Phase 8: GPU-binding routing ===\n");
        g_evaluator.ReleaseCache();

        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto* srcDesc   = registry.FindByName(L"Gamut Source");
        auto* statsDesc = registry.FindByName(L"Luminance Statistics");
        auto* ictcpDesc = registry.FindByName(L"ICtCp Tone Map (HDR -> SDR)");
        if (!srcDesc || !statsDesc || !ictcpDesc) {
            TEST("GpuBinding_DescriptorsFound", false);
            return;
        }
        TEST("GpuBinding_ICtCpIsD3D11Compute",
             ictcpDesc->shaderType == ShaderLab::Graph::CustomShaderType::D3D11ComputeShader);

        // ---- Sub-test 1: Feature flag ON, with binding ----
        {
            ShaderLab::Effects::SourceNodeFactory sf;
            ShaderLab::Graph::EffectGraph g;
            auto srcNode   = ShaderLab::Effects::ShaderLabEffects::CreateNode(*srcDesc);
            auto statsNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*statsDesc);
            auto ictcpNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*ictcpDesc);
            auto srcId   = g.AddNode(std::move(srcNode));
            auto statsId = g.AddNode(std::move(statsNode));
            auto ictcpId = g.AddNode(std::move(ictcpNode));

            g.Connect(srcId, 0, statsId, 0);
            g.Connect(srcId, 0, ictcpId, 0);

            // Bind ICtCp's TargetPeakNits to Luminance Statistics' Mean.
            g.BindProperty(ictcpId, L"TargetPeakNits", statsId, L"Mean", 0);

            ShaderLab::Performance::SetGpuBindingsEnabled(true);
            uint64_t beforeDet = ShaderLab::Performance::GpuBindingDetections();

            Evaluate(g, sf);
            g_dc->SetTarget(nullptr);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();
            // Run twice so the first-frame just-created delay clears.
            Evaluate(g, sf);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();

            uint64_t afterDet = ShaderLab::Performance::GpuBindingDetections();
            TEST("GpuBinding_TelemetryIncremented", afterDet > beforeDet);

            auto* n = g.FindNode(ictcpId);
            TEST("GpuBinding_ICtCpHasOutput_FlagOn", n && n->cachedOutput != nullptr);
            TEST("GpuBinding_ICtCpNoRuntimeError_FlagOn", n && n->runtimeError.empty());

            ShaderLab::Performance::SetGpuBindingsEnabled(false);
        }

        // ---- Sub-test 2: Feature flag OFF, with binding ----
        // CPU readback path. Should still work.
        {
            ShaderLab::Effects::SourceNodeFactory sf;
            ShaderLab::Graph::EffectGraph g;
            auto srcNode   = ShaderLab::Effects::ShaderLabEffects::CreateNode(*srcDesc);
            auto statsNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*statsDesc);
            auto ictcpNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*ictcpDesc);
            auto srcId   = g.AddNode(std::move(srcNode));
            auto statsId = g.AddNode(std::move(statsNode));
            auto ictcpId = g.AddNode(std::move(ictcpNode));
            g.Connect(srcId, 0, statsId, 0);
            g.Connect(srcId, 0, ictcpId, 0);

            ShaderLab::Graph::PropertyBinding pb;
            ShaderLab::Graph::ComponentSource cs;
            cs.sourceNodeId    = statsId;
            cs.sourceFieldName = L"Mean";
            cs.sourceComponent = 0;
            pb.sources.push_back(cs);
            g.BindProperty(ictcpId, L"TargetPeakNits", statsId, L"Mean", 0);

            g_evaluator.ReleaseCache();
            Evaluate(g, sf);
            g_dc->SetTarget(nullptr);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();
            Evaluate(g, sf);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();

            auto* n = g.FindNode(ictcpId);
            TEST("GpuBinding_ICtCpHasOutput_FlagOff", n && n->cachedOutput != nullptr);
            TEST("GpuBinding_ICtCpNoRuntimeError_FlagOff", n && n->runtimeError.empty());
        }

        // ---- Sub-test 3: Feature flag ON, NO binding ----
        // Baseline (cbuffer mode) path even with flag on.
        {
            ShaderLab::Effects::SourceNodeFactory sf;
            ShaderLab::Graph::EffectGraph g;
            auto srcNode   = ShaderLab::Effects::ShaderLabEffects::CreateNode(*srcDesc);
            auto ictcpNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*ictcpDesc);
            auto srcId   = g.AddNode(std::move(srcNode));
            auto ictcpId = g.AddNode(std::move(ictcpNode));
            g.Connect(srcId, 0, ictcpId, 0);

            ShaderLab::Performance::SetGpuBindingsEnabled(true);
            g_evaluator.ReleaseCache();
            Evaluate(g, sf);
            g_dc->SetTarget(nullptr);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();
            Evaluate(g, sf);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();

            auto* n = g.FindNode(ictcpId);
            TEST("GpuBinding_ICtCpHasOutput_FlagOnNoBinding", n && n->cachedOutput != nullptr);
            TEST("GpuBinding_ICtCpNoRuntimeError_FlagOnNoBinding", n && n->runtimeError.empty());

            ShaderLab::Performance::SetGpuBindingsEnabled(false);
        }
    }

    // ----- Phase 8c: skip-CPU-readback when GPU-routed -----------------
    //
    // Verifies the Performance::IsSkipUnneededCpuReadbackEnabled() path:
    //   Source -> LumStats -> ICtCp Tone Map (bound via TargetPeakNits = LumStats.Mean).
    // With the skip flag ON and the GPU-binding flag ON, the LumStats
    // dispatch's CPU Map() should be elided -- but ICtCp must still
    // produce correct image output (because it reads LumStats's GPU SRV).
    // With the host hint adding LumStats to interest set, readback runs
    // again. With skip flag OFF, readback always runs (default behavior).
    void TestSkipCpuReadback()
    {
        printf("\n=== Phase 8c: skip-CPU-readback ===\n");
        g_evaluator.ReleaseCache();
        g_evaluator.ClearCpuAnalysisInterest();

        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto* srcDesc   = registry.FindByName(L"Gamut Source");
        auto* statsDesc = registry.FindByName(L"Luminance Statistics");
        auto* ictcpDesc = registry.FindByName(L"ICtCp Tone Map (HDR -> SDR)");
        if (!srcDesc || !statsDesc || !ictcpDesc) {
            TEST("SkipReadback_DescriptorsFound", false);
            return;
        }

        auto buildGraph = [&](ShaderLab::Graph::EffectGraph& g,
                              uint32_t& srcId, uint32_t& statsId, uint32_t& ictcpId) {
            auto srcNode   = ShaderLab::Effects::ShaderLabEffects::CreateNode(*srcDesc);
            auto statsNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*statsDesc);
            auto ictcpNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*ictcpDesc);
            srcId   = g.AddNode(std::move(srcNode));
            statsId = g.AddNode(std::move(statsNode));
            ictcpId = g.AddNode(std::move(ictcpNode));
            g.Connect(srcId, 0, statsId, 0);
            g.Connect(srcId, 0, ictcpId, 0);
            g.BindProperty(ictcpId, L"TargetPeakNits", statsId, L"Mean", 0);
        };

        // ---- Sub-test 1: skip flag ON + GPU flag ON, no host hint -------
        // LumStats.Mean is read by ICtCp via GPU SRV -> readback skipped.
        {
            ShaderLab::Effects::SourceNodeFactory sf;
            ShaderLab::Graph::EffectGraph g;
            uint32_t srcId, statsId, ictcpId;
            buildGraph(g, srcId, statsId, ictcpId);

            ShaderLab::Performance::SetGpuBindingsEnabled(true);
            ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(true);
            uint64_t beforeSkip = ShaderLab::Performance::SkippedCpuReadbacks();

            // Two-pass: first creates effects, second produces output.
            Evaluate(g, sf);
            g_dc->SetTarget(nullptr);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();
            Evaluate(g, sf);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();

            uint64_t afterSkip = ShaderLab::Performance::SkippedCpuReadbacks();
            TEST("SkipReadback_CounterIncrementsWhenSkipping",
                 afterSkip > beforeSkip);

            // ICtCp's image output must still be valid (it reads
            // LumStats.Mean via the upstream SRV at t1).
            auto* ictcp = g.FindNode(ictcpId);
            TEST("SkipReadback_ICtCpHasOutput",
                 ictcp && ictcp->cachedOutput != nullptr);

            ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(false);
            ShaderLab::Performance::SetGpuBindingsEnabled(false);
        }

        // ---- Sub-test 2: skip flag ON + host hint forces readback ------
        // Adding LumStats to CpuAnalysisInterest re-engages readback so
        // the Properties panel / MCP can read fresh values. Validates
        // the host's escape hatch for any node that needs CPU values.
        {
            ShaderLab::Effects::SourceNodeFactory sf;
            ShaderLab::Graph::EffectGraph g;
            uint32_t srcId, statsId, ictcpId;
            buildGraph(g, srcId, statsId, ictcpId);

            ShaderLab::Performance::SetGpuBindingsEnabled(true);
            ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(true);
            g_evaluator.SetCpuAnalysisInterest({ statsId });

            Evaluate(g, sf);
            g_dc->SetTarget(nullptr);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();
            Evaluate(g, sf);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();

            auto* stats = g.FindNode(statsId);
            float mean = 0.0f;
            if (stats) {
                for (const auto& f : stats->analysisOutput.fields)
                    if (f.name == L"Mean") { mean = f.components[0]; break; }
            }
            TEST("SkipReadback_HostHintForcesReadback", mean > 0.0f);

            g_evaluator.ClearCpuAnalysisInterest();
            ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(false);
            ShaderLab::Performance::SetGpuBindingsEnabled(false);
        }

        // ---- Sub-test 3: skip flag OFF preserves baseline ---------------
        // Default behavior: every dispatch reads back regardless.
        {
            ShaderLab::Effects::SourceNodeFactory sf;
            ShaderLab::Graph::EffectGraph g;
            uint32_t srcId, statsId, ictcpId;
            buildGraph(g, srcId, statsId, ictcpId);

            ShaderLab::Performance::SetGpuBindingsEnabled(true);
            ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(false);

            Evaluate(g, sf);
            g_dc->SetTarget(nullptr);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();
            Evaluate(g, sf);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();

            auto* stats = g.FindNode(statsId);
            float mean = 0.0f;
            if (stats) {
                for (const auto& f : stats->analysisOutput.fields)
                    if (f.name == L"Mean") { mean = f.components[0]; break; }
            }
            TEST("SkipReadback_FlagOffAlwaysReadsBack", mean > 0.0f);

            ShaderLab::Performance::SetGpuBindingsEnabled(false);
        }

        // ---- Sub-test 4: hint throttle ---------------------------------
        // With throttle large, repeated frames with the same hint do NOT
        // re-readback so SkippedCpuReadbacks() advances. With throttle 0,
        // every frame reads back so the skipped counter stays put.
        {
            ShaderLab::Effects::SourceNodeFactory sf;
            ShaderLab::Graph::EffectGraph g;
            uint32_t srcId, statsId, ictcpId;
            buildGraph(g, srcId, statsId, ictcpId);
            // Drop the LumStats <- ICtCp binding so LumStats is purely
            // a host-hint readback target (no CPU-routed binding consumer
            // would force readback every frame).
            g.UnbindProperty(ictcpId, L"TargetPeakNits");

            ShaderLab::Performance::SetGpuBindingsEnabled(true);
            ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(true);
            ShaderLab::Performance::SetCpuAnalysisHintThrottleMs(60'000);
            g_evaluator.SetCpuAnalysisInterest({ statsId });

            // First eval pair (fresh hint -> read).
            Evaluate(g, sf);
            g_dc->SetTarget(nullptr);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();
            Evaluate(g, sf);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();

            uint64_t beforeSkipped = ShaderLab::Performance::SkippedCpuReadbacks();
            for (int i = 0; i < 4; ++i)
            {
                Evaluate(g, sf);
                g_dc->BeginDraw();
                g_evaluator.ProcessDeferredCompute(g, g_dc.get());
                g_dc->EndDraw();
            }
            uint64_t afterSkipped = ShaderLab::Performance::SkippedCpuReadbacks();
            TEST("SkipReadback_HintThrottleSkipsRepeatFrames",
                 afterSkipped > beforeSkipped);

            ShaderLab::Performance::SetCpuAnalysisHintThrottleMs(0);
            // With throttle=0 every frame should re-read LumStats, so the
            // hinted node gets a fresh value. Mutate fields then check
            // it gets repopulated by the next ProcessDeferredCompute.
            auto* statsNode = g.FindNode(statsId);
            if (statsNode)
                statsNode->analysisOutput.fields.clear();
            Evaluate(g, sf);
            g_dc->BeginDraw();
            g_evaluator.ProcessDeferredCompute(g, g_dc.get());
            g_dc->EndDraw();
            bool refreshed = statsNode &&
                statsNode->analysisOutput.type == ShaderLab::Graph::AnalysisOutputType::Typed &&
                !statsNode->analysisOutput.fields.empty();
            TEST("SkipReadback_ThrottleZeroReadsHintEveryFrame", refreshed);

            ShaderLab::Performance::SetCpuAnalysisHintThrottleMs(2000);
            g_evaluator.ClearCpuAnalysisInterest();
            ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(false);
            ShaderLab::Performance::SetGpuBindingsEnabled(false);
        }
    }
    //
    // The Phase 7 plan to move the MCP server engine-side and add a
    // ShaderLabHeadless.exe console host depends on being able to render
    // the graph and read pixels back without any DXGI swap chain. The
    // existing TestEffectChain already proves the *evaluate* half works
    // off-screen (the test runner has no swap chain). This test proves
    // the *read pixels back* half works too -- the missing capability
    // for graph_snapshot / render_capture_node / image_stats MCP routes
    // when running in a no-UI host.
    void TestHeadlessReadback()
    {
        printf("\n=== Phase 7 Spike: Headless Readback ===\n");

        // Build the smallest finite-extent graph we can. Gamut Source is
        // a ShaderLab effect with a fixed output size (scRGB FP16) and
        // produces a deterministic CIE xy chromaticity diagram. Any
        // bounded source works; we just need "evaluate -> finite cached
        // output -> can read a pixel".
        auto& reg = ShaderLab::Effects::ShaderLabEffects::Instance();

        ShaderLab::Graph::EffectGraph g;
        ShaderLab::Effects::SourceNodeFactory sf;

        auto srcNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *reg.FindByName(L"Gamut Source"));
        auto srcId = g.AddNode(std::move(srcNode));

        Evaluate(g, sf);
        auto* node = g.FindNode(srcId);
        if (!node || !node->cachedOutput) {
            TEST("Gamut Source produces cachedOutput", false);
            return;
        }
        TEST("Gamut Source produces cachedOutput", true);

        // Sample the source onto a small but non-trivial target. A 32x32
        // target with no srcRect lets D2D figure out its own tiling.
        // Map and read pixel (16, 16) for the test.
        const UINT kSize = 32;
        winrt::com_ptr<ID2D1Bitmap1> targetBmp;
        D2D1_BITMAP_PROPERTIES1 targetProps{};
        targetProps.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
        targetProps.dpiX = 96.0f;
        targetProps.dpiY = 96.0f;
        HRESULT hr = g_dc->CreateBitmap(D2D1::SizeU(kSize, kSize), nullptr, 0, targetProps, targetBmp.put());
        if (FAILED(hr)) { TEST("CreateBitmap (target)", false); return; }

        winrt::com_ptr<ID2D1Bitmap1> stagingBmp;
        D2D1_BITMAP_PROPERTIES1 stagingProps{};
        stagingProps.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        stagingProps.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        stagingProps.dpiX = 96.0f;
        stagingProps.dpiY = 96.0f;
        hr = g_dc->CreateBitmap(D2D1::SizeU(kSize, kSize), nullptr, 0, stagingProps, stagingBmp.put());
        if (FAILED(hr)) { TEST("CreateBitmap (staging)", false); return; }

        float oldDpiX, oldDpiY;
        g_dc->GetDpi(&oldDpiX, &oldDpiY);
        g_dc->SetDpi(96.0f, 96.0f);

        g_dc->SetTarget(targetBmp.get());
        g_dc->BeginDraw();
        g_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        // No srcRect, no offset: render whatever's there. D2D figures out
        // its own bounds; for the Gamut Source's fixed-size output this
        // is well-defined.
        g_dc->DrawImage(node->cachedOutput);
        hr = g_dc->EndDraw();
        g_dc->SetTarget(nullptr);
        g_dc->SetDpi(oldDpiX, oldDpiY);
        if (FAILED(hr)) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                "EndDraw target (hr=0x%08X)", static_cast<uint32_t>(hr));
            TEST(buf, false);
            return;
        }

        D2D1_POINT_2U dstPt = { 0, 0 };
        D2D1_RECT_U srcRectU = { 0, 0, kSize, kSize };
        hr = stagingBmp->CopyFromBitmap(&dstPt, targetBmp.get(), &srcRectU);
        if (FAILED(hr)) { TEST("CopyFromBitmap (staging)", false); return; }

        D2D1_MAPPED_RECT mapped{};
        hr = stagingBmp->Map(D2D1_MAP_OPTIONS_READ, &mapped);
        if (FAILED(hr)) { TEST("Map staging bitmap", false); return; }

        // Read pixel (16, 16) from the mapped buffer.
        const uint8_t* row = mapped.bits + 16 * mapped.pitch;
        const float* px = reinterpret_cast<const float*>(row) + 16 * 4;
        const float r = px[0], green = px[1], b = px[2], a = px[3];
        stagingBmp->Unmap();

        // The exact value depends on Gamut Source's content at (128,128),
        // which we don't try to predict here. The Phase 7 spike only
        // needs to confirm: (a) the readback path runs without hanging
        // or failing, and (b) the bytes that came back are finite,
        // non-NaN floats. That's the headless-host capability we need
        // for graph_snapshot / render_capture_node / image_stats MCP
        // routes.
        bool finite = std::isfinite(r) && std::isfinite(green)
            && std::isfinite(b) && std::isfinite(a);
        TEST("Headless pixel readback completes without hang", true);
        TEST("Headless pixel readback returns finite floats", finite);

        std::printf("    pixel = (%.4f, %.4f, %.4f, %.4f)\n", r, green, b, a);
    }

    void TestSnapshot()
    {
        printf("\n=== GraphUiSnapshot ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        ShaderLab::Graph::EffectGraph g;
        auto srcDesc = registry.FindByName(L"Gamut Source");
        auto dstDesc = registry.FindByName(L"Gamut Source");
        auto srcId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*srcDesc));
        auto dstId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*dstDesc));
        g.Connect(srcId, 0, dstId, 0);

        // Stash a fake cachedOutput on a node to confirm the snapshot strips
        // it (raw ID2D1Image* must never escape into UI code).
        auto* live = g.FindNode(srcId);
        live->cachedOutput = reinterpret_cast<ID2D1Image*>(static_cast<uintptr_t>(0xDEADBEEFu));
        live->runtimeError = L"oops";
        live->dirty = true;

        auto snap = ShaderLab::Graph::BuildGraphUiSnapshot(
            g, /*previewId*/ dstId, /*graphGen*/ 7, /*frameGen*/ 99);
        TEST("BuildSnapshot returns non-null",        snap != nullptr);
        TEST("Snapshot graphGeneration",              snap->graphGeneration == 7);
        TEST("Snapshot frameGeneration",              snap->frameGeneration == 99);
        TEST("Snapshot previewNodeId",                snap->previewNodeId == dstId);
        TEST("Snapshot node count matches live",      snap->nodes.size() == 2);
        TEST("Snapshot edge count matches live",      snap->edges.size() == 1);
        TEST("Snapshot lookup by id works",           snap->FindNode(srcId) != nullptr);
        TEST("Snapshot lookup unknown id is null",    snap->FindNode(99999u) == nullptr);
        TEST("Snapshot strips cachedOutput",          snap->FindNode(srcId)->cachedOutput == nullptr);
        TEST("Snapshot preserves runtimeError",       snap->FindNode(srcId)->runtimeError == L"oops");
        TEST("Snapshot preserves dirty flag",         snap->FindNode(srcId)->dirty);
        TEST("Snapshot edge mirrors connection",
             snap->edges[0].sourceNodeId == srcId && snap->edges[0].destNodeId == dstId);

        // Mutating the live graph after snapshot must not affect the snap.
        g.RemoveNode(srcId);
        TEST("Live mutation does not affect snapshot",
             snap->nodes.size() == 2 && snap->FindNode(srcId) != nullptr);

        // atomic<shared_ptr> publication round-trip.
        std::atomic<std::shared_ptr<const ShaderLab::Graph::GraphUiSnapshot>> latest{ snap };
        auto loaded = latest.load();
        TEST("atomic<shared_ptr> publication round-trips", loaded == snap);
    }

    void TestRenderThreadDispatcher()
    {
        printf("\n=== RenderThreadDispatcher ===\n");

        // ---- Synchronous mode runs inline, no threading. ------------------
        {
            ShaderLab::Rendering::RenderThreadDispatcher d{ /*synchronous=*/true };
            int counter = 0;
            d.DispatchAsync([&]{ counter++; });
            TEST("synchronous mode runs DispatchAsync inline",
                 counter == 1);

            int sum = d.DispatchSync([]{ return 7 + 35; });
            TEST("synchronous mode DispatchSync returns value", sum == 42);

            // Re-entry from inside a closure runs inline.
            int reentry = 0;
            d.DispatchSync([&]{
                d.DispatchSync([&]{ reentry = 99; });
            });
            TEST("synchronous mode allows re-entrant DispatchSync",
                 reentry == 99);
        }

        // ---- Async-with-consumer-thread happy path. -----------------------
        {
            ShaderLab::Rendering::RenderThreadDispatcher d;
            std::atomic<bool> stop{ false };
            std::thread consumer([&]{
                d.RegisterConsumer();
                while (!stop.load(std::memory_order_acquire))
                {
                    d.WaitFor(std::chrono::milliseconds(50));
                    d.Drain();
                }
                d.Drain();
            });

            std::atomic<int> hits{ 0 };
            for (int i = 0; i < 100; ++i)
                d.DispatchAsync([&]{ hits.fetch_add(1, std::memory_order_relaxed); });

            // DispatchSync from a non-consumer thread blocks until done.
            int v = d.DispatchSync([&]{
                return hits.load(std::memory_order_relaxed);
            });
            TEST("async drain ran all enqueued closures (>=100)", v >= 100);
            TEST("async DispatchSync returned a value seen on consumer",
                 v == 100);

            stop.store(true, std::memory_order_release);
            d.Wake();
            consumer.join();
        }

        // ---- Re-entrant DispatchSync from inside a closure runs inline. ---
        {
            ShaderLab::Rendering::RenderThreadDispatcher d;
            std::atomic<bool> stop{ false };
            std::thread consumer([&]{
                d.RegisterConsumer();
                while (!stop.load(std::memory_order_acquire))
                {
                    d.WaitFor(std::chrono::milliseconds(50));
                    d.Drain();
                }
                d.Drain();
            });

            int outer = 0, inner = 0;
            d.DispatchSync([&]{
                outer = 1;
                // From inside a closure (= consumer thread), DispatchSync
                // must NOT requeue and self-deadlock; it must run inline.
                d.DispatchSync([&]{ inner = 2; });
            });
            TEST("re-entrant DispatchSync did not deadlock",
                 outer == 1 && inner == 2);

            stop.store(true, std::memory_order_release);
            d.Wake();
            consumer.join();
        }

        // ---- Closure exception propagates back through DispatchSync. ------
        {
            ShaderLab::Rendering::RenderThreadDispatcher d{ /*synchronous=*/true };
            bool threw = false;
            try
            {
                d.DispatchSync([]() -> int {
                    throw std::runtime_error("expected test failure");
                });
            }
            catch (const std::exception&) { threw = true; }
            TEST("DispatchSync re-throws closure exception", threw);
        }

        // ---- Shutdown cancels pending closures and unblocks waiters. ------
        {
            ShaderLab::Rendering::RenderThreadDispatcher d;
            std::atomic<bool> consumerRan{ false };
            std::thread consumer([&]{
                d.RegisterConsumer();
                d.Wait();          // returns when shutdown signaled
                consumerRan.store(true, std::memory_order_release);
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            d.Shutdown();
            consumer.join();
            TEST("Shutdown wakes Wait()", consumerRan.load());
            TEST("Shutdown clears queue", d.QueueDepth() == 0);
            TEST("IsShuttingDown reflects state", d.IsShuttingDown());
        }

        // ---- P19: many concurrent producers, single consumer. ------------
        // Stress test: 8 producer threads each post 250 sync closures
        // returning a unique tag. All 2000 closures must run on the consumer
        // thread (single thread id seen) and each producer must observe its
        // own tag back.
        {
            ShaderLab::Rendering::RenderThreadDispatcher d;
            std::atomic<bool> stop{ false };
            std::atomic<int> totalRan{ 0 };
            std::atomic<std::thread::id> consumerId{};
            std::atomic<bool> mixedThread{ false };
            std::thread consumer([&]{
                d.RegisterConsumer();
                consumerId.store(std::this_thread::get_id(), std::memory_order_release);
                while (!stop.load(std::memory_order_acquire)) {
                    d.WaitFor(std::chrono::milliseconds(5));
                    d.Drain();
                }
                d.Drain();
            });

            constexpr int producers = 8;
            constexpr int perProducer = 250;
            std::vector<std::thread> producerThreads;
            std::atomic<int> mismatches{ 0 };
            producerThreads.reserve(producers);
            for (int p = 0; p < producers; ++p) {
                producerThreads.emplace_back([&, p]{
                    for (int i = 0; i < perProducer; ++i) {
                        int tag = p * 1000 + i;
                        int got = d.DispatchSync([&, tag]{
                            if (std::this_thread::get_id() != consumerId.load())
                                mixedThread.store(true);
                            totalRan.fetch_add(1, std::memory_order_relaxed);
                            return tag;
                        });
                        if (got != tag) mismatches.fetch_add(1);
                    }
                });
            }
            for (auto& t : producerThreads) t.join();
            stop.store(true, std::memory_order_release);
            d.Wake();
            consumer.join();

            TEST("8 producers x 250 closures all ran",
                totalRan.load() == producers * perProducer);
            TEST("every closure ran on the consumer thread",
                !mixedThread.load());
            TEST("every DispatchSync returned its own tag",
                mismatches.load() == 0);
        }

        // ---- P19: shutdown with many pending closures unblocks all. ------
        // Producers that are blocked in DispatchSync must wake when Shutdown
        // is called. They get an exception (std::runtime_error from the
        // dispatcher), not a hang.
        {
            ShaderLab::Rendering::RenderThreadDispatcher d;
            std::thread consumer([&]{
                d.RegisterConsumer();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                // never drain -- closures pile up in the queue
            });

            constexpr int producers = 4;
            std::vector<std::thread> producerThreads;
            std::atomic<int> threwCount{ 0 };
            std::atomic<int> noThrowCount{ 0 };
            for (int p = 0; p < producers; ++p) {
                producerThreads.emplace_back([&]{
                    try {
                        d.DispatchSync([]{ return 1; });
                        noThrowCount.fetch_add(1);
                    } catch (...) {
                        threwCount.fetch_add(1);
                    }
                });
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            d.Shutdown();
            for (auto& t : producerThreads) t.join();
            consumer.join();

            TEST("Shutdown unblocks pending DispatchSync producers",
                threwCount.load() + noThrowCount.load() == producers);
            TEST("at least some producers observed shutdown",
                threwCount.load() > 0);
        }

        // ---- Step 7: Shutdown FAILS pending promises FAST (no timeout). --
        // A DispatchSync in flight when Shutdown() fires must throw promptly,
        // not eat its full timeout -- that is what makes the timeout ladder
        // enforceable during GUI shutdown / adapter switch.
        {
            ShaderLab::Rendering::RenderThreadDispatcher d;
            std::thread consumer([&] { d.RegisterConsumer(); std::this_thread::sleep_for(
                std::chrono::milliseconds(10)); /* never drains */ });
            std::atomic<bool> threw{ false };
            auto t0 = std::chrono::steady_clock::now();
            std::thread producer([&] {
                try { d.DispatchSync([] { return 7; }, std::chrono::seconds(30)); }
                catch (...) { threw.store(true); }
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            d.Shutdown();
            producer.join();
            consumer.join();
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            TEST("Shutdown fails pending DispatchSync", threw.load());
            TEST("Shutdown fail is fast (<2s, not the 30s timeout)", elapsedMs < 2000);
        }

        // ---- Step 7: DispatchSync AFTER Shutdown fails fast, not queued. -
        // (No consumer registered on this thread: a producer-side call takes
        // the queue path, where shutdown fails it fast. A re-entrant call
        // from the consumer thread would legitimately run inline instead.)
        {
            ShaderLab::Rendering::RenderThreadDispatcher d;
            d.Shutdown();
            bool threw = false;
            auto t0 = std::chrono::steady_clock::now();
            try { d.DispatchSync([] { return 1; }, std::chrono::seconds(30)); }
            catch (...) { threw = true; }
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            TEST("DispatchSync after Shutdown throws immediately", threw && elapsedMs < 2000);
        }

        // ---- Step 7: ResetConsumer (adapter switch) fails pending too. ---
        {
            ShaderLab::Rendering::RenderThreadDispatcher d;
            std::thread consumer([&] { d.RegisterConsumer(); std::this_thread::sleep_for(
                std::chrono::milliseconds(10)); });
            std::atomic<bool> threw{ false };
            std::thread producer([&] {
                try { d.DispatchSync([] { return 3; }, std::chrono::seconds(30)); }
                catch (...) { threw.store(true); }
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            d.ResetConsumer();
            producer.join();
            consumer.join();
            TEST("ResetConsumer fails pending DispatchSync", threw.load());
            // After reset the dispatcher is reusable (adapter respawn).
            TEST("ResetConsumer leaves dispatcher usable", !d.IsShuttingDown());
        }
    }
}

// ============================================================================
// McpRouter (stdio-migration Step 2): the router owns the query split, so
// ?since= on /node/{id}/logs reaches the handler over raw HTTP — previously
// the listener stripped it before routing and the filter was silently
// ignored. These tests pin the (path, query, body) handler contract,
// HasRoute, and the Response noReply discriminator.
// ============================================================================
// ============================================================================
// ImageLoader::SupportedExtensions -- the file-picker filter is derived from
// the WIC decoders registered on THIS machine, not from a hardcoded list.
// ============================================================================
static void TestImageLoaderExtensions()
{
    using ShaderLab::Tests::TEST;
    printf("\n=== ImageLoader supported extensions ===\n");

    auto exts = ShaderLab::Effects::ImageLoader::SupportedExtensions();

    printf("  %zu extension(s): ", exts.size());
    for (size_t i = 0; i < exts.size(); ++i)
        printf("%ls%s", exts[i].c_str(), (i + 1 < exts.size()) ? " " : "\n");

    TEST("ImageExt_NotEmpty", !exts.empty());

    // Every entry must be a filter FileOpenPicker will accept: a dot, at least
    // one character, lowercase, and no path/wildcard characters. One malformed
    // entry from a badly-registered third-party codec would throw E_INVALIDARG
    // and take out the entire dialog.
    bool wellFormed = true;
    for (const auto& e : exts)
    {
        if (e.size() < 2 || e.front() != L'.') { wellFormed = false; break; }
        if (e.find_first_of(L" \t\\/:*?\"<>|") != std::wstring::npos) { wellFormed = false; break; }
        for (wchar_t c : e)
            if (c != towlower(c)) { wellFormed = false; break; }
        if (!wellFormed) break;
    }
    TEST("ImageExt_AllWellFormed", wellFormed);

    // Sorted and de-duplicated: WIC reports the same extension from more than
    // one decoder (.tif from TIFF and from some RAW codecs).
    TEST("ImageExt_SortedUnique",
        std::is_sorted(exts.begin(), exts.end()) &&
        std::adjacent_find(exts.begin(), exts.end()) == exts.end());

    // The formats WIC has always shipped must be present; if these are missing
    // the enumeration is broken and the fallback list would be in play.
    auto has = [&](const wchar_t* e) {
        return std::find(exts.begin(), exts.end(), std::wstring(e)) != exts.end();
    };
    TEST("ImageExt_HasBaselineFormats", has(L".png") && has(L".jpg") && has(L".bmp") && has(L".tif"));

    // Informational, NOT a gate: HEIF/AVIF ship as installable OS extensions,
    // so their presence is a property of the machine. The point of the change
    // is that whatever this machine can decode is what the picker offers.
    printf("  [info] .heic %ls  .heif %ls  .avif %ls  .jxr %ls\n",
        has(L".heic") ? L"present" : L"absent",
        has(L".heif") ? L"present" : L"absent",
        has(L".avif") ? L"present" : L"absent",
        has(L".jxr")  ? L"present" : L"absent");
}

static void TestMcpRouter()
{
    using ShaderLab::Tests::TEST;   // this fn sits outside the anonymous
                                    // namespace that pulls TEST in above
    printf("\n=== McpRouter ===\n");

    ShaderLab::McpRouter router;
    std::wstring gotPath, gotQuery;
    int calls = 0;
    router.AddRoute(L"GET", L"/probe/",
        [&](const std::wstring& path, const std::wstring& query, const std::string&)
            -> ShaderLab::Mcp::Response {
            ++calls; gotPath = path; gotQuery = query;
            return { 200, "{}" };
        });
    // Catch-all, to prove longest-prefix still wins when a query is present.
    router.AddRoute(L"GET", L"/",
        [&](const std::wstring&, const std::wstring&, const std::string&)
            -> ShaderLab::Mcp::Response {
            return { 200, R"({"catchall":true})" };
        });

    auto r1 = router.RouteRequest(L"GET", L"/probe/7/logs?since=3", "");
    TEST("Router_MatchIgnoresQuery", r1.statusCode == 200 && calls == 1);
    TEST("Router_QueryReachesHandler", gotQuery == L"since=3");
    TEST("Router_PathStrippedForHandler", gotPath == L"/probe/7/logs");

    auto r2 = router.RouteRequest(L"GET", L"/probe/7/logs", "");
    TEST("Router_EmptyQueryIsEmpty", r2.statusCode == 200 && gotQuery.empty());

    auto r3 = router.RouteRequest(L"GET", L"/elsewhere", "");
    TEST("Router_CatchAllStillMatches",
        r3.statusCode == 200 && r3.body.find("catchall") != std::string::npos);

    TEST("Router_HasRouteIgnoresQuery", router.HasRoute(L"GET", L"/probe/x?y=1"));
    TEST("Router_HasRouteMethodMiss", !router.HasRoute(L"POST", L"/probe/x"));
    TEST("Router_HasSpecificRouteTrue", router.HasSpecificRoute(L"GET", L"/probe/x?y=1"));
    TEST("Router_HasSpecificRouteExcludesCatchAll", !router.HasSpecificRoute(L"GET", L"/elsewhere"));

    ShaderLab::McpRouter bare;
    TEST("Router_NoMatch404", bare.RouteRequest(L"GET", L"/nope", "").statusCode == 404);

    auto none = ShaderLab::Mcp::Response::None();
    TEST("Router_ResponseNoneIsNoReply", none.noReply && none.statusCode == 202);
    TEST("Router_ResponseDefaultReplies", ShaderLab::Mcp::Response{}.noReply == false);
}

// ============================================================================
// McpJsonRpc dispatcher (stdio-migration Step 3): engine-side JSON-RPC over
// the route registry, driven directly through RouteRequest — no HTTP needed.
// Pins the stdio-conformance contract: single-line messages, id echo on
// every error path, zero-byte notifications keyed off an absent id, batch
// rejection, guarded params, and the absent-route tool error that replaces
// the old silent fall-through into the POST / catch-all.
// ============================================================================
static void TestMcpJsonRpc()
{
    using ShaderLab::Tests::TEST;
    printf("\n=== McpJsonRpc dispatcher ===\n");

    ShaderLab::McpRouter router;
    router.AddRoute(L"GET", L"/graph/overview",
        [](const std::wstring&, const std::wstring&, const std::string&) -> ShaderLab::Mcp::Response {
            return { 200, R"({"previewNodeId":0,"nodes":[],"edges":[]})" };
        });
    ShaderLab::Mcp::JsonRpcOptions opts;
    opts.hostKind = "headless";
    ShaderLab::Mcp::RegisterJsonRpcEndpoint(router, std::move(opts));

    auto post = [&](const std::string& body) {
        return router.RouteRequest(L"POST", L"/", body);
    };

    auto init = post(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}})");
    TEST("Rpc_InitializeSingleLine", init.body.find('\n') == std::string::npos);
    TEST("Rpc_InitializeVersion2025_06_18", init.body.find("\"2025-06-18\"") != std::string::npos);
    TEST("Rpc_InitializeHostKind", init.body.find("shaderlab-headless") != std::string::npos);

    auto tl = post(R"({"jsonrpc":"2.0","id":"abc","method":"tools/list"})");
    TEST("Rpc_ToolsListSingleLine", tl.body.find('\n') == std::string::npos);
    TEST("Rpc_ToolsListEchoesStringId", tl.body.find("\"id\":\"abc\"") != std::string::npos);
    // tools/list is filtered by route presence, the same predicate tools/call
    // enforces -- this fixture registers ONLY GET /graph/overview, so that is
    // the one tool it may advertise. Before the filter the whole 39-entry
    // catalog was emitted regardless of host, so a headless session advertised
    // the GUI-only tools and then refused them on call.
    TEST("Rpc_ToolsListHasCatalog", tl.body.find("\"graph_overview\"") != std::string::npos);
    TEST("Rpc_ToolsListOmitsUnroutedTool", tl.body.find("\"graph_add_node\"") == std::string::npos);

    auto noid = post(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    TEST("Rpc_NotificationNoReply", noid.noReply && noid.body.empty());

    auto missing = post(R"({"jsonrpc":"2.0","id":7})");
    TEST("Rpc_MissingMethodKeepsId", missing.body.find("\"id\":7") != std::string::npos);

    auto batch = post(R"([{"jsonrpc":"2.0","id":9,"method":"ping"}])");
    TEST("Rpc_BatchRejected", batch.body.find("-32600") != std::string::npos);

    auto call = post(R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"graph_overview","arguments":{}}})");
    TEST("Rpc_ToolCallRoutes", call.body.find("previewNodeId") != std::string::npos
        && call.body.find("\"isError\":false") != std::string::npos);

    auto absent = post(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"graph_snapshot","arguments":{}}})");
    TEST("Rpc_AbsentRouteToolErrors", absent.body.find("\"isError\":true") != std::string::npos
        && absent.body.find("not available") != std::string::npos);

    auto unk = post(R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"nope","arguments":{}}})");
    TEST("Rpc_UnknownToolErrors", unk.body.find("\"isError\":true") != std::string::npos);

    auto arrParams = post(R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":[1,2]})");
    TEST("Rpc_ArrayParamsGuarded", arrParams.body.find("-32602") != std::string::npos
        && arrParams.body.find("\"id\":5") != std::string::npos);

    TEST("Rpc_EscaperControlChars",   // literal split: \x is greedy, \x01b would be ESC
        ShaderLab::Mcp::JsonEscape("a\x01" "b\nc") == "a\\u0001b\\nc");

    TEST("Rpc_CatalogCount", ShaderLab::Mcp::ToolCatalog().size() == 45);  // +perf_render_mode +perf_subrect_demand +perf_compute_submit +perf_gpu_bindings +render_video
    bool catalogSingleLine = true;
    for (const auto& t : ShaderLab::Mcp::ToolCatalog())
        if (std::string_view(t.listJson).find('\n') != std::string_view::npos)
            catalogSingleLine = false;
    TEST("Rpc_CatalogEntriesSingleLine", catalogSingleLine);
}

// ============================================================================
// McpFrame + McpCrypto (stdio-migration Step 4): wire codec and the
// P-256 ECDH -> HKDF-SHA256 -> AES-256-GCM session stack, as pure units.
// ============================================================================
// Session-client start/stop lifecycle.
//
// Regression guard for the toolbar's "disable MCP" path. Stop() used to
// CloseHandle() the pipe from the UI thread while the session thread was
// inside a blocking ReadFile/WriteFile on that same handle -- undefined per
// Win32 (a Debug build raises STATUS_INVALID_HANDLE, and a recycled handle
// value lets the session thread write into an unrelated object). It now sets
// the stop flag and calls CancelSynchronousIo on the session thread, leaving
// the close to the thread that owns the handle.
//
// No hub is running here, so the client sits in its connect/backoff loop --
// which is exactly the "freshly launched, not yet registered" window the
// crash was reported in. What this pins: Stop() is safe before the pipe is
// ever published, the duplicated thread handle is published and retired
// correctly, and Run() returns promptly rather than hanging the caller's
// join().
static void TestMcpSessionClientLifecycle()
{
    using ShaderLab::Tests::TEST;
    printf("\n=== McpSessionClient lifecycle ===\n");
    using namespace ShaderLab::Mcp;

    ShaderLab::McpRouter router;

    // Point at a pipe name nothing is serving so CreateFileW fails fast and
    // the client stays in the pre-registration window.
    auto makeOpts = [] {
        SessionClientOptions o;
        o.pipeBaseName = L"ShaderLab.mcp.unittest.nohub." +
            std::to_wstring(GetCurrentProcessId());
        o.sessionId = L"{00000000-0000-0000-0000-00000000TEST}";
        o.label = L"unit-test-session";
        return o;
    };

    // Stop() immediately after launch, repeatedly. Any handle misuse here is
    // what took the app down on a toggle click.
    bool allJoined = true;
    for (int i = 0; i < 25 && allJoined; ++i)
    {
        McpSessionClient client(router, makeOpts());
        std::thread t([&client] { client.Run(); });
        client.Stop();                       // races the connect attempt
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (t.joinable() && std::chrono::steady_clock::now() < deadline)
        {
            t.join();
            break;
        }
        if (t.joinable()) { allJoined = false; t.detach(); }
    }
    TEST("Stop() during connect returns promptly (25x, no hang)", allJoined);

    // Stop() before Run() ever starts, and Stop() called twice, must both be
    // no-ops rather than touching a handle that was never published.
    bool safeEdges = true;
    try
    {
        McpSessionClient neverRan(router, makeOpts());
        neverRan.Stop();
        neverRan.Stop();

        McpSessionClient client(router, makeOpts());
        std::thread t([&client] { client.Run(); });
        client.Stop();
        client.Stop();                       // second Stop after the first
        t.join();
    }
    catch (...) { safeEdges = false; }
    TEST("Stop() is safe before Run() and when called twice", safeEdges);

    // The discriminating case. The two checks above never publish a pipe
    // handle (nothing is listening), so the old CloseHandle path would pass
    // them too. Here a stub pipe server accepts the connection and then
    // deliberately never answers the hello, leaving the session thread parked
    // in a blocking synchronous ReadFile -- precisely the state the UI thread
    // used to close the handle out from under.
    {
        auto opts = makeOpts();
        opts.pipeBaseName = L"ShaderLab.mcp.unittest.stub." +
            std::to_wstring(GetCurrentProcessId());
        const std::wstring pipePath = L"\\\\.\\pipe\\" + opts.pipeBaseName;

        std::atomic<bool> serverReady{ false };
        std::atomic<bool> serverStop{ false };
        std::thread server([&] {
            HANDLE p = CreateNamedPipeW(pipePath.c_str(), PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                1, 64 * 1024, 64 * 1024, 0, nullptr);
            serverReady.store(true);
            if (p == INVALID_HANDLE_VALUE) return;
            ConnectNamedPipe(p, nullptr);
            while (!serverStop.load()) Sleep(20);   // never reply
            CloseHandle(p);
        });
        while (!serverReady.load()) Sleep(5);

        McpSessionClient client(router, opts);
        std::thread t([&client] { client.Run(); });
        Sleep(400);   // connect + send hello + block reading the ack

        const auto t0 = std::chrono::steady_clock::now();
        client.Stop();
        t.join();
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        serverStop.store(true);
        // Release a server still parked in ConnectNamedPipe (only possible if
        // the client never got there) so this join cannot hang the suite.
        HANDLE poke = CreateFileW(pipePath.c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (poke != INVALID_HANDLE_VALUE) CloseHandle(poke);
        server.join();

        printf("  Stop() while blocked in ReadFile returned in %lld ms\n",
               static_cast<long long>(elapsedMs));
        TEST("Stop() unblocks a session parked in a synchronous read",
             elapsedMs < 5000);
    }
}

static void TestMcpFrameCrypto()
{
    using ShaderLab::Tests::TEST;
    printf("\n=== McpFrame + McpCrypto ===\n");
    using namespace ShaderLab::Mcp;

    {
        Frame f;
        f.header.channelId = 7;
        f.header.seq = 0x1122334455667788ull;
        f.body = { 1, 2, 3, 4, 5 };
        std::vector<uint8_t> wire;
        TEST("Frame_EncodeSmall", EncodeFrame(f, wire) && wire.size() == 4 + 12 + 5);

        auto d = TryDecodeFrame(wire);
        TEST("Frame_RoundTripSmall",
            d.status == FrameDecodeStatus::Ok &&
            d.consumed == wire.size() &&
            d.frame.header.channelId == 7 &&
            d.frame.header.seq == 0x1122334455667788ull &&
            d.frame.body == f.body);

        bool truncOk = true;
        for (size_t cut : { size_t(0), size_t(3), size_t(4), size_t(10), wire.size() - 1 })
        {
            auto t = TryDecodeFrame(std::span(wire.data(), cut));
            if (t.status != FrameDecodeStatus::NeedMoreData || t.consumed != 0) truncOk = false;
        }
        TEST("Frame_TruncatedNeedsMore", truncOk);

        std::vector<uint8_t> two = wire;
        Frame g = f; g.header.seq = 9; g.body = { 42 };
        EncodeFrame(g, two);
        auto d1 = TryDecodeFrame(two);
        auto d2 = TryDecodeFrame(std::span(two.data() + d1.consumed, two.size() - d1.consumed));
        TEST("Frame_SequentialDecode",
            d1.status == FrameDecodeStatus::Ok && d2.status == FrameDecodeStatus::Ok &&
            d2.frame.header.seq == 9 && d2.frame.body.size() == 1 &&
            d1.consumed + d2.consumed == two.size());
    }
    {
        // 40 MB round-trip — the plan's own number.
        Frame f;
        f.header.channelId = 1;
        f.header.seq = 42;
        f.body.resize(40ull * 1024 * 1024);
        for (size_t i = 0; i < f.body.size(); i += 4096)
            f.body[i] = static_cast<uint8_t>(i >> 12);
        std::vector<uint8_t> wire;
        bool enc = EncodeFrame(f, wire);
        auto d = TryDecodeFrame(wire);
        TEST("Frame_40MBRoundTrip",
            enc && d.status == FrameDecodeStatus::Ok && d.frame.body == f.body);
    }
    {
        // Declared length over the 64 MB cap: explicit Oversize, zero
        // consumed — the stream is poisoned, never resynchronized.
        std::vector<uint8_t> over = { 0x01, 0x00, 0x00, 0x04, 0, 0, 0, 0 };  // 0x04000001
        auto d = TryDecodeFrame(over);
        TEST("Frame_OversizeExplicit", d.status == FrameDecodeStatus::Oversize && d.consumed == 0);

        Frame f; f.body.resize(kMaxFrameBytes);  // + header pushes past the cap
        std::vector<uint8_t> out;
        TEST("Frame_EncodeRefusesOversize", !EncodeFrame(f, out) && out.empty());

        std::vector<uint8_t> bad = { 4, 0, 0, 0, 1, 2, 3, 4 };  // len < header size
        TEST("Frame_MalformedShortLen",
            TryDecodeFrame(bad).status == FrameDecodeStatus::Malformed);
    }

    {
        // RFC 5869 test case A.1 (SHA-256).
        std::vector<uint8_t> ikm(22, 0x0b);
        std::vector<uint8_t> salt = { 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c };
        std::vector<uint8_t> info = { 0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9 };
        uint8_t okm[42]{};
        bool ok = HkdfSha256(ikm, salt, info, okm);
        static const uint8_t expect[42] = {
            0x3c,0xb2,0x5f,0x25,0xfa,0xac,0xd5,0x7a,0x90,0x43,0x4f,0x64,0xd0,0x36,
            0x2f,0x2a,0x2d,0x2d,0x0a,0x90,0xcf,0x1a,0x5a,0x4c,0x5d,0xb0,0x2d,0x56,
            0xec,0xc4,0xc5,0xbf,0x34,0x00,0x72,0x08,0xd5,0xb8,0x87,0x18,0x58,0x65 };
        TEST("Crypto_HkdfRfc5869A1", ok && memcmp(okm, expect, sizeof(expect)) == 0);
    }

    {
        auto a = EcdhKeyPair::Generate();
        auto b = EcdhKeyPair::Generate();
        TEST("Crypto_KeyPairGenerate", a && b && !a->PublicBlob().empty());
        if (!a || !b) return;

        auto ka = DeriveSessionKeys(*a, b->PublicBlob(), /*isInitiator=*/true);
        auto kb = DeriveSessionKeys(*b, a->PublicBlob(), /*isInitiator=*/false);
        TEST("Crypto_HandshakeMirrors",
            ka && kb && ka->sendKey == kb->recvKey && ka->recvKey == kb->sendKey &&
            ka->sendKey != ka->recvKey);
        if (!ka || !kb) return;

        auto sealer = GcmChannel::Create(ka->sendKey);
        auto opener = GcmChannel::Create(kb->recvKey);
        TEST("Crypto_ChannelCreate", sealer.has_value() && opener.has_value());
        if (!sealer || !opener) return;

        std::vector<uint8_t> plain(1024 * 1024);
        for (size_t i = 0; i < plain.size(); ++i) plain[i] = static_cast<uint8_t>(i * 31);
        std::vector<uint8_t> aad = { 9, 9, 9, 1, 2, 3 };   // stand-in clear frame header
        std::vector<uint8_t> box, opened;
        bool s = sealer->Seal(5, aad, plain, box);
        bool o = opener->Open(5, aad, box, opened);
        TEST("Crypto_SealOpenRoundTrip", s && o && opened == plain &&
            box.size() == plain.size() + kGcmTagBytes);

        std::vector<uint8_t> dump;
        std::vector<uint8_t> tampered = box; tampered[100] ^= 0x01;
        TEST("Crypto_TamperedCiphertextFails", !opener->Open(5, aad, tampered, dump));

        std::vector<uint8_t> tamperedTag = box; tamperedTag.back() ^= 0x01;
        TEST("Crypto_TamperedTagFails", !opener->Open(5, aad, tamperedTag, dump));

        std::vector<uint8_t> aad2 = aad; aad2[0] ^= 1;
        TEST("Crypto_TamperedAadFails", !opener->Open(5, aad2, box, dump));

        TEST("Crypto_SequenceDesyncFails", !opener->Open(6, aad, box, dump));

        auto wrongDir = GcmChannel::Create(kb->sendKey);
        TEST("Crypto_WrongDirectionKeyFails",
            wrongDir && !wrongDir->Open(5, aad, box, dump));
    }
}

// ============================================================================
// McpPeerIdentity (stdio-migration Step 4): identity resolution against a
// loopback pipe (covering the ambiguously-documented server-PID-from-
// client-handle call the spike measured) and the full pairing-policy
// matrix with synthesized identities.
// ============================================================================
static void TestMcpPeerIdentity()
{
    using ShaderLab::Tests::TEST;
    printf("\n=== McpPeerIdentity ===\n");
    using namespace ShaderLab::Mcp;

    auto self = ResolveProcessIdentity(::GetCurrentProcessId());
    TEST("Peer_SelfResolves", self.has_value());
    if (!self) return;
    TEST("Peer_SelfIsUnpackaged", self->kind == PeerIdentityKind::Unpackaged);
    TEST("Peer_SelfHasImageDir", !self->imageDirectory.empty());

    {
        auto pipeName = std::format(L"\\\\.\\pipe\\ShaderLabTest.{}.{}",
            ::GetCurrentProcessId(), ::GetTickCount64());
        HANDLE server = ::CreateNamedPipeW(pipeName.c_str(),
            PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 4096, 4096, 0, nullptr);
        HANDLE client = ::CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, 0, nullptr);
        bool connected = false;
        if (server != INVALID_HANDLE_VALUE && client != INVALID_HANDLE_VALUE)
            connected = ::ConnectNamedPipe(server, nullptr) || ::GetLastError() == ERROR_PIPE_CONNECTED;
        TEST("Peer_LoopbackPipeConnected", connected);

        ULONG clientPid = 0, serverPid = 0;
        bool cq = connected && ::GetNamedPipeClientProcessId(server, &clientPid);
        bool sq = connected && ::GetNamedPipeServerProcessId(client, &serverPid);
        TEST("Peer_PipeClientPidFromServerHandle", cq && clientPid == ::GetCurrentProcessId());
        TEST("Peer_PipeServerPidFromClientHandle", sq && serverPid == ::GetCurrentProcessId());

        auto viaServer = connected ? ResolvePipeClientIdentity(server) : std::nullopt;
        auto viaClient = connected ? ResolvePipeServerIdentity(client) : std::nullopt;
        TEST("Peer_PipeIdentitiesResolve",
            viaServer && viaClient &&
            viaServer->pid == ::GetCurrentProcessId() &&
            viaClient->pid == ::GetCurrentProcessId());

        if (client != INVALID_HANDLE_VALUE) ::CloseHandle(client);
        if (server != INVALID_HANDLE_VALUE) ::CloseHandle(server);
    }

    {
        auto buildId = LocalBuildId();
        TEST("Peer_BuildIdNonEmpty", !buildId.empty());

        PeerIdentity unpackA = *self, unpackB = *self;

        ::SetEnvironmentVariableW(L"SHADERLAB_MCP_ALLOW_UNPACKAGED", nullptr);
        TEST("Peer_UnpackagedRefusedWithoutGate",
            EvaluatePairing(unpackA, unpackB, buildId, buildId)
                == PairingVerdict::RefusedUnpackagedNotAllowed);

        ::SetEnvironmentVariableW(L"SHADERLAB_MCP_ALLOW_UNPACKAGED", L"1");
        TEST("Peer_UnpackagedAcceptedWithGate",
            EvaluatePairing(unpackA, unpackB, buildId, buildId) == PairingVerdict::Accept);
        TEST("Peer_UnpackagedBuildMismatchRefused",
            EvaluatePairing(unpackA, unpackB, buildId, L"other#abi9")
                == PairingVerdict::RefusedMismatch);

        PeerIdentity otherDir = unpackB;
        otherDir.imageDirectory = L"C:\\somewhere\\else";
        TEST("Peer_UnpackagedDirMismatchRefused",
            EvaluatePairing(unpackA, otherDir, buildId, buildId)
                == PairingVerdict::RefusedMismatch);

        // Shared-parent relaxation (Step 6): sibling per-project out dirs
        // under the same <Platform>\<Config> root pair, exact-dir does not.
        PeerIdentity sib = *self;
        sib.imageDirectory = L"C:\\build\\ARM64\\Debug\\ShaderLabHeadless";
        PeerIdentity sib2 = *self;
        sib2.imageDirectory = L"C:\\build\\ARM64\\Debug\\ShaderLabMcpBroker";
        TEST("Peer_UnpackagedSiblingDirAccepted",
            EvaluatePairing(sib, sib2, buildId, buildId) == PairingVerdict::Accept);
        PeerIdentity farDir = sib2;
        farDir.imageDirectory = L"C:\\build\\x64\\Release\\Elsewhere";
        TEST("Peer_UnpackagedDifferentRootRefused",
            EvaluatePairing(sib, farDir, buildId, buildId) == PairingVerdict::RefusedMismatch);

        PeerIdentity packagedA;
        packagedA.kind = PeerIdentityKind::Packaged;
        packagedA.packageFamilyName = L"ShaderLab_9v3yd384n9j18";
        PeerIdentity packagedB = packagedA;
        TEST("Peer_PackagedSamePfnAccepted",
            EvaluatePairing(packagedA, packagedB, buildId, L"anything") == PairingVerdict::Accept);

        PeerIdentity packagedC = packagedA;
        packagedC.packageFamilyName = L"SomeoneElse_abc123";
        TEST("Peer_PackagedPfnMismatchRefused",
            EvaluatePairing(packagedA, packagedC, buildId, buildId)
                == PairingVerdict::RefusedMismatch);

        // Mixed is refused even while the unpackaged gate is SET.
        TEST("Peer_MixedAlwaysRefused",
            EvaluatePairing(packagedA, unpackA, buildId, buildId) == PairingVerdict::RefusedMixed);

        ::SetEnvironmentVariableW(L"SHADERLAB_MCP_ALLOW_UNPACKAGED", nullptr);
    }
}

// ============================================================================
// McpChannel (stdio-migration Step 6): the shim↔session SecureChannel used
// end-to-end over the relay. Two SecureChannels handshaking + sealing in
// process — the same code the shim (initiator) and session (acceptor) run,
// with no pipe in the loop.
// ============================================================================
static void TestMcpChannel()
{
    using ShaderLab::Tests::TEST;
    printf("\n=== McpChannel ===\n");
    using namespace ShaderLab::Mcp;

    auto shim = SecureChannel::Create(/*initiator=*/true);
    auto session = SecureChannel::Create(/*initiator=*/false);
    TEST("Channel_Create", shim.has_value() && session.has_value());
    if (!shim || !session) return;

    TEST("Channel_NotReadyBeforeHandshake", !shim->Ready() && !session->Ready());

    // Exchange hello bodies (the seq-0 handshake frames).
    bool h1 = session->OnPeerHello(shim->HelloBody());
    bool h2 = shim->OnPeerHello(session->HelloBody());
    TEST("Channel_HandshakeReady", h1 && h2 && shim->Ready() && session->Ready());

    const uint32_t cid = 5;

    // Shim -> session request at seq 1.
    std::string reqStr = R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})";
    std::vector<uint8_t> req(reqStr.begin(), reqStr.end());
    auto sealedReq = shim->Seal(cid, 1, req);
    TEST("Channel_ShimSeals", sealedReq.has_value());
    auto openedReq = sealedReq ? session->Open(cid, 1, *sealedReq) : std::nullopt;
    TEST("Channel_SessionOpens", openedReq.has_value() && *openedReq == req);

    // Session -> shim response at its own seq 1 (separate direction key).
    std::string respStr = R"({"jsonrpc":"2.0","id":1,"result":{"tools":[]}})";
    std::vector<uint8_t> resp(respStr.begin(), respStr.end());
    auto sealedResp = session->Seal(cid, 1, resp);
    auto openedResp = sealedResp ? shim->Open(cid, 1, *sealedResp) : std::nullopt;
    TEST("Channel_ResponseRoundTrip", openedResp.has_value() && *openedResp == resp);

    // A tampered sealed body fails to open.
    if (sealedReq)
    {
        auto bad = *sealedReq; bad[0] ^= 0x40;
        TEST("Channel_TamperRejected", !session->Open(cid, 1, bad).has_value());
    }

    // AAD binds the channelId: opening the same bytes on a different
    // channelId fails (a misrouted frame can't be replayed onto another
    // channel).
    if (sealedReq)
        TEST("Channel_WrongChannelIdRejected", !session->Open(cid + 1, 1, *sealedReq).has_value());

    // Wrong seq (desync / replay) fails.
    if (sealedReq)
        TEST("Channel_WrongSeqRejected", !session->Open(cid, 2, *sealedReq).has_value());

    // Distinct channels derive distinct keys (fresh ephemerals): a frame
    // sealed on one channel's shim doesn't open on a different pairing.
    auto shim2 = SecureChannel::Create(true);
    auto session2 = SecureChannel::Create(false);
    if (shim2 && session2)
    {
        session2->OnPeerHello(shim2->HelloBody());
        shim2->OnPeerHello(session2->HelloBody());
        auto s = shim2->Seal(cid, 1, req);
        TEST("Channel_CrossChannelKeyIsolation",
            s.has_value() && !session->Open(cid, 1, *s).has_value());
    }
}

int main(int argc, char* argv[])
{
    winrt::init_apartment();
    MFStartup(MF_VERSION);

    // Parse adapter flag.
    bool useWarp = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--adapter" && i + 1 < argc) {
            useWarp = (std::string(argv[i + 1]) == "warp");
            i++;
        }
    }

    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("ShaderLab Test Runner\n");
    printf("=====================\n");

    // Create D3D11 device.
    UINT d3dFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    winrt::com_ptr<ID3D11Device> baseDevice;
    winrt::com_ptr<ID3D11DeviceContext> baseCtx;
    D3D_DRIVER_TYPE driverType = useWarp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    HRESULT hr = D3D11CreateDevice(nullptr, driverType, nullptr, d3dFlags,
        featureLevels, ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION, baseDevice.put(), nullptr, baseCtx.put());
    if (FAILED(hr)) {
        printf("FATAL: D3D11CreateDevice failed 0x%08X\n", (uint32_t)hr);
        return 1;
    }
    g_d3dDevice = baseDevice.as<ID3D11Device5>();
    g_d3dContext = baseCtx.as<ID3D11DeviceContext4>();

    // Enable multithread protection.
    winrt::com_ptr<ID3D10Multithread> mt;
    g_d3dDevice.as(mt);
    if (mt) mt->SetMultithreadProtected(TRUE);

    printf("Device: %s\n", useWarp ? "WARP" : "Hardware");

    // Create D2D.
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory7), reinterpret_cast<void**>(g_d2dFactory.put()));
    winrt::com_ptr<IDXGIDevice> dxgiDev;
    baseDevice->QueryInterface(dxgiDev.put());
    winrt::com_ptr<ID2D1Device6> d2dDevice;
    g_d2dFactory->CreateDevice(dxgiDev.as<IDXGIDevice>().get(),
        reinterpret_cast<ID2D1Device**>(d2dDevice.put()));
    d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        reinterpret_cast<ID2D1DeviceContext**>(g_dc.put()));

    // Register custom effects.
    winrt::com_ptr<ID2D1Factory1> factory1;
    g_d2dFactory->QueryInterface(factory1.put());
    ShaderLab::Effects::RegisterEngineD2DEffects(factory1.get());

    // Run tests.
    TestGraphOperations();
    TestSerialization();
    TestPixelShaderSignatures();
    TestEffectCatalogCount();
    TestSourceEffects();
    TestComputeChainFreshness();
    TestAnalysisEffects();
    TestBuiltInD2DEffects();
    TestPropertyBindings();
    TestMathNodes();
    TestClockNode();
    TestShaderCompilation();
    TestBytecodeCache();
    TestEffectChain();
    TestLuminanceStatistics();
    TestGpuBindingRouting();
    TestBoundConsumerFreshSameFrame();
    TestLane3PixelConsumer();
    TestPerfPassRegressions();
    TestVideoSourceSeekAndZeroCopy();
    TestVideoExportHelpers();
    TestSkipCpuReadback();
    TestHeadlessReadback();
    TestSnapshot();
    TestRenderThreadDispatcher();
    TestMcpRouter();
    TestImageLoaderExtensions();
    TestMcpJsonRpc();
    TestMcpSessionClientLifecycle();
    TestMcpFrameCrypto();
    TestMcpPeerIdentity();
    TestMcpChannel();

    // ---- Math test bench (Phase 2) -----------------------------------------
    {
        ShaderLab::Tests::ShaderTestBench bench;
        bench.Initialize(g_d3dDevice.get(), g_d3dContext.get());
        ShaderLab::Tests::TestTransferFunctions(bench);
        ShaderLab::Tests::TestColorMatrices(bench);
        ShaderLab::Tests::TestMobiusReinhard(bench);
        ShaderLab::Tests::TestDeltaE(bench);
        ShaderLab::Tests::TestGamut(bench);
        bench.Shutdown();
    }

    // Summary.
    printf("\n========================================\n");
    if (g_failed == 0)
        printf("ALL %d TESTS PASSED\n", g_passed);
    else
        printf("%d PASSED, %d FAILED out of %d\n", g_passed, g_failed, g_passed + g_failed);
    printf("========================================\n");

    MFShutdown();
    winrt::uninit_apartment();
    return g_failed;
}
