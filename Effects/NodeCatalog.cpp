#include "pch_engine.h"
#include "NodeCatalog.h"
#include "../Graph/GraphModel.h"
#include "BuiltInEffects.h"
#include "CustomComputeShaderEffect.h"
#include "CustomPixelShaderEffect.h"
#include "DxgiDuplicationSourceProvider.h"
#include "EffectRegistry.h"
#include "ShaderLabEffects.h"
#include "SourceNodeFactory.h"

namespace ShaderLab::Effects
{
    namespace
    {
        Graph::EffectNode CreateCustomShaderNode(std::wstring_view name)
        {
            const bool isCompute = name == L"Custom Compute Shader";
            const bool isD3D11   = name == L"Custom D3D11 Compute Shader";
            Graph::EffectNode node;
            node.type = (isCompute || isD3D11) ? Graph::NodeType::ComputeShader : Graph::NodeType::PixelShader;
            node.name = std::wstring(name);
            if (!isD3D11)
            {
                node.effectClsid = isCompute
                    ? CustomComputeShaderEffect::CLSID_CustomComputeShader
                    : CustomPixelShaderEffect::CLSID_CustomPixelShader;
                node.outputPins.push_back({ L"Output", 0 });
            }

            Graph::CustomEffectDefinition def;
            def.shaderType = isD3D11 ? Graph::CustomShaderType::D3D11ComputeShader
                           : isCompute ? Graph::CustomShaderType::ComputeShader
                           : Graph::CustomShaderType::PixelShader;
            CoCreateGuid(&def.shaderGuid);
            def.inputNames.push_back(L"Source");
            node.inputPins.push_back({ L"I0", 0 });
            if (isCompute) { def.threadGroupX = 8; def.threadGroupY = 8; def.threadGroupZ = 1; }
            if (isD3D11)
            {
                def.analysisOutputType = Graph::AnalysisOutputType::Typed;
                def.analysisFields.push_back({ L"Result", Graph::AnalysisFieldType::Float4 });
            }
            Graph::DeriveContracts(def, node.properties);
            node.customEffect = std::move(def);
            return node;
        }

        // The file name, or `fallback` without a file.
        std::wstring DisplayName(const std::wstring& filePath, const wchar_t* fallback)
        {
            return filePath.empty() ? std::wstring(fallback) : filePath.substr(filePath.find_last_of(L"\\/") + 1);
        }
    }

    Graph::EffectNode CreateCustomEffectNode(Graph::CustomEffectDefinition definition, std::wstring name)
    {
        using Graph::CustomShaderType;
        Graph::EffectNode node;
        node.type = definition.shaderType == CustomShaderType::PixelShader
            ? Graph::NodeType::PixelShader : Graph::NodeType::ComputeShader;
        node.name = std::move(name);
        for (uint32_t i = 0; i < definition.inputNames.size(); ++i)
            node.inputPins.push_back({ definition.inputNames[i], i });
        if (definition.shaderType != CustomShaderType::D3D11ComputeShader)
        {
            node.outputPins.push_back({ L"Output", 0 });
            node.effectClsid = definition.shaderType == CustomShaderType::PixelShader
                ? CustomPixelShaderEffect::CLSID_CustomPixelShader
                : CustomComputeShaderEffect::CLSID_CustomComputeShader;
        }
        for (const auto& parameter : definition.parameters)
            node.properties[parameter.name] = parameter.defaultValue;
        Graph::DeriveContracts(definition, node.properties);
        node.customEffect = std::move(definition);
        return node;
    }

    NodeCreateResult CreateNodeByName(
        std::wstring_view name, const NodeCreateArgs& args, const Graph::GraphModel& graph)
    {
        NodeCreateResult result;
        if (name == L"Custom Compute Shader" || name == L"Custom Pixel Shader" ||
            name == L"Custom D3D11 Compute Shader")
        {
            result.node = CreateCustomShaderNode(name);
            return result;
        }
        if (auto* descriptor = ShaderLabEffects::Instance().FindByName(name))
        {
            result.node = ShaderLabEffects::CreateNode(*descriptor);
            return result;
        }
        if (auto* descriptor = EffectRegistry::Instance().FindByName(name))
        {
            result.node = EffectRegistry::CreateNode(*descriptor);
            return result;
        }

        // A node type, not an effect. In the GUI each Output gets a window.
        if (name == L"Output")
        {
            result.node = EffectRegistry::CreateOutputNode();
            const size_t outputCount = graph.GetOutputNodeIds().size();
            if (outputCount > 0)
                result.node->name = L"Output " + std::to_wstring(outputCount + 1);
            return result;
        }

        // Desktop duplication is pure data (adapter and output index), so
        // unlike Windows Graphics Capture, which needs the OS picker, it can be
        // created over MCP and headless.
        if (name == L"Desktop Duplication" || name == L"DXGI Duplicate Output" || name == L"Display Capture")
        {
            const auto outputs = DxgiDuplicationSourceProvider::EnumerateOutputs();
            auto match = std::find_if(outputs.begin(), outputs.end(), [&](const auto& output)
            {
                return output.adapterIndex == args.adapterIndex && output.outputIndex == args.outputIndex;
            });
            if (match == outputs.end())
            {
                // A capture of a missing output would render black, which is
                // hard to tell from a correctly captured black screen.
                std::wstring available;
                for (const auto& output : outputs)
                {
                    if (!available.empty()) available += L", ";
                    available += std::format(L"{}:{}", output.adapterIndex, output.outputIndex);
                }
                result.error = std::format(L"No DXGI output {}:{}. Available adapter:output pairs: {}",
                    args.adapterIndex, args.outputIndex, available.empty() ? L"(none)" : available);
                return result;
            }
            const std::wstring label = match->deviceName.empty()
                ? std::format(L"DXGI {}:{}", args.adapterIndex, args.outputIndex)
                : std::format(L"DXGI {} ({},{})", match->deviceName, args.adapterIndex, args.outputIndex);
            result.node = SourceNodeFactory::CreateDxgiDuplicateOutputSourceNode(args.adapterIndex, args.outputIndex, label);
            result.needsPrepare = true;
            result.desktopDuplication = true;
            return result;
        }

        if (name == L"Video Source" || name == L"Video")
        {
            result.node = SourceNodeFactory::CreateVideoSourceNode(args.filePath, DisplayName(args.filePath, L"Video Source"));
            result.needsPrepare = !args.filePath.empty();
            return result;
        }
        if (name == L"Image Source" || name == L"Image")
        {
            result.node = SourceNodeFactory::CreateImageSourceNode(args.filePath, DisplayName(args.filePath, L"Image Source"));
            result.needsPrepare = !args.filePath.empty();
            return result;
        }

        result.error = MissingBuiltInMessage(name);
        if (result.error.empty()) result.error = L"Unknown effect name: " + std::wstring(name);
        return result;
    }
}
