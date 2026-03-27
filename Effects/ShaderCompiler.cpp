#include "pch.h"
#include "ShaderCompiler.h"

namespace ShaderLab::Effects
{
    // -----------------------------------------------------------------------
    // Error message helper
    // -----------------------------------------------------------------------

    std::wstring ShaderCompileResult::ErrorMessage() const
    {
        if (!errors)
            return {};
        auto* msg = static_cast<const char*>(errors->GetBufferPointer());
        auto len = errors->GetBufferSize();
        if (!msg || len == 0)
            return {};
        // Convert UTF-8 error text to wide string.
        int wideLen = MultiByteToWideChar(CP_UTF8, 0, msg, static_cast<int>(len), nullptr, 0);
        std::wstring result(wideLen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, msg, static_cast<int>(len), result.data(), wideLen);
        return result;
    }

    // -----------------------------------------------------------------------
    // Compile from file
    // -----------------------------------------------------------------------

    ShaderCompileResult ShaderCompiler::CompileFromFile(
        const std::filesystem::path& hlslPath,
        const std::string& entryPoint,
        const std::string& target)
    {
        // Read the file into memory.
        std::ifstream file(hlslPath, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            ShaderCompileResult result;
            // Create a synthetic error blob.
            std::string err = "Failed to open file: " + hlslPath.string();
            D3DCreateBlob(err.size(), result.errors.put());
            if (result.errors)
                memcpy(result.errors->GetBufferPointer(), err.data(), err.size());
            return result;
        }

        auto size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::string source(static_cast<size_t>(size), '\0');
        file.read(source.data(), size);

        return CompileFromString(source, hlslPath.filename().string(), entryPoint, target);
    }

    // -----------------------------------------------------------------------
    // Compile from string
    // -----------------------------------------------------------------------

    ShaderCompileResult ShaderCompiler::CompileFromString(
        const std::string& hlslSource,
        const std::string& sourceName,
        const std::string& entryPoint,
        const std::string& target)
    {
        ShaderCompileResult result;

        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

        HRESULT hr = D3DCompile(
            hlslSource.data(),
            hlslSource.size(),
            sourceName.c_str(),
            nullptr,                // defines
            D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entryPoint.c_str(),
            target.c_str(),
            flags,
            0,                      // effect flags
            result.bytecode.put(),
            result.errors.put());

        result.succeeded = SUCCEEDED(hr);
        return result;
    }

    // -----------------------------------------------------------------------
    // Reflection
    // -----------------------------------------------------------------------

    ShaderReflectionResult ShaderCompiler::Reflect(ID3DBlob* bytecode)
    {
        ShaderReflectionResult result;
        if (!bytecode)
            return result;

        winrt::com_ptr<ID3D11ShaderReflection> reflection;
        HRESULT hr = D3DReflect(
            bytecode->GetBufferPointer(),
            bytecode->GetBufferSize(),
            IID_ID3D11ShaderReflection,
            reflection.put_void());

        if (FAILED(hr))
            return result;

        D3D11_SHADER_DESC shaderDesc{};
        reflection->GetDesc(&shaderDesc);
        result.boundResources = shaderDesc.BoundResources;

        // Count texture SRV inputs.
        for (uint32_t i = 0; i < shaderDesc.BoundResources; ++i)
        {
            D3D11_SHADER_INPUT_BIND_DESC bindDesc{};
            reflection->GetResourceBindingDesc(i, &bindDesc);
            if (bindDesc.Type == D3D_SIT_TEXTURE)
                ++result.inputCount;
        }

        // Enumerate constant buffers.
        for (uint32_t cbIdx = 0; cbIdx < shaderDesc.ConstantBuffers; ++cbIdx)
        {
            auto* cbReflection = reflection->GetConstantBufferByIndex(cbIdx);
            D3D11_SHADER_BUFFER_DESC cbDesc{};
            cbReflection->GetDesc(&cbDesc);

            ShaderConstantBuffer cb;
            int wideLen = MultiByteToWideChar(CP_UTF8, 0, cbDesc.Name, -1, nullptr, 0);
            cb.name.resize(wideLen - 1);
            MultiByteToWideChar(CP_UTF8, 0, cbDesc.Name, -1, cb.name.data(), wideLen);
            cb.sizeBytes = cbDesc.Size;

            for (uint32_t varIdx = 0; varIdx < cbDesc.Variables; ++varIdx)
            {
                auto* varReflection = cbReflection->GetVariableByIndex(varIdx);
                D3D11_SHADER_VARIABLE_DESC varDesc{};
                varReflection->GetDesc(&varDesc);

                auto* typeReflection = varReflection->GetType();
                D3D11_SHADER_TYPE_DESC typeDesc{};
                typeReflection->GetDesc(&typeDesc);

                ShaderVariable sv;
                wideLen = MultiByteToWideChar(CP_UTF8, 0, varDesc.Name, -1, nullptr, 0);
                sv.name.resize(wideLen - 1);
                MultiByteToWideChar(CP_UTF8, 0, varDesc.Name, -1, sv.name.data(), wideLen);
                sv.offset = varDesc.StartOffset;
                sv.size = varDesc.Size;
                sv.type = typeDesc.Type;
                sv.rows = typeDesc.Rows;
                sv.columns = typeDesc.Columns;

                cb.variables.push_back(std::move(sv));
            }

            result.constantBuffers.push_back(std::move(cb));
        }

        return result;
    }
}
