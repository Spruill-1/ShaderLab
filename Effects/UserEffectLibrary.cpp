#include "pch_engine.h"
#include "ShaderLabEffects.h"
#include "ShaderCompiler.h"
#include "ShaderVariants.h"
#include "../Graph/EffectGraph.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <shlobj.h>

// User effects: ShaderLab effects registered from files outside the build.
// A user effect file is a saved graph. Each shader node's embedded definition
// is registered like a built-in: an Add Effect menu entry, graph_add_node by
// name, and the Update Effect prompt for graphs holding an older copy.

namespace ShaderLab::Effects
{
    namespace
    {
        namespace fs = std::filesystem;
        namespace WDJ = winrt::Windows::Data::Json;

        bool ReadUtf8File(const fs::path& path, std::wstring& out, std::wstring& error)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in) { error = L"cannot open"; return false; }
            std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
                static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF)
                bytes.erase(0, 3);
            if (bytes.empty()) { error = L"file is empty"; return false; }
            int wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            if (wideLength <= 0) { error = L"not valid UTF-8"; return false; }
            out.resize(static_cast<size_t>(wideLength));
            MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), out.data(), wideLength);
            return true;
        }

        // Compile with the evaluator's generic-variant macros. Graph loading
        // compiles without them, so its result cannot validate an effect
        // that uses shaderlab_params.hlsli.
        bool CompilesLikeTheEvaluator(const Graph::CustomEffectDefinition& def, std::wstring& error)
        {
            std::string hlsl(def.hlslSource.begin(), def.hlslSource.end());
            for (auto& ch : hlsl) if (ch == '\r') ch = '\n';
            const auto macroDefs = GenericVariantMacros(def);
            std::vector<ShaderCompiler::MacroDef> macros;
            for (const auto& [name, value] : macroDefs) macros.push_back({ name.c_str(), value.c_str() });
            const std::string target =
                def.shaderType == Graph::CustomShaderType::PixelShader ? "ps_5_0" : "cs_5_0";
            auto result = ShaderCompiler::CompileFromString(hlsl, "UserEffect", "main", target, macros);
            if (result.succeeded) return true;
            error = result.ErrorMessage();
            return false;
        }

        std::wstring Widen(const char* text)
        {
            std::string narrow(text ? text : "");
            return std::wstring(narrow.begin(), narrow.end());
        }

        // Inverse of CreateNode's byte-wise widening of hlslSource. Characters
        // above 255 can only appear in HLSL comments, so they become '?'.
        std::string NarrowHlsl(const std::wstring& wide)
        {
            std::string narrow;
            narrow.reserve(wide.size());
            for (wchar_t ch : wide) narrow.push_back(ch < 256 ? static_cast<char>(ch) : '?');
            return narrow;
        }

        ShaderLabEffectDescriptor DescriptorFromNode(const Graph::EffectNode& node,
                                                     const std::wstring& category,
                                                     const fs::path& file)
        {
            const auto& def = *node.customEffect;
            ShaderLabEffectDescriptor descriptor;
            descriptor.name = node.name;
            descriptor.category = category;
            descriptor.shaderType = def.shaderType;
            descriptor.effectId = def.shaderLabEffectId.empty() ? node.name : def.shaderLabEffectId;
            descriptor.effectVersion = (std::max)(def.shaderLabEffectVersion, 1u);
            descriptor.hlslSource = NarrowHlsl(def.hlslSource);
            descriptor.inputNames = def.inputNames;
            descriptor.lookupInputCount = def.lookupInputCount;
            descriptor.variadicInputs = def.variadicInputs;
            descriptor.parameters = def.parameters;
            descriptor.analysisFields = def.analysisFields;
            descriptor.analysisOutputType = def.analysisOutputType;
            descriptor.threadGroupX = def.threadGroupX;
            descriptor.threadGroupY = def.threadGroupY;
            descriptor.threadGroupZ = def.threadGroupZ;
            // Read back off the node what CreateNode derives these from.
            descriptor.dataOnly = node.outputPins.empty();
            // Both compute shader kinds can produce an image.
            descriptor.hasImageOutput = (def.shaderType == Graph::CustomShaderType::ComputeShader ||
                                         def.shaderType == Graph::CustomShaderType::D3D11ComputeShader) &&
                                        !node.outputPins.empty();
            // Properties that are not declared parameters are hidden cbuffer
            // values, such as a compute node's output size.
            for (const auto& [key, value] : node.properties)
            {
                bool declared = std::any_of(descriptor.parameters.begin(), descriptor.parameters.end(),
                    [&](const Graph::ParameterDefinition& param) { return param.name == key; });
                if (!declared) descriptor.hiddenDefaults[key] = value;
            }
            descriptor.sourcePath = file.wstring();
            return descriptor;
        }
    }

    UserEffectConflict ResolveUserEffectConflict(
        const ShaderLabEffectDescriptor& existing,
        const ShaderLabEffectDescriptor& incoming)
    {
        // `incoming` shares an id or name with `existing`. A built-in is never
        // replaced, since that would change what saved graphs upgrade to and
        // drop built-in hooks such as deriveConstants. Between user effects
        // the higher effectVersion wins; a tie keeps the file loaded first.
        if (!existing.IsUserEffect())
            return UserEffectConflict::Reject;
        return incoming.effectVersion > existing.effectVersion
            ? UserEffectConflict::Replace
            : UserEffectConflict::Reject;
    }

    std::wstring ShaderLabEffects::DefaultUserEffectsDirectory()
    {
        wchar_t* localAppData = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) || !localAppData)
            return {};
        std::wstring dir = std::wstring(localAppData) + L"\\ShaderLab\\effects";
        ::CoTaskMemFree(localAppData);
        return dir;
    }

    const UserEffectLoadReport& ShaderLabEffects::LoadUserEffects(const std::wstring& directory)
    {
        auto& report = m_userReport;
        report.directories.push_back(directory);

        std::error_code errorCode;
        if (directory.empty() || !fs::is_directory(directory, errorCode))
            return report;   // no user effects on this machine: not an error

        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(directory, errorCode))
        {
            if (!entry.is_regular_file()) continue;
            if (_wcsicmp(entry.path().extension().c_str(), L".json") == 0)
                files.push_back(entry.path());
        }
        if (errorCode)
        {
            report.errors.push_back(directory + L": cannot list directory (" + Widen(errorCode.message().c_str()) + L")");
            return report;
        }
        // Sort so conflict resolution does not depend on directory order.
        std::sort(files.begin(), files.end());

        for (const auto& file : files)
        {
            const std::wstring fileName = file.filename().wstring();
            auto fail = [&](const std::wstring& why) { report.errors.push_back(fileName + L": " + why); };

            std::wstring text, readError;
            if (!ReadUtf8File(file, text, readError)) { fail(readError); continue; }

            Graph::EffectGraph graph;
            std::wstring category = L"User";
            try
            {
                graph = Graph::EffectGraph::FromJson(winrt::hstring(text));
                auto root = WDJ::JsonObject::Parse(text);
                if (root.HasKey(L"category"))
                {
                    std::wstring fileCategory(root.GetNamedString(L"category"));
                    if (!fileCategory.empty()) category = fileCategory;
                }
            }
            catch (winrt::hresult_error const& error) { fail(L"not a ShaderLab graph (" + std::wstring(error.message()) + L")"); continue; }
            catch (std::exception const& error)       { fail(Widen(error.what())); continue; }

            bool anyShader = false;
            for (const auto& node : graph.Nodes())
            {
                if (!node.customEffect.has_value()) continue;   // sources, D2D built-ins, outputs
                anyShader = true;
                std::wstring compileError;
                if (!CompilesLikeTheEvaluator(*node.customEffect, compileError))
                {
                    // Report only the first line of the compiler log.
                    auto lineEnd = compileError.find_first_of(L"\r\n");
                    fail(L"'" + node.name + L"' does not compile: " + compileError.substr(0, lineEnd));
                    continue;
                }

                auto incoming = DescriptorFromNode(node, category, file);

                // A taken id or name is a conflict: ids key the upgrade path,
                // names key the menu and graph_add_node.
                auto existing = std::find_if(m_effects.begin(), m_effects.end(),
                    [&](const ShaderLabEffectDescriptor& effect)
                    {
                        return effect.effectId == incoming.effectId ||
                               _wcsicmp(effect.name.c_str(), incoming.name.c_str()) == 0;
                    });
                if (existing != m_effects.end())
                {
                    if (ResolveUserEffectConflict(*existing, incoming) == UserEffectConflict::Reject)
                    {
                        if (!existing->IsUserEffect())
                            fail(L"'" + incoming.name + L"' has the id or name of the built-in '" +
                                 existing->name + L"', and built-ins are never replaced; not loaded");
                        else
                            fail(L"'" + incoming.name + L"' v" + std::to_wstring(incoming.effectVersion) +
                                 L" is not newer than v" + std::to_wstring(existing->effectVersion) + L" from " +
                                 fs::path(existing->sourcePath).filename().wstring() + L"; not loaded");
                        continue;
                    }
                    report.loaded.push_back(incoming.name);
                    *existing = std::move(incoming);   // in place: pointers into m_effects stay valid
                    continue;
                }
                report.loaded.push_back(incoming.name);
                m_effects.push_back(std::move(incoming));
            }
            if (!anyShader) fail(L"contains no shader nodes");
        }
        return report;
    }
}
