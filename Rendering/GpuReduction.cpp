#include "pch_engine.h"
#include "GpuReduction.h"

namespace ShaderLab::Rendering
{
    // The reduction shader: 32x32 = 1024 threads in one group.
    // Each thread strides across the input, accumulates per-thread partials,
    // then reduces in groupshared memory. Output: 8 uints packed into a buffer.
    //
    // Buffer layout (uint):
    //   [0] = minBits (float as uint, IEEE 754 ordering for non-negative)
    //   [1] = maxBits (float as uint)
    //   [2] = sumHi   (high 32 bits of fixed-point sum: value * 10000)
    //   [3] = sumLo   (low 32 bits — we only use Lo for simplicity)
    //   [4] = sampleCount
    //   [5] = totalPixels
    //   [6] = nonzeroPixels
    //   [7] = reserved
    static const char s_reductionHLSL[] = R"(
cbuffer Constants : register(b0)
{
    uint Width;
    uint Height;
    uint Channel;      // 0=luminance, 1=R, 2=G, 3=B, 4=A
    uint NonzeroOnly;  // 0=all, 1=nonzero only
};

Texture2D<float4> Input : register(t0);
RWBuffer<uint> Result : register(u0);

#define GROUP_SIZE 32
#define THREAD_COUNT (GROUP_SIZE * GROUP_SIZE)
#define HIST_BINS 256

groupshared float gs_min[THREAD_COUNT];
groupshared float gs_max[THREAD_COUNT];
groupshared float gs_sum[THREAD_COUNT];
groupshared uint  gs_count[THREAD_COUNT];
groupshared uint  gs_total[THREAD_COUNT];
groupshared uint  gs_nonzero[THREAD_COUNT];
groupshared uint  gs_hist[HIST_BINS];
groupshared float gs_histMin;  // broadcast after reduction
groupshared float gs_histMax;

float GetValue(float4 pix, uint ch)
{
    float result = 0.2126 * pix.r + 0.7152 * pix.g + 0.0722 * pix.b;
    if (ch == 1) result = pix.r;
    else if (ch == 2) result = pix.g;
    else if (ch == 3) result = pix.b;
    else if (ch == 4) result = pix.a;
    return result;
}

[numthreads(GROUP_SIZE, GROUP_SIZE, 1)]
void main(uint3 GTid : SV_GroupThreadID)
{
    uint tid = GTid.x + GTid.y * GROUP_SIZE;

    // ---- Pass 1: compute min/max/sum/count (no histogram yet) ----
    float tMin = 1e30;
    float tMax = -1e30;
    float tSum = 0;
    uint  tCount = 0;
    uint  tTotal = 0;
    uint  tNonzero = 0;

    for (uint y = GTid.y; y < Height; y += GROUP_SIZE)
    {
        for (uint x = GTid.x; x < Width; x += GROUP_SIZE)
        {
            float4 pix = Input[int2(x, y)];
            float v = GetValue(pix, Channel);

            tTotal++;
            bool nz = abs(v) > 0.0001;
            if (nz) tNonzero++;

            if (NonzeroOnly == 1 && !nz) continue;

            tMin = min(tMin, v);
            tMax = max(tMax, v);
            tSum += v;
            tCount++;
        }
    }

    gs_min[tid] = tMin;
    gs_max[tid] = tMax;
    gs_sum[tid] = tSum;
    gs_count[tid] = tCount;
    gs_total[tid] = tTotal;
    gs_nonzero[tid] = tNonzero;

    GroupMemoryBarrierWithGroupSync();

    // Parallel reduction for min/max/sum/count.
    for (uint stride = THREAD_COUNT / 2; stride > 0; stride >>= 1)
    {
        if (tid < stride)
        {
            gs_min[tid] = min(gs_min[tid], gs_min[tid + stride]);
            gs_max[tid] = max(gs_max[tid], gs_max[tid + stride]);
            gs_sum[tid] += gs_sum[tid + stride];
            gs_count[tid] += gs_count[tid + stride];
            gs_total[tid] += gs_total[tid + stride];
            gs_nonzero[tid] += gs_nonzero[tid + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    // Thread 0 broadcasts the data range for histogram binning.
    if (tid == 0)
    {
        gs_histMin = gs_min[0];
        gs_histMax = gs_max[0];
    }
    GroupMemoryBarrierWithGroupSync();

    // ---- Pass 2: build adaptive histogram over [min, max] ----
    float hMin = gs_histMin;
    float hMax = gs_histMax;
    float hRange = hMax - hMin;

    // Clear histogram bins.
    for (uint bi = tid; bi < HIST_BINS; bi += THREAD_COUNT)
        gs_hist[bi] = 0;
    GroupMemoryBarrierWithGroupSync();

    if (hRange > 0.0 && gs_count[0] > 0)
    {
        for (uint y2 = GTid.y; y2 < Height; y2 += GROUP_SIZE)
        {
            for (uint x2 = GTid.x; x2 < Width; x2 += GROUP_SIZE)
            {
                float4 pix = Input[int2(x2, y2)];
                float v = GetValue(pix, Channel);
                bool nz = abs(v) > 0.0001;
                if (NonzeroOnly == 1 && !nz) continue;

                float normalized = saturate((v - hMin) / hRange);
                uint bin = min((uint)(normalized * 255.0), 255u);
                InterlockedAdd(gs_hist[bin], 1u);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // Thread 0 writes stats.
    if (tid == 0)
    {
        Result[0] = asuint(gs_min[0]);
        Result[1] = asuint(gs_max[0]);
        Result[2] = asuint(gs_sum[0]);
        Result[3] = 0;
        Result[4] = gs_count[0];
        Result[5] = gs_total[0];
        Result[6] = gs_nonzero[0];
        Result[7] = 0;
    }

    // All threads cooperate to write histogram bins to result buffer.
    for (uint hbi = tid; hbi < HIST_BINS; hbi += THREAD_COUNT)
        Result[8 + hbi] = gs_hist[hbi];
}
)";

    void GpuReduction::Initialize(ID3D11Device* device)
    {
        if (!device) return;

        // Compile the reduction shader.
        winrt::com_ptr<ID3DBlob> blob;
        winrt::com_ptr<ID3DBlob> errors;
        HRESULT hr = D3DCompile(
            s_reductionHLSL, sizeof(s_reductionHLSL),
            "ImageStatsReduction", nullptr, nullptr,
            "main", "cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, blob.put(), errors.put());

        if (FAILED(hr))
        {
            if (errors)
            {
                OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
            }
            return;
        }

        hr = device->CreateComputeShader(
            blob->GetBufferPointer(), blob->GetBufferSize(),
            nullptr, m_shader.put());
        if (FAILED(hr)) return;

        // Create result buffer (8 stats + 256 histogram bins = 264 uints).
        D3D11_BUFFER_DESC bufDesc{};
        bufDesc.ByteWidth = TOTAL_UINTS * sizeof(uint32_t);
        bufDesc.Usage = D3D11_USAGE_DEFAULT;
        bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        hr = device->CreateBuffer(&bufDesc, nullptr, m_resultBuffer.put());
        if (FAILED(hr)) return;

        // UAV for the result buffer.
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = TOTAL_UINTS;
        uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        hr = device->CreateUnorderedAccessView(m_resultBuffer.get(), &uavDesc, m_resultUAV.put());
        if (FAILED(hr)) return;

        // Staging buffer for CPU readback.
        bufDesc.ByteWidth = TOTAL_UINTS * sizeof(uint32_t);
        bufDesc.Usage = D3D11_USAGE_STAGING;
        bufDesc.BindFlags = 0;
        bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        bufDesc.MiscFlags = 0;
        hr = device->CreateBuffer(&bufDesc, nullptr, m_stagingBuffer.put());
        if (FAILED(hr)) return;

        // Constant buffer (4 uints = 16 bytes).
        bufDesc.ByteWidth = 16;
        bufDesc.Usage = D3D11_USAGE_DYNAMIC;
        bufDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bufDesc.MiscFlags = 0;
        hr = device->CreateBuffer(&bufDesc, nullptr, m_cbuffer.put());
    }

    ImageStats GpuReduction::Reduce(
        ID3D11DeviceContext* ctx,
        ID3D11Texture2D* input,
        uint32_t channel,
        bool nonzeroOnly)
    {
        ImageStats stats{};
        if (!m_shader || !ctx || !input) return stats;

        // Get input dimensions.
        D3D11_TEXTURE2D_DESC texDesc{};
        input->GetDesc(&texDesc);

        // Create SRV for the input texture.
        winrt::com_ptr<ID3D11Device> device;
        ctx->GetDevice(device.put());

        winrt::com_ptr<ID3D11ShaderResourceView> srv;
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = texDesc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        HRESULT hr = device->CreateShaderResourceView(input, &srvDesc, srv.put());
        if (FAILED(hr)) return stats;

        // Update constant buffer.
        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = ctx->Map(m_cbuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr))
        {
            uint32_t* cb = static_cast<uint32_t*>(mapped.pData);
            cb[0] = texDesc.Width;
            cb[1] = texDesc.Height;
            cb[2] = channel;
            cb[3] = nonzeroOnly ? 1 : 0;
            ctx->Unmap(m_cbuffer.get(), 0);
        }

        // Clear result buffer to initial values.
        uint32_t clearValues[4] = { 0, 0, 0, 0 };
        ctx->ClearUnorderedAccessViewUint(m_resultUAV.get(), clearValues);

        // Dispatch.
        ctx->CSSetShader(m_shader.get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[] = { srv.get() };
        ctx->CSSetShaderResources(0, 1, srvs);
        ID3D11UnorderedAccessView* uavs[] = { m_resultUAV.get() };
        ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        ID3D11Buffer* cbs[] = { m_cbuffer.get() };
        ctx->CSSetConstantBuffers(0, 1, cbs);

        ctx->Dispatch(1, 1, 1);

        // Clear shader state.
        ID3D11ShaderResourceView* nullSRV[] = { nullptr };
        ctx->CSSetShaderResources(0, 1, nullSRV);
        ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
        ctx->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
        ctx->CSSetShader(nullptr, nullptr, 0);

        // Copy result to staging buffer and read back.
        ctx->CopyResource(m_stagingBuffer.get(), m_resultBuffer.get());

        hr = ctx->Map(m_stagingBuffer.get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr))
        {
            const uint32_t* result = static_cast<const uint32_t*>(mapped.pData);
            float minVal = *reinterpret_cast<const float*>(&result[0]);
            float maxVal = *reinterpret_cast<const float*>(&result[1]);
            float sumVal = *reinterpret_cast<const float*>(&result[2]);
            uint32_t sampleCount = result[4];
            uint32_t totalPixels = result[5];
            uint32_t nonzeroPixels = result[6];

            stats.min = (sampleCount > 0) ? minVal : 0;
            stats.max = (sampleCount > 0) ? maxVal : 0;
            stats.sum = sumVal;
            stats.samples = sampleCount;
            stats.totalPixels = totalPixels;
            stats.nonzeroPixels = nonzeroPixels;
            stats.mean = (sampleCount > 0) ? sumVal / static_cast<float>(sampleCount) : 0;

            // Compute median and P95 from adaptive histogram.
            // Histogram bins span [minVal, maxVal] (set by the shader's second pass).
            float histRange = maxVal - minVal;
            if (sampleCount > 0 && histRange > 0)
            {
                const uint32_t* hist = result + STATS_UINTS;
                uint32_t halfCount = sampleCount / 2;
                uint32_t p95Count = static_cast<uint32_t>(sampleCount * 0.95f);
                uint32_t cumulative = 0;
                bool foundMedian = false, foundP95 = false;

                for (uint32_t bi = 0; bi < HIST_BINS; ++bi)
                {
                    cumulative += hist[bi];
                    // Map bin center back to value in [min, max].
                    float binValue = minVal + ((static_cast<float>(bi) + 0.5f) / 255.0f) * histRange;
                    if (!foundMedian && cumulative >= halfCount)
                    {
                        stats.median = binValue;
                        foundMedian = true;
                    }
                    if (!foundP95 && cumulative >= p95Count)
                    {
                        stats.p95 = binValue;
                        foundP95 = true;
                    }
                    if (foundMedian && foundP95) break;
                }
            }

            ctx->Unmap(m_stagingBuffer.get(), 0);
        }

        return stats;
    }
}
