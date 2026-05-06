#include "pch_engine.h"
#include "ShaderCompiler.h"
#include "../Graph/PropertyValue.h"

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

        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
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

    ShaderReflectionResult ShaderCompiler::Reflect(const std::vector<uint8_t>& bytecode)
    {
        ShaderReflectionResult result;
        if (bytecode.empty()) return result;

        winrt::com_ptr<ID3D11ShaderReflection> reflection;
        HRESULT hr = D3DReflect(bytecode.data(), bytecode.size(),
            IID_PPV_ARGS(reflection.put()));
        if (FAILED(hr)) return result;

        // Reuse the blob-based Reflect by creating a temporary blob.
        winrt::com_ptr<ID3DBlob> blob;
        D3DCreateBlob(bytecode.size(), blob.put());
        if (blob)
        {
            memcpy(blob->GetBufferPointer(), bytecode.data(), bytecode.size());
            return Reflect(blob.get());
        }
        return result;
    }

    // ----- Typed PropertyValue -> cbuffer pack ---------------------------------

    namespace
    {
        // Convert a float source value to the HLSL slot type and write it.
        // Returns the bytes written.
        uint32_t WriteScalarConverted(BYTE* dest, uint32_t remaining,
            D3D_SHADER_VARIABLE_TYPE hlslType, float src)
        {
            switch (hlslType)
            {
            case D3D_SVT_UINT:
            {
                if (remaining < sizeof(uint32_t)) return 0;
                uint32_t u = static_cast<uint32_t>(src);
                memcpy(dest, &u, sizeof(uint32_t));
                return sizeof(uint32_t);
            }
            case D3D_SVT_INT:
            {
                if (remaining < sizeof(int32_t)) return 0;
                int32_t i = static_cast<int32_t>(src);
                memcpy(dest, &i, sizeof(int32_t));
                return sizeof(int32_t);
            }
            case D3D_SVT_BOOL:
            {
                if (remaining < sizeof(BOOL)) return 0;
                BOOL b = (src > 0.5f) ? TRUE : FALSE;
                memcpy(dest, &b, sizeof(BOOL));
                return sizeof(BOOL);
            }
            case D3D_SVT_FLOAT:
            default:
                if (remaining < sizeof(float)) return 0;
                memcpy(dest, &src, sizeof(float));
                return sizeof(float);
            }
        }
    }

    bool PackPropertyToCBuffer(BYTE* dest, uint32_t remaining,
        D3D_SHADER_VARIABLE_TYPE hlslType, uint32_t hlslColumns,
        const Graph::PropertyValue& value)
    {
        if (!dest || remaining == 0) return false;
        if (hlslColumns == 0) hlslColumns = 1;

        return std::visit([dest, remaining, hlslType, hlslColumns](auto&& v) -> bool
        {
            using T = std::decay_t<decltype(v)>;
            using namespace winrt::Windows::Foundation::Numerics;

            // Scalars (single-column slots): convert through float for
            // float/int/uint/bool slot types so the property's stored
            // representation doesn't dictate the bytes written. Vector
            // slot types receive scalar-broadcast in the .x component
            // only — matching the prior behavior (no broadcast was done).
            if constexpr (std::is_same_v<T, float>)
            {
                return WriteScalarConverted(dest, remaining, hlslType, v) > 0;
            }
            else if constexpr (std::is_same_v<T, int32_t>)
            {
                return WriteScalarConverted(dest, remaining, hlslType,
                    static_cast<float>(v)) > 0;
            }
            else if constexpr (std::is_same_v<T, uint32_t>)
            {
                // uint property: if the slot is also uint/int, write the
                // raw value (don't lose precision via float). If the slot
                // is float, convert.
                if (hlslType == D3D_SVT_UINT || hlslType == D3D_SVT_INT)
                {
                    if (remaining < sizeof(uint32_t)) return false;
                    memcpy(dest, &v, sizeof(uint32_t));
                    return true;
                }
                return WriteScalarConverted(dest, remaining, hlslType,
                    static_cast<float>(v)) > 0;
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                return WriteScalarConverted(dest, remaining, hlslType,
                    v ? 1.0f : 0.0f) > 0;
            }
            else if constexpr (std::is_same_v<T, float2>)
            {
                if (remaining < sizeof(float) * 2) return false;
                memcpy(dest, &v.x, sizeof(float));
                memcpy(dest + sizeof(float), &v.y, sizeof(float));
                return true;
            }
            else if constexpr (std::is_same_v<T, float3>)
            {
                if (remaining < sizeof(float) * 3) return false;
                memcpy(dest, &v.x, sizeof(float));
                memcpy(dest + sizeof(float), &v.y, sizeof(float));
                memcpy(dest + sizeof(float) * 2, &v.z, sizeof(float));
                return true;
            }
            else if constexpr (std::is_same_v<T, float4>)
            {
                if (remaining < sizeof(float) * 4) return false;
                memcpy(dest, &v.x, sizeof(float));
                memcpy(dest + sizeof(float), &v.y, sizeof(float));
                memcpy(dest + sizeof(float) * 2, &v.z, sizeof(float));
                memcpy(dest + sizeof(float) * 3, &v.w, sizeof(float));
                return true;
            }
            // std::wstring / matrix / vector<float>: not packable as a
            // simple cbuffer slot. Caller handles array bindings via
            // separate paths.
            return false;
        }, value);
    }
}
