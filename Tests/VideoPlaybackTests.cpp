#include "pch_engine.h"
#include "TestCommon.h"
#include "Graph/EffectGraph.h"
#include "Rendering/GraphEvaluator.h"
#include "Rendering/FramePacer.h"
#include "Rendering/RenderThreadDispatcher.h"
#include "Rendering/PreviewRenderStats.h"
#include "Effects/SourceNodeFactory.h"
#include "Effects/ShaderLabEffects.h"
#include "Effects/Performance.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

// Real-time harnesses for Clock-driven video playback and render-loop pacing,
// plus the video provider's playback controls, seeks and decoder surface pool.
// The harnesses report an [info] table every run; their targets are asserted
// while the matching cEnforce flag is set and timing assertions are on (see
// TIMING_TEST). SHADERLAB_VIDEO_MATRIX=full runs every clip against every
// tick rate.

namespace ShaderLab::Tests
{
    namespace
    {
        // Playback targets: video time tracks the Clock at any tick rate,
        // dropping frames when ticks are slower than the clip. The bound Time
        // is the Clock's value from the previous evaluation, so lag behind the
        // Clock is allowed one tick more than lag behind Time.
        constexpr bool cEnforcePlaybackTargets = true;
        constexpr double cTargetSpeedTolerance = 0.02;      // |video time / clock time - 1|
        constexpr double cTargetMaxFinalLagFrames = 2.0;    // plus one tick
        constexpr double cTargetMaxTimeLagFrames = 1.5;     // behind the Time the video was given
        constexpr int cTargetMaxBackwardSteps = 0;

        // Pacing targets: a loop whose work fits in a frame runs at 60 Hz.
        // p90 rather than p95: over a few hundred frames p95 rests on a
        // handful of scheduler outliers.
        constexpr bool cEnforcePacingTargets = true;
        constexpr double cTargetPacingMinHz = 58.0;
        constexpr double cTargetPacingP50ToleranceMs = 0.5;
        constexpr double cTargetPacingP90Ms = 18.0;
        // A reader taking the worker's lock shared while frames overrun.
        constexpr double cTargetOverrunLockWaitMs = 50.0;

        constexpr double cRunSeconds = 4.0;
        constexpr double cFullRunSeconds = 6.0;
        constexpr double cWarmupSeconds = 0.5;
        constexpr double cDefaultWorkMs = 2.0;
        constexpr double cPacingSeconds = 3.0;
        constexpr double cComparisonPacingSeconds = 1.5;
        constexpr int cWorkerWaitMs = 16;      // the fixed WaitFor that FramePacer replaced, for comparison
        constexpr uint32_t cWorkerRateHz = 60; // RenderWorkerLoop's FramePacer at a 60 Hz display
        constexpr int cMinUploadsPerRun = 10;

        // Frame index encoding of the video_index_* fixtures: 10 vertical
        // bars, bar b white when bit b of the index is set, in the top half,
        // and the complement in the bottom half.
        constexpr int cIndexBars = 10;
        constexpr float cMinBarContrast = 0.3f;

        using Clock = std::chrono::steady_clock;

        double SecondsBetween(Clock::time_point from, Clock::time_point to)
        {
            return std::chrono::duration<double>(to - from).count();
        }

        void SpinFor(double milliseconds)
        {
            if (milliseconds <= 0.0) return;
            const auto end = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double, std::milli>(milliseconds));
            while (Clock::now() < end)
                YieldProcessor();
        }

        float HalfToFloat(uint16_t half)
        {
            const uint32_t exponent = (half >> 10) & 0x1F;
            const uint32_t mantissa = half & 0x3FF;
            float value;
            if (exponent == 0) value = std::ldexp(static_cast<float>(mantissa), -24);
            else if (exponent == 31) value = mantissa ? NAN : INFINITY;
            else value = std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
            return (half & 0x8000) ? -value : value;
        }

        // Frame index from a 2-row FP16 staging copy of a video_index_* frame
        // (top-half row, then bottom-half row), or -1 when unreadable.
        int ReadFrameIndex(ID3D11DeviceContext* context, ID3D11Texture2D* staging, UINT width)
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
                return -1;
            const auto* top = static_cast<const uint16_t*>(mapped.pData);
            const auto* bottom = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mapped.pData) + mapped.RowPitch);
            int index = 0;
            bool readable = true;
            for (int bar = 0; bar < cIndexBars; ++bar)
            {
                const UINT x = (2 * bar + 1) * width / (2 * cIndexBars);
                const float topValue = HalfToFloat(top[x * 4]);
                const float bottomValue = HalfToFloat(bottom[x * 4]);
                if (!(std::abs(topValue - bottomValue) >= cMinBarContrast)) readable = false;
                if (topValue > bottomValue) index |= 1 << bar;
            }
            context->Unmap(staging, 0);
            return readable ? index : -1;
        }

        double Percentile(std::vector<double> values, double fraction)
        {
            if (values.empty()) return 0.0;
            std::sort(values.begin(), values.end());
            const size_t index = static_cast<size_t>(fraction * (values.size() - 1) + 0.5);
            return values[(std::min)(index, values.size() - 1)];
        }

        std::wstring FindVideoFixture(const wchar_t* name)
        {
            wchar_t buffer[MAX_PATH]{};
            ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
            std::filesystem::path dir = std::filesystem::path(buffer).parent_path();
            for (int i = 0; i < 6 && !dir.empty(); ++i)
            {
                const auto candidate = dir / L"Tests" / L"fixtures" / name;
                if (std::filesystem::exists(candidate)) return candidate.wstring();
                dir = dir.parent_path();
            }
            return {};
        }

        bool FullMatrixRequested()
        {
            char value[16]{};
            const DWORD length = ::GetEnvironmentVariableA("SHADERLAB_VIDEO_MATRIX", value, sizeof(value));
            return length > 0 && length < sizeof(value) && std::string(value) == "full";
        }

        // Sleeps to an absolute deadline with a high-resolution waitable
        // timer; plain Sleep rounds up to the 15.6 ms system tick.
        class DeadlineSleeper
        {
        public:
            DeadlineSleeper()
                : m_timer(::CreateWaitableTimerExW(nullptr, nullptr,
                      CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)) {}
            ~DeadlineSleeper() { if (m_timer) ::CloseHandle(m_timer); }
            DeadlineSleeper(const DeadlineSleeper&) = delete;
            DeadlineSleeper& operator=(const DeadlineSleeper&) = delete;

            void SleepUntil(Clock::time_point deadline)
            {
                const auto remaining = deadline - Clock::now();
                if (remaining <= Clock::duration::zero()) return;
                if (!m_timer)
                {
                    std::this_thread::sleep_until(deadline);
                    return;
                }
                LARGE_INTEGER due{};
                due.QuadPart = -static_cast<LONGLONG>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100);
                if (::SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE))
                    ::WaitForSingleObject(m_timer, INFINITE);
                else
                    std::this_thread::sleep_until(deadline);
            }

        private:
            HANDLE m_timer{ nullptr };
        };

        float NodeFloat(const Graph::EffectNode& node, const wchar_t* name, float fallback)
        {
            auto it = node.properties.find(name);
            if (it != node.properties.end())
                if (auto* value = std::get_if<float>(&it->second)) return *value;
            return fallback;
        }

        // A status value, or N/A when negative.
        std::string FormatStat(double value, const char* format)
        {
            if (value < 0.0) return "N/A";
            char text[32];
            std::snprintf(text, sizeof(text), format, value);
            return text;
        }

        // The render worker's per-tick Clock advance.
        void AdvanceClock(Graph::EffectNode& clockNode, double deltaSeconds)
        {
            const float startTime = NodeFloat(clockNode, L"StartTime", 0.0f);
            const float stopTime = NodeFloat(clockNode, L"StopTime", 10.0f);
            const float speed = NodeFloat(clockNode, L"Speed", 1.0f);
            const bool loop = NodeFloat(clockNode, L"Loop", 1.0f) > 0.5f;
            double duration = static_cast<double>(stopTime - startTime);
            if (duration <= 0.0) duration = 1.0;
            clockNode.clockTime += deltaSeconds * speed;
            if (loop)
            {
                while (clockNode.clockTime >= duration) clockNode.clockTime -= duration;
                while (clockNode.clockTime < 0.0) clockNode.clockTime += duration;
            }
            else
            {
                clockNode.clockTime = std::clamp(clockNode.clockTime, 0.0, duration);
            }
            clockNode.dirty = true;
        }

        // Playback harness

        enum class Pacing { Deadline, WorkerWait };

        struct PlaybackCase
        {
            const wchar_t* clip;
            Pacing pacing;
            double tickHz;      // Deadline only
            double workMs;
        };

        struct PlaybackResult
        {
            std::string error;
            double fps{ 0 };
            int ticks{ 0 };
            double achievedTickHz{ 0 };
            double speed{ 0 };
            int framesAdvanced{ 0 };
            int distinctUploads{ 0 };
            int skipped{ 0 };
            int duplicates{ 0 };
            int backwards{ 0 };
            int maxJump{ 0 };
            double maxLagFrames{ 0 };
            double maxTimeLagFrames{ 0 };
            double finalLagFrames{ 0 };
            uint64_t decodes{ 0 };
            uint64_t uploads{ 0 };
            uint64_t dropped{ 0 };
            uint64_t seeks{ 0 };
            int unreadable{ 0 };
            int timestampMismatches{ 0 };
            long long timestampOffset{ 0 };
            std::string firstUploads;
        };

        struct UploadSample
        {
            double elapsed{ 0 };
            double frameTime{ -1 };
            int index{ -1 };
        };

        struct TickSample
        {
            double elapsed{ 0 };
            double clockTime{ 0 };
            double timeValue{ 0 };   // the Time the video was ticked with
            int shownUpload{ -1 };
        };

        std::string PacingLabel(const PlaybackCase& testCase)
        {
            char label[48];
            if (testCase.pacing == Pacing::WorkerWait)
                std::snprintf(label, sizeof(label), "worker %uHz+%.0fms", cWorkerRateHz, testCase.workMs);
            else
                std::snprintf(label, sizeof(label), "%.0f Hz +%.0fms", testCase.tickHz, testCase.workMs);
            return label;
        }

        // Drives a Clock-bound video node the way RenderWorkerLoop does, in
        // real time: advance the Clock, resolve source bindings, tick and
        // upload videos, prepare sources, evaluate, draw, then pace.
        PlaybackResult RunPlayback(const PlaybackCase& testCase, const std::wstring& path, double runSeconds,
                                   ID2D1DeviceContext5* dc, ID3D11Device* device, ID3D11DeviceContext* context)
        {
            using Effects::SourceNodeFactory;
            using Effects::ShaderLabEffects;
            PlaybackResult result;

            SourceNodeFactory factory;
            Rendering::GraphEvaluator evaluator;
            Graph::EffectGraph graph;
            const auto* clockDescriptor = ShaderLabEffects::Instance().FindByName(L"Clock");
            if (!clockDescriptor) { result.error = "no Clock effect"; return result; }
            const uint32_t clockId = graph.AddNode(ShaderLabEffects::CreateNode(*clockDescriptor));
            const uint32_t videoId = graph.AddNode(SourceNodeFactory::CreateVideoSourceNode(path));
            // A fixed long Clock range keeps the run free of Clock wraps.
            graph.FindNode(clockId)->properties[L"AutoDuration"] = 0.0f;
            graph.FindNode(clockId)->properties[L"StopTime"] = 3600.0f;
            if (!graph.BindProperty(videoId, L"Time", clockId, L"Time", 0).empty())
            {
                result.error = "BindProperty failed";
                return result;
            }
            auto& nodes = const_cast<std::vector<Graph::EffectNode>&>(graph.Nodes());
            auto prepareSources = [&](double deltaSeconds)
            {
                for (auto& node : nodes)
                    if (node.type == Graph::NodeType::Source)
                        factory.PrepareSourceNode(node, dc, deltaSeconds, device, context);
            };
            prepareSources(0.0);
            evaluator.Evaluate(graph, dc);
            auto* provider = factory.GetVideoProvider(videoId);
            if (!provider || !provider->OutputTexture())
            {
                result.error = "video did not open";
                return result;
            }
            result.fps = provider->FrameRate();
            const UINT width = provider->FrameWidth();
            const UINT height = provider->FrameHeight();
            D3D11_TEXTURE2D_DESC outputDesc{};
            provider->OutputTexture()->GetDesc(&outputDesc);
            if (outputDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || result.fps <= 0.0)
            {
                result.error = "unexpected output format or frame rate";
                return result;
            }

            // Two rows per upload (top and bottom bar halves) go to a staging
            // pool and are read after the run, so readback never stalls a tick.
            const double maxTickHz = testCase.pacing == Pacing::Deadline ? testCase.tickHz : static_cast<double>(cWorkerRateHz);
            const size_t poolSize = static_cast<size_t>(runSeconds * maxTickHz * 1.25) + 64;
            D3D11_TEXTURE2D_DESC stagingDesc{};
            stagingDesc.Width = width;
            stagingDesc.Height = 2;
            stagingDesc.MipLevels = 1;
            stagingDesc.ArraySize = 1;
            stagingDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            stagingDesc.SampleDesc.Count = 1;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            std::vector<winrt::com_ptr<ID3D11Texture2D>> stagingPool(poolSize);
            for (auto& staging : stagingPool)
                if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, staging.put())))
                {
                    result.error = "staging allocation failed";
                    return result;
                }
            const UINT topRow = height / 4;
            const UINT bottomRow = height * 3 / 4;

            winrt::com_ptr<ID2D1Bitmap1> drawTarget;
            D2D1_BITMAP_PROPERTIES1 targetProps{};
            targetProps.pixelFormat = { DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
            targetProps.dpiX = targetProps.dpiY = 96.0f;
            targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
            if (FAILED(dc->CreateBitmap({ width, height }, nullptr, 0, targetProps, drawTarget.put())))
            {
                result.error = "draw target allocation failed";
                return result;
            }

            Rendering::RenderThreadDispatcher dispatcher;
            dispatcher.RegisterConsumer();
            DeadlineSleeper sleeper;
            Rendering::FramePacer workerPacer(cWorkerRateHz);
            std::vector<UploadSample> uploads;
            std::vector<TickSample> ticks;
            uploads.reserve(poolSize);
            ticks.reserve(poolSize);
            const uint64_t decodesBefore = provider->DecodeCount();
            const uint64_t uploadsBefore = provider->UploadSuccesses();
            const uint64_t droppedBefore = provider->DroppedFrames();
            const uint64_t seeksBefore = provider->SeekCount();

            graph.FindNode(clockId)->isPlaying = true;
            const auto start = Clock::now();
            auto last = start;
            const auto period = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(testCase.pacing == Pacing::Deadline ? 1.0 / testCase.tickHz : 0.0));
            for (long long tick = 0;; ++tick)
            {
                if (testCase.pacing == Pacing::Deadline)
                    sleeper.SleepUntil(start + period * tick);
                const auto now = Clock::now();
                const double elapsed = SecondsBetween(start, now);
                if (elapsed >= runSeconds) break;
                double deltaSeconds = SecondsBetween(last, now);
                last = now;
                if (deltaSeconds > 0.1) deltaSeconds = 0.016;

                auto* clockNode = graph.FindNode(clockId);
                AdvanceClock(*clockNode, deltaSeconds);
                evaluator.ResolveSourceBindings(graph);
                const float timeUsed = NodeFloat(*graph.FindNode(videoId), L"Time", 0.0f);
                if (factory.TickAndUploadVideos(nodes, dc, deltaSeconds))
                {
                    if (uploads.size() >= stagingPool.size())
                    {
                        result.error = "staging pool exhausted";
                        break;
                    }
                    auto* staging = stagingPool[uploads.size()].get();
                    const D3D11_BOX topBox{ 0, topRow, 0, width, topRow + 1, 1 };
                    const D3D11_BOX bottomBox{ 0, bottomRow, 0, width, bottomRow + 1, 1 };
                    context->CopySubresourceRegion(staging, 0, 0, 0, 0, provider->OutputTexture(), 0, &topBox);
                    context->CopySubresourceRegion(staging, 0, 0, 1, 0, provider->OutputTexture(), 0, &bottomBox);
                    UploadSample sample;
                    sample.elapsed = elapsed;
                    sample.frameTime = provider->UploadedFrameTime();
                    uploads.push_back(sample);
                }
                prepareSources(deltaSeconds);
                evaluator.Evaluate(graph, dc);
                dc->SetTarget(drawTarget.get());
                dc->BeginDraw();
                dc->Clear(D2D1::ColorF(0, 0, 0, 0));
                if (auto* videoNode = graph.FindNode(videoId); videoNode && videoNode->cachedOutput)
                    dc->DrawImage(videoNode->cachedOutput);
                dc->EndDraw();
                dc->SetTarget(nullptr);

                TickSample tickSample;
                tickSample.elapsed = elapsed;
                tickSample.clockTime = graph.FindNode(clockId)->clockTime;
                tickSample.timeValue = timeUsed;
                tickSample.shownUpload = static_cast<int>(uploads.size()) - 1;
                ticks.push_back(tickSample);

                SpinFor(testCase.workMs);
                if (testCase.pacing == Pacing::WorkerWait)
                {
                    dispatcher.WaitUntil(workerPacer.WaitDeadline(Clock::now()));
                    workerPacer.BeginFrame(Clock::now());
                }
            }
            result.decodes = provider->DecodeCount() - decodesBefore;
            result.uploads = provider->UploadSuccesses() - uploadsBefore;
            result.dropped = provider->DroppedFrames() - droppedBefore;
            result.seeks = provider->SeekCount() - seeksBefore;
            result.ticks = static_cast<int>(ticks.size());
            result.achievedTickHz = ticks.size() > 1 ? (ticks.size() - 1) / (ticks.back().elapsed - ticks.front().elapsed) : 0.0;

            // Recover each uploaded frame's index from its bars.
            for (size_t i = 0; i < uploads.size(); ++i)
            {
                uploads[i].index = ReadFrameIndex(context, stagingPool[i].get(), width);
                if (uploads[i].index < 0) ++result.unreadable;
            }

            // The pixels must name the frame the provider says it uploaded, up
            // to a constant offset: MF timestamps include the B-frame delay.
            bool haveOffset = false;
            for (const auto& upload : uploads)
            {
                if (upload.index < 0) continue;
                const long long fromTime = std::llround(upload.frameTime * result.fps);
                if (!haveOffset) { result.timestampOffset = upload.index - fromTime; haveOffset = true; }
                else if (upload.index - fromTime != result.timestampOffset) ++result.timestampMismatches;
            }

            // Upload sequence after warmup: steps of one are normal playback.
            int previous = -1;
            for (size_t i = 0; i < uploads.size() && i < 8; ++i)
                result.firstUploads += std::to_string(uploads[i].index) + " ";
            for (const auto& upload : uploads)
            {
                if (upload.index < 0 || upload.elapsed < cWarmupSeconds) continue;
                if (previous >= 0)
                {
                    const int step = upload.index - previous;
                    if (step > 1) result.skipped += step - 1;
                    else if (step == 0) ++result.duplicates;
                    else if (step < 0) ++result.backwards;
                    result.maxJump = (std::max)(result.maxJump, step);
                }
                ++result.distinctUploads;
                previous = upload.index;
            }

            // Speed and lag against the Clock, after warmup, on the provider's
            // time axis (frame index minus the timestamp offset).
            const TickSample* firstSample = nullptr;
            const TickSample* lastSample = nullptr;
            for (const auto& tickSample : ticks)
            {
                if (tickSample.elapsed < cWarmupSeconds || tickSample.shownUpload < 0) continue;
                const int shown = uploads[tickSample.shownUpload].index;
                if (shown < 0) continue;
                if (!firstSample) firstSample = &tickSample;
                lastSample = &tickSample;
                const double lagFrames = tickSample.clockTime * result.fps - (shown - result.timestampOffset);
                result.maxLagFrames = (std::max)(result.maxLagFrames, lagFrames);
                const double timeLagFrames = tickSample.timeValue * result.fps - (shown - result.timestampOffset);
                result.maxTimeLagFrames = (std::max)(result.maxTimeLagFrames, timeLagFrames);
            }
            if (firstSample && lastSample && lastSample != firstSample)
            {
                const int firstShown = uploads[firstSample->shownUpload].index;
                const int lastShown = uploads[lastSample->shownUpload].index;
                result.framesAdvanced = lastShown - firstShown;
                result.speed = (result.framesAdvanced / result.fps) / (lastSample->clockTime - firstSample->clockTime);
                result.finalLagFrames = lastSample->clockTime * result.fps - (lastShown - result.timestampOffset);
            }
            return result;
        }

        void ReportPlaybackHeader()
        {
            std::printf("  [info] after %.1f s warmup: speed = video time / clock time; adv/skip/dup/back/jump "
                "over uploaded frame indices; lag in frames behind the Clock, timeLag behind the Time "
                "the video was given (the binding reads the Clock's previous value)\n", cWarmupSeconds);
            std::printf("  [info] %-28s %-17s %6s %4s %6s %5s %5s %4s %4s %4s %7s %8s %7s %7s %7s %5s %7s\n",
                "clip", "pacing", "tickHz", "fps", "speed", "adv", "skip", "dup", "back", "jump",
                "maxLag", "finalLag", "decodes", "uploads", "dropped", "seeks", "timeLag");
        }

        void ReportPlaybackRow(const PlaybackCase& testCase, const PlaybackResult& result)
        {
            std::printf("  [info] %-28ls %-17s %6.1f %4.0f %6.3f %5d %5d %4d %4d %4d %7.1f %8.1f %7llu %7llu %7llu %5llu %7.1f\n",
                testCase.clip, PacingLabel(testCase).c_str(), result.achievedTickHz, result.fps, result.speed,
                result.framesAdvanced, result.skipped, result.duplicates, result.backwards, result.maxJump,
                result.maxLagFrames, result.finalLagFrames,
                static_cast<unsigned long long>(result.decodes), static_cast<unsigned long long>(result.uploads),
                static_cast<unsigned long long>(result.dropped), static_cast<unsigned long long>(result.seeks),
                result.maxTimeLagFrames);
        }
    }

    // Clock-driven video playback rate, measured in real time through the
    // SourceNodeFactory path the render worker uses. Lag is in frames: Clock
    // time times fps, minus the shown frame's timestamp times fps.
    void TestVideoPlaybackRate(ID2D1DeviceContext5* dc, ID3D11Device* device, ID3D11DeviceContext* context)
    {
        std::printf("\n=== Video playback rate (Clock-driven, real time) ===\n");
        const bool full = FullMatrixRequested();
        const double runSeconds = full ? cFullRunSeconds : cRunSeconds;

        std::vector<PlaybackCase> cases;
        if (full)
        {
            const wchar_t* clips[] = { L"video_index_h264_180p24.mp4", L"video_index_h264_180p30.mp4",
                                       L"video_index_h264_180p60.mp4" };
            for (const auto* clip : clips)
            {
                cases.push_back({ clip, Pacing::WorkerWait, 0.0, 4.0 });
                for (double tickHz : { 30.0, 45.0, 60.0, 120.0 })
                    cases.push_back({ clip, Pacing::Deadline, tickHz, cDefaultWorkMs });
            }
            cases.push_back({ L"video_index_h264_720p60.mp4", Pacing::WorkerWait, 0.0, 4.0 });
            cases.push_back({ L"video_index_h264_720p60.mp4", Pacing::Deadline, 60.0, cDefaultWorkMs });
        }
        else
        {
            cases = {
                { L"video_index_h264_180p60.mp4", Pacing::WorkerWait, 0.0, 4.0 },
                { L"video_index_h264_180p30.mp4", Pacing::WorkerWait, 0.0, 4.0 },
                { L"video_index_h264_180p24.mp4", Pacing::Deadline, 60.0, cDefaultWorkMs },
                { L"video_index_h264_180p60.mp4", Pacing::Deadline, 30.0, cDefaultWorkMs },
                { L"video_index_h264_180p60.mp4", Pacing::Deadline, 60.0, cDefaultWorkMs },
                { L"video_index_h264_180p60.mp4", Pacing::Deadline, 120.0, cDefaultWorkMs },
                { L"video_index_h264_720p60.mp4", Pacing::Deadline, 60.0, cDefaultWorkMs },
            };
        }
        std::printf("  [info] %zu runs of %.0f s (%s matrix; SHADERLAB_VIDEO_MATRIX=full for all)\n",
            cases.size(), runSeconds, full ? "full" : "default");

        bool headerPrinted = false;
        bool allRan = true, allRecovered = true, allMatched = true, targetsMet = true;
        int runsDone = 0;
        for (const auto& testCase : cases)
        {
            const std::wstring path = FindVideoFixture(testCase.clip);
            if (path.empty())
            {
                std::printf("  [SKIP] %ls not found -- run Tests\\fixtures\\MakeVideoFixtures.ps1\n", testCase.clip);
                continue;
            }
            const PlaybackResult result = RunPlayback(testCase, path, runSeconds, dc, device, context);
            if (!result.error.empty())
            {
                std::printf("  [info] %ls %s: %s\n", testCase.clip, PacingLabel(testCase).c_str(), result.error.c_str());
                allRan = false;
                continue;
            }
            if (!headerPrinted) { ReportPlaybackHeader(); headerPrinted = true; }
            ReportPlaybackRow(testCase, result);
            std::printf("  [info]   first uploads: %s(timestamp offset %lld frames)\n",
                result.firstUploads.c_str(), result.timestampOffset);
            ++runsDone;
            if (result.distinctUploads < cMinUploadsPerRun) allRan = false;
            if (result.unreadable > 0) allRecovered = false;
            if (result.timestampMismatches > 0)
            {
                std::printf("  [info]   %d uploads whose pixels disagree with their timestamp\n",
                    result.timestampMismatches);
                allMatched = false;
            }
            const double tickFrames = result.achievedTickHz > 0.0 ? result.fps / result.achievedTickHz : 0.0;
            const bool met = std::abs(result.speed - 1.0) <= cTargetSpeedTolerance &&
                             result.finalLagFrames <= cTargetMaxFinalLagFrames + tickFrames &&
                             result.maxTimeLagFrames <= cTargetMaxTimeLagFrames &&
                             result.backwards <= cTargetMaxBackwardSteps;
            if (!met)
            {
                targetsMet = false;
                std::printf("  [info]   misses target (speed 1+-%.2f, final lag <= %.0f frames + one tick, "
                    "lag behind Time <= %.0f frames, no backward steps)\n",
                    cTargetSpeedTolerance, cTargetMaxFinalLagFrames, cTargetMaxTimeLagFrames);
            }
        }
        if (runsDone == 0)
            return;
        TEST("VideoPlayback_HarnessRan", allRan);
        TEST("VideoPlayback_FrameIndexRecoveredFromPixels", allRecovered);
        TEST("VideoPlayback_PixelsMatchUploadedFrameTime", allMatched);
        if (cEnforcePlaybackTargets)
            TIMING_TEST("VideoPlayback_TracksClock", targetsMet);
    }

    // Playback controls on the provider itself: a held time stays put, a
    // short forward jump decodes on, a long one or a backward one seeks,
    // Tick follows wall time and wraps, and Close returns with a full queue.
    namespace
    {
        // True when the uploaded frame is the one shown at `seconds`.
        bool ShowsTime(const Effects::VideoSourceProvider& video, double seconds)
        {
            constexpr double cTolerance = 1e-4;
            const double start = video.UploadedFrameTime();
            if (start < 0.0) return false;
            if (start <= video.FirstFrameTime() + cTolerance && seconds < start) return true;
            return seconds >= start - cTolerance && seconds < start + video.UploadedFrameDuration() - cTolerance;
        }

        // Uploads until the frame for `seconds` is up; the seconds it took, or -1.
        double UploadUntilShown(Effects::VideoSourceProvider& video, ID2D1DeviceContext5* dc, double seconds)
        {
            const auto start = Clock::now();
            while (SecondsBetween(start, Clock::now()) < 5.0)
            {
                video.UploadIfReady(dc);
                if (ShowsTime(video, seconds)) return SecondsBetween(start, Clock::now());
                ::Sleep(1);
            }
            return -1.0;
        }
    }

    void TestVideoPlaybackControl(ID2D1DeviceContext5* dc, ID3D11Device* device, ID3D11DeviceContext* context)
    {
        std::printf("\n=== Video playback controls (decode-ahead, drop, seek) ===\n");
        const std::wstring path = FindVideoFixture(L"video_index_h264_720p60.mp4");
        if (path.empty())
        {
            std::printf("  [SKIP] video_index_h264_720p60.mp4 not found -- run Tests\\fixtures\\MakeVideoFixtures.ps1\n");
            return;
        }
        Effects::VideoSourceProvider video;
        const bool opened = video.Open(path, dc, device, context);
        TEST("VideoControl_Opens", opened);
        if (!opened) return;
        const double frame = 1.0 / video.FrameRate();
        ::Sleep(100);   // let decode-ahead time a few reads first

        // A time reached by decoding forward from the start, then held.
        video.PlayTo(1.0);
        const bool reached = UploadUntilShown(video, dc, 1.0) >= 0.0;
        const uint64_t seeksAtHold = video.SeekCount();
        const uint64_t uploadsAtHold = video.UploadSuccesses();
        for (int i = 0; i < 60; ++i)
        {
            video.PlayTo(1.0);
            video.UploadIfReady(dc);
            ::Sleep(5);
        }
        TEST("VideoControl_ForwardDecodeReachesTime", reached && seeksAtHold == 0);
        TEST("VideoControl_HeldTimeHoldsFrame", ShowsTime(video, 1.0) && video.SeekCount() == seeksAtHold &&
                                                video.UploadSuccesses() == uploadsAtHold);

        // Ten frames ahead: under the seek threshold, so decoded forward.
        const double shortJump = 1.0 + 10.5 * frame;
        video.PlayTo(shortJump);
        const double shortSeconds = UploadUntilShown(video, dc, shortJump);
        TEST("VideoControl_ShortJumpDecodesForward", shortSeconds >= 0.0 && video.SeekCount() == seeksAtHold);

        // Into the next keyframe interval (keyframes every 2 s): one seek.
        const double longJump = 3.5;
        video.PlayTo(longJump);
        const double longSeconds = UploadUntilShown(video, dc, longJump);
        TEST("VideoControl_LongJumpSeeks", longSeconds >= 0.0 && video.SeekCount() == seeksAtHold + 1);

        // Sustained forward decode from here, for the cost comparison.
        const double decodeTarget = longJump + 60.5 * frame;
        video.PlayTo(decodeTarget);
        const double decodeSeconds = UploadUntilShown(video, dc, decodeTarget);
        const double threshold = video.SeekThresholdFrames();
        std::printf("  [info] 720p60: seek threshold %.0f frames (overhead %.1f ms / %.3f ms per read, mean preroll %.2f s); "
                    "10 frames forward %.1f ms, jump to %.1f s %.1f ms, 60 frames forward %.1f ms (%.2f ms/frame)\n",
            threshold, video.SeekOverheadSeconds() * 1000.0, video.DecodeSecondsPerFrame() * 1000.0,
            video.MeanSeekPrerollSeconds(), shortSeconds * 1000.0, longJump, longSeconds * 1000.0,
            decodeSeconds * 1000.0, decodeSeconds * 1000.0 / 60.0);

        // Backward: a seek, and the frame for the earlier time.
        const uint64_t seeksBeforeBack = video.SeekCount();
        video.PlayTo(2.0);
        const bool back = UploadUntilShown(video, dc, 2.0) >= 0.0;
        TEST("VideoControl_BackwardSeeks", back && video.SeekCount() == seeksBeforeBack + 1);

        // Forward within the keyframe interval the seek to 3.5 s landed in: a
        // seek would land behind the decode head, so the gap is decoded
        // through however long it is.
        const uint64_t seeksBeforeSameGop = video.SeekCount();
        const double sameGopTarget = 3.45;
        video.PlayTo(sameGopTarget);
        const bool sameGopShown = UploadUntilShown(video, dc, sameGopTarget) >= 0.0;
        std::printf("  [info] same-interval jump of %.0f frames against a %.0f-frame threshold: %llu seeks\n",
            (sameGopTarget - 2.0) / frame, video.SeekThresholdFrames(),
            static_cast<unsigned long long>(video.SeekCount() - seeksBeforeSameGop));
        TEST("VideoControl_SameKeyframeIntervalDecodesForward",
             sameGopShown && video.SeekCount() == seeksBeforeSameGop);
        std::printf("  [info] dropped %llu frames, %llu seeks, %llu decodes, %llu uploads\n",
            static_cast<unsigned long long>(video.DroppedFrames()), static_cast<unsigned long long>(video.SeekCount()),
            static_cast<unsigned long long>(video.DecodeCount()), static_cast<unsigned long long>(video.UploadSuccesses()));

        // Free-running Tick at 30 Hz: the frame follows wall time, not one frame per tick.
        video.Seek(0.0);
        UploadUntilShown(video, dc, 0.0);
        video.Play();
        DeadlineSleeper sleeper;
        const auto tickStart = Clock::now();
        auto last = tickStart;
        double firstShown = -1.0, firstElapsed = 0.0, lastShown = -1.0, lastElapsed = 0.0;
        for (int tick = 1; SecondsBetween(tickStart, Clock::now()) < 2.0; ++tick)
        {
            sleeper.SleepUntil(tickStart + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(tick / 30.0)));
            const auto now = Clock::now();
            video.Tick(SecondsBetween(last, now));
            last = now;
            if (video.UploadIfReady(dc))
            {
                const double elapsed = SecondsBetween(tickStart, now);
                if (elapsed < 0.3) continue;
                if (firstShown < 0.0) { firstShown = video.UploadedFrameTime(); firstElapsed = elapsed; }
                lastShown = video.UploadedFrameTime();
                lastElapsed = elapsed;
            }
        }
        const double tickSpeed = lastElapsed > firstElapsed ? (lastShown - firstShown) / (lastElapsed - firstElapsed) : 0.0;
        std::printf("  [info] Tick at 30 Hz on a 60 fps clip: video time / wall time %.3f\n", tickSpeed);
        TIMING_TEST("VideoControl_TickFollowsWallTime", std::abs(tickSpeed - 1.0) < 0.03);

        // Tick across the end of a looping clip wraps to the start.
        video.Seek(video.Duration() - 0.25);
        UploadUntilShown(video, dc, video.Duration() - 0.25);
        const auto wrapStart = Clock::now();
        last = wrapStart;
        double minShown = 1e9;
        for (int tick = 1; tick <= 30; ++tick)
        {
            sleeper.SleepUntil(wrapStart + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(tick / 60.0)));
            const auto now = Clock::now();
            video.Tick(SecondsBetween(last, now));
            last = now;
            if (video.UploadIfReady(dc)) minShown = (std::min)(minShown, video.UploadedFrameTime());
        }
        std::printf("  [info] Tick 0.5 s from 0.25 s before the end: earliest frame shown after %.3f s\n", minShown);
        TEST("VideoControl_TickLoopsAtEnd", minShown < 0.3);
        video.Close();

        // Close while the queue is full and nothing uploads.
        Effects::VideoSourceProvider idle;
        if (idle.Open(path, dc, device, context))
        {
            idle.PlayTo(0.5);
            ::Sleep(300);
            const auto closeStart = Clock::now();
            idle.Close();
            const double closeSeconds = SecondsBetween(closeStart, Clock::now());
            std::printf("  [info] Close with a full queue: %.1f ms\n", closeSeconds * 1000.0);
            TEST("VideoControl_CloseWithFullQueue", closeSeconds < 1.0 && idle.UploadedFrameTime() == -1.0);
        }
    }

    // Decoder surface pool probe: how many hardware-decoded samples a caller
    // can hold before ReadSample blocks waiting for a surface to come back.
    // Each read runs on a helper thread with a timeout; on a stall the held
    // samples are released, which lets the blocked read return.
    namespace
    {
        struct SurfacePoolProbe
        {
            int held{ 0 };          // samples held when the next read stalled
            bool stalled{ false };
            bool hardware{ false }; // every sample was a DXGI surface
            std::string error;
        };

        SurfacePoolProbe ProbeSurfacePool(const std::wstring& path, ID3D11Device* device, UINT extraOutputSamples)
        {
            SurfacePoolProbe probe;
            UINT resetToken = 0;
            winrt::com_ptr<IMFDXGIDeviceManager> manager;
            if (FAILED(MFCreateDXGIDeviceManager(&resetToken, manager.put())) ||
                FAILED(manager->ResetDevice(device, resetToken)))
            {
                probe.error = "DXGI device manager";
                return probe;
            }
            winrt::com_ptr<IMFAttributes> attributes;
            MFCreateAttributes(attributes.put(), 2);
            attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, manager.get());
            winrt::com_ptr<IMFSourceReader> reader;
            if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), attributes.get(), reader.put())))
            {
                probe.error = "source reader";
                return probe;
            }
            const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
            reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
            reader->SetStreamSelection(stream, TRUE);
            winrt::com_ptr<IMFMediaType> outputType;
            MFCreateMediaType(outputType.put());
            outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            if (FAILED(reader->SetCurrentMediaType(stream, nullptr, outputType.get())))
            {
                probe.error = "NV12 output";
                return probe;
            }
            if (extraOutputSamples > 0)
            {
                winrt::com_ptr<IMFSourceReaderEx> readerEx;
                winrt::com_ptr<IMFTransform> decoder;
                winrt::com_ptr<IMFAttributes> outputAttributes;
                GUID category{};
                if (!reader.try_as(readerEx) ||
                    FAILED(readerEx->GetTransformForStream(stream, 0, &category, decoder.put())) ||
                    FAILED(decoder->GetOutputStreamAttributes(0, outputAttributes.put())) ||
                    FAILED(outputAttributes->SetUINT32(MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT, extraOutputSamples)))
                {
                    probe.error = "MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT not settable";
                    return probe;
                }
            }

            constexpr int cMaxHeld = 64;
            probe.hardware = true;
            std::vector<winrt::com_ptr<IMFSample>> held;
            for (int i = 0; i < cMaxHeld; ++i)
            {
                winrt::com_ptr<IMFSample> sample;
                HRESULT readResult = E_FAIL;
                DWORD flags = 0;
                std::mutex mutex;
                std::condition_variable doneSignal;
                bool done = false;
                std::thread readThread([&]
                {
                    DWORD actualStream = 0;
                    LONGLONG timestamp = 0;
                    winrt::com_ptr<IMFSample> result;
                    const HRESULT hr = reader->ReadSample(stream, 0, &actualStream, &flags, &timestamp, result.put());
                    std::lock_guard lock(mutex);
                    readResult = hr;
                    sample = std::move(result);
                    done = true;
                    doneSignal.notify_one();
                });
                bool finished;
                {
                    std::unique_lock lock(mutex);
                    finished = doneSignal.wait_for(lock, std::chrono::seconds(2), [&] { return done; });
                }
                if (!finished)
                {
                    probe.stalled = true;
                    probe.held = static_cast<int>(held.size());
                    held.clear();
                    readThread.join();
                    return probe;
                }
                readThread.join();
                if (FAILED(readResult) || (flags & MF_SOURCE_READERF_ENDOFSTREAM) || !sample) break;
                winrt::com_ptr<IMFMediaBuffer> buffer;
                winrt::com_ptr<IMFDXGIBuffer> dxgiBuffer;
                if (FAILED(sample->GetBufferByIndex(0, buffer.put())) || !buffer.try_as(dxgiBuffer))
                    probe.hardware = false;
                held.push_back(std::move(sample));
            }
            probe.held = static_cast<int>(held.size());
            return probe;
        }
    }

    void TestVideoDecoderSurfacePool(ID3D11Device* device)
    {
        std::printf("\n=== Video decoder surface pool ===\n");
        bool anyRan = false;
        int smallestPool = 1000;
        for (const wchar_t* clip : { L"video_index_h264_180p60.mp4", L"video_index_h264_720p60.mp4",
                                     L"video_sdr_h264_30f.mp4", L"video_hdr10_hevc_30f.mp4" })
        {
            const std::wstring path = FindVideoFixture(clip);
            if (path.empty()) continue;
            for (UINT extra : { 0u, 8u })
            {
                const auto probe = ProbeSurfacePool(path, device, extra);
                if (!probe.error.empty())
                {
                    std::printf("  [info] %-28ls extra %u: %s\n", clip, extra, probe.error.c_str());
                    continue;
                }
                anyRan = true;
                if (probe.stalled) smallestPool = (std::min)(smallestPool, probe.held);
                std::printf("  [info] %-28ls MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT %u: %s after holding %d samples (%s)\n",
                    clip, extra, probe.stalled ? "ReadSample stalls" : "no stall", probe.held,
                    probe.hardware ? "DXGI surfaces" : "not all DXGI surfaces");
            }
        }
        TEST("VideoSurfacePool_ProbeRan", anyRan);
        // The provider holds its queue plus the frame being converted.
        std::printf("  [info] provider holds at most %zu samples; smallest pool measured %d\n",
            Effects::VideoSourceProvider::DecodeAheadFrames() + 1, smallestPool);
        TEST("VideoSurfacePool_QueueFitsPool",
             static_cast<int>(Effects::VideoSourceProvider::DecodeAheadFrames()) + 1 < smallestPool);
    }

    // Render worker pacing: the throttled loop waits on
    // RenderThreadDispatcher::WaitUntil(FramePacer::WaitDeadline) with nothing
    // queued. The fixed WaitFor(16 ms) it replaced is reported for comparison.
    void TestRenderLoopPacing()
    {
        std::printf("\n=== Render loop pacing ===\n");
        Rendering::RenderThreadDispatcher dispatcher;
        dispatcher.RegisterConsumer();
        DeadlineSleeper sleeper;

        struct PacingResult { double hz; double p50; double p90; double p95; double max; int iterations; };
        auto measure = [&](double workMs, double seconds, const std::function<void(Clock::time_point)>& pace)
        {
            std::vector<double> periods;
            const auto start = Clock::now();
            auto previous = start;
            int iterations = 0;
            while (true)
            {
                const auto now = Clock::now();
                if (SecondsBetween(start, now) >= seconds) break;
                if (iterations > 0) periods.push_back(SecondsBetween(previous, now) * 1000.0);
                previous = now;
                ++iterations;
                SpinFor(workMs);
                pace(start);
            }
            PacingResult result{};
            result.iterations = iterations;
            result.hz = iterations > 1 ? (iterations - 1) / SecondsBetween(start, previous) : 0.0;
            result.p50 = Percentile(periods, 0.50);
            result.p90 = Percentile(periods, 0.90);
            result.p95 = Percentile(periods, 0.95);
            result.max = periods.empty() ? 0.0 : *std::max_element(periods.begin(), periods.end());
            return result;
        };
        auto report = [](const char* label, const PacingResult& result)
        {
            std::printf("  [info] %-30s %7.1f %8.2f %8.2f %8.2f %8.2f\n",
                label, result.hz, result.p50, result.p90, result.p95, result.max);
        };

        std::printf("  [info] %-30s %7s %8s %8s %8s %8s\n", "loop", "Hz", "p50 ms", "p90 ms", "p95 ms", "max ms");
        const double periodMs = 1000.0 / cWorkerRateHz;
        bool ran = true, targetsMet = true;
        for (double workMs : { 0.0, 4.0, 10.0, 14.0 })
        {
            Rendering::FramePacer pacer(cWorkerRateHz);
            const auto result = measure(workMs, cPacingSeconds, [&](Clock::time_point)
            {
                dispatcher.WaitUntil(pacer.WaitDeadline(Clock::now()));
                pacer.BeginFrame(Clock::now());
            });
            char label[48];
            std::snprintf(label, sizeof(label), "worker pacer %u Hz + %.0fms", cWorkerRateHz, workMs);
            report(label, result);
            if (result.iterations < 10) ran = false;
            if (result.hz < cTargetPacingMinHz || std::abs(result.p50 - periodMs) > cTargetPacingP50ToleranceMs ||
                result.p90 > cTargetPacingP90Ms)
                targetsMet = false;
        }
        {
            // Work longer than the period: each frame starts after a short idle gap.
            Rendering::FramePacer pacer(cWorkerRateHz);
            const auto result = measure(25.0, cComparisonPacingSeconds, [&](Clock::time_point)
            {
                dispatcher.WaitUntil(pacer.WaitDeadline(Clock::now()));
                pacer.BeginFrame(Clock::now());
            });
            report("worker pacer 60 Hz + 25ms", result);
        }
        for (double workMs : { 0.0, 4.0, 10.0, 14.0 })
        {
            const auto result = measure(workMs, cComparisonPacingSeconds, [&](Clock::time_point)
            {
                dispatcher.WaitFor(std::chrono::milliseconds(cWorkerWaitMs));
            });
            char label[48];
            std::snprintf(label, sizeof(label), "old WaitFor(%d) + %.0fms", cWorkerWaitMs, workMs);
            report(label, result);
        }
        {
            long long frame = 0;
            const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / 60.0));
            const auto result = measure(4.0, cComparisonPacingSeconds, [&](Clock::time_point start)
            {
                sleeper.SleepUntil(start + period * ++frame);
            });
            report("reference: 60 Hz deadline + 4ms", result);
        }
        if (!targetsMet)
            std::printf("  [info] worker pacing misses target (>= %.0f Hz, p50 within %.1f ms of %.2f, "
                "p90 <= %.0f ms with work below a frame)\n",
                cTargetPacingMinHz, cTargetPacingP50ToleranceMs, periodMs, cTargetPacingP90Ms);
        TEST("RenderPacing_HarnessRan", ran);
        if (cEnforcePacingTargets)
            TIMING_TEST("RenderPacing_SixtyHz", targetsMet);

        // Frames that overrun the period, each taking an exclusive lock twice
        // the way the worker takes m_graphMutex, while another thread takes it
        // shared the way the UI thread does. Reported without the idle gap too.
        auto overrunLockWait = [&](bool idleGap)
        {
            std::shared_mutex mutex;
            std::atomic<bool> stop{ false };
            std::thread worker([&]
            {
                Rendering::RenderThreadDispatcher workerDispatcher;
                workerDispatcher.RegisterConsumer();
                Rendering::FramePacer pacer(cWorkerRateHz);
                while (!stop.load())
                {
                    workerDispatcher.WaitUntil(idleGap ? pacer.WaitDeadline(Clock::now()) : pacer.Deadline());
                    pacer.BeginFrame(Clock::now());
                    { std::unique_lock lock(mutex); SpinFor(12.0); }
                    { std::unique_lock lock(mutex); SpinFor(12.0); }
                }
            });
            std::vector<double> waitsMs;
            std::atomic<bool> readerDone{ false };
            std::thread reader([&]
            {
                for (int i = 0; i < 20; ++i)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(7));
                    const auto begin = Clock::now();
                    { std::shared_lock lock(mutex); }
                    waitsMs.push_back(SecondsBetween(begin, Clock::now()) * 1000.0);
                }
                readerDone = true;
            });
            // A reader that never gets the lock is released by stopping the worker.
            const auto deadline = Clock::now() + std::chrono::seconds(10);
            while (!readerDone.load() && Clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const bool finished = readerDone.load();
            stop = true;
            worker.join();
            reader.join();
            const double maxWait = waitsMs.empty() ? 0.0 : *std::max_element(waitsMs.begin(), waitsMs.end());
            return finished ? maxWait : 1e9;
        };
        const double withGap = overrunLockWait(true);
        const double withoutGap = overrunLockWait(false);
        std::printf("  [info] shared lock wait while 24 ms frames overrun: max %.1f ms with the idle gap, "
            "%.1f ms without\n", withGap, withoutGap);
        TIMING_TEST("RenderPacing_OverrunLetsReadersIn", withGap < cTargetOverrunLockWaitMs);
    }

    // Seeks while the target keeps moving: a Clock-bound video jumped to the
    // middle of a keyframe interval must seek once and catch up, not restart
    // the seek on every tick while it decodes up from the keyframe. A second
    // seek is allowed when the moving target passes a keyframe already known
    // to lie ahead before the first seek's frame arrives.
    void TestVideoSeekWhilePlaying(ID2D1DeviceContext5* dc, ID3D11Device* device, ID3D11DeviceContext* context)
    {
        std::printf("\n=== Video seeks while playing ===\n");
        struct SeekClip { const wchar_t* name; double maxCatchUpSeconds; };
        const SeekClip clips[] = {
            { L"video_index_h264_720p60.mp4", 1.0 },        // keyframes every 2 s
            { L"video_index_h264_720p60_g600.mp4", 3.0 },   // one keyframe in 10 s
        };
        // Mid-interval targets, backward and forward, leaving room to play on
        // before the end of the 10 s clips.
        const double jumps[] = { 5.5, 1.6, 6.6, 3.4, 0.8, 4.7, 2.5, 5.9 };
        constexpr int cMaxSeeksPerJump = 2;
        bool anyRan = false, allCaughtUp = true, fewSeeks = true, fastEnough = true;
        for (const auto& clip : clips)
        {
            const std::wstring path = FindVideoFixture(clip.name);
            if (path.empty())
            {
                std::printf("  [SKIP] %ls not found -- run Tests\\fixtures\\MakeVideoFixtures.ps1\n", clip.name);
                continue;
            }
            Effects::VideoSourceProvider video;
            if (!video.Open(path, dc, device, context))
            {
                std::printf("  [info] %ls did not open\n", clip.name);
                allCaughtUp = false;
                continue;
            }
            anyRan = true;
            ::Sleep(100);
            const double frame = 1.0 / video.FrameRate();
            DeadlineSleeper sleeper;
            int maxSeeks = 0, caughtUp = 0;
            double totalCatchUp = 0.0, maxCatchUp = 0.0;
            for (double jump : jumps)
            {
                const uint64_t seeksBefore = video.SeekCount();
                const double maxElapsed = (std::min)(5.0, video.Duration() - jump - 0.1);
                const auto jumpStart = Clock::now();
                double catchUp = -1.0;
                for (int tick = 0;; ++tick)
                {
                    sleeper.SleepUntil(jumpStart + std::chrono::duration_cast<Clock::duration>(
                        std::chrono::duration<double>(tick / 60.0)));
                    const double elapsed = SecondsBetween(jumpStart, Clock::now());
                    const double target = jump + elapsed;
                    video.PlayTo(target);
                    video.UploadIfReady(dc);
                    const double shown = video.UploadedFrameTime();
                    if (catchUp < 0.0 && shown >= target - 2.5 * frame && shown <= target + 0.5 * frame)
                        catchUp = elapsed;
                    // Keep playing a little after catching up.
                    if ((catchUp >= 0.0 && elapsed > catchUp + 0.25) || elapsed > maxElapsed) break;
                }
                const int seeks = static_cast<int>(video.SeekCount() - seeksBefore);
                if (seeks > 1)
                    std::printf("  [info] %ls jump to %.1f s took %d seeks\n", clip.name, jump, seeks);
                maxSeeks = (std::max)(maxSeeks, seeks);
                if (catchUp >= 0.0)
                {
                    ++caughtUp;
                    totalCatchUp += catchUp;
                    maxCatchUp = (std::max)(maxCatchUp, catchUp);
                }
                else
                {
                    std::printf("  [info] %ls jump to %.1f s never caught up (frame on screen %.3f s, %d seeks)\n",
                        clip.name, jump, video.UploadedFrameTime(), seeks);
                }
            }
            const int jumpCount = static_cast<int>(std::size(jumps));
            std::printf("  [info] %-34ls %d of %d jumps caught up, mean %.0f ms, max %.0f ms; at most %d seeks per jump "
                "(%llu in all); %zu keyframes learned, mean preroll %.2f s, threshold %.0f frames\n",
                clip.name, caughtUp, jumpCount, caughtUp ? totalCatchUp * 1000.0 / caughtUp : 0.0, maxCatchUp * 1000.0,
                maxSeeks, static_cast<unsigned long long>(video.SeekCount()), video.KnownKeyframeCount(),
                video.MeanSeekPrerollSeconds(), video.SeekThresholdFrames());
            if (caughtUp != jumpCount) allCaughtUp = false;
            if (maxSeeks > cMaxSeeksPerJump) fewSeeks = false;
            if (maxCatchUp > clip.maxCatchUpSeconds) fastEnough = false;

            // The long-interval clip: a forward gap inside the span a seek
            // already decoded through is decoded, not sought, however long.
            if (std::wstring(clip.name).find(L"_g600") != std::wstring::npos)
            {
                video.Seek(9.5);
                UploadUntilShown(video, dc, 9.5);
                video.PlayTo(1.0);
                UploadUntilShown(video, dc, 1.0);
                const uint64_t seeksBefore = video.SeekCount();
                video.PlayTo(8.0);
                const bool shown = UploadUntilShown(video, dc, 8.0) >= 0.0;
                TEST("VideoSeek_LongIntervalDecodesForward", shown && video.SeekCount() == seeksBefore);
            }
        }
        if (!anyRan) return;
        TEST("VideoSeek_EveryJumpCatchesUp", allCaughtUp);
        TEST("VideoSeek_NoSeekStorm", fewSeeks);
        TIMING_TEST("VideoSeek_CatchUpTime", fastEnough);
    }

    // A non-looping video whose Time is past its end shows opaque black, and
    // its frame again once Time comes back.
    void TestVideoPastEndShowsBlack(ID2D1DeviceContext5* dc, ID3D11Device* device, ID3D11DeviceContext* context)
    {
        std::printf("\n=== Video past the end ===\n");
        const std::wstring path = FindVideoFixture(L"video_index_h264_180p30.mp4");
        if (path.empty())
        {
            std::printf("  [SKIP] video_index_h264_180p30.mp4 not found -- run Tests\\fixtures\\MakeVideoFixtures.ps1\n");
            return;
        }
        Effects::SourceNodeFactory factory;
        std::vector<Graph::EffectNode> nodes;
        nodes.push_back(Effects::SourceNodeFactory::CreateVideoSourceNode(path));
        nodes.back().id = 1;
        nodes.back().properties[L"Loop"] = false;
        auto& node = nodes.back();
        factory.PrepareSourceNode(node, dc, 0.0, device, context);
        auto* provider = factory.GetVideoProvider(node.id);
        TEST("VideoPastEnd_Opens", provider != nullptr);
        if (!provider) return;
        const UINT width = provider->FrameWidth();
        const UINT height = provider->FrameHeight();

        // Brightest value and lowest alpha along the bar row of the node's image.
        auto readRow = [&](float& maxColor, float& minAlpha)
        {
            maxColor = -1.0f;
            minAlpha = -1.0f;
            if (!node.cachedOutput) return false;
            D2D1_BITMAP_PROPERTIES1 targetProps{};
            targetProps.pixelFormat = { DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
            targetProps.dpiX = targetProps.dpiY = 96.0f;
            targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
            D2D1_BITMAP_PROPERTIES1 readProps = targetProps;
            readProps.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
            winrt::com_ptr<ID2D1Bitmap1> target, readback;
            if (FAILED(dc->CreateBitmap({ width, height }, nullptr, 0, targetProps, target.put())) ||
                FAILED(dc->CreateBitmap({ width, height }, nullptr, 0, readProps, readback.put())))
                return false;
            dc->SetTarget(target.get());
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(0.5f, 0.5f, 0.5f, 0.0f));
            dc->DrawImage(node.cachedOutput);
            const HRESULT drawResult = dc->EndDraw();
            dc->SetTarget(nullptr);
            if (FAILED(drawResult)) return false;
            const D2D1_POINT_2U origin{ 0, 0 };
            const D2D1_RECT_U all{ 0, 0, width, height };
            D2D1_MAPPED_RECT mapped{};
            if (FAILED(readback->CopyFromBitmap(&origin, target.get(), &all)) ||
                FAILED(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped)))
                return false;
            const auto* row = reinterpret_cast<const uint16_t*>(mapped.bits + static_cast<size_t>(height / 4) * mapped.pitch);
            maxColor = 0.0f;
            minAlpha = 1e9f;
            for (UINT x = 0; x < width; ++x)
            {
                for (int channel = 0; channel < 3; ++channel)
                    maxColor = (std::max)(maxColor, HalfToFloat(row[x * 4 + channel]));
                minAlpha = (std::min)(minAlpha, HalfToFloat(row[x * 4 + 3]));
            }
            readback->Unmap();
            return true;
        };

        auto showTime = [&](float seconds)
        {
            node.properties[L"Time"] = seconds;
            for (int i = 0; i < 200; ++i)
            {
                factory.TickAndUploadVideos(nodes, dc, 0.0);
                if (provider->UploadedFrameTime() >= 0.0 &&
                    std::abs(provider->UploadedFrameTime() - seconds) < 0.1)
                    break;
                ::Sleep(5);
            }
            factory.TickAndUploadVideos(nodes, dc, 0.0);
            factory.PrepareSourceNode(node, dc, 0.0, device, context);
        };

        float maxColor = 0.0f, minAlpha = 0.0f;
        showTime(9.0f);
        const bool frameRead = readRow(maxColor, minAlpha);
        std::printf("  [info] Time 9 s: brightest %.3f\n", maxColor);
        TEST("VideoPastEnd_FrameBeforeEnd", frameRead && maxColor > 0.3f);

        node.properties[L"Time"] = static_cast<float>(provider->Duration() + 1.0);
        factory.TickAndUploadVideos(nodes, dc, 0.0);
        factory.PrepareSourceNode(node, dc, 0.0, device, context);
        const bool blackRead = readRow(maxColor, minAlpha);
        std::printf("  [info] Time past the end: brightest %.4f, alpha %.3f\n", maxColor, minAlpha);
        TEST("VideoPastEnd_ShowsOpaqueBlack", blackRead && node.cachedOutput != provider->CurrentBitmap() &&
                                              maxColor < 1e-3f && minAlpha > 0.999f);

        showTime(2.0f);
        TEST("VideoPastEnd_FrameAgainAfterReturn", node.cachedOutput == provider->CurrentBitmap() &&
                                                   readRow(maxColor, minAlpha) && maxColor > 0.3f);
    }

    // Downstream redraws of a Clock-bound video
    namespace
    {
        constexpr double cRedrawRunSeconds = 3.0;
        constexpr double cRedrawTickHz = 60.0;
        constexpr double cRedrawSettleSeconds = 3.0;
        constexpr uint64_t cRedrawCountTolerance = 2;

        struct RedrawCase
        {
            const wchar_t* clip;
            bool clockPlaying;
        };

        struct RedrawResult
        {
            std::string error;
            int ticks{ 0 };
            double seconds{ 0 };
            uint64_t uploads{ 0 };
            uint64_t imageRedraws{ 0 };   // Nit Map reading the video's image
            uint64_t fieldRedraws{ 0 };   // Nit Map whose Opacity is bound to the video's FrameTime
            int compared{ 0 };
            int staleTicks{ 0 };          // image consumer showing a frame other than the video's
            int unreadable{ 0 };
            int fieldMismatches{ 0 };     // bound Opacity differing from the video's FrameTime
            double statusFps{ -1.0 };     // PreviewRenderStats at the end of the run, as the status bar reads it
            double statusMs{ -1.0 };
        };

        // Clock -> video -> pass-through Nit Map, plus a second Nit Map bound to
        // the video's FrameTime, ticked like RenderWorkerLoop at 60 Hz: advance
        // the Clock, resolve source bindings, upload videos, push dirty along
        // edges, prepare sources, evaluate, draw. Counts are after warmup.
        RedrawResult RunRedraws(const RedrawCase& testCase, const std::wstring& path,
                                ID2D1DeviceContext5* dc, ID3D11Device* device, ID3D11DeviceContext* context)
        {
            using Effects::SourceNodeFactory;
            using Effects::ShaderLabEffects;
            RedrawResult result;
            const auto& library = ShaderLabEffects::Instance();
            const auto* clockDescriptor = library.FindByName(L"Clock");
            const auto* nitMapDescriptor = library.FindByName(L"Nit Map");
            const auto* patternDescriptor = library.FindByName(L"Gamut Source");
            if (!clockDescriptor || !nitMapDescriptor || !patternDescriptor)
            {
                result.error = "missing Clock, Nit Map or Gamut Source";
                return result;
            }

            SourceNodeFactory factory;
            Rendering::GraphEvaluator evaluator;
            Graph::EffectGraph graph;
            const uint32_t clockId = graph.AddNode(ShaderLabEffects::CreateNode(*clockDescriptor));
            const uint32_t videoId = graph.AddNode(SourceNodeFactory::CreateVideoSourceNode(path));
            const uint32_t imageConsumerId = graph.AddNode(ShaderLabEffects::CreateNode(*nitMapDescriptor));
            const uint32_t patternId = graph.AddNode(ShaderLabEffects::CreateNode(*patternDescriptor));
            const uint32_t fieldConsumerId = graph.AddNode(ShaderLabEffects::CreateNode(*nitMapDescriptor));
            graph.FindNode(clockId)->properties[L"AutoDuration"] = 0.0f;
            graph.FindNode(clockId)->properties[L"StopTime"] = 3600.0f;
            graph.FindNode(clockId)->clockTime = 1.0;
            // Nit Map at zero opacity passes its input through.
            graph.FindNode(imageConsumerId)->properties[L"Opacity"] = 0.0f;
            graph.Connect(videoId, 0, imageConsumerId, 0);
            graph.Connect(patternId, 0, fieldConsumerId, 0);
            if (!graph.BindProperty(videoId, L"Time", clockId, L"Time", 0).empty())
            {
                result.error = "BindProperty Time failed";
                return result;
            }

            auto& nodes = const_cast<std::vector<Graph::EffectNode>&>(graph.Nodes());
            auto prepareSources = [&](double deltaSeconds)
            {
                for (auto& node : nodes)
                    if (node.type == Graph::NodeType::Source && (node.dirty || factory.GetVideoProvider(node.id)))
                        factory.PrepareSourceNode(node, dc, deltaSeconds, device, context);
            };
            // RenderWorkerLoop's downstream push of dirty along edges.
            auto propagateDirty = [&]
            {
                std::vector<uint32_t> queue;
                for (const auto& node : graph.Nodes())
                    if (node.dirty) queue.push_back(node.id);
                for (size_t i = 0; i < queue.size(); ++i)
                    for (const auto* edge : graph.GetOutputEdges(queue[i]))
                        if (auto* downstream = graph.FindNode(edge->destNodeId); downstream && !downstream->dirty)
                        {
                            downstream->dirty = true;
                            queue.push_back(edge->destNodeId);
                        }
            };
            auto frame = [&](double deltaSeconds)
            {
                evaluator.ResolveSourceBindings(graph);
                factory.TickAndUploadVideos(nodes, dc, deltaSeconds);
                propagateDirty();
                prepareSources(deltaSeconds);
                evaluator.Evaluate(graph, dc);
            };

            // Settle: the video open with its first frame up and both Nit Maps rendering.
            prepareSources(0.0);
            auto* provider = factory.GetVideoProvider(videoId);
            if (!provider || !provider->OutputTexture())
            {
                result.error = "video did not open";
                return result;
            }
            const auto settleStart = Clock::now();
            while (SecondsBetween(settleStart, Clock::now()) < cRedrawSettleSeconds)
            {
                frame(0.0);
                if (provider->UploadedFrameTime() >= 0.0 && graph.FindNode(imageConsumerId)->cachedOutput &&
                    graph.FindNode(fieldConsumerId)->cachedOutput)
                    break;
                ::Sleep(5);
            }
            if (provider->UploadedFrameTime() < 0.0 || !graph.FindNode(imageConsumerId)->cachedOutput)
            {
                result.error = "graph did not settle";
                return result;
            }
            // FrameTime exists once the video has been ticked.
            if (!graph.BindProperty(fieldConsumerId, L"Opacity", videoId, L"FrameTime", 0).empty())
            {
                result.error = "BindProperty FrameTime failed";
                return result;
            }

            const UINT width = provider->FrameWidth();
            const UINT height = provider->FrameHeight();
            D3D11_TEXTURE2D_DESC targetDesc{};
            targetDesc.Width = width;
            targetDesc.Height = height;
            targetDesc.MipLevels = 1;
            targetDesc.ArraySize = 1;
            targetDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            targetDesc.SampleDesc.Count = 1;
            targetDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            winrt::com_ptr<ID3D11Texture2D> targetTexture;
            winrt::com_ptr<ID2D1Bitmap1> drawTarget;
            D2D1_BITMAP_PROPERTIES1 targetProps{};
            targetProps.pixelFormat = { DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
            targetProps.dpiX = targetProps.dpiY = 96.0f;
            targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
            if (FAILED(device->CreateTexture2D(&targetDesc, nullptr, targetTexture.put())) ||
                FAILED(dc->CreateBitmapFromDxgiSurface(targetTexture.as<IDXGISurface>().get(), targetProps, drawTarget.put())))
            {
                result.error = "draw target allocation failed";
                return result;
            }

            // Each tick copies the bar rows of the video's frame and of the
            // drawn consumer output; they are read after the run.
            const size_t poolSize = static_cast<size_t>((cWarmupSeconds + cRedrawRunSeconds) * cRedrawTickHz * 1.25) + 64;
            D3D11_TEXTURE2D_DESC stagingDesc = targetDesc;
            stagingDesc.Height = 2;
            stagingDesc.BindFlags = 0;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            std::vector<winrt::com_ptr<ID3D11Texture2D>> videoRows(poolSize), shownRows(poolSize);
            for (size_t i = 0; i < poolSize; ++i)
                if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, videoRows[i].put())) ||
                    FAILED(device->CreateTexture2D(&stagingDesc, nullptr, shownRows[i].put())))
                {
                    result.error = "staging allocation failed";
                    return result;
                }
            const D3D11_BOX topBox{ 0, height / 4, 0, width, height / 4 + 1, 1 };
            const D3D11_BOX bottomBox{ 0, height * 3 / 4, 0, width, height * 3 / 4 + 1, 1 };
            auto copyRows = [&](ID3D11Texture2D* staging, ID3D11Texture2D* source)
            {
                context->CopySubresourceRegion(staging, 0, 0, 0, 0, source, 0, &topBox);
                context->CopySubresourceRegion(staging, 0, 0, 1, 0, source, 0, &bottomBox);
            };

            DeadlineSleeper sleeper;
            graph.FindNode(clockId)->isPlaying = testCase.clockPlaying;
            const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / cRedrawTickHz));
            const auto start = Clock::now();
            auto last = start;
            bool measuring = false;
            uint64_t uploadsAtStart = 0, imageAtStart = 0, fieldAtStart = 0;
            Clock::time_point measureStart{};
            // The status bar's numbers for the image consumer, bracketed as RenderFrameToOffscreen does.
            Rendering::PreviewRenderStats previewStats;
            previewStats.Initialize(device);
            previewStats.SetNode(imageConsumerId);
            std::vector<bool> measuredTick;
            for (long long tick = 0; tick < static_cast<long long>(poolSize); ++tick)
            {
                sleeper.SleepUntil(start + period * tick);
                const auto now = Clock::now();
                const double elapsed = SecondsBetween(start, now);
                if (elapsed >= cWarmupSeconds + cRedrawRunSeconds) break;
                if (!measuring && elapsed >= cWarmupSeconds)
                {
                    measuring = true;
                    measureStart = now;
                    uploadsAtStart = provider->UploadSuccesses();
                    imageAtStart = evaluator.OutputChangeCount(imageConsumerId);
                    fieldAtStart = evaluator.OutputChangeCount(fieldConsumerId);
                }
                double deltaSeconds = SecondsBetween(last, now);
                last = now;
                if (deltaSeconds > 0.1) deltaSeconds = 1.0 / cRedrawTickHz;

                if (testCase.clockPlaying)
                    AdvanceClock(*graph.FindNode(clockId), deltaSeconds);
                previewStats.BeginFrame(Clock::now());
                const uint64_t changesBefore = evaluator.OutputChangeCount(imageConsumerId);
                frame(deltaSeconds);

                dc->SetTarget(drawTarget.get());
                dc->BeginDraw();
                dc->Clear(D2D1::ColorF(0, 0, 0, 0));
                if (auto* consumer = graph.FindNode(imageConsumerId); consumer && consumer->cachedOutput)
                    dc->DrawImage(consumer->cachedOutput);
                dc->EndDraw();
                dc->SetTarget(nullptr);
                copyRows(videoRows[tick].get(), provider->OutputTexture());
                copyRows(shownRows[tick].get(), targetTexture.get());
                measuredTick.push_back(measuring);
                previewStats.EndFrame(evaluator.OutputChangeCount(imageConsumerId) != changesBefore, Clock::now());
                previewStats.Publish(Clock::now());
                result.statusFps = previewStats.Fps();
                result.statusMs = previewStats.Ms();

                const auto* video = graph.FindNode(videoId);
                const auto* fieldConsumer = graph.FindNode(fieldConsumerId);
                float frameTime = -1.0f;
                for (const auto& field : video->analysisOutput.fields)
                    if (field.name == L"FrameTime") frameTime = field.components[0];
                if (measuring && NodeFloat(*fieldConsumer, L"Opacity", -2.0f) != frameTime)
                    ++result.fieldMismatches;
            }
            if (!measuring)
            {
                result.error = "run ended before warmup";
                return result;
            }
            result.seconds = SecondsBetween(measureStart, last);
            result.uploads = provider->UploadSuccesses() - uploadsAtStart;
            result.imageRedraws = evaluator.OutputChangeCount(imageConsumerId) - imageAtStart;
            result.fieldRedraws = evaluator.OutputChangeCount(fieldConsumerId) - fieldAtStart;

            for (size_t tick = 0; tick < measuredTick.size(); ++tick)
            {
                if (!measuredTick[tick]) continue;
                ++result.ticks;
                const int videoIndex = ReadFrameIndex(context, videoRows[tick].get(), width);
                const int shownIndex = ReadFrameIndex(context, shownRows[tick].get(), width);
                if (videoIndex < 0 || shownIndex < 0)
                {
                    ++result.unreadable;
                    continue;
                }
                ++result.compared;
                if (shownIndex != videoIndex) ++result.staleTicks;
            }
            return result;
        }
    }

    // A Clock-bound video counts as changed only when a frame is uploaded, so
    // the nodes reading its image re-render at the clip's frame rate rather
    // than at the tick rate. Nodes bound to its analysis fields still follow them.
    void TestVideoDownstreamRedraws(ID2D1DeviceContext5* dc, ID3D11Device* device, ID3D11DeviceContext* context)
    {
        std::printf("\n=== Video downstream redraws (Clock-driven, %.0f Hz ticks) ===\n", cRedrawTickHz);
        const RedrawCase cases[] = {
            { L"video_index_h264_180p24.mp4", true },
            { L"video_index_h264_180p60.mp4", true },
            { L"video_index_h264_180p24.mp4", false },
        };
        std::printf("  [info] %-28s %-7s %5s %9s %9s %9s %9s %5s %10s %9s\n", "clip", "clock", "ticks",
            "uploads/s", "image/s", "field/s", "ticks/img", "stale", "status fps", "status ms");
        bool allRan = true, noneStale = true, imageFollowsUploads = true, pausedIdle = true;
        bool fieldsTrack = true, ratesMet = true;
        bool statusNotApplicable = true, statusRatesMet = true, statusTimed = true;
        int runsDone = 0;
        for (const auto& testCase : cases)
        {
            const std::wstring path = FindVideoFixture(testCase.clip);
            if (path.empty())
            {
                std::printf("  [SKIP] %ls not found -- run Tests\\fixtures\\MakeVideoFixtures.ps1\n", testCase.clip);
                continue;
            }
            const RedrawResult result = RunRedraws(testCase, path, dc, device, context);
            if (!result.error.empty())
            {
                std::printf("  [info] %ls: %s\n", testCase.clip, result.error.c_str());
                allRan = false;
                continue;
            }
            ++runsDone;
            const double seconds = result.seconds > 0.0 ? result.seconds : 1.0;
            std::printf("  [info] %-28ls %-7s %5d %9.1f %9.1f %9.1f %9.2f %5d %10s %9s\n", testCase.clip,
                testCase.clockPlaying ? "playing" : "paused", result.ticks, result.uploads / seconds,
                result.imageRedraws / seconds, result.fieldRedraws / seconds,
                result.imageRedraws > 0 ? static_cast<double>(result.ticks) / result.imageRedraws : 0.0,
                result.staleTicks, FormatStat(result.statusFps, "%.1f").c_str(),
                FormatStat(result.statusMs, "%.2f").c_str());
            if (result.compared < result.ticks / 2) allRan = false;
            if (result.staleTicks > 0 || result.unreadable > 0) noneStale = false;
            const uint64_t difference = result.imageRedraws > result.uploads
                ? result.imageRedraws - result.uploads : result.uploads - result.imageRedraws;
            if (difference > cRedrawCountTolerance) imageFollowsUploads = false;
            if (result.fieldMismatches > 0 || result.fieldRedraws + cRedrawCountTolerance < result.uploads)
                fieldsTrack = false;
            if (!testCase.clockPlaying)
            {
                if (result.imageRedraws > 1) pausedIdle = false;
                if (result.statusFps >= 0.0 || result.statusMs >= 0.0) statusNotApplicable = false;
                continue;
            }
            const double expectedRate = std::wstring(testCase.clip).find(L"p24") != std::wstring::npos ? 24.0 : 60.0;
            const double imageRate = result.imageRedraws / seconds;
            if (imageRate < expectedRate * 0.8 || imageRate > expectedRate * 1.05) ratesMet = false;
            if (result.statusFps < expectedRate * 0.8 || result.statusFps > expectedRate * 1.05) statusRatesMet = false;
            if (!(result.statusMs > 0.0)) statusTimed = false;
        }
        if (runsDone == 0)
            return;
        TEST("VideoRedraw_HarnessRan", allRan);
        TEST("VideoRedraw_DownstreamShowsCurrentFrame", noneStale);
        TEST("VideoRedraw_ImageConsumerFollowsUploads", imageFollowsUploads);
        TEST("VideoRedraw_PausedClockIdle", pausedIdle);
        TEST("VideoRedraw_FieldBindingTracksFrameTime", fieldsTrack);
        TIMING_TEST("VideoRedraw_ClipFrameRate", ratesMet);
        TEST("VideoRedraw_StatusNotApplicableWhenPaused", statusNotApplicable);
        TEST("VideoRedraw_StatusTimesRenders", statusTimed);
        TIMING_TEST("VideoRedraw_StatusFpsIsClipRate", statusRatesMet);
    }
}

namespace ShaderLab::Tests
{
    // The status bar's render rate and frame time: the rate meter on a fake
    // clock, and the cost of the whole-frame GPU timestamps it reads.
    void TestPreviewRenderStats(ID2D1DeviceContext5* dc, ID3D11Device* device)
    {
        std::printf("\n=== Preview render rate and frame time ===\n");
        using Rendering::RenderRateMeter;
        using Clock = RenderRateMeter::Clock;
        const Clock::time_point origin = Clock::time_point{} + std::chrono::seconds(100);
        auto at = [&](double seconds)
        {
            return origin + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
        };
        auto closeTo = [](std::optional<double> rate, double expected, double tolerance)
        {
            return rate.has_value() && std::abs(*rate - expected) <= tolerance;
        };

        {
            RenderRateMeter meter;
            TEST("RenderRate_NoRendersIsNA", !meter.Rate(origin).has_value());
            for (int i = 0; i <= 48; ++i)
                meter.RecordRender(at(i / 24.0));
            TEST("RenderRate_Steady24", closeTo(meter.Rate(at(2.0 + 0.01)), 24.0, 0.01));
            TEST("RenderRate_StoppedDecays", closeTo(meter.Rate(at(2.5)), 13.0, 1.0));
            TEST("RenderRate_StoppedIsNAAfterWindow", !meter.Rate(at(3.01)).has_value());
            meter.RecordRender(at(10.0));
            TEST("RenderRate_SingleRenderCountsOverWindow", closeTo(meter.Rate(at(10.2)), 1.0, 1e-9));
            TEST("RenderRate_SingleRenderAgesOut", !meter.Rate(at(11.1)).has_value());
            meter.Reset();
            TEST("RenderRate_ResetIsNA", !meter.Rate(at(10.2)).has_value());
        }
        {
            // 60 fps with +-2 ms of jitter on each render.
            RenderRateMeter meter;
            for (int i = 0; i <= 120; ++i)
                meter.RecordRender(at(i / 60.0 + ((i * 7) % 5 - 2) * 0.001));
            TEST("RenderRate_Jittered60", closeTo(meter.Rate(at(2.0 + 0.005)), 60.0, 1.0));
        }
        {
            // A graph rendering every 2 s still has a rate until a render is overdue.
            RenderRateMeter meter;
            for (int i = 0; i <= 3; ++i)
                meter.RecordRender(at(i * 2.0));
            TEST("RenderRate_SlowGraph", closeTo(meter.Rate(at(6.5)), 0.5, 1e-9));
            TEST("RenderRate_SlowGraphOverdueIsNA", !meter.Rate(at(8.5)).has_value());
        }

        // Timer cost: the same small draw with and without the timestamps.
        Rendering::GpuFrameTimer timer;
        const bool timerReady = timer.Initialize(device);
        TEST("GpuFrameTimer_Initializes", timerReady);
        if (!timerReady) return;
        winrt::com_ptr<ID2D1Bitmap1> target;
        D2D1_BITMAP_PROPERTIES1 targetProps{};
        targetProps.pixelFormat = { DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        targetProps.dpiX = targetProps.dpiY = 96.0f;
        targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
        winrt::com_ptr<ID2D1Effect> flood;
        if (FAILED(dc->CreateBitmap({ 512, 512 }, nullptr, 0, targetProps, target.put())) ||
            FAILED(dc->CreateEffect(CLSID_D2D1Flood, flood.put())))
        {
            TEST("GpuFrameTimer_DrawSetup", false);
            return;
        }
        constexpr int cFrames = 400;
        auto drawFrames = [&](bool timed)
        {
            const auto start = Clock::now();
            for (int i = 0; i < cFrames; ++i)
            {
                const auto frameStart = Clock::now();
                if (timed) timer.BeginFrame();
                flood->SetValue(D2D1_FLOOD_PROP_COLOR, D2D1_VECTOR_4F{ (i % 7) / 7.0f, 0.5f, 0.25f, 1.0f });
                dc->SetTarget(target.get());
                dc->BeginDraw();
                dc->DrawImage(flood.get());
                dc->EndDraw();
                dc->SetTarget(nullptr);
                if (timed)
                {
                    timer.EndFrame(true, std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count());
                    timer.Collect();
                }
            }
            return std::chrono::duration<double, std::micro>(Clock::now() - start).count() / cFrames;
        };
        drawFrames(false);   // warm up
        double untimedUs = 1e9, timedUs = 1e9;
        for (int round = 0; round < 3; ++round)
        {
            untimedUs = (std::min)(untimedUs, drawFrames(false));
            timedUs = (std::min)(timedUs, drawFrames(true));
        }
        for (int i = 0; i < 50 && timer.KeptFrames() == 0; ++i)
        {
            ::Sleep(2);
            timer.Collect();
        }
        const auto last = timer.Last();
        const std::optional<double> lastMs = last ? std::optional<double>(last->gpuMs) : std::nullopt;
        std::printf("  [info] %d small draws: %.1f us/frame untimed, %.1f us/frame with frame timestamps "
            "(%+.1f us); %llu frames timed, last %.3f ms\n", cFrames, untimedUs, timedUs, timedUs - untimedUs,
            static_cast<unsigned long long>(timer.KeptFrames()), lastMs ? *lastMs : -1.0);
        TEST("GpuFrameTimer_ReportsFrames", timer.KeptFrames() > 0 && lastMs && *lastMs > 0.0);
        TIMING_TEST("GpuFrameTimer_CheapPerFrame", timedUs - untimedUs < 250.0);

        // 2 ms of CPU work inside the frame must show up in its total, whether
        // or not this driver runs the start timestamp at frame start.
        constexpr double cCpuWorkMs = 2.0;
        // The cost loop above outran the GPU and left every slot in flight;
        // paced frames retire them so the spun frames get slots.
        for (int i = 0; i < 20; ++i)
        {
            ::Sleep(2);
            timer.BeginFrame();
            timer.EndFrame(false, 0.0);
        }
        for (int i = 0; i < 20; ++i)
        {
            const auto frameStart = Clock::now();
            timer.BeginFrame();
            SpinFor(cCpuWorkMs);
            dc->SetTarget(target.get());
            dc->BeginDraw();
            dc->DrawImage(flood.get());
            dc->EndDraw();
            dc->SetTarget(nullptr);
            timer.EndFrame(true, std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count());
        }
        // Later frames follow, as in the render loop; some drivers retire a
        // frame's queries only once more work is submitted. Earlier frames
        // can still be retiring, so wait for one of the spun ones.
        auto spunRetired = [&] { auto frame = timer.Last(); return frame && frame->cpuMs >= cCpuWorkMs; };
        for (int i = 0; i < 100 && !spunRetired(); ++i)
        {
            ::Sleep(2);
            timer.BeginFrame();
            timer.EndFrame(false, 0.0);
        }
        const auto spun = timer.Last();
        std::printf("  [info] timer: %llu kept, %llu disjoint\n", static_cast<unsigned long long>(timer.KeptFrames()),
            static_cast<unsigned long long>(timer.DisjointFrames()));
        std::printf("  [info] frame with %.1f ms of CPU work inside: %.3f ms total (CPU %.3f, GPU span %.3f)\n",
            cCpuWorkMs, spun ? spun->totalMs : -1.0, spun ? spun->cpuMs : -1.0, spun ? spun->gpuMs : -1.0);
        TEST("GpuFrameTimer_SpansFrameStartToGpuDone",
            spun && spun->totalMs >= cCpuWorkMs && spun->totalMs < cCpuWorkMs + 3.0);
    }
}

namespace ShaderLab::Tests
{
    namespace
    {
        // Per-tick tolerances for the tick-work test.
        constexpr int cTickWorkMaxUploadPasses = 3;
        constexpr double cTickWorkMaxIdleTickMs = 1.0;      // video tick + source prep, no new frame
        constexpr double cTickWorkMaxUploadTickMs = 5.0;    // video tick with one 180p upload
        constexpr uint64_t cTickWorkCountTolerance = 2;

        std::string EnvString(const char* name)
        {
            char buffer[MAX_PATH]{};
            const DWORD length = ::GetEnvironmentVariableA(name, buffer, sizeof(buffer));
            return length > 0 && length < sizeof(buffer) ? std::string(buffer) : std::string();
        }

        struct FrameCostOptions
        {
            double runSeconds{ 4.0 };
            bool zeroCopy{ true };
            bool clockPlaying{ true };
            // The GUI's settings: async compile, async analysis readback and
            // skipping unneeded readbacks.
            bool guiSettings{ false };
            UINT targetWidth{ 1920 };
            UINT targetHeight{ 1080 };
        };

        struct FrameCostResult
        {
            std::string error;
            int ticks{ 0 };
            int uploadTicks{ 0 };
            int zeroCopyUploads{ 0 };
            int maxUploadsPerTick{ 0 };
            int idlePasses{ 0 };          // Evaluate passes on ticks without a new frame
            int idleDispatches{ 0 };
            int uploadPasses{ 0 };
            int maxUploadPasses{ 0 };
            int uploadDispatches{ 0 };
            uint64_t uploads{ 0 };
            uint64_t decodes{ 0 };
            double seconds{ 0 };
            double setupSeconds{ 0 };
            std::vector<double> idleTickMs, uploadTickMs, prepMs, frameStartMs, evalMs, computeMs, drawMs;
            std::map<uint32_t, uint64_t> nodeChanges;
            double statusFps{ -1 };
            double statusMs{ -1 };
        };

        // The render worker's needed-marking: roots are Output nodes, dirty
        // typed-analysis nodes and the preview, walked upstream along edges
        // and bindings.
        void MarkNeeded(Graph::EffectGraph& graph, uint32_t previewId)
        {
            auto& nodes = const_cast<std::vector<Graph::EffectNode>&>(graph.Nodes());
            std::vector<uint32_t> queue{ previewId };
            for (auto& node : nodes)
            {
                node.needed = false;
                if (node.type == Graph::NodeType::Output) queue.push_back(node.id);
                if (node.dirty && node.customEffect.has_value() &&
                    node.customEffect->analysisOutputType == Graph::AnalysisOutputType::Typed)
                    queue.push_back(node.id);
            }
            std::set<uint32_t> visited;
            while (!queue.empty())
            {
                const uint32_t id = queue.back();
                queue.pop_back();
                if (!visited.insert(id).second) continue;
                auto* node = graph.FindNode(id);
                if (!node) continue;
                node->needed = true;
                for (const auto* edge : graph.GetInputEdges(id))
                    queue.push_back(edge->sourceNodeId);
                for (const auto& [name, binding] : node->propertyBindings)
                {
                    if (binding.wholeArray) queue.push_back(binding.wholeArraySourceNodeId);
                    for (const auto& source : binding.sources)
                        if (source.has_value()) queue.push_back(source->sourceNodeId);
                }
            }
        }

        // Ticks a Clock-driven video graph at 60 Hz as RenderWorkerLoop and
        // RenderFrameToOffscreen do, timing each phase and counting the
        // Evaluate passes and compute dispatches of every tick.
        FrameCostResult RunFrameCost(Graph::EffectGraph& graph, uint32_t clockId, uint32_t videoId, uint32_t previewId,
                                     const FrameCostOptions& options,
                                     ID2D1DeviceContext5* dc, ID3D11Device* device, ID3D11DeviceContext* context)
        {
            FrameCostResult result;
            Effects::SourceNodeFactory factory;
            Rendering::GraphEvaluator evaluator;
            const bool skipReadbackBefore = Performance::IsSkipUnneededCpuReadbackEnabled();
            const bool asyncReadbackBefore = Performance::IsAsyncAnalysisReadbackEnabled();
            Performance::SetSkipUnneededCpuReadbackEnabled(options.guiSettings);
            Performance::SetAsyncAnalysisReadbackEnabled(options.guiSettings);
            evaluator.SetAsyncCompile(options.guiSettings);
            struct RestoreSettings
            {
                bool skip, async;
                ~RestoreSettings()
                {
                    Performance::SetSkipUnneededCpuReadbackEnabled(skip);
                    Performance::SetAsyncAnalysisReadbackEnabled(async);
                }
            } restore{ skipReadbackBefore, asyncReadbackBefore };

            auto& nodes = const_cast<std::vector<Graph::EffectNode>&>(graph.Nodes());
            auto propagateDirty = [&]
            {
                std::vector<uint32_t> queue;
                for (const auto& node : graph.Nodes())
                    if (node.dirty) queue.push_back(node.id);
                for (size_t i = 0; i < queue.size(); ++i)
                    for (const auto* edge : graph.GetOutputEdges(queue[i]))
                        if (auto* downstream = graph.FindNode(edge->destNodeId); downstream && !downstream->dirty)
                        {
                            downstream->dirty = true;
                            queue.push_back(edge->destNodeId);
                        }
            };
            for (auto& node : nodes)
                if (node.type == Graph::NodeType::Source)
                    factory.PrepareSourceNode(node, dc, 0.0, device, context);
            auto* provider = factory.GetVideoProvider(videoId);
            if (!provider || !provider->OutputTexture())
            {
                result.error = "video did not open";
                return result;
            }
            provider->SetZeroCopyEnabled(options.zeroCopy);

            winrt::com_ptr<ID2D1Bitmap1> drawTarget;
            D2D1_BITMAP_PROPERTIES1 targetProps{};
            targetProps.pixelFormat = { DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
            targetProps.dpiX = targetProps.dpiY = 96.0f;
            targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
            if (FAILED(dc->CreateBitmap({ options.targetWidth, options.targetHeight }, nullptr, 0, targetProps, drawTarget.put())))
            {
                result.error = "draw target allocation failed";
                return result;
            }

            auto msSince = [](Clock::time_point from)
            {
                return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
            };
            // The preview drawn fitted into the target, as the GUI's auto-fit does.
            auto drawPreview = [&]
            {
                auto* preview = graph.FindNode(previewId);
                if (!preview || !preview->cachedOutput) return;
                D2D1_RECT_F bounds{};
                if (FAILED(dc->GetImageLocalBounds(preview->cachedOutput, &bounds))) return;
                const float width = bounds.right - bounds.left;
                const float height = bounds.bottom - bounds.top;
                if (!(width > 0.0f && height > 0.0f && width < 1e6f && height < 1e6f)) return;
                const float scale = (std::min)(options.targetWidth / width, options.targetHeight / height);
                dc->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
                dc->DrawImage(preview->cachedOutput);
                dc->SetTransform(D2D1::Matrix3x2F::Identity());
            };
            struct FramePhases { int passes{ 0 }; int dispatches{ 0 }; double evalMs{ 0 }; double computeMs{ 0 }; double drawMs{ 0 }; };
            // Evaluate outside the draw session, deferred compute inside it.
            auto evaluateFrame = [&]
            {
                FramePhases phases;
                MarkNeeded(graph, previewId);
                const auto evalStart = Clock::now();
                phases.passes = 1;
                evaluator.Evaluate(graph, dc);
                if (graph.HasDirtyNodes())
                {
                    evaluator.Evaluate(graph, dc);
                    ++phases.passes;
                }
                phases.evalMs = msSince(evalStart);
                const auto computeStart = Clock::now();
                dc->SetTarget(drawTarget.get());
                dc->BeginDraw();
                if (evaluator.ProcessDeferredCompute(graph, dc))
                {
                    phases.dispatches = static_cast<int>(evaluator.DispatchesLastFrame());
                    if (graph.HasDirtyNodes())
                    {
                        evaluator.SetDeferredComputeFrozen(true);
                        evaluator.Evaluate(graph, dc);
                        evaluator.SetDeferredComputeFrozen(false);
                        ++phases.passes;
                    }
                }
                phases.computeMs = msSince(computeStart);
                const auto drawStart = Clock::now();
                dc->Clear(D2D1::ColorF(0, 0, 0, 1));
                drawPreview();
                dc->EndDraw();
                dc->SetTarget(nullptr);
                phases.drawMs = msSince(drawStart);
                return phases;
            };

            // Settle until every needed shader, variants included, is compiled.
            const auto setupStart = Clock::now();
            for (int frame = 0; SecondsBetween(setupStart, Clock::now()) < 300.0; ++frame)
            {
                evaluateFrame();
                bool compiling = false;
                for (const auto& node : graph.Nodes())
                    if (node.needed && (node.compilePending || node.variantsCompiling > 0)) compiling = true;
                if (frame >= 3 && !compiling) break;
                if (compiling) ::Sleep(20);
            }
            result.setupSeconds = SecondsBetween(setupStart, Clock::now());

            Rendering::PreviewRenderStats previewStats;
            previewStats.Initialize(device);
            previewStats.SetNode(previewId);
            DeadlineSleeper sleeper;
            const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / 60.0));
            if (auto* clockNode = graph.FindNode(clockId)) clockNode->isPlaying = options.clockPlaying;
            const auto start = Clock::now();
            auto last = start;
            bool measuring = false;
            uint64_t uploadsAtStart = 0, decodesAtStart = 0;
            std::map<uint32_t, uint64_t> changesAtStart;
            Clock::time_point measureStart{};
            for (long long tick = 0;; ++tick)
            {
                sleeper.SleepUntil(start + period * tick);
                const auto now = Clock::now();
                const double elapsed = SecondsBetween(start, now);
                if (elapsed >= cWarmupSeconds + options.runSeconds) break;
                if (!measuring && elapsed >= cWarmupSeconds)
                {
                    measuring = true;
                    measureStart = now;
                    uploadsAtStart = provider->UploadSuccesses();
                    decodesAtStart = provider->DecodeCount();
                    for (const auto& node : graph.Nodes())
                        changesAtStart[node.id] = evaluator.OutputChangeCount(node.id);
                }
                double deltaSeconds = SecondsBetween(last, now);
                last = now;
                if (deltaSeconds > 0.1) deltaSeconds = 0.016;

                // RenderWorkerLoop: Clock, source bindings, video upload, dirty push.
                if (auto* clockNode = graph.FindNode(clockId); clockNode && options.clockPlaying)
                    AdvanceClock(*clockNode, deltaSeconds);
                const auto tickStart = Clock::now();
                evaluator.ResolveSourceBindings(graph);
                const uint64_t uploadsBefore = provider->UploadSuccesses();
                factory.TickAndUploadVideos(nodes, dc, deltaSeconds);
                const int uploadsThisTick = static_cast<int>(provider->UploadSuccesses() - uploadsBefore);
                propagateDirty();
                const double tickMs = msSince(tickStart);

                // RenderFrameToOffscreen: frame timers, source preparation, evaluation, draw.
                const auto frameStart = Clock::now();
                const uint64_t changesBefore = evaluator.OutputChangeCount(previewId);
                previewStats.BeginFrame(Clock::now());
                const double frameStartMs = msSince(frameStart);
                const auto prepStart = Clock::now();
                for (auto& node : nodes)
                    if (node.type == Graph::NodeType::Source && (node.dirty || factory.GetVideoProvider(node.id)))
                        factory.PrepareSourceNode(node, dc, deltaSeconds, device, context);
                const double prepMs = msSince(prepStart);
                const FramePhases phases = evaluateFrame();
                previewStats.EndFrame(evaluator.OutputChangeCount(previewId) != changesBefore, Clock::now());
                previewStats.Publish(Clock::now());

                if (!measuring) continue;
                ++result.ticks;
                if (uploadsThisTick > 0)
                {
                    ++result.uploadTicks;
                    if (provider->LastUploadWasZeroCopy()) ++result.zeroCopyUploads;
                    result.uploadTickMs.push_back(tickMs + prepMs);
                    result.uploadPasses += phases.passes;
                    result.maxUploadPasses = (std::max)(result.maxUploadPasses, phases.passes);
                    result.uploadDispatches += phases.dispatches;
                }
                else
                {
                    result.idleTickMs.push_back(tickMs + prepMs);
                    result.idlePasses += phases.passes;
                    result.idleDispatches += phases.dispatches;
                }
                result.maxUploadsPerTick = (std::max)(result.maxUploadsPerTick, uploadsThisTick);
                result.prepMs.push_back(prepMs);
                result.frameStartMs.push_back(frameStartMs);
                result.evalMs.push_back(phases.evalMs);
                result.computeMs.push_back(phases.computeMs);
                result.drawMs.push_back(phases.drawMs);
            }
            if (!measuring)
            {
                result.error = "run ended before warmup";
                return result;
            }
            result.seconds = SecondsBetween(measureStart, last);
            result.uploads = provider->UploadSuccesses() - uploadsAtStart;
            result.decodes = provider->DecodeCount() - decodesAtStart;
            for (const auto& node : graph.Nodes())
                result.nodeChanges[node.id] = evaluator.OutputChangeCount(node.id) - changesAtStart[node.id];
            result.statusFps = previewStats.Fps();
            result.statusMs = previewStats.Ms();
            return result;
        }

        void ReportFrameCost(const char* label, const FrameCostResult& result)
        {
            if (!result.error.empty())
            {
                std::printf("  [info] %s: %s\n", label, result.error.c_str());
                return;
            }
            const int idleTicks = result.ticks - result.uploadTicks;
            const double seconds = result.seconds > 0.0 ? result.seconds : 1.0;
            std::printf("  [info] %s: %d ticks in %.2f s after %.1f s setup; uploads %.1f/s (%d zero-copy, max %d/tick), "
                "decodes %.1f/s; passes/tick %.2f idle, %.2f with a frame (max %d); dispatches %d idle, %d with a frame; "
                "status %s fps %s ms\n",
                label, result.ticks, seconds, result.setupSeconds, result.uploads / seconds, result.zeroCopyUploads,
                result.maxUploadsPerTick, result.decodes / seconds,
                idleTicks > 0 ? static_cast<double>(result.idlePasses) / idleTicks : 0.0,
                result.uploadTicks > 0 ? static_cast<double>(result.uploadPasses) / result.uploadTicks : 0.0,
                result.maxUploadPasses, result.idleDispatches, result.uploadDispatches,
                FormatStat(result.statusFps, "%.1f").c_str(), FormatStat(result.statusMs, "%.1f").c_str());
            auto row = [](const char* name, const std::vector<double>& values)
            {
                std::printf("  [info]   %-28s p50 %7.2f  p90 %7.2f  max %7.2f ms\n", name,
                            Percentile(values, 0.5), Percentile(values, 0.9), Percentile(values, 1.0));
            };
            row("video tick + prep, no frame", result.idleTickMs);
            row("video tick + prep, new frame", result.uploadTickMs);
            row("source prep", result.prepMs);
            row("frame start (GPU timer)", result.frameStartMs);
            row("evaluate", result.evalMs);
            row("deferred compute", result.computeMs);
            row("draw + EndDraw", result.drawMs);
            std::string changes;
            for (const auto& [id, count] : result.nodeChanges)
                changes += std::to_string(id) + ":" + std::to_string(count) + " ";
            std::printf("  [info]   output changes per node: %s\n", changes.c_str());
        }
    }

    // The work a Clock-driven video costs per render tick. A tick without a
    // new frame runs one Evaluate pass and no compute dispatch; a tick with
    // one uploads at most one frame and dispatches each compute consumer
    // once. A paused Clock leaves the graph idle.
    void TestVideoTickWork(ID2D1DeviceContext5* dc, ID3D11Device* device, ID3D11DeviceContext* context)
    {
        std::printf("\n=== Video tick work (Clock-driven, 60 Hz ticks) ===\n");
        const std::wstring path = FindVideoFixture(L"video_index_h264_180p24.mp4");
        if (path.empty())
        {
            std::printf("  [SKIP] video_index_h264_180p24.mp4 not found -- run Tests\\fixtures\\MakeVideoFixtures.ps1\n");
            return;
        }
        using Effects::ShaderLabEffects;
        const auto& library = ShaderLabEffects::Instance();
        const auto* clockDescriptor = library.FindByName(L"Clock");
        const auto* statsDescriptor = library.FindByName(L"Luminance Statistics");
        const auto* nitMapDescriptor = library.FindByName(L"Nit Map");
        if (!clockDescriptor || !statsDescriptor || !nitMapDescriptor)
        {
            TEST("VideoTickWork_Setup", false);
            return;
        }

        // Clock -> video -> Nit Map (previewed), and Luminance Statistics
        // reading the video with the Nit Map's Opacity bound to its Max.
        auto run = [&](bool clockPlaying)
        {
            Graph::EffectGraph graph;
            const uint32_t clockId = graph.AddNode(ShaderLabEffects::CreateNode(*clockDescriptor));
            const uint32_t videoId = graph.AddNode(Effects::SourceNodeFactory::CreateVideoSourceNode(path));
            const uint32_t statsId = graph.AddNode(ShaderLabEffects::CreateNode(*statsDescriptor));
            const uint32_t previewId = graph.AddNode(ShaderLabEffects::CreateNode(*nitMapDescriptor));
            graph.FindNode(clockId)->properties[L"AutoDuration"] = 0.0f;
            graph.FindNode(clockId)->properties[L"StopTime"] = 3600.0f;
            graph.FindNode(clockId)->clockTime = 1.0;
            graph.Connect(videoId, 0, statsId, 0);
            graph.Connect(videoId, 0, previewId, 0);
            graph.BindProperty(videoId, L"Time", clockId, L"Time", 0);
            graph.BindProperty(previewId, L"Opacity", statsId, L"Max", 0);
            FrameCostOptions options;
            options.runSeconds = 2.0;
            options.clockPlaying = clockPlaying;
            options.targetWidth = 320;
            options.targetHeight = 180;
            return RunFrameCost(graph, clockId, videoId, previewId, options, dc, device, context);
        };

        const FrameCostResult playing = run(true);
        ReportFrameCost("clock playing", playing);
        const FrameCostResult paused = run(false);
        ReportFrameCost("clock paused ", paused);
        TEST("VideoTickWork_HarnessRan", playing.error.empty() && paused.error.empty() &&
                                         playing.uploadTicks > cMinUploadsPerRun);
        if (!playing.error.empty() || !paused.error.empty())
            return;

        const int idleTicks = playing.ticks - playing.uploadTicks;
        TEST("VideoTickWork_AtMostOneUploadPerTick", playing.maxUploadsPerTick <= 1);
        TEST("VideoTickWork_IdleTickOnePass", idleTicks > 0 && playing.idlePasses == idleTicks);
        TEST("VideoTickWork_IdleTickNoDispatch", playing.idleDispatches == 0);
        TEST("VideoTickWork_FrameTickBoundedPasses", playing.maxUploadPasses <= cTickWorkMaxUploadPasses);
        const uint64_t dispatches = static_cast<uint64_t>(playing.uploadDispatches);
        TEST("VideoTickWork_OneDispatchPerFrame",
             dispatches + cTickWorkCountTolerance >= playing.uploads && dispatches <= playing.uploads + cTickWorkCountTolerance);
        TEST("VideoTickWork_PausedIdle", paused.uploads <= 1 && paused.idleDispatches + paused.uploadDispatches <= 1 &&
                                         paused.idlePasses == paused.ticks - paused.uploadTicks);
        TIMING_TEST("VideoTickWork_IdleTickCheap", Percentile(playing.idleTickMs, 0.9) < cTickWorkMaxIdleTickMs);
        TIMING_TEST("VideoTickWork_FrameTickCheap", Percentile(playing.uploadTickMs, 0.9) < cTickWorkMaxUploadTickMs);
    }

    // Diagnostic, not part of the suite: SHADERLAB_TESTS=videoperf with
    // SHADERLAB_PERF_GRAPH (a graph JSON with a Clock-bound video),
    // SHADERLAB_PERF_PREVIEW (node id), and optionally SHADERLAB_PERF_TARGET
    // (WxH), SHADERLAB_PERF_EFFECTS (a user effect folder),
    // SHADERLAB_PERF_GUI=1 (the GUI's compile and readback settings) and
    // SHADERLAB_PERF_CPU_PATH=1 (a second run without zero-copy).
    void TestVideoFrameCost(ID2D1DeviceContext5* dc, ID3D11Device* device, ID3D11DeviceContext* context)
    {
        std::printf("\n=== Video frame cost (Clock-driven, 60 Hz ticks) ===\n");
        if (const auto effects = EnvString("SHADERLAB_PERF_EFFECTS"); !effects.empty())
            Effects::ShaderLabEffects::Instance().LoadUserEffects(winrt::to_hstring(effects).c_str());
        const auto graphPath = EnvString("SHADERLAB_PERF_GRAPH");
        const auto previewText = EnvString("SHADERLAB_PERF_PREVIEW");
        if (graphPath.empty() || previewText.empty())
        {
            std::printf("  [info] set SHADERLAB_PERF_GRAPH and SHADERLAB_PERF_PREVIEW\n");
            return;
        }
        FrameCostOptions options;
        options.runSeconds = 6.0;
        options.guiSettings = EnvString("SHADERLAB_PERF_GUI") == "1";
        if (const auto size = EnvString("SHADERLAB_PERF_TARGET"); !size.empty())
            sscanf_s(size.c_str(), "%ux%u", &options.targetWidth, &options.targetHeight);
        const bool cpuPathToo = EnvString("SHADERLAB_PERF_CPU_PATH") == "1";
        std::ifstream file(graphPath, std::ios::binary);
        const std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const uint32_t previewId = static_cast<uint32_t>(std::stoul(previewText));
        for (int pass = 0; pass < (cpuPathToo ? 2 : 1); ++pass)
        {
            auto graph = Graph::EffectGraph::FromJson(winrt::to_hstring(json));
            Effects::ShaderLabEffects::RestoreRuntimeFlags(graph);
            uint32_t clockId = 0, videoId = 0;
            for (const auto& node : graph.Nodes())
            {
                if (node.isClock) clockId = node.id;
                if (node.type == Graph::NodeType::Source && node.properties.count(L"IsVideo")) videoId = node.id;
            }
            options.zeroCopy = pass == 0;
            std::printf("  [info] %s, preview %u, target %ux%u\n", graphPath.c_str(), previewId,
                        options.targetWidth, options.targetHeight);
            ReportFrameCost(pass == 0 ? "zero-copy" : "CPU upload",
                RunFrameCost(graph, clockId, videoId, previewId, options, dc, device, context));
        }
    }
}
