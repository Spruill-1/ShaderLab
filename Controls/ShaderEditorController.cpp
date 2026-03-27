#include "pch.h"
#include "ShaderEditorController.h"

namespace Numerics = winrt::Windows::Foundation::Numerics;

namespace ShaderLab::Controls
{
    // -----------------------------------------------------------------------
    // Compilation
    // -----------------------------------------------------------------------

    EditorCompileResult ShaderEditorController::Compile(
        const std::string& hlslSource,
        const std::string& shaderType,
        const std::string& entryPoint)
    {
        EditorCompileResult result;

        auto compileResult = Effects::ShaderCompiler::CompileFromString(
            hlslSource, "ShaderEditor", entryPoint, shaderType);

        result.succeeded = compileResult.succeeded;
        result.bytecode = compileResult.bytecode;
        result.errorText = compileResult.ErrorMessage();
        result.errorLine = ParseErrorLine(result.errorText);

        // On success, reflect to discover constant buffers.
        if (result.succeeded && result.bytecode)
        {
            result.reflection = Effects::ShaderCompiler::Reflect(result.bytecode.get());
            result.autoProperties = BuildAutoProperties(result.reflection);
        }

        m_lastResult = result;
        return result;
    }

    EditorCompileResult ShaderEditorController::CompileFromFile(
        const std::filesystem::path& hlslPath,
        const std::string& shaderType,
        const std::string& entryPoint)
    {
        EditorCompileResult result;

        auto compileResult = Effects::ShaderCompiler::CompileFromFile(
            hlslPath, entryPoint, shaderType);

        result.succeeded = compileResult.succeeded;
        result.bytecode = compileResult.bytecode;
        result.errorText = compileResult.ErrorMessage();
        result.errorLine = ParseErrorLine(result.errorText);

        if (result.succeeded && result.bytecode)
        {
            result.reflection = Effects::ShaderCompiler::Reflect(result.bytecode.get());
            result.autoProperties = BuildAutoProperties(result.reflection);
        }

        m_lastResult = result;
        return result;
    }

    // -----------------------------------------------------------------------
    // Error parsing
    // -----------------------------------------------------------------------

    uint32_t ShaderEditorController::ParseErrorLine(const std::wstring& errorText)
    {
        // D3DCompile errors have the format:
        //   sourceName(line,col): error XNNNN: message
        // e.g. "ShaderEditor(12,5): error X3000: syntax error"
        // We extract the line number from the first error.
        if (errorText.empty())
            return 0;

        auto parenPos = errorText.find(L'(');
        if (parenPos == std::wstring::npos)
            return 0;

        auto commaPos = errorText.find(L',', parenPos);
        if (commaPos == std::wstring::npos)
            commaPos = errorText.find(L')', parenPos);
        if (commaPos == std::wstring::npos)
            return 0;

        std::wstring lineStr = errorText.substr(parenPos + 1, commaPos - parenPos - 1);
        try
        {
            return static_cast<uint32_t>(std::stoul(lineStr));
        }
        catch (...)
        {
            return 0;
        }
    }

    // -----------------------------------------------------------------------
    // Auto-property generation from reflection
    // -----------------------------------------------------------------------

    std::vector<AutoProperty> ShaderEditorController::BuildAutoProperties(
        const Effects::ShaderReflectionResult& reflection)
    {
        std::vector<AutoProperty> properties;

        for (const auto& cb : reflection.constantBuffers)
        {
            // Skip system-reserved constant buffers (e.g., $Globals may contain
            // user variables, but buffers starting with "$" other than $Globals
            // are internal).
            if (cb.name.starts_with(L"$") && cb.name != L"$Globals")
                continue;

            for (const auto& var : cb.variables)
            {
                AutoProperty prop;
                prop.name = var.name;
                prop.variable = var;
                prop.defaultValue = DefaultValueForVariable(var);
                properties.push_back(std::move(prop));
            }
        }

        return properties;
    }

    Graph::PropertyValue ShaderEditorController::DefaultValueForVariable(
        const Effects::ShaderVariable& variable)
    {
        // Map D3D shader variable types to PropertyValue defaults.
        switch (variable.type)
        {
        case D3D_SVT_FLOAT:
        {
            if (variable.columns == 1 && variable.rows == 1)
                return 0.0f;
            if (variable.columns == 2 && variable.rows == 1)
                return Numerics::float2{ 0.0f, 0.0f };
            if (variable.columns == 3 && variable.rows == 1)
                return Numerics::float3{ 0.0f, 0.0f, 0.0f };
            if (variable.columns == 4 && variable.rows == 1)
                return Numerics::float4{ 0.0f, 0.0f, 0.0f, 0.0f };
            // Matrix and higher types — fall through to float.
            return 0.0f;
        }
        case D3D_SVT_INT:
            return static_cast<int32_t>(0);
        case D3D_SVT_UINT:
            return static_cast<uint32_t>(0);
        case D3D_SVT_BOOL:
            return false;
        default:
            // Unknown type — default to float.
            return 0.0f;
        }
    }

    // -----------------------------------------------------------------------
    // Default shader templates
    // -----------------------------------------------------------------------

    std::string ShaderEditorController::DefaultPixelShaderTemplate()
    {
        return R"(// ShaderLab Pixel Shader
// Texture inputs are bound as Texture2D in register(t0), t1, etc.
// Constant buffer variables appear as auto-generated properties.

Texture2D InputTexture : register(t0);
SamplerState InputSampler : register(s0);

cbuffer Constants : register(b0)
{
    float Intensity;    // 0.0 to 1.0
    float2 Offset;      // UV offset
};

float4 main(
    float4 pos      : SV_POSITION,
    float4 posScene : SCENE_POSITION,
    float4 uv       : TEXCOORD0
) : SV_TARGET
{
    float2 texCoord = uv.xy + Offset;
    float4 color = InputTexture.Sample(InputSampler, texCoord);

    // Apply intensity adjustment.
    color.rgb *= Intensity;

    return color;
}
)";
    }

    std::string ShaderEditorController::DefaultComputeShaderTemplate()
    {
        return R"(// ShaderLab Compute Shader
// Input textures are SRVs; output is a UAV.
// Thread group size: [numthreads(8, 8, 1)]

Texture2D<float4> InputTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);

cbuffer Constants : register(b0)
{
    float Intensity;    // 0.0 to 1.0
    uint2 OutputSize;   // Output dimensions
};

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= OutputSize.x || DTid.y >= OutputSize.y)
        return;

    float4 color = InputTexture[DTid.xy];

    // Apply intensity adjustment.
    color.rgb *= Intensity;

    OutputTexture[DTid.xy] = color;
}
)";
    }
}
