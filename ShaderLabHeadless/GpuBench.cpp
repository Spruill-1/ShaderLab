#include "pch_engine.h"
#include "GpuBench.h"
#include "HeadlessEngine.h"
#include "../Graph/GraphEdits.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <vector>

namespace ShaderLab::Headless
{
    namespace
    {
        namespace WDJ = winrt::Windows::Data::Json;
        using Rendering::GpuSpan;

        struct BenchSamples
        {
            std::vector<double> drawMs, frameMs, evalMs, cpuEvalMs, cpuFrameMs;
            // Per compute node: its dispatch, which the evaluator brackets as
            // its own submission.
            std::map<uint32_t, std::vector<double>> nodeMs;
        };

        double MinOf(const std::vector<double>& v)
        {
            return v.empty() ? 0.0 : *std::min_element(v.begin(), v.end());
        }

        double MedianOf(std::vector<double> v)
        {
            if (v.empty()) return 0.0;
            std::sort(v.begin(), v.end());
            return v[v.size() / 2];
        }

        // The target is the node's output size, so the real pixel count is
        // measured; a request can project the same content to another size.
        D2D1_SIZE_U BenchSize(HeadlessEngine& engine, const Graph::EffectNode& node, const GpuBenchRequest& request)
        {
            uint32_t width = 0, height = 0;
            if (node.cachedOutput)
            {
                D2D1_RECT_F bounds{};
                if (SUCCEEDED(engine.Dc()->GetImageLocalBounds(node.cachedOutput, &bounds)))
                {
                    width = static_cast<uint32_t>((std::max)(0.0f, bounds.right - bounds.left));
                    height = static_cast<uint32_t>((std::max)(0.0f, bounds.bottom - bounds.top));
                }
            }
            if (request.width) width = *request.width;
            if (request.height) height = *request.height;
            if (width == 0 || height == 0) { width = request.fallbackWidth; height = request.fallbackHeight; }
            return D2D1::SizeU(width, height);
        }

        // One iteration: re-dirty the sources, render the frame, draw the
        // node into `target`, and block for this iteration's GPU result.
        // False when the node has no output or the draw failed.
        bool RunIteration(HeadlessEngine& engine, uint32_t nodeId, ID2D1Bitmap1* target,
                          BenchSamples& samples)
        {
            auto& graph = engine.Model();
            auto& timer = engine.Timer();
            auto* dc = engine.Dc();
            for (auto& source : graph.MutableNodes())
                if (source.type == Graph::NodeType::Source) Graph::MarkNodeChanged(source);

            // The Frame span is everything a video frame pays: source upload,
            // evaluation, the compute reductions, and the draw. The Evaluate
            // span is the frame minus the final draw; the Draw span isolates
            // the pixel-shader chain.
            timer.BeginFrame();
            timer.Begin(GpuSpan::Frame);
            timer.Begin(GpuSpan::Evaluate);
            // CPU wall-clock of the frame beside its GPU span: the walk is
            // mostly CPU work (property application, cache lookups, binding
            // resolution) that submits little to the GPU.
            const auto cpuStart = std::chrono::steady_clock::now();
            engine.RenderFrame();
            samples.cpuEvalMs.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuStart).count());
            timer.End(GpuSpan::Evaluate, dc);

            // A frame may add or remove nodes, so the node is looked up again.
            const auto* node = graph.FindNode(nodeId);
            auto* image = node ? node->cachedOutput : nullptr;
            if (!image) return false;
            winrt::com_ptr<ID2D1Image> previousTarget;
            dc->GetTarget(previousTarget.put());
            dc->SetTarget(target);
            timer.Begin(GpuSpan::Draw);
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(0, 0, 0, 0));
            dc->DrawImage(image);
            const HRESULT drawResult = dc->EndDraw();   // flushes, so the span can close
            // EndDraw does not wait for the GPU: this is CPU work plus any
            // synchronous GPU waits inside the frame (readback Maps).
            samples.cpuFrameMs.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuStart).count());
            timer.End(GpuSpan::Draw);
            timer.End(GpuSpan::Frame);
            timer.EndFrame();
            dc->SetTarget(previousTarget.get());
            if (FAILED(drawResult)) return false;

            // Block for this iteration's result: sampling without waiting
            // records the previous frame's number again and again.
            if (timer.CollectBlocking())
            {
                samples.drawMs.push_back(timer.SpanMs(GpuSpan::Draw));
                samples.frameMs.push_back(timer.SpanMs(GpuSpan::Frame));
                samples.evalMs.push_back(timer.SpanMs(GpuSpan::Evaluate));
                for (const auto& n : graph.Nodes())
                    if (n.type == Graph::NodeType::ComputeShader)
                        if (double ms = timer.NodeMs(n.id); ms > 0.0)
                            samples.nodeMs[n.id].push_back(ms);
            }
            return true;
        }

        void InsertStatistics(WDJ::JsonObject& body, const BenchSamples& samples, double megapixels)
        {
            if (!samples.drawMs.empty())
            {
                std::vector<double> sorted = samples.drawMs;
                std::sort(sorted.begin(), sorted.end());
                double sum = 0.0;
                for (double v : sorted) sum += v;
                body.Insert(L"gpuMinMs", WDJ::JsonValue::CreateNumberValue(sorted.front()));
                body.Insert(L"gpuMedianMs", WDJ::JsonValue::CreateNumberValue(sorted[sorted.size() / 2]));
                body.Insert(L"gpuMeanMs", WDJ::JsonValue::CreateNumberValue(sum / static_cast<double>(sorted.size())));
                body.Insert(L"gpuMaxMs", WDJ::JsonValue::CreateNumberValue(sorted.back()));
                if (megapixels > 0.0)
                    body.Insert(L"gpuMsPerMpx", WDJ::JsonValue::CreateNumberValue(sorted.front() / megapixels));
            }
            if (!samples.frameMs.empty())
            {
                body.Insert(L"gpuFrameMinMs", WDJ::JsonValue::CreateNumberValue(MinOf(samples.frameMs)));
                body.Insert(L"gpuEvalMinMs", WDJ::JsonValue::CreateNumberValue(MinOf(samples.evalMs)));
                body.Insert(L"cpuEvalMinMs", WDJ::JsonValue::CreateNumberValue(MinOf(samples.cpuEvalMs)));
                body.Insert(L"gpuFrameMedianMs", WDJ::JsonValue::CreateNumberValue(MedianOf(samples.frameMs)));
                body.Insert(L"cpuEvalMedianMs", WDJ::JsonValue::CreateNumberValue(MedianOf(samples.cpuEvalMs)));
                body.Insert(L"cpuFrameMedianMs", WDJ::JsonValue::CreateNumberValue(MedianOf(samples.cpuFrameMs)));
                WDJ::JsonObject perNode;
                for (const auto& [nodeId, values] : samples.nodeMs)
                    perNode.Insert(std::to_wstring(nodeId), WDJ::JsonValue::CreateNumberValue(MedianOf(values)));
                body.Insert(L"computeNodeMedianMs", perNode);
            }
        }
    }

    WDJ::JsonObject RunGpuBench(HeadlessEngine& engine, const GpuBenchRequest& request)
    {
        WDJ::JsonObject body;
        const uint32_t iterations = (std::max)(1u, (std::min)(request.iterations, 500u));
        auto* node = engine.Model().FindNode(request.nodeId);
        if (!engine.Timer().IsEnabled())
        {
            body.Insert(L"error", WDJ::JsonValue::CreateStringValue(L"GPU timing is off -- pass --gpu-timing"));
            return body;
        }
        if (!node)
        {
            body.Insert(L"error", WDJ::JsonValue::CreateStringValue(L"node not found"));
            return body;
        }

        const D2D1_SIZE_U size = BenchSize(engine, *node, request);
        winrt::com_ptr<ID2D1Bitmap1> target;
        const D2D1_BITMAP_PROPERTIES1 targetProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(engine.Dc()->CreateBitmap(size, nullptr, 0, &targetProps, target.put())))
        {
            body.Insert(L"error", WDJ::JsonValue::CreateStringValue(L"could not create bench target"));
            return body;
        }

        BenchSamples samples;
        samples.drawMs.reserve(iterations);
        samples.frameMs.reserve(iterations);
        samples.evalMs.reserve(iterations);
        for (uint32_t i = 0; i < iterations; ++i)
            if (!RunIteration(engine, request.nodeId, target.get(), samples)) break;

        const double megapixels = static_cast<double>(size.width) * static_cast<double>(size.height) / 1e6;
        body.Insert(L"nodeId", WDJ::JsonValue::CreateNumberValue(request.nodeId));
        body.Insert(L"width", WDJ::JsonValue::CreateNumberValue(size.width));
        body.Insert(L"height", WDJ::JsonValue::CreateNumberValue(size.height));
        body.Insert(L"megapixels", WDJ::JsonValue::CreateNumberValue(megapixels));
        body.Insert(L"iterations", WDJ::JsonValue::CreateNumberValue(iterations));
        body.Insert(L"samples", WDJ::JsonValue::CreateNumberValue(static_cast<double>(samples.drawMs.size())));
        body.Insert(L"disjointDrops", WDJ::JsonValue::CreateNumberValue(engine.Timer().DisjointDrops()));
        InsertStatistics(body, samples, megapixels);
        return body;
    }
}
