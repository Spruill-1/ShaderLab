#pragma once

#include "pch_engine.h"

namespace ShaderLab::Rendering
{
    // Result of a GPU image statistics reduction.
    struct ImageStats
    {
        float min{ 0 };
        float max{ 0 };
        float mean{ 0 };
        float median{ 0 };
        float p95{ 0 };
        float sum{ 0 };
        uint32_t samples{ 0 };
        uint32_t totalPixels{ 0 };
        uint32_t nonzeroPixels{ 0 };
    };

    // GPU-accelerated image statistics using D3D11 compute shader.
    // Single-dispatch reduction: one thread group (32x32=1024 threads)
    // strides across the entire image, reduces in groupshared memory.
    class GpuReduction
    {
    public:
        GpuReduction() = default;

        // Initialize with a D3D11 device. Compiles the reduction shader.
        void Initialize(ID3D11Device* device);

        // Compute statistics on a D3D11 texture.
        // channel: 0=luminance, 1=R, 2=G, 3=B, 4=A
        // nonzeroOnly: if true, min/max/mean/sum exclude zero pixels
        ImageStats Reduce(
            ID3D11DeviceContext* ctx,
            ID3D11Texture2D* input,
            uint32_t channel,
            bool nonzeroOnly);

        bool IsInitialized() const { return m_shader != nullptr; }

    private:
        winrt::com_ptr<ID3D11ComputeShader> m_shader;
        winrt::com_ptr<ID3D11Buffer> m_resultBuffer;    // UAV output (8 stats + 256 histogram bins)
        winrt::com_ptr<ID3D11Buffer> m_stagingBuffer;   // CPU readback
        winrt::com_ptr<ID3D11Buffer> m_cbuffer;          // Constants
        winrt::com_ptr<ID3D11UnorderedAccessView> m_resultUAV;

        static constexpr uint32_t HIST_BINS = 256;
        static constexpr uint32_t STATS_UINTS = 8;
        static constexpr uint32_t TOTAL_UINTS = STATS_UINTS + HIST_BINS;  // 264
    };
}
