#include "pch_engine.h"
#include "../TestSupport.h"
#include "Effects/ColorMathCpu.h"
#include "Effects/EffectRegistry.h"
#include "Effects/SourceNodeFactory.h"
#include "Graph/GraphModel.h"
#include "Rendering/PixelTrace.h"
#include "Rendering/PreviewViewState.h"

#include <cmath>
#include <cstdio>

// The pixel trace and the preview's view state.
namespace
{
    using namespace ShaderLab::Tests;
    namespace Rendering = ShaderLab::Rendering;

    bool Near(float a, float b, float tolerance = 1e-4f) { return std::abs(a - b) <= tolerance; }

    SL_TEST_CASE(TestPixelTrace, Tag::Gpu)
    {
        printf("\n=== Pixel Trace ===\n");

        // A wide-gamut flood feeds both inputs of a Composite: a diamond.
        ShaderLab::Graph::GraphModel graph;
        ShaderLab::Effects::SourceNodeFactory factory;
        const uint32_t flood = graph.AddNode(ShaderLab::Effects::SourceNodeFactory::CreateFloodSourceNode(
            { 0.5f, 2.0f, -0.25f, 1.0f }, L"Flood"));
        const uint32_t composite = graph.AddNode(ShaderLab::Effects::EffectRegistry::CreateNode(
            *ShaderLab::Effects::EffectRegistry::Instance().FindByName(L"Composite")));
        graph.Connect(flood, 0, composite, 0);
        graph.Connect(flood, 0, composite, 1);
        PrepareSourcesAndEvaluate(graph, factory);

        const auto result = Rendering::TracePixel(graph, composite, 7, 3, g_dc.get());
        TEST("Trace_Succeeded", result.root.has_value() && SUCCEEDED(result.hr));
        if (!result.root) return;
        const auto& root = *result.root;
        TEST("Trace_RootIsTheNode", root.nodeId == composite && root.hasPixel);
        TEST("Trace_BothPinsListed", root.inputs.size() == 2 &&
             root.inputs[0].inputPin == 0 && root.inputs[1].inputPin == 1 &&
             root.inputs[0].nodeId == flood && root.inputs[1].nodeId == flood);
        // Source-over of an opaque colour on itself is that colour.
        const auto& p = root.pixel;
        printf("  [info] traced scRGB %.4f %.4f %.4f %.4f\n", p.scR, p.scG, p.scB, p.scA);
        TEST("Trace_ReadsSignedScRgb", Near(p.scR, 0.5f, 2e-3f) && Near(p.scG, 2.0f, 2e-3f) &&
             Near(p.scB, -0.25f, 2e-3f) && Near(p.scA, 1.0f, 2e-3f));
        TEST("Trace_SharedUpstreamReadOnce", root.inputs.size() == 2 &&
             root.inputs[0].pixel.scG == root.inputs[1].pixel.scG && root.inputs[0].hasPixel);

        TEST("Trace_MissingNodeHasNoRoot", !Rendering::TracePixel(graph, 9999, 0, 0, g_dc.get()).root.has_value());

        // A node without an output yet reads no pixel, rather than zeros.
        ShaderLab::Graph::GraphModel pending;
        const uint32_t blur = pending.AddNode(ShaderLab::Effects::EffectRegistry::CreateNode(
            *ShaderLab::Effects::EffectRegistry::Instance().FindByName(L"Gaussian Blur")));
        const auto unread = Rendering::TracePixel(pending, blur, 0, 0, g_dc.get());
        TEST("Trace_NoOutputNoPixel", unread.root.has_value() && !unread.root->hasPixel);
    }

    SL_TEST_CASE(TestTracedPixelRepresentations, Tag::Fast)
    {
        printf("\n=== Traced pixel representations ===\n");
        const auto white = Rendering::DescribePixel(1.0f, 1.0f, 1.0f, 1.0f);
        TEST("Describe_SdrWhiteIs80Nits", Near(white.luminanceNits, 80.0f, 1e-3f));
        TEST("Describe_SrgbWhite", white.sR == 255 && white.sG == 255 && white.sB == 255 && white.sA == 255);
        // 80 nits in PQ is about 0.4859 on every channel (white is white in BT.2020 too).
        TEST("Describe_Hdr10White", Near(white.pqR, 0.4859f, 1e-3f) && Near(white.pqG, white.pqR, 1e-5f));

        // HDR10 is BT.2020: Rec.709 red has a green component there, which a
        // per-channel PQ of the Rec.709 values would drop.
        const auto red = Rendering::DescribePixel(1.0f, 0.0f, 0.0f, 1.0f);
        printf("  [info] Rec.709 red -> PQ %.4f %.4f %.4f\n", red.pqR, red.pqG, red.pqB);
        TEST("Describe_Hdr10UsesBt2020Primaries", red.pqG > 0.15f && red.pqB > 0.05f && red.pqR > red.pqG);

        // BT.2020 green in signed scRGB is pure green in HDR10 at 80 nits.
        const auto green = Rendering::DescribePixel(-0.5876f, 1.1329f, -0.1006f, 1.0f);
        printf("  [info] BT.2020 green -> PQ %.4f %.4f %.4f\n", green.pqR, green.pqG, green.pqB);
        TEST("Describe_Hdr10KeepsWideGamut", Near(green.pqG, 0.4859f, 5e-3f) && green.pqR < 0.1f && green.pqB < 0.1f);
        TEST("Describe_SrgbClipsNegative", green.sR == 0 && green.sB == 0);
    }

    SL_TEST_CASE(TestPreviewViewState, Tag::Fast)
    {
        printf("\n=== Preview view state ===\n");
        using Rendering::PreviewView;

        const auto fit = Rendering::FitPreviewView({ 0, 0, 200, 100 }, 400, 400);
        TEST("Fit_Centred", fit && Near(fit->zoom, 1.9f) && Near(fit->panX, 10.0f) && Near(fit->panY, 105.0f));
        TEST("Fit_EmptyImageWaits", !Rendering::FitPreviewView({ 0, 0, 0, 0 }, 400, 400));
        TEST("Fit_NoViewportWaits", !Rendering::FitPreviewView({ 0, 0, 10, 10 }, 0, 400));
        const auto unbounded = Rendering::FitPreviewView({ -1e7f, -1e7f, 1e7f, 1e7f }, 400, 400);
        TEST("Fit_UnboundedIsIdentity", unbounded && *unbounded == PreviewView{});
        const auto offset = Rendering::FitPreviewView({ 50, 50, 250, 150 }, 400, 400);
        TEST("Fit_HonoursOrigin", offset && Near(offset->panX, 10.0f - 50.0f * 1.9f));

        const auto zoomed = Rendering::ZoomPreviewAbout({ 1.0f, 0.0f, 0.0f }, 100.0f, 50.0f, 2.0f);
        TEST("Zoom_KeepsPointFixed", Near(zoomed.zoom, 2.0f) && Near(zoomed.panX, -100.0f) && Near(zoomed.panY, -50.0f));
        TEST("Zoom_Clamped", Near(Rendering::ZoomPreviewAbout({ 90.0f, 0, 0 }, 0, 0, 2.0f).zoom, 100.0f));

        Rendering::PreviewViewState state;
        state.SetViewport(400, 400);
        state.SelectNode(5);
        TEST("State_NewNodeFits", state.FitPending() && state.AutoFit() && state.NodeId() == 5);
        const auto frame = state.Frame();
        TEST("State_FitApplies", state.FitTo(frame, { 0, 0, 200, 100 }) && !state.FitPending() &&
             Near(state.View().zoom, 1.9f));

        // An edit while a frame renders beats the fit computed from that frame.
        const auto stale = state.Frame();
        state.SetView({ 3.0f, 1.0f, 2.0f });
        TEST("State_EditEndsAutoFit", !state.AutoFit());
        TEST("State_StaleFitDropped", !state.FitTo(stale, { 0, 0, 10, 10 }) && Near(state.View().zoom, 3.0f));

        // Each node keeps its own view.
        state.SelectNode(6);
        TEST("State_OtherNodeFits", state.FitPending() && state.AutoFit());
        state.SelectNode(5);
        TEST("State_ViewRemembered", state.View() == (PreviewView{ 3.0f, 1.0f, 2.0f }) && !state.FitPending());

        state.SetSelectedNode(9);
        TEST("State_SelectionInFrame", state.Frame().selectedNodeId == 9);
        state.RequestFit(/*keepFitted=*/false);
        TEST("State_OneShotFit", state.FitPending() && !state.AutoFit());
        state.Reset();
        TEST("State_ResetForgets", state.NodeId() == 0 && state.FitPending());
        state.SelectNode(5);
        TEST("State_ResetForgetsViews", state.AutoFit());
    }
}
