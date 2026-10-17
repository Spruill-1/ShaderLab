#include "pch_engine.h"
#include "McpSerialize.h"
#include "McpJsonWriter.h"
#include "Effects/ShaderVariants.h"

#include <atomic>

namespace ShaderLab::Mcp
{
    std::string Base64Encode(std::span<const uint8_t> bytes)
    {
        static constexpr char cAlphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((bytes.size() + 2) / 3) * 4);
        size_t i = 0;
        for (; i + 2 < bytes.size(); i += 3)
        {
            const uint32_t v = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8) | uint32_t(bytes[i + 2]);
            out.push_back(cAlphabet[(v >> 18) & 0x3F]);
            out.push_back(cAlphabet[(v >> 12) & 0x3F]);
            out.push_back(cAlphabet[(v >> 6) & 0x3F]);
            out.push_back(cAlphabet[v & 0x3F]);
        }
        if (i < bytes.size())
        {
            const bool two = i + 1 < bytes.size();
            uint32_t v = uint32_t(bytes[i]) << 16;
            if (two) v |= uint32_t(bytes[i + 1]) << 8;
            out.push_back(cAlphabet[(v >> 18) & 0x3F]);
            out.push_back(cAlphabet[(v >> 12) & 0x3F]);
            out.push_back(two ? cAlphabet[(v >> 6) & 0x3F] : '=');
            out.push_back('=');
        }
        return out;
    }

    const char* NodeTypeName(Graph::NodeType type)
    {
        switch (type)
        {
        case Graph::NodeType::Source:        return "Source";
        case Graph::NodeType::BuiltInEffect: return "BuiltInEffect";
        case Graph::NodeType::PixelShader:   return "PixelShader";
        case Graph::NodeType::ComputeShader: return "ComputeShader";
        case Graph::NodeType::Output:        return "Output";
        }
        return "Unknown";
    }

    namespace
    {
        void WriteAnalysisField(JsonWriter& json, const Graph::AnalysisFieldValue& field)
        {
            json.BeginObject()
                .Key("name").String(field.name)
                .Key("type").String(Graph::AnalysisFieldTypeTag(field.type));
            const uint32_t components = Graph::AnalysisFieldComponentCount(field.type);
            std::span<const float> values(field.components.data(), components);
            if (Graph::AnalysisFieldIsArray(field.type))
            {
                values = field.arrayData;
                json.Key("count").Int(components > 0 ? field.arrayData.size() / components : size_t{ 0 });
            }
            json.Key("value").BeginArray();
            for (float value : values)
                json.Float(value);
            json.EndArray().EndObject();
        }

        void WritePropertyValue(JsonWriter& json, const Graph::PropertyValue& value)
        {
            using namespace winrt::Windows::Foundation::Numerics;
            std::visit([&json](const auto& v)
            {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, float>)
                    json.Float(v);
                else if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, uint32_t>)
                    json.Int(v);
                else if constexpr (std::is_same_v<T, bool>)
                    json.Bool(v);
                else if constexpr (std::is_same_v<T, std::wstring>)
                    json.String(v);
                else if constexpr (std::is_same_v<T, float2>)
                    json.BeginArray().Float(v.x).Float(v.y).EndArray();
                else if constexpr (std::is_same_v<T, float3>)
                    json.BeginArray().Float(v.x).Float(v.y).Float(v.z).EndArray();
                else if constexpr (std::is_same_v<T, float4>)
                    json.BeginArray().Float(v.x).Float(v.y).Float(v.z).Float(v.w).EndArray();
                else if constexpr (std::is_same_v<T, D2D1_MATRIX_5X4_F>)
                    json.String("<matrix>");
                else if constexpr (std::is_same_v<T, std::vector<float>>)
                    json.String("<curve>");
                else
                    json.Null();
            }, value);
        }

        std::wstring GuidText(const GUID& guid)
        {
            wchar_t buffer[64]{};
            ::StringFromGUID2(guid, buffer, 64);
            return buffer;
        }

        void WriteBindings(JsonWriter& json, const Graph::EffectNode& node)
        {
            json.Key("propertyBindings").BeginObject();
            for (const auto& [propertyName, binding] : node.propertyBindings)
            {
                json.Key(WideToUtf8(propertyName)).BeginObject();
                if (binding.wholeArray)
                {
                    json.Key("wholeArray").Bool(true)
                        .Key("sourceNodeId").Int(binding.wholeArraySourceNodeId)
                        .Key("sourceFieldName").String(binding.wholeArraySourceFieldName);
                }
                else
                {
                    json.Key("sources").BeginArray();
                    for (const auto& source : binding.sources)
                    {
                        if (!source.has_value()) { json.Null(); continue; }
                        json.BeginObject()
                            .Key("nodeId").Int(source->sourceNodeId)
                            .Key("field").String(source->sourceFieldName)
                            .Key("index").Int(source->sourceIndex)
                            .Key("comp").Int(source->sourceComponent)
                            .EndObject();
                    }
                    json.EndArray();
                }
                json.EndObject();
            }
            json.EndObject();
        }

        template <class Pins>
        void WritePins(JsonWriter& json, const char* key, const Pins& pins)
        {
            json.Key(key).BeginArray();
            for (const auto& pin : pins)
                json.BeginObject().Key("name").String(pin.name).Key("index").Int(pin.index).EndObject();
            json.EndArray();
        }

        void WriteCustomEffect(JsonWriter& json, const Graph::EffectNode& node)
        {
            const auto& def = *node.customEffect;
            using CST = Graph::CustomShaderType;
            const char* shaderType =
                def.shaderType == CST::PixelShader ? "PixelShader" :
                def.shaderType == CST::D3D11ComputeShader ? "D3D11ComputeShader" :
                "ComputeShader";
            json.Key("customEffect").BeginObject()
                .Key("shaderType").String(shaderType)
                .Key("compiled").Bool(def.isCompiled())
                .Key("bytecodeSize").Int(def.compiledBytecode.size());
            json.Key("inputNames").BeginArray();
            for (const auto& name : def.inputNames)
                json.String(name);
            json.EndArray();
            json.Key("parameters").BeginArray();
            for (const auto& parameter : def.parameters)
            {
                json.BeginObject()
                    .Key("name").String(parameter.name)
                    .Key("type").String(parameter.typeName)
                    .Key("min").Fixed(parameter.minValue, 4)
                    .Key("max").Fixed(parameter.maxValue, 4)
                    .Key("step").Fixed(parameter.step, 4);
                if (!parameter.enumLabels.empty())
                {
                    json.Key("options").BeginArray();
                    for (const auto& label : parameter.enumLabels)
                        json.String(label);
                    json.EndArray();
                }
                if (parameter.gpuBindable) json.Key("gpuBindable").Bool(true);
                if (parameter.specialize)  json.Key("specialize").Bool(true);
                json.EndObject();
            }
            json.EndArray();
            // How many option variants the definition prebuilds, and how many are still compiling.
            if (const size_t combinations = Effects::OptionCombinationCount(def))
                json.Key("optionVariants").Int(combinations).Key("variantsCompiling").Int(node.variantsCompiling);
            json.Key("hlslSource").String(def.hlslSource);
            if (!def.analysisFields.empty())
            {
                json.Key("analysisFields").BeginArray();
                for (const auto& field : def.analysisFields)
                {
                    json.BeginObject()
                        .Key("name").String(field.name)
                        .Key("type").String(Graph::AnalysisFieldTypeTag(field.type));
                    if (Graph::AnalysisFieldIsArray(field.type))
                        json.Key("length").Int(field.arrayLength);
                    json.EndObject();
                }
                json.EndArray();
            }
            json.EndObject();
        }

        void WriteChromaticity(JsonWriter& json, const char* key, const Rendering::ChromaticityXY& xy)
        {
            json.Key(key).BeginArray().Fixed(xy.x, 4).Fixed(xy.y, 4).EndArray();
        }
    }

    std::string AnalysisFieldToJson(const Graph::AnalysisFieldValue& field)
    {
        JsonWriter json;
        WriteAnalysisField(json, field);
        return json.Take();
    }

    std::string PropertyValueToJson(const Graph::PropertyValue& value)
    {
        JsonWriter json;
        WritePropertyValue(json, value);
        return json.Take();
    }

    std::string NodeToJson(const Graph::EffectNode& node)
    {
        JsonWriter json;
        json.BeginObject()
            .Key("id").Int(node.id)
            .Key("name").String(node.name)
            .Key("type").String(NodeTypeName(node.type))
            .Key("position").BeginArray().Fixed(node.position.x, 1).Fixed(node.position.y, 1).EndArray();
        // A source's file reads back under the name set-property writes.
        if (node.mediaPath.has_value() && !node.mediaPath->empty())
            json.Key("shaderPath").String(*node.mediaPath);

        json.Key("properties").BeginObject();
        for (const auto& [key, value] : node.properties)
        {
            json.Key(WideToUtf8(key));
            WritePropertyValue(json, value);
        }
        json.EndObject();

        if (!node.propertyBindings.empty())
            WriteBindings(json, node);

        WritePins(json, "inputPins", node.inputPins);
        WritePins(json, "outputPins", node.outputPins);

        if (node.effectClsid.has_value())
            json.Key("effectClsid").String(GuidText(*node.effectClsid));
        if (!node.runtimeError.empty())
            json.Key("runtimeError").String(node.runtimeError);

        if (node.customEffect.has_value())
            WriteCustomEffect(json, node);

        using AOT = Graph::AnalysisOutputType;
        if (node.analysisOutput.type == AOT::Typed && !node.analysisOutput.fields.empty())
        {
            json.Key("analysisResults").BeginArray();
            for (const auto& field : node.analysisOutput.fields)
                WriteAnalysisField(json, field);
            json.EndArray();
        }
        else if (node.analysisOutput.type == AOT::Histogram && !node.analysisOutput.data.empty())
        {
            json.Key("analysisResults").BeginObject()
                .Key("type").String("histogram")
                .Key("channel").Int(node.analysisOutput.channelIndex)
                .Key("bins").Int(node.analysisOutput.data.size())
                .EndObject();
        }
        return json.EndObject().Take();
    }

    std::string DisplayProfileToJson(const Rendering::DisplayProfile& profile)
    {
        JsonWriter json;
        json.BeginObject()
            .Key("name").String(profile.profileName)
            .Key("hdrEnabled").Bool(profile.caps.hdrEnabled)
            .Key("bitsPerColor").Int(profile.caps.bitsPerColor)
            .Key("sdrWhiteNits").Fixed(profile.caps.sdrWhiteLevelNits, 2)
            .Key("peakNits").Fixed(profile.caps.maxLuminanceNits, 2)
            .Key("minNits").Fixed(profile.caps.minLuminanceNits, 4)
            .Key("maxFullFrameNits").Fixed(profile.caps.maxFullFrameLuminanceNits, 2)
            .Key("gamut").String(Rendering::GamutIdToString(profile.gamut))
            .Key("isSimulated").Bool(profile.isSimulated);
        WriteChromaticity(json, "primaryRed", profile.primaryRed);
        WriteChromaticity(json, "primaryGreen", profile.primaryGreen);
        WriteChromaticity(json, "primaryBlue", profile.primaryBlue);
        WriteChromaticity(json, "whitePoint", profile.whitePoint);
        return json.EndObject().Take();
    }

    TempFileResult WriteTempPng(std::wstring_view prefix, std::span<const uint8_t> png)
    {
        static std::atomic<uint32_t> sSequence{ 0 };
        TempFileResult result;
        wchar_t tempDir[MAX_PATH + 1]{};
        if (::GetTempPathW(MAX_PATH + 1, tempDir) == 0)
        {
            result.error = std::format("GetTempPath failed (Win32 error {})", ::GetLastError());
            return result;
        }
        const std::wstring path = std::format(L"{}{}_{}_{}.png", tempDir, prefix, ::GetCurrentProcessId(),
            sSequence.fetch_add(1, std::memory_order_relaxed));
        HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            result.error = std::format("could not create the temp file (Win32 error {})", ::GetLastError());
            return result;
        }
        DWORD written = 0;
        const BOOL ok = ::WriteFile(file, png.data(), static_cast<DWORD>(png.size()), &written, nullptr);
        const DWORD writeError = ::GetLastError();
        ::CloseHandle(file);
        if (!ok || written != png.size())
        {
            ::DeleteFileW(path.c_str());
            result.error = std::format("could not write the temp file (Win32 error {})", ok ? 0u : writeError);
            return result;
        }
        result.path = path;
        return result;
    }
}
