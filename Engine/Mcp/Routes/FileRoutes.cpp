#include "pch_engine.h"
#include "RouteSupport.h"
#include "../McpRouter.h"
#include "../McpSerialize.h"

#include "../../../Graph/GraphModel.h"
#include "../../../Rendering/GraphEvaluator.h"
#include "../../../Rendering/EffectGraphFile.h"
#include "../../../Rendering/SourceRuntime.h"
#include "../../../Effects/SourceNodeFactory.h"

#include <filesystem>

// Replacing the whole graph: clear, load from JSON, and loading or saving a
// .effectgraph package or bare graph JSON on disk. File I/O runs on the
// calling thread; only the graph swap or serialisation is dispatched.
namespace ShaderLab::Mcp::Routes
{
    namespace
    {
        std::optional<Graph::GraphModel> ParseGraph(const std::wstring& json, std::string& error)
        {
            try
            {
                return Graph::GraphModel::FromJson(winrt::hstring(json));
            }
            catch (const winrt::hresult_error& ex) { error = WideToUtf8(std::wstring(ex.message())); }
            catch (const std::exception& ex)       { error = ex.what(); }
            if (error.empty()) error = "invalid graph JSON";
            return std::nullopt;
        }

        // Replaces the graph the way File > Open does and prepares its
        // sources. Runs inside Dispatch.
        void AdoptLoadedGraph(EngineContext& ctx, IEngineCommandSink& sink,
            Graph::GraphModel&& loaded, const std::wstring& mediaDir)
        {
            if (ctx.evaluator) ctx.evaluator->ReleaseCache();
            *ctx.graph = std::move(loaded);
            ctx.graph->MarkAllDirty();
            if (ctx.sourceFactory)
            {
                // Close the old graph's providers before its media directory is deleted.
                ctx.sourceFactory->PruneOrphans(ctx.graph->Nodes());
                ctx.sourceFactory->ForgetVideoSettleTimeouts();
                if (ctx.dc)
                    Rendering::PrepareSources(*ctx.graph, *ctx.sourceFactory,
                        { ctx.dc, ctx.d3dDevice, ctx.d3dContext }, Rendering::SourceScope::All);
            }
            sink.OnGraphMediaDirChanged(mediaDir);
            sink.OnGraphLoaded();
        }

        // Reads a { "path": "<absolute path>" } body. Returns the 400 to send on failure.
        std::optional<Response> ReadPathArgument(const std::string& body, std::wstring& path, WDJ::JsonObject& request)
        {
            if (!ParseObjectBody(body, request))
                return Error(400, "body must be a JSON object with a path");
            std::string argError;
            auto value = ReadStringArg(request, L"path", argError);
            if (!value)
                return Error(400, "path (an absolute file path) is required");
            path = *value;
            if (path.empty() || !std::filesystem::path(path).is_absolute())
                return Error(400, WideToUtf8(L"path must be absolute: " + path));
            path = std::filesystem::path(path).lexically_normal().wstring();
            return std::nullopt;
        }

        std::wstring TempRoot()
        {
            wchar_t buffer[MAX_PATH + 1]{};
            const DWORD length = ::GetTempPathW(MAX_PATH + 1, buffer);
            return std::wstring(buffer, length);
        }

        void RemoveExtractDir(const std::wstring& dir)
        {
            std::error_code ec;
            if (!dir.empty()) std::filesystem::remove_all(dir, ec);   // best effort: temp dir
        }

        // POST /graph/clear: drops the graph and the evaluator cache.
        // OnGraphCleared lets the host close output windows and reset its
        // preview, as the toolbar's clear does.
        void RegisterClear(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"POST", L"/graph/clear",
                [&sink](const std::wstring&, const std::wstring&, const std::string&) -> Response
                {
                    return sink.Dispatch([&sink](EngineContext& ctx) -> Response
                    {
                        if (ctx.evaluator) ctx.evaluator->ReleaseCache();
                        ctx.graph->Clear();
                        ctx.graph->MarkAllDirty();
                        if (ctx.sourceFactory)
                        {
                            ctx.sourceFactory->PruneOrphans(ctx.graph->Nodes());
                            ctx.sourceFactory->ForgetVideoSettleTimeouts();
                        }
                        sink.OnGraphMediaDirChanged({});
                        sink.OnGraphCleared();
                        return Json(200, R"({"ok":true})");
                    });
                });
        }

        // POST /graph/load: the body is the graph JSON. Parsed on the calling
        // thread; OnGraphLoaded runs the host's per-load setup.
        void RegisterLoad(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"POST", L"/graph/load",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    std::string parseError;
                    auto loaded = ParseGraph(std::wstring(winrt::to_hstring(body)), parseError);
                    if (!loaded)
                        return Error(400, parseError);
                    return sink.Dispatch([&](EngineContext& ctx) -> Response
                    {
                        AdoptLoadedGraph(ctx, sink, std::move(*loaded), {});
                        return Json(200, R"({"ok":true})");
                    });
                });
        }

        // The load-file answer: what was loaded, and each node with an error
        // or a missing source file.
        std::string LoadReport(const Graph::GraphModel& graph, const std::wstring& path, bool package,
            size_t mediaFiles, const std::wstring& mediaDir)
        {
            JsonWriter json;
            json.BeginObject()
                .Key("ok").Bool(true)
                .Key("path").String(path)
                .Key("format").String(package ? "package" : "json")
                .Key("nodeCount").Int(graph.Nodes().size())
                .Key("nodes").BeginArray();
            for (const auto& node : graph.Nodes())
                json.BeginObject().Key("id").Int(node.id).Key("name").String(node.name)
                    .Key("type").String(NodeTypeName(node.type)).EndObject();
            json.EndArray()
                .Key("mediaFiles").Int(mediaFiles)
                .Key("extractDir").String(mediaDir)
                .Key("errors").BeginArray();
            for (const auto& node : graph.Nodes())
            {
                std::wstring nodeError = node.runtimeError;
                if (nodeError.empty() && node.type == Graph::NodeType::Source
                    && node.mediaPath.has_value() && !node.mediaPath->empty())
                {
                    std::error_code existsError;
                    if (!std::filesystem::exists(*node.mediaPath, existsError))
                        nodeError = L"source file not found: " + *node.mediaPath;
                }
                if (nodeError.empty()) continue;
                json.BeginObject().Key("nodeId").Int(node.id).Key("name").String(node.name)
                    .Key("error").String(nodeError).EndObject();
            }
            json.EndArray().EndObject();
            return json.Take();
        }

        // POST /graph/load-file { path }: a .effectgraph package or bare graph
        // JSON, told apart by content. The file is read and its media
        // extracted on the calling thread, so a large package does not hold
        // the render thread.
        void RegisterLoadFile(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"POST", L"/graph/load-file",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    std::wstring path;
                    WDJ::JsonObject request{ nullptr };
                    if (auto failure = ReadPathArgument(body, path, request))
                        return *failure;
                    std::error_code ec;
                    if (!std::filesystem::is_regular_file(path, ec))
                        return Error(400, WideToUtf8(L"file not found: " + path));

                    std::wstring loadError;
                    auto file = Rendering::EffectGraphFile::LoadAny(path, TempRoot(), loadError);
                    if (!file)
                        return Error(400, WideToUtf8(loadError));

                    std::string parseError;
                    auto loaded = ParseGraph(file->graphJson, parseError);
                    if (!loaded)
                    {
                        RemoveExtractDir(file->extractDir);
                        return Error(400, WideToUtf8(L"'" + path + L"' is not a valid graph: ") + parseError);
                    }
                    Rendering::EffectGraphFile::ResolveMediaTokens(*loaded, file->mediaMap);
                    // Load names a directory even when there was no media to put in it.
                    const std::wstring mediaDir = file->mediaMap.empty() ? std::wstring() : file->extractDir;

                    // If Dispatch throws (a timeout), the closure may still run
                    // and adopt the directory, so it is not deleted here.
                    return sink.Dispatch([&](EngineContext& ctx) -> Response
                    {
                        AdoptLoadedGraph(ctx, sink, std::move(*loaded), mediaDir);
                        return Json(200, LoadReport(*ctx.graph, path, file->package, file->mediaMap.size(), mediaDir));
                    });
                });
        }

        // POST /graph/save-file { path, embedMedia? }: a path ending in .json is
        // written as bare graph JSON, anything else as a package; embedMedia
        // defaults to true for a package. The graph is serialised inside
        // Dispatch and written after it returns.
        void RegisterSaveFile(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"POST", L"/graph/save-file",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    std::wstring path;
                    WDJ::JsonObject request{ nullptr };
                    if (auto failure = ReadPathArgument(body, path, request))
                        return *failure;

                    std::wstring extension = std::filesystem::path(path).extension().wstring();
                    for (auto& c : extension) c = static_cast<wchar_t>(::towlower(c));
                    const bool package = extension != L".json";

                    bool embedMedia = package;
                    // null keeps the default.
                    if (request.HasKey(L"embedMedia") &&
                        request.GetNamedValue(L"embedMedia").ValueType() != WDJ::JsonValueType::Null)
                    {
                        std::string argError;
                        auto flag = ReadBoolArg(request, L"embedMedia", argError);
                        if (!flag)
                            return Error(400, "embedMedia must be a boolean");
                        embedMedia = *flag;
                    }
                    if (embedMedia && !package)
                        return Error(400, "embedMedia needs a package; use a .effectgraph path");

                    std::error_code ec;
                    const auto parent = std::filesystem::path(path).parent_path();
                    if (!std::filesystem::is_directory(parent, ec))
                        return Error(400, WideToUtf8(L"folder not found: " + parent.wstring()));
                    if (std::filesystem::is_directory(path, ec))
                        return Error(400, WideToUtf8(L"path is a folder: " + path));

                    std::wstring graphJson;
                    std::vector<Rendering::EffectGraphFile::MediaEntry> media;
                    std::vector<std::wstring> warnings;
                    size_t nodeCount = 0;
                    Response serialized = sink.Dispatch([&](EngineContext& ctx) -> Response
                    {
                        nodeCount = ctx.graph->Nodes().size();
                        graphJson = Rendering::EffectGraphFile::SerializeForSave(*ctx.graph, embedMedia, media);
                        const std::wstring extractedPrefix = TempRoot() + L"ShaderLab-";
                        for (const auto& node : ctx.graph->Nodes())
                        {
                            if (node.type != Graph::NodeType::Source || !node.mediaPath.has_value()) continue;
                            const std::wstring& sourcePath = *node.mediaPath;
                            if (sourcePath.empty() || sourcePath.starts_with(L"media://")) continue;
                            std::error_code existsError;
                            if (!std::filesystem::exists(sourcePath, existsError))
                                warnings.push_back(std::format(L"node {}: source file not found, saved as a path: {}", node.id, sourcePath));
                            else if (!embedMedia && sourcePath.starts_with(extractedPrefix))
                                warnings.push_back(std::format(L"node {}: source is extracted package media in the temp folder, which is deleted when this graph is replaced; embed it to keep it: {}", node.id, sourcePath));
                        }
                        return Json(200, "{}");
                    });
                    if (serialized.statusCode != 200)
                        return serialized;

                    Rendering::EffectGraphFile::SaveStats stats;
                    const bool saved = package
                        ? Rendering::EffectGraphFile::Save(path, graphJson, media, {}, &stats)
                        : Rendering::EffectGraphFile::SaveJson(path, graphJson, &stats.bytesWritten);
                    if (!saved)
                        return Error(500, WideToUtf8(std::format(L"could not write '{}' (Win32 error {})", path, ::GetLastError())));

                    const uint64_t fileSize = std::filesystem::file_size(path, ec);
                    JsonWriter json;
                    json.BeginObject()
                        .Key("ok").Bool(true)
                        .Key("path").String(path)
                        .Key("format").String(package ? "package" : "json")
                        .Key("embedMedia").Bool(embedMedia)
                        .Key("nodeCount").Int(nodeCount)
                        .Key("mediaEmbedded").Int(media.size())
                        .Key("mediaUnchanged").Int(stats.mediaUnchanged)
                        .Key("mediaWritten").Int(stats.mediaWritten)
                        .Key("inPlace").Bool(stats.inPlace)
                        .Key("bytesWritten").Int(stats.bytesWritten)
                        .Key("fileSize").Int(ec ? uint64_t{ 0 } : fileSize)
                        .Key("warnings").BeginArray();
                    for (const auto& warning : warnings)
                        json.String(warning);
                    json.EndArray().EndObject();
                    return Json(200, json.Take());
                });
        }
    }

    void RegisterFileRoutes(McpRouter& router, IEngineCommandSink& sink)
    {
        RegisterClear(router, sink);
        RegisterLoad(router, sink);
        RegisterLoadFile(router, sink);
        RegisterSaveFile(router, sink);
    }
}
