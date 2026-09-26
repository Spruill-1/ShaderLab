#include "pch_engine.h"
#include "VideoExport.h"
#include "GraphEvaluator.h"
#include "../Graph/EffectGraph.h"
#include "../Effects/ShaderLabEffects.h"
#include "../Effects/SourceNodeFactory.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <mutex>
#include <thread>

namespace ShaderLab::Rendering
{
    using Graph::EffectGraph;
    using Graph::EffectNode;
    using Graph::NodeType;

    namespace
    {
        // ---- small helpers ------------------------------------------------

        float GetF(const EffectNode& n, const wchar_t* key, float def)
        {
            auto it = n.properties.find(key);
            if (it == n.properties.end()) return def;
            if (auto* f = std::get_if<float>(&it->second)) return *f;
            if (auto* u = std::get_if<uint32_t>(&it->second)) return static_cast<float>(*u);
            if (auto* i = std::get_if<int32_t>(&it->second)) return static_cast<float>(*i);
            return def;
        }

        bool GetF2(const EffectNode& n, const wchar_t* key, float& x, float& y)
        {
            auto it = n.properties.find(key);
            if (it == n.properties.end()) return false;
            if (auto* v = std::get_if<winrt::Windows::Foundation::Numerics::float2>(&it->second))
            {
                x = v->x; y = v->y;
                return true;
            }
            return false;
        }

        std::wstring Quote(const std::wstring& s)
        {
            if (s.find_first_of(L" \t\"") == std::wstring::npos) return s;
            std::wstring out = L"\"";
            for (wchar_t c : s) { if (c == L'"') out += L'\\'; out += c; }
            return out + L"\"";
        }

        std::wstring FormatFps(double fps)
        {
            // Integral rates stay integral ("60"); NTSC-style rates are
            // written as the exact rational ffmpeg expects.
            if (std::abs(fps - std::round(fps)) < 1e-9) return std::format(L"{}", static_cast<long long>(std::round(fps)));
            const double ntsc = std::round(fps * 1.001);
            if (std::abs(fps - ntsc / 1.001) < 1e-6) return std::format(L"{}000/1001", static_cast<long long>(ntsc));
            return std::format(L"{:.6f}", fps);
        }

        // ---- ffmpeg child process -----------------------------------------
        //
        // stdin carries raw frames; stdout+stderr share one pipe that a
        // reader thread drains CONTINUOUSLY. Leaving it undrained deadlocks
        // the export: ffmpeg blocks writing its log, stops reading stdin, and
        // our frame write blocks forever.
        class FfmpegProcess
        {
        public:
            ~FfmpegProcess() { Abort(); }

            bool Start(const std::wstring& exe, const std::wstring& args, bool withStdin, std::wstring& error)
            {
                SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
                HANDLE outRead = nullptr, outWrite = nullptr, inRead = nullptr;
                if (!CreatePipe(&outRead, &outWrite, &sa, 0))
                {
                    error = L"CreatePipe(stdout) failed";
                    return false;
                }
                SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
                if (withStdin)
                {
                    if (!CreatePipe(&inRead, &m_stdinWrite, &sa, 1 << 20))
                    {
                        CloseHandle(outRead); CloseHandle(outWrite);
                        error = L"CreatePipe(stdin) failed";
                        return false;
                    }
                    SetHandleInformation(m_stdinWrite, HANDLE_FLAG_INHERIT, 0);
                }
                STARTUPINFOW si{ sizeof(si) };
                si.dwFlags = STARTF_USESTDHANDLES;
                si.hStdInput = withStdin ? inRead : GetStdHandle(STD_INPUT_HANDLE);
                si.hStdOutput = outWrite;
                si.hStdError = outWrite;
                PROCESS_INFORMATION pi{};
                std::wstring cmd = Quote(exe) + L" " + args;
                const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                    CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
                // The child owns its ends now; ours must close or EOF never arrives.
                CloseHandle(outWrite);
                if (inRead) CloseHandle(inRead);
                if (!ok)
                {
                    const DWORD e = GetLastError();
                    CloseHandle(outRead);
                    if (m_stdinWrite) { CloseHandle(m_stdinWrite); m_stdinWrite = nullptr; }
                    error = std::format(L"could not start ffmpeg ({}): Win32 error {}", exe, e);
                    return false;
                }
                CloseHandle(pi.hThread);
                m_process = pi.hProcess;
                m_outRead = outRead;
                m_reader = std::thread([this] {
                    char buf[4096];
                    DWORD n = 0;
                    while (ReadFile(m_outRead, buf, sizeof(buf), &n, nullptr) && n > 0)
                    {
                        std::scoped_lock lock(m_logMutex);
                        m_log.append(buf, n);
                        if (m_log.size() > 256 * 1024) m_log.erase(0, m_log.size() - 128 * 1024);
                    }
                });
                return true;
            }

            bool Write(const void* data, size_t size)
            {
                const auto* p = static_cast<const uint8_t*>(data);
                while (size > 0)
                {
                    DWORD chunk = static_cast<DWORD>((std::min)(size, size_t(1) << 24));
                    DWORD written = 0;
                    if (!WriteFile(m_stdinWrite, p, chunk, &written, nullptr) || written == 0)
                        return false;   // broken pipe: ffmpeg exited; the log says why
                    p += written;
                    size -= written;
                }
                return true;
            }

            // Close stdin (end of stream) and wait for exit. Returns the exit
            // code, or -1 on timeout (the process is then terminated).
            int Finish(DWORD timeoutMs)
            {
                if (m_stdinWrite) { CloseHandle(m_stdinWrite); m_stdinWrite = nullptr; }
                int code = -1;
                if (m_process)
                {
                    if (WaitForSingleObject(m_process, timeoutMs) == WAIT_OBJECT_0)
                    {
                        DWORD c = 0;
                        GetExitCodeProcess(m_process, &c);
                        code = static_cast<int>(c);
                    }
                    else
                    {
                        TerminateProcess(m_process, 1);
                        WaitForSingleObject(m_process, 5000);
                    }
                    CloseHandle(m_process);
                    m_process = nullptr;
                }
                if (m_reader.joinable()) m_reader.join();
                if (m_outRead) { CloseHandle(m_outRead); m_outRead = nullptr; }
                return code;
            }

            void Abort()
            {
                if (m_process) TerminateProcess(m_process, 1);
                Finish(5000);
            }

            std::string Log()
            {
                std::scoped_lock lock(m_logMutex);
                return m_log;
            }

        private:
            HANDLE m_process{ nullptr };
            HANDLE m_stdinWrite{ nullptr };
            HANDLE m_outRead{ nullptr };
            std::thread m_reader;
            std::mutex m_logMutex;
            std::string m_log;
        };

        std::string Tail(const std::string& s, size_t n)
        {
            return s.size() <= n ? s : s.substr(s.size() - n);
        }

        // ---- GPU conversion -------------------------------------------------

        // HLSL helpers. Kept separate from the kernel so the shader test bench
        // can call them directly and compare against textbook values.
        const char* kHelpersHLSL = R"HLSL(
// ---- Video export conversion (VideoExport.cpp) ----
// HDR10: scRGB (linear Rec.709, 1.0 = 80 nits) -> BT.2020 linear nits,
// clamped to [0, 10000] -> PQ. Colours outside BT.2020 are clipped here; there
// is no representation for them in the deliverable.
float3 VideoEncodeHdr10(float3 scRGB)
{
    float3 rgb2020 = mul(XYZ_TO_REC2020, mul(REC709_TO_XYZ, scRGB));
    float3 nits = clamp(rgb2020 * 80.0, 0.0, 10000.0);
    return float3(PQ_InvEOTF(nits.r), PQ_InvEOTF(nits.g), PQ_InvEOTF(nits.b));
}

// SDR: the sRGB curve on [0, 1] scRGB (1.0 = SDR white), tagged BT.709 by the
// container -- what a screen recorder does. Out-of-[0,1] values clip.
float3 VideoEncodeSdr(float3 scRGB)
{
    return LinearToSRGB(saturate(scRGB));
}

// Content light of one pixel for MaxCLL / MaxFALL: the largest BT.2020 linear
// component in nits, clamped to the PQ range (CTA-861.3 defines both on the
// max of R, G, B).
float VideoContentNits(float3 scRGB)
{
    float3 rgb2020 = mul(XYZ_TO_REC2020, mul(REC709_TO_XYZ, scRGB));
    float3 nits = clamp(rgb2020 * 80.0, 0.0, 10000.0);
    return max(nits.r, max(nits.g, nits.b));
}

// Limited-range code values, UNSHIFTED (10-bit for mode 0, 8-bit for mode 1).
// Mode 0: BT.2020 non-constant-luminance.  Mode 1: BT.709.
float VideoLumaCode(float3 rgbPrime, uint mode)
{
    if (mode == 0)
    {
        float y = dot(rgbPrime, float3(0.2627, 0.6780, 0.0593));
        return clamp(floor(64.0 + 876.0 * y + 0.5), 0.0, 1023.0);
    }
    float y8 = dot(rgbPrime, float3(0.2126, 0.7152, 0.0722));
    return clamp(floor(16.0 + 219.0 * y8 + 0.5), 0.0, 255.0);
}

// Cb / Cr as signed, unscaled differences; averaged BEFORE quantising.
float2 VideoChromaDiff(float3 rgbPrime, uint mode)
{
    if (mode == 0)
    {
        float y = dot(rgbPrime, float3(0.2627, 0.6780, 0.0593));
        return float2((rgbPrime.b - y) / 1.8814, (rgbPrime.r - y) / 1.4746);
    }
    float y8 = dot(rgbPrime, float3(0.2126, 0.7152, 0.0722));
    return float2((rgbPrime.b - y8) / 1.8556, (rgbPrime.r - y8) / 1.5748);
}

float2 VideoChromaCode(float2 cbcr, uint mode)
{
    if (mode == 0)
        return clamp(floor(512.0 + 896.0 * cbcr + 0.5), 0.0, 1023.0);
    return clamp(floor(128.0 + 224.0 * cbcr + 0.5), 0.0, 255.0);
}
)HLSL";

        const char* kKernelHLSL = R"HLSL(
Texture2D<float4>          Src   : register(t0);
RWTexture2D<uint>          OutY  : register(u0);
RWTexture2D<uint2>         OutUV : register(u1);
RWStructuredBuffer<float2> Stats : register(u2);   // per group: (max, sum) content nits

cbuffer Constants : register(b0)
{
    uint Width;     // even
    uint Height;    // even
    uint Mode;      // 0 = HDR10 p010, 1 = SDR nv12
    uint GroupsX;
};

groupshared float gsMax[64];
groupshared float gsSum[64];

// One thread per 2x2 block = one chroma sample.
//
// Chroma is LEFT-COSITED (MPEG-2 / H.264 / HEVC default, chroma_sample_loc 0):
// horizontally it sits ON luma column 2i, so it is filtered [1 2 1]/4 over
// columns 2i-1, 2i, 2i+1; vertically it sits between rows 2j and 2j+1, so
// those two are averaged. A plain 2x2 box average would describe CENTRED
// chroma and put every colour edge half a luma pixel off. Filtering happens
// on the non-linear (encoded) values, as every standard encoder chain does.
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    uint mode = Mode;
    uint w = Width;
    uint h = Height;
    uint gx = GroupsX;
    uint cw = w / 2;
    uint ch = h / 2;

    float blockMax = 0.0;
    float blockSum = 0.0;
    if (id.x < cw && id.y < ch)
    {
        int x0 = int(id.x) * 2;
        int y0 = int(id.y) * 2;
        float2 chroma = float2(0.0, 0.0);
        [unroll]
        for (int dy = 0; dy < 2; ++dy)
        {
            [unroll]
            for (int k = 0; k < 3; ++k)
            {
                int x = clamp(x0 - 1 + k, 0, int(w) - 1);
                float3 lin = Src.Load(int3(x, y0 + dy, 0)).rgb;
                float3 enc = (mode == 0) ? VideoEncodeHdr10(lin) : VideoEncodeSdr(lin);
                float weight = (k == 1) ? 0.25 : 0.125;   // [1 2 1]/4, halved for the 2-row average
                chroma += weight * VideoChromaDiff(enc, mode);
                if (k >= 1)
                {
                    // Columns x0 and x0+1 are this block's own pixels.
                    uint code = uint(VideoLumaCode(enc, mode));
                    OutY[uint2(x0 + k - 1, y0 + dy)] = (mode == 0) ? (code << 6) : code;
                    float nits = VideoContentNits(lin);
                    blockMax = max(blockMax, nits);
                    blockSum += nits;
                }
            }
        }
        uint2 cc = uint2(VideoChromaCode(chroma, mode));
        OutUV[id.xy] = (mode == 0) ? (cc << 6) : cc;
    }

    gsMax[gi] = blockMax;
    gsSum[gi] = blockSum;
    GroupMemoryBarrierWithGroupSync();
    [unroll]
    for (uint s = 32; s > 0; s >>= 1)
    {
        if (gi < s)
        {
            gsMax[gi] = max(gsMax[gi], gsMax[gi + s]);
            gsSum[gi] += gsSum[gi + s];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0)
        Stats[gid.y * gx + gid.x] = float2(gsMax[0], gsSum[0]);
}
)HLSL";

        struct FrameStats { double maxNits{ 0 }; double meanNits{ 0 }; };

        // Converts one scRGB FP32 texture into packed planes + light stats.
        //
        // D3D11 work is recorded on a private deferred context and submitted
        // as ONE ExecuteCommandList (decision #87): the device's immediate
        // context also carries Direct2D's work, and multithread protection
        // makes single calls atomic, not sequences.
        class Converter
        {
        public:
            bool Init(ID3D11Device* dev, ID3D11DeviceContext* immediate,
                      uint32_t w, uint32_t h, bool hdr, std::wstring& error)
            {
                m_dev.copy_from(dev);
                m_imm.copy_from(immediate);
                m_w = w; m_h = h; m_hdr = hdr;
                m_groupsX = (w / 2 + 7) / 8;
                m_groupsY = (h / 2 + 7) / 8;

                std::string src = Effects::GetColorMathHLSL() + kHelpersHLSL + kKernelHLSL;
                winrt::com_ptr<ID3DBlob> blob, errs;
                HRESULT hr = D3DCompile(src.data(), src.size(), "VideoExportConvert", nullptr, nullptr,
                    "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.put(), errs.put());
                if (FAILED(hr))
                {
                    error = L"conversion shader failed to compile: " +
                        (errs ? winrt::to_hstring(std::string(static_cast<const char*>(errs->GetBufferPointer()),
                                                              errs->GetBufferSize())).c_str()
                              : std::wstring(L"(no log)"));
                    return false;
                }
                if (FAILED(dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, m_cs.put())))
                { error = L"CreateComputeShader failed"; return false; }

                auto tex = [&](uint32_t tw, uint32_t th, DXGI_FORMAT fmt, UINT bind, D3D11_USAGE usage,
                               UINT cpu, winrt::com_ptr<ID3D11Texture2D>& out) -> bool {
                    D3D11_TEXTURE2D_DESC d{};
                    d.Width = tw; d.Height = th; d.MipLevels = 1; d.ArraySize = 1;
                    d.Format = fmt; d.SampleDesc.Count = 1; d.Usage = usage;
                    d.BindFlags = bind; d.CPUAccessFlags = cpu;
                    return SUCCEEDED(dev->CreateTexture2D(&d, nullptr, out.put()));
                };
                const DXGI_FORMAT yFmt  = hdr ? DXGI_FORMAT_R16_UINT    : DXGI_FORMAT_R8_UINT;
                const DXGI_FORMAT uvFmt = hdr ? DXGI_FORMAT_R16G16_UINT : DXGI_FORMAT_R8G8_UINT;
                // FP32, not FP16: this target exists only to hand the node's
                // values to the converter, and an FP16 hop adds a quantisation
                // step D3D lets GPUs implement as round-toward-zero. Measured
                // with an FP16 target: HDR luma biased -0.055 code on average
                // against an FP32 reference, SDR unaffected.
                if (!tex(w, h, DXGI_FORMAT_R32G32B32A32_FLOAT,
                         D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, m_src) ||
                    !tex(w, h, yFmt, D3D11_BIND_UNORDERED_ACCESS, D3D11_USAGE_DEFAULT, 0, m_y) ||
                    !tex(w / 2, h / 2, uvFmt, D3D11_BIND_UNORDERED_ACCESS, D3D11_USAGE_DEFAULT, 0, m_uv) ||
                    !tex(w, h, yFmt, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, m_yStage) ||
                    !tex(w / 2, h / 2, uvFmt, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, m_uvStage))
                { error = std::format(L"could not allocate {}x{} conversion textures", w, h); return false; }

                if (FAILED(dev->CreateShaderResourceView(m_src.get(), nullptr, m_srcSrv.put())) ||
                    FAILED(dev->CreateUnorderedAccessView(m_y.get(), nullptr, m_yUav.put())) ||
                    FAILED(dev->CreateUnorderedAccessView(m_uv.get(), nullptr, m_uvUav.put())))
                { error = L"could not create conversion views"; return false; }

                const UINT groups = m_groupsX * m_groupsY;
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = groups * sizeof(float) * 2;
                bd.Usage = D3D11_USAGE_DEFAULT;
                bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
                bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
                bd.StructureByteStride = sizeof(float) * 2;
                if (FAILED(dev->CreateBuffer(&bd, nullptr, m_stats.put())))
                { error = L"could not create stats buffer"; return false; }
                D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.Format = DXGI_FORMAT_UNKNOWN;
                ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
                ud.Buffer.NumElements = groups;
                if (FAILED(dev->CreateUnorderedAccessView(m_stats.get(), &ud, m_statsUav.put())))
                { error = L"could not create stats view"; return false; }
                bd.Usage = D3D11_USAGE_STAGING; bd.BindFlags = 0; bd.MiscFlags = 0;
                bd.StructureByteStride = 0; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(dev->CreateBuffer(&bd, nullptr, m_statsStage.put())))
                { error = L"could not create stats staging"; return false; }

                D3D11_BUFFER_DESC cb{};
                cb.ByteWidth = 16; cb.Usage = D3D11_USAGE_DEFAULT; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                const uint32_t consts[4] = { w, h, hdr ? 0u : 1u, m_groupsX };
                D3D11_SUBRESOURCE_DATA init{ consts, 0, 0 };
                if (FAILED(dev->CreateBuffer(&cb, &init, m_cb.put())))
                { error = L"could not create constants"; return false; }

                if (FAILED(dev->CreateDeferredContext(0, m_deferred.put())))
                { error = L"CreateDeferredContext failed"; return false; }
                return true;
            }

            ID3D11Texture2D* Source() const { return m_src.get(); }

            // Convert the current contents of Source(). `frame` receives the
            // packed planes (Y then interleaved UV, no row padding) when
            // non-null.
            bool Convert(std::vector<uint8_t>* frame, FrameStats& stats, std::wstring& error)
            {
                ID3D11DeviceContext* d = m_deferred.get();
                ID3D11ShaderResourceView* srvs[] = { m_srcSrv.get() };
                ID3D11UnorderedAccessView* uavs[] = { m_yUav.get(), m_uvUav.get(), m_statsUav.get() };
                ID3D11Buffer* cbs[] = { m_cb.get() };
                d->CSSetShader(m_cs.get(), nullptr, 0);
                d->CSSetShaderResources(0, 1, srvs);
                d->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
                d->CSSetConstantBuffers(0, 1, cbs);
                d->Dispatch(m_groupsX, m_groupsY, 1);
                ID3D11ShaderResourceView* nullSrv[] = { nullptr };
                ID3D11UnorderedAccessView* nullUav[] = { nullptr, nullptr, nullptr };
                d->CSSetShaderResources(0, 1, nullSrv);
                d->CSSetUnorderedAccessViews(0, 3, nullUav, nullptr);
                d->CSSetShader(nullptr, nullptr, 0);
                d->CopyResource(m_statsStage.get(), m_stats.get());
                if (frame)
                {
                    d->CopyResource(m_yStage.get(), m_y.get());
                    d->CopyResource(m_uvStage.get(), m_uv.get());
                }
                winrt::com_ptr<ID3D11CommandList> list;
                if (FAILED(d->FinishCommandList(FALSE, list.put())) || !list)
                { error = L"FinishCommandList failed"; return false; }
                m_imm->ExecuteCommandList(list.get(), TRUE);

                // Stats: reduce the per-group partials in double.
                D3D11_MAPPED_SUBRESOURCE m{};
                if (FAILED(m_imm->Map(m_statsStage.get(), 0, D3D11_MAP_READ, 0, &m)))
                { error = L"Map(stats) failed"; return false; }
                const auto* p = static_cast<const float*>(m.pData);
                double mx = 0.0, sum = 0.0;
                for (UINT g = 0; g < m_groupsX * m_groupsY; ++g)
                {
                    mx = (std::max)(mx, static_cast<double>(p[2 * g]));
                    sum += p[2 * g + 1];
                }
                m_imm->Unmap(m_statsStage.get(), 0);
                stats.maxNits = mx;
                stats.meanNits = sum / (static_cast<double>(m_w) * m_h);

                if (frame)
                {
                    const size_t bpc = m_hdr ? 2 : 1;
                    const size_t yRow = m_w * bpc;          // one sample per pixel
                    const size_t uvRow = (m_w / 2) * 2 * bpc; // interleaved U,V
                    frame->resize(yRow * m_h + uvRow * (m_h / 2));
                    uint8_t* out = frame->data();
                    auto copyPlane = [&](ID3D11Texture2D* stage, size_t rowBytes, uint32_t rows) -> bool {
                        D3D11_MAPPED_SUBRESOURCE pm{};
                        if (FAILED(m_imm->Map(stage, 0, D3D11_MAP_READ, 0, &pm))) return false;
                        for (uint32_t r = 0; r < rows; ++r)
                            std::memcpy(out + r * rowBytes, static_cast<const uint8_t*>(pm.pData) + size_t(r) * pm.RowPitch, rowBytes);
                        m_imm->Unmap(stage, 0);
                        out += rowBytes * rows;
                        return true;
                    };
                    if (!copyPlane(m_yStage.get(), yRow, m_h) || !copyPlane(m_uvStage.get(), uvRow, m_h / 2))
                    { error = L"Map(planes) failed"; return false; }
                }
                return true;
            }

        private:
            winrt::com_ptr<ID3D11Device> m_dev;
            winrt::com_ptr<ID3D11DeviceContext> m_imm, m_deferred;
            winrt::com_ptr<ID3D11ComputeShader> m_cs;
            winrt::com_ptr<ID3D11Texture2D> m_src, m_y, m_uv, m_yStage, m_uvStage;
            winrt::com_ptr<ID3D11ShaderResourceView> m_srcSrv;
            winrt::com_ptr<ID3D11UnorderedAccessView> m_yUav, m_uvUav, m_statsUav;
            winrt::com_ptr<ID3D11Buffer> m_stats, m_statsStage, m_cb;
            uint32_t m_w{ 0 }, m_h{ 0 }, m_groupsX{ 0 }, m_groupsY{ 0 };
            bool m_hdr{ true };
        };

        // ---- per-frame evaluation -------------------------------------------

        // The shape every host uses (MainWindow::RenderFrameToOffscreen,
        // headless runEval): Evaluate OUTSIDE a draw session, then inside one
        // ProcessDeferredCompute and a frozen sweep for whatever it dirtied.
        // The node's output is then drawn 1:1 into `target` (SOURCE_COPY,
        // nearest, no transform) from `srcRect` in image space.
        bool RenderFrame(EffectGraph& graph, GraphEvaluator& evaluator, ID2D1DeviceContext5* dc,
                         uint32_t nodeId, ID2D1Bitmap1* target, const D2D1_RECT_F* srcRect,
                         bool twoPass, D2D1_RECT_F* boundsOut, std::wstring& error)
        {
            graph.MarkAllDirty();
            dc->SetTarget(nullptr);
            evaluator.Evaluate(graph, dc);
            if (twoPass) evaluator.Evaluate(graph, dc);   // new custom effects need two passes

            dc->SetTarget(target);
            dc->BeginDraw();
            evaluator.ProcessDeferredCompute(graph, dc);
            if (graph.HasDirtyNodes())
            {
                evaluator.SetDeferredComputeFrozen(true);
                evaluator.Evaluate(graph, dc);
                evaluator.SetDeferredComputeFrozen(false);
            }
            auto* node = graph.FindNode(nodeId);
            ID2D1Image* image = node ? node->cachedOutput : nullptr;
            if (image)
            {
                if (boundsOut) dc->GetImageLocalBounds(image, boundsOut);
                if (srcRect)
                {
                    // ProcessDeferredCompute retargets the context for its
                    // pre-renders; draw into OUR target explicitly.
                    dc->SetTarget(target);
                    dc->SetTransform(D2D1::Matrix3x2F::Identity());
                    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
                    const D2D1_POINT_2F origin = D2D1::Point2F(0.0f, 0.0f);
                    dc->DrawImage(image, &origin, srcRect,
                        D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
                }
            }
            const HRESULT hr = dc->EndDraw();
            dc->SetTarget(nullptr);
            if (!image)
            {
                error = node && !node->runtimeError.empty()
                    ? std::format(L"node {} produced no image: {}", nodeId, node->runtimeError)
                    : std::format(L"node {} produced no image output", nodeId);
                return false;
            }
            if (FAILED(hr))
            {
                error = std::format(L"EndDraw failed 0x{:08X}", static_cast<uint32_t>(hr));
                return false;
            }
            return true;
        }

        const EffectNode* FindWorkingSpace(const EffectGraph& graph)
        {
            for (const auto& n : graph.Nodes())
                if (n.customEffect.has_value() && n.customEffect->shaderLabEffectId == L"Working Space")
                    return &n;
            return nullptr;
        }

        struct Mastering { float rx, ry, gx, gy, bx, by, wx, wy, maxNits, minNits; };
    }

    // ---- public ---------------------------------------------------------------

    const std::string& VideoConvertHelpersHLSL()
    {
        static const std::string s = kHelpersHLSL;
        return s;
    }

    std::wstring FindFfmpeg(const std::wstring& explicitPath, std::wstring& error)
    {
        const std::wstring hint =
            L"Pass the path explicitly (headless: --ffmpeg <path>; MCP: ffmpegPath), or put ffmpeg.exe "
            L"on PATH. One way to install it: `winget install Gyan.FFmpeg`.";
        if (!explicitPath.empty())
        {
            std::error_code ec;
            std::filesystem::path p(explicitPath);
            if (std::filesystem::is_directory(p, ec)) p /= L"ffmpeg.exe";
            if (std::filesystem::is_regular_file(p, ec)) return p.wstring();
            error = std::format(L"ffmpeg not found at '{}'. {}", explicitPath, hint);
            return {};
        }
        wchar_t found[MAX_PATH * 2]{};
        if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, static_cast<DWORD>(std::size(found)), found, nullptr) > 0)
            return found;
        error = L"ffmpeg was not given and is not on PATH. " + hint;
        return {};
    }

    uint32_t SetClocksToTime(EffectGraph& graph, double seconds)
    {
        uint32_t n = 0;
        for (auto& node : const_cast<std::vector<EffectNode>&>(graph.Nodes()))
        {
            if (!node.isClock) continue;
            const float start = GetF(node, L"StartTime", 0.0f);
            const float stop  = GetF(node, L"StopTime", 10.0f);
            const float speed = GetF(node, L"Speed", 1.0f);
            const bool  loop  = GetF(node, L"Loop", 1.0f) > 0.5f;
            double duration = static_cast<double>(stop) - start;
            if (duration <= 0.0) duration = 1.0;   // the tick's own rule
            double t = seconds * speed;
            if (loop)
            {
                t = std::fmod(t, duration);
                if (t < 0.0) t += duration;
            }
            else
            {
                t = std::clamp(t, 0.0, duration);
            }
            node.clockTime = t;
            node.clockTickBucket = -1;
            node.dirty = true;
            ++n;
        }
        return n;
    }

    double LongestClockDuration(const EffectGraph& graph)
    {
        double longest = 0.0;
        for (const auto& node : graph.Nodes())
        {
            if (!node.isClock) continue;
            const float speed = GetF(node, L"Speed", 1.0f);
            if (std::abs(speed) < 1e-6f) continue;
            double duration = static_cast<double>(GetF(node, L"StopTime", 10.0f)) - GetF(node, L"StartTime", 0.0f);
            if (duration <= 0.0) duration = 1.0;
            longest = (std::max)(longest, duration / std::abs(speed));
        }
        return longest;
    }

    VideoExportResult ExportVideo(
        const VideoExportRequest& req,
        EffectGraph& graph,
        GraphEvaluator& evaluator,
        Effects::SourceNodeFactory& sourceFactory,
        DisplayMonitor* displayMonitor,
        ID2D1DeviceContext* dcBase,
        ID3D11Device* device,
        ID3D11DeviceContext* immediate)
    {
        (void)sourceFactory;
        (void)displayMonitor;
        VideoExportResult r;
        const auto t0 = std::chrono::steady_clock::now();
        auto fail = [&](std::wstring msg) -> VideoExportResult& {
            r.ok = false;
            r.error = std::move(msg);
            r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            return r;
        };

        winrt::com_ptr<ID2D1DeviceContext5> dc;
        if (!dcBase || FAILED(dcBase->QueryInterface(dc.put())) || !device || !immediate)
            return fail(L"no rendering device");
        if (!graph.FindNode(req.nodeId))
            return fail(std::format(L"node {} not found", req.nodeId));
        if (req.outputPath.empty())
            return fail(L"no output path");
        if (!(req.fps > 0.0 && req.fps <= 480.0))
            return fail(L"fps must be in (0, 480]");

        // ---- ffmpeg + encoder ----------------------------------------------
        std::wstring ffErr;
        r.ffmpegPath = FindFfmpeg(req.ffmpegPath, ffErr);
        if (r.ffmpegPath.empty()) return fail(ffErr);

        const bool hdr = req.format == VideoFormat::Hdr10;
        VideoCodec codec = req.codec;
        if (codec == VideoCodec::Default) codec = hdr ? VideoCodec::Hevc : VideoCodec::H264;
        if (hdr && codec == VideoCodec::H264)
            return fail(L"HDR10 needs HEVC or AV1 (H.264 has no 10-bit profile players accept for HDR10)");
        if (req.lossless && codec == VideoCodec::Av1)
            return fail(L"--lossless is not supported with AV1 (SVT-AV1 has no lossless mode); use HEVC");
        r.encoder = codec == VideoCodec::Hevc ? L"libx265" : codec == VideoCodec::Av1 ? L"libsvtav1" : L"libx264";
        {
            FfmpegProcess probe;
            std::wstring perr;
            if (!probe.Start(r.ffmpegPath, L"-hide_banner -encoders", false, perr)) return fail(perr);
            probe.Finish(30000);
            const std::string list = probe.Log();
            const std::string needle = " " + winrt::to_string(r.encoder) + " ";
            if (list.find(needle) == std::string::npos)
                return fail(std::format(L"this ffmpeg ({}) has no {} encoder. Use a build that includes it "
                                        L"(e.g. the Gyan 'full' or 'essentials' builds).", r.ffmpegPath, r.encoder));
        }

        // ---- timeline ------------------------------------------------------
        r.fps = req.fps;
        r.start = req.start;
        r.duration = req.duration.value_or(LongestClockDuration(graph));
        if (!(r.duration > 0.0))
            return fail(L"the graph has no Clock with a non-zero Speed; pass a duration");
        r.frames = static_cast<uint32_t>((std::max)(1.0, std::round(r.duration * r.fps)));
        if (r.frames > 100000) return fail(L"more than 100000 frames requested");

        for (const auto& n : graph.Nodes())
        {
            if (n.type != NodeType::Source) continue;
            if (n.properties.count(L"IsDxgiDuplicateOutput") || n.properties.count(L"IsWindowsGraphicsCapture"))
                r.warnings.push_back(std::format(L"node {} is a live capture source; it is not time-stepped", n.id));
            else if (n.shaderPath && Effects::SourceNodeFactory::IsVideoFile(*n.shaderPath))
                r.warnings.push_back(std::format(L"node {} is a video file; video sources are not time-stepped by export yet", n.id));
        }

        float oldDpiX = 96.0f, oldDpiY = 96.0f;
        dc->GetDpi(&oldDpiX, &oldDpiY);
        dc->SetDpi(96.0f, 96.0f);   // 1 DIP == 1 pixel
        struct DpiRestore { ID2D1DeviceContext5* dc; float x, y; ~DpiRestore() { dc->SetDpi(x, y); } } dpiRestore{ dc.get(), oldDpiX, oldDpiY };

        // ---- prime: settle new effects, measure the output rect ----------
        std::wstring err;
        D2D1_RECT_F bounds{};
        {
            winrt::com_ptr<ID2D1Bitmap1> scratch;
            D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
            if (FAILED(dc->CreateBitmap(D2D1::SizeU(1, 1), nullptr, 0, bp, scratch.put())))
                return fail(L"could not create scratch target");
            SetClocksToTime(graph, r.start);
            if (!RenderFrame(graph, evaluator, dc.get(), req.nodeId, scratch.get(), nullptr, true, &bounds, err))
                return fail(err);
        }
        // Snap OUTWARD to whole pixels (floor the origin, ceil the extent): a
        // fractional origin drawn as-is would resample the whole frame at a
        // sub-pixel offset. Infinite / NaN extents fail the range test below.
        bounds.left   = std::floor(bounds.left);
        bounds.top    = std::floor(bounds.top);
        bounds.right  = std::ceil(bounds.right);
        bounds.bottom = std::ceil(bounds.bottom);
        const double bw = static_cast<double>(bounds.right) - bounds.left;
        const double bh = static_cast<double>(bounds.bottom) - bounds.top;
        if (!(bw >= 2.0 && bh >= 2.0) || bw > 16384.0 || bh > 16384.0)
            return fail(std::format(L"node {} output is {}x{} -- not a finite frame; crop it to the size you want to export",
                                    req.nodeId, bw, bh));
        r.width  = static_cast<uint32_t>(bw) & ~1u;   // 4:2:0 needs even dimensions
        r.height = static_cast<uint32_t>(bh) & ~1u;
        if (r.width != static_cast<uint32_t>(bw) || r.height != static_cast<uint32_t>(bh))
            r.warnings.push_back(std::format(L"output {}x{} is odd; exporting {}x{} (4:2:0 needs even dimensions)",
                                             static_cast<uint32_t>(bw), static_cast<uint32_t>(bh), r.width, r.height));
        const D2D1_RECT_F srcRect = D2D1::RectF(bounds.left, bounds.top,
                                                bounds.left + r.width, bounds.top + r.height);

        Converter conv;
        if (!conv.Init(device, immediate, r.width, r.height, hdr, err)) return fail(err);
        winrt::com_ptr<IDXGISurface> surface;
        conv.Source()->QueryInterface(surface.put());
        winrt::com_ptr<ID2D1Bitmap1> target;
        {
            D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
            if (!surface || FAILED(dc->CreateBitmapFromDxgiSurface(surface.get(), &bp, target.put())))
                return fail(L"could not wrap the conversion source as a D2D target");
        }

        bool boundsWarned = false;
        auto renderAt = [&](uint32_t frame, std::vector<uint8_t>* packed, FrameStats& st) -> bool {
            SetClocksToTime(graph, r.start + frame / r.fps);
            D2D1_RECT_F b{};
            if (!RenderFrame(graph, evaluator, dc.get(), req.nodeId, target.get(), &srcRect, false, &b, err))
                return false;
            // Compare with the same outward snap the frame rect used.
            if (!boundsWarned && (std::floor(b.left) != bounds.left || std::floor(b.top) != bounds.top ||
                                  std::ceil(b.right) != bounds.right || std::ceil(b.bottom) != bounds.bottom))
            {
                r.warnings.push_back(std::format(L"node output bounds change over time (first at frame {}); every "
                    L"frame is cropped/padded to the first frame's {}x{} rect", frame, r.width, r.height));
                boundsWarned = true;
            }
            return conv.Convert(packed, st, err);
        };

        // ---- HDR10 static metadata ----------------------------------------
        Mastering md{ 0.680f, 0.320f, 0.265f, 0.690f, 0.150f, 0.060f, 0.3127f, 0.3290f, 1000.0f, 0.005f };
        const bool wantMetadata = hdr && req.mastering != MasteringMode::None;
        if (wantMetadata && req.mastering == MasteringMode::WorkingSpace)
        {
            const EffectNode* ws = FindWorkingSpace(graph);
            if (!ws) return fail(L"mastering=working-space needs a Working Space node in the graph");
            bool okp = GetF2(*ws, L"RedPrimary", md.rx, md.ry) && GetF2(*ws, L"GreenPrimary", md.gx, md.gy) &&
                       GetF2(*ws, L"BluePrimary", md.bx, md.by) && GetF2(*ws, L"WhitePoint", md.wx, md.wy);
            md.maxNits = GetF(*ws, L"PeakNits", 0.0f);
            md.minNits = GetF(*ws, L"MinNits", 0.0f);
            if (!okp || md.maxNits <= 0.0f) return fail(L"the Working Space node has no primaries / peak luminance yet");
        }

        double measuredCll = 0.0, measuredFall = 0.0;
        const bool haveOverride = req.maxCll.has_value() && req.maxFall.has_value();
        if (wantMetadata && !haveOverride)
        {
            // Pass 1: render + measure only. x265 needs MaxCLL / MaxFALL in the
            // stream headers, before the first frame is encoded.
            for (uint32_t f = 0; f < r.frames; ++f)
            {
                FrameStats st;
                if (!renderAt(f, nullptr, st)) return fail(std::format(L"frame {} (measure pass): {}", f, err));
                measuredCll = (std::max)(measuredCll, st.maxNits);
                measuredFall = (std::max)(measuredFall, st.meanNits);
            }
        }
        const uint32_t cll  = haveOverride ? *req.maxCll  : static_cast<uint32_t>(std::ceil(measuredCll));
        const uint32_t fall = haveOverride ? *req.maxFall : static_cast<uint32_t>(std::ceil(measuredFall));

        // ---- ffmpeg command -------------------------------------------------
        const std::wstring prim = hdr ? L"bt2020" : L"bt709";
        const std::wstring trc  = hdr ? L"smpte2084" : L"bt709";
        const std::wstring mtx  = hdr ? L"bt2020nc" : L"bt709";
        const std::wstring colourTags = std::format(
            L"-color_range tv -color_primaries {} -color_trc {} -colorspace {} -chroma_sample_location left",
            prim, trc, mtx);
        std::wstring args = std::format(
            L"-hide_banner -loglevel warning -y -f rawvideo -pix_fmt {} -video_size {}x{} -framerate {} {} -i - -an ",
            hdr ? L"p010le" : L"nv12", r.width, r.height, FormatFps(r.fps), colourTags);

        const std::wstring outPix = hdr ? L"yuv420p10le" : L"yuv420p";
        std::wstring ext = std::filesystem::path(req.outputPath).extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        const bool isMp4 = (ext == L".mp4" || ext == L".mov" || ext == L".m4v");

        if (codec == VideoCodec::Hevc)
        {
            const int crf = req.crf >= 0 ? req.crf : (hdr ? 18 : 20);
            std::wstring xp = std::format(
                L"log-level=error:repeat-headers=1:colorprim={}:transfer={}:colormatrix={}:range=limited:chromaloc=0",
                prim, trc, mtx);
            if (wantMetadata)
            {
                auto u = [](float v, float scale) { return static_cast<long long>(std::llround(v * scale)); };
                xp += std::format(L":hdr10=1:master-display=G({},{})B({},{})R({},{})WP({},{})L({},{}):max-cll={},{}",
                    u(md.gx, 50000), u(md.gy, 50000), u(md.bx, 50000), u(md.by, 50000),
                    u(md.rx, 50000), u(md.ry, 50000), u(md.wx, 50000), u(md.wy, 50000),
                    u(md.maxNits, 10000), u(md.minNits, 10000), cll, fall);
            }
            if (req.lossless) xp += L":lossless=1";
            args += std::format(L"-c:v libx265 -preset {} {}-pix_fmt {} -x265-params {} ",
                req.preset.empty() ? L"medium" : req.preset,
                req.lossless ? L"" : std::format(L"-crf {} ", crf), outPix, xp);
            if (isMp4) args += L"-tag:v hvc1 ";
        }
        else if (codec == VideoCodec::Av1)
        {
            const int crf = req.crf >= 0 ? req.crf : 30;
            std::wstring sp;
            if (wantMetadata)
                sp = std::format(L"-svtav1-params mastering-display=G({:.4f},{:.4f})B({:.4f},{:.4f})R({:.4f},{:.4f})WP({:.4f},{:.4f})L({:.4f},{:.4f}):content-light={},{} ",
                    md.gx, md.gy, md.bx, md.by, md.rx, md.ry, md.wx, md.wy, md.maxNits, md.minNits, cll, fall);
            args += std::format(L"-c:v libsvtav1 -preset {} -crf {} -pix_fmt {} {}",
                req.preset.empty() ? L"6" : req.preset, crf, outPix, sp);
        }
        else
        {
            const int crf = req.crf >= 0 ? req.crf : 18;
            args += std::format(L"-c:v libx264 -preset {} {}-pix_fmt {} -x264-params chromaloc=0 ",
                req.preset.empty() ? L"medium" : req.preset,
                req.lossless ? L"-qp 0 " : std::format(L"-crf {} ", crf), outPix);
        }
        args += colourTags + L" ";
        if (isMp4) args += L"-movflags +faststart ";
        args += Quote(req.outputPath);
        r.commandLine = Quote(r.ffmpegPath) + L" " + args;

        // ---- pass 2: render, convert, encode -------------------------------
        FfmpegProcess ff;
        if (!ff.Start(r.ffmpegPath, args, true, err)) return fail(err);
        std::vector<uint8_t> packed;
        double pass2Cll = 0.0, pass2Fall = 0.0;
        for (uint32_t f = 0; f < r.frames; ++f)
        {
            FrameStats st;
            if (!renderAt(f, &packed, st))
            {
                ff.Abort();
                r.ffmpegLogTail = Tail(ff.Log(), 8192);
                return fail(std::format(L"frame {}: {}", f, err));
            }
            pass2Cll = (std::max)(pass2Cll, st.maxNits);
            pass2Fall = (std::max)(pass2Fall, st.meanNits);
            if (!ff.Write(packed.data(), packed.size()))
            {
                ff.Finish(60000);
                r.ffmpegLogTail = Tail(ff.Log(), 8192);
                return fail(std::format(L"ffmpeg stopped accepting frames at frame {} (see ffmpegLogTail)", f));
            }
        }
        const int code = ff.Finish(30 * 60 * 1000);
        r.ffmpegLogTail = Tail(ff.Log(), 8192);
        if (code != 0)
            return fail(std::format(L"ffmpeg exited with code {} (see ffmpegLogTail)", code));

        r.maxCll = pass2Cll;
        r.maxFall = pass2Fall;
        r.staticMetadataWritten = wantMetadata && codec != VideoCodec::H264;
        if (wantMetadata && !haveOverride &&
            (std::abs(pass2Cll - measuredCll) > 0.5 || std::abs(pass2Fall - measuredFall) > 0.5))
        {
            r.warnings.push_back(std::format(L"the measure pass and the encode pass disagreed (MaxCLL {:.1f} vs {:.1f}, "
                L"MaxFALL {:.1f} vs {:.1f}): the graph did not render deterministically; the header carries the first",
                measuredCll, pass2Cll, measuredFall, pass2Fall));
        }
        r.ok = true;
        r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return r;
    }
}
