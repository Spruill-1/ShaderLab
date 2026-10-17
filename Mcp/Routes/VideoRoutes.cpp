#include "pch_engine.h"
#include "RouteSupport.h"
#include "../McpRouter.h"

#include "Engine/Utf8.h"
#include "../../Rendering/VideoExport.h"

// POST /render/video: renders a node over a fixed timeline and encodes it with
// ffmpeg (Rendering/VideoExport). Opt-in per host: the export holds the
// dispatch thread for the whole encode, which a host with a live preview
// cannot give up, so only headless registers it.
namespace ShaderLab::Mcp
{
    namespace
    {
        namespace WDJ = winrt::Windows::Data::Json;

        std::string VideoResultJson(const Rendering::VideoExportResult& r)
        {
            WDJ::JsonObject o;
            o.Insert(L"ok", WDJ::JsonValue::CreateBooleanValue(r.ok));
            if (!r.ok) o.Insert(L"error", WDJ::JsonValue::CreateStringValue(r.error));
            o.Insert(L"ffmpegPath", WDJ::JsonValue::CreateStringValue(r.ffmpegPath));
            o.Insert(L"encoder", WDJ::JsonValue::CreateStringValue(r.encoder));
            o.Insert(L"commandLine", WDJ::JsonValue::CreateStringValue(r.commandLine));
            o.Insert(L"width", WDJ::JsonValue::CreateNumberValue(r.width));
            o.Insert(L"height", WDJ::JsonValue::CreateNumberValue(r.height));
            o.Insert(L"frames", WDJ::JsonValue::CreateNumberValue(r.frames));
            o.Insert(L"fps", WDJ::JsonValue::CreateNumberValue(r.fps));
            o.Insert(L"start", WDJ::JsonValue::CreateNumberValue(r.start));
            o.Insert(L"duration", WDJ::JsonValue::CreateNumberValue(r.duration));
            o.Insert(L"maxCll", WDJ::JsonValue::CreateNumberValue(r.maxCll));
            o.Insert(L"maxFall", WDJ::JsonValue::CreateNumberValue(r.maxFall));
            o.Insert(L"staticMetadataWritten", WDJ::JsonValue::CreateBooleanValue(r.staticMetadataWritten));
            o.Insert(L"seconds", WDJ::JsonValue::CreateNumberValue(r.seconds));
            WDJ::JsonArray w;
            for (const auto& s : r.warnings) w.Append(WDJ::JsonValue::CreateStringValue(s));
            o.Insert(L"warnings", w);
            if (!r.ok && !r.ffmpegLogTail.empty())
                o.Insert(L"ffmpegLogTail", WDJ::JsonValue::CreateStringValue(winrt::to_hstring(r.ffmpegLogTail)));
            return winrt::to_string(o.Stringify());
        }

        Response VideoError(std::wstring message)
        {
            Rendering::VideoExportResult failed;
            failed.error = std::move(message);
            return { 400, VideoResultJson(failed) };
        }
    }

    void RegisterVideoExportRoute(McpRouter& router, IEngineCommandSink& sink)
    {
        router.AddRoute(L"POST", L"/render/video",
            [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
            {
                WDJ::JsonObject j{ nullptr };
                if (!ParseObjectBody(body, j))
                    return { 400, R"({"ok":false,"error":"body must be a JSON object"})" };

                std::string argError;
                auto str = [&](const wchar_t* key) { return ReadStringArg(j, key, argError).value_or(L""); };
                auto num = [&](const wchar_t* key) { return ReadNumberArg(j, key, argError); };

                Rendering::VideoExportRequest req;
                const std::wstring format = str(L"format"), codec = str(L"codec"), mastering = str(L"mastering");
                auto node = ReadUintArg(j, L"nodeId", argError);
                req.outputPath = str(L"outputPath");
                if (auto v = num(L"fps")) req.fps = *v;
                if (auto v = num(L"start")) req.start = *v;
                if (auto v = num(L"duration")) req.duration = *v;
                if (auto v = num(L"crf")) req.crf = static_cast<int>(*v);
                req.preset = str(L"preset");
                req.lossless = ReadBoolArg(j, L"lossless", argError).value_or(false);
                auto cll = ReadUintArg(j, L"maxCll", argError);
                auto fall = ReadUintArg(j, L"maxFall", argError);
                if (cll && fall) { req.maxCll = *cll; req.maxFall = *fall; }
                req.ffmpegPath = str(L"ffmpegPath");
                if (!argError.empty())
                    return VideoError(Utf8ToWide(argError));
                std::wstring optionError;
                if (!Rendering::ParseVideoOptions(format, codec, mastering, req, optionError))
                    return VideoError(optionError);
                if (!node || req.outputPath.empty())
                    return { 400, R"({"ok":false,"error":"nodeId and outputPath are required"})" };
                req.nodeId = *node;

                return sink.Dispatch([&req](EngineContext& ctx) -> Response
                {
                    auto result = Rendering::ExportVideo(req, ctx.Model(), ctx.Evaluator(),
                        ctx.Dc(), ctx.D3DDevice(), ctx.D3DContext());
                    return { static_cast<uint16_t>(result.ok ? 200 : 500), VideoResultJson(result) };
                });
            });
    }
}
