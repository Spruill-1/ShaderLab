#include "pch_engine.h"
#include "../TestSupport.h"
#include "Engine/Api/EffectGraph.h"
#include "Engine/Api/EffectGraphInternal.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

// The public EffectGraph API, used as an outside program would: a context on
// the test device, texture inputs, D2D and ShaderLab effects, and results read
// back through D3D11.
namespace
{
    using namespace ShaderLab::Tests;

    constexpr UINT cWidth = 64, cHeight = 32;

    uint16_t ToHalf(float f)
    {
        uint32_t b; std::memcpy(&b, &f, 4);
        if ((b & 0x7FFFFFFF) == 0) return static_cast<uint16_t>((b >> 16) & 0x8000);
        return static_cast<uint16_t>(((b >> 16) & 0x8000) | ((((b >> 23) & 0xFF) - 112) << 10) | ((b >> 13) & 0x3FF));
    }

    float FromHalf(uint16_t h)
    {
        const uint32_t sign = (h & 0x8000u) << 16, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
        uint32_t bits = exponent == 0 ? sign : sign | ((exponent + 112) << 23) | (mantissa << 13);
        float f; std::memcpy(&f, &bits, 4);
        return f;
    }

    // Grey FP16 texels: the left half `left`, the right half `right`, alpha 1.
    std::vector<uint16_t> GreyTexels(float left, float right)
    {
        std::vector<uint16_t> texels(cWidth * cHeight * 4);
        for (UINT y = 0; y < cHeight; ++y)
            for (UINT x = 0; x < cWidth; ++x)
            {
                uint16_t* p = &texels[(y * cWidth + x) * 4];
                p[0] = p[1] = p[2] = ToHalf(x < cWidth / 2 ? left : right);
                p[3] = ToHalf(1.0f);
            }
        return texels;
    }

    winrt::com_ptr<ID3D11Texture2D> MakeTexture(DXGI_FORMAT format, UINT bind, const void* texels, UINT pitch)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = cWidth; desc.Height = cHeight; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = format; desc.SampleDesc.Count = 1; desc.BindFlags = bind;
        D3D11_SUBRESOURCE_DATA init{ texels, pitch, 0 };
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(g_d3dDevice->CreateTexture2D(&desc, texels ? &init : nullptr, texture.put()));
        return texture;
    }

    // The texture's bytes, through a staging copy.
    std::vector<uint8_t> ReadBack(ID3D11Texture2D* texture, UINT bytesPerPixel)
    {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::com_ptr<ID3D11Texture2D> staging;
        winrt::check_hresult(g_d3dDevice->CreateTexture2D(&desc, nullptr, staging.put()));
        g_d3dContext->CopyResource(staging.get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(g_d3dContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::vector<uint8_t> bytes(desc.Width * desc.Height * bytesPerPixel);
        for (UINT y = 0; y < desc.Height; ++y)
            std::memcpy(&bytes[y * desc.Width * bytesPerPixel], static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                        desc.Width * bytesPerPixel);
        g_d3dContext->Unmap(staging.get(), 0);
        return bytes;
    }

    float RedAt(const std::vector<uint8_t>& fp16, UINT x, UINT y)
    {
        uint16_t h; std::memcpy(&h, &fp16[(y * cWidth + x) * 8], 2);
        return FromHalf(h);
    }

    winrt::com_ptr<IEffectGraphContext> MakeContext()
    {
        winrt::com_ptr<IEffectGraphContext> context;
        winrt::check_hresult(EffectGraphCreateContext(g_d3dDevice.get(), nullptr, IID_PPV_ARGS(context.put())));
        return context;
    }

    float FieldOf(IEffectGraphNode* node, const wchar_t* name)
    {
        winrt::com_ptr<IEffectGraphValues> values;
        float value = -1.0f;
        if (SUCCEEDED(node->GetValues(values.put()))) values->GetFloat(name, &value);
        return value;
    }

    SL_TEST_CASE(TestEffectGraphContext, Tag::Gpu)
    {
        printf("\n=== EffectGraph: context ===\n");

        // A device without BGRA support cannot host Direct2D.
        winrt::com_ptr<ID3D11Device> noBgra;
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                          noBgra.put(), nullptr, nullptr);
        winrt::com_ptr<IEffectGraphContext> refused;
        TEST("Api_NoBgraRefused", EffectGraphCreateContext(noBgra.get(), nullptr, IID_PPV_ARGS(refused.put())) ==
                                      EFFECTGRAPH_E_UNSUPPORTED_DEVICE && !refused);

        auto context = MakeContext();
        winrt::com_ptr<ID3D11Device> device;
        context->GetD3DDevice(device.put());
        TEST("Api_UsesCallersDevice", device.get() == static_cast<ID3D11Device*>(g_d3dDevice.get()));
        winrt::com_ptr<ID2D1DeviceContext5> dc;
        context->GetD2DDeviceContext(dc.put());
        TEST("Api_HasDeviceContext", dc != nullptr);

        winrt::com_ptr<IEffectGraphNode> unknown;
        TEST("Api_UnknownEffect", context->CreateEffect(L"No Such Effect", unknown.put()) == EFFECTGRAPH_E_UNKNOWN_EFFECT);

        // ShaderLab reaches its own objects through the internal interface.
        auto internal = context.try_as<IEffectGraphContextInternal>();
        winrt::com_ptr<IEffectGraphNode> stats;
        winrt::check_hresult(context->CreateEffect(L"Luminance Statistics", stats.put()));
        winrt::com_ptr<IEffectGraphNode> again;
        const bool sameObject = internal && SUCCEEDED(internal->NodeFor(internal->NodeId(stats.get()), again.put())) &&
                                again.get() == stats.get();
        TEST("Api_InternalSameNodeObject", sameObject);
        TEST("Api_InternalSeesGraph", internal && internal->Model().Nodes().size() == 1);
    }

    SL_TEST_CASE(TestEffectGraphStatistics, Tag::Gpu)
    {
        printf("\n=== EffectGraph: statistics from a texture ===\n");
        auto context = MakeContext();
        auto texels = GreyTexels(1.0f, 3.0f);
        auto texture = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, texels.data(), cWidth * 8);

        winrt::com_ptr<IEffectGraphNode> input, stats;
        winrt::check_hresult(context->CreateTextureInput(texture.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, input.put()));
        winrt::check_hresult(context->CreateEffect(L"Luminance Statistics", stats.put()));
        TEST("Stats_Connect", SUCCEEDED(stats->SetInput(0, input.get())));
        const HRESULT realized = context->Realize(stats.get(), nullptr);
        TEST("Stats_Realize", SUCCEEDED(realized));
        TEST("Stats_MatchTexture", FieldOf(stats.get(), L"Samples") == cWidth * cHeight &&
             std::abs(FieldOf(stats.get(), L"Max") - 240.0f) < 0.05f && std::abs(FieldOf(stats.get(), L"Mean") - 160.0f) < 0.05f);

        // New texels in the same texture, then Invalidate: the next Realize sees them.
        texels = GreyTexels(2.0f, 6.0f);
        g_d3dContext->UpdateSubresource(texture.get(), 0, nullptr, texels.data(), cWidth * 8, 0);
        auto asInput = input.try_as<IEffectGraphInput>();
        if (asInput) asInput->Invalidate();
        context->Realize(stats.get(), nullptr);
        TEST("Stats_FollowInvalidate", asInput && std::abs(FieldOf(stats.get(), L"Max") - 480.0f) < 0.1f);
        TEST("Stats_PlainNodeIsNotInput", !stats.try_as<IEffectGraphInput>());

        winrt::com_ptr<IEffectGraphValues> values;
        stats->GetValues(values.put());
        EFFECTGRAPH_FIELD_INFO info{};
        const UINT32 index = values ? values->GetFieldIndex(L"Max") : EFFECTGRAPH_INVALID_INDEX;
        TEST("Stats_FieldInfo", index != EFFECTGRAPH_INVALID_INDEX && SUCCEEDED(values->GetFieldInfo(index, &info)) &&
             info.type == EFFECTGRAPH_FIELD_TYPE_FLOAT && info.componentCount == 1 && info.elementCount == 1);
    }

    SL_TEST_CASE(TestEffectGraphRealizeTo, Tag::Gpu)
    {
        printf("\n=== EffectGraph: rendering into a texture ===\n");
        auto context = MakeContext();
        auto texels = GreyTexels(0.25f, 0.5f);
        auto texture = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, texels.data(), cWidth * 8);
        auto target = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                                  nullptr, 0);
        winrt::com_ptr<IEffectGraphNode> input;
        winrt::check_hresult(context->CreateTextureInput(texture.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, input.put()));

        // The input alone comes back exactly.
        TEST("RealizeTo_InputRoundTrips", SUCCEEDED(context->RealizeTo(input.get(), target.get())));
        auto pixels = ReadBack(target.get(), 8);
        TEST("RealizeTo_ExactCopy", RedAt(pixels, 3, 3) == 0.25f && RedAt(pixels, 60, 20) == 0.5f);

        // A D2D effect by CLSID, its property set by D2D's own index.
        winrt::com_ptr<IEffectGraphNode> matrix;
        winrt::check_hresult(context->CreateD2DEffect(CLSID_D2D1ColorMatrix, matrix.put()));
        TEST("D2D_IndexMatchesD2D", matrix->GetPropertyIndex(L"ColorMatrix") == D2D1_COLORMATRIX_PROP_COLOR_MATRIX &&
             matrix->GetType(D2D1_COLORMATRIX_PROP_COLOR_MATRIX) == D2D1_PROPERTY_TYPE_MATRIX_5X4);
        const D2D1_MATRIX_5X4_F doubled = D2D1::Matrix5x4F(2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 0, 0);
        TEST("D2D_SetByIndex", SUCCEEDED(matrix->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, doubled)));
        matrix->SetInput(0, input.get());
        TEST("RealizeTo_D2DEffect", SUCCEEDED(context->RealizeTo(matrix.get(), target.get())));
        pixels = ReadBack(target.get(), 8);
        TEST("RealizeTo_Doubled", RedAt(pixels, 3, 3) == 0.5f && RedAt(pixels, 60, 20) == 1.0f);
        if (RedAt(pixels, 3, 3) != 0.5f) printf("    (debug) red = %.6f, %.6f\n", RedAt(pixels, 3, 3), RedAt(pixels, 60, 20));

        // An sRGB 8-bit target gamma-encodes: linear 0.25 is sRGB 137, 0.5 is 188.
        auto srgb = MakeTexture(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                                nullptr, 0);
        TEST("RealizeTo_Srgb", SUCCEEDED(context->RealizeTo(input.get(), srgb.get())));
        auto bytes = ReadBack(srgb.get(), 4);
        const int left = bytes[(3 * cWidth + 3) * 4 + 2], right = bytes[(20 * cWidth + 60) * 4 + 2];
        TEST("RealizeTo_SrgbEncoded", std::abs(left - 137) <= 1 && std::abs(right - 188) <= 1);
        if (std::abs(left - 137) > 1 || std::abs(right - 188) > 1) printf("    (debug) sRGB red = %d, %d\n", left, right);
    }

    SL_TEST_CASE(TestEffectGraphBindings, Tag::Gpu)
    {
        printf("\n=== EffectGraph: bindings and the graph rules ===\n");
        auto context = MakeContext();
        auto texels = GreyTexels(1.0f, 3.0f);
        auto texture = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, texels.data(), cWidth * 8);
        winrt::com_ptr<IEffectGraphNode> input, stats, toneMap;
        winrt::check_hresult(context->CreateTextureInput(texture.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, input.put()));
        winrt::check_hresult(context->CreateEffect(L"Luminance Statistics", stats.put()));
        winrt::check_hresult(context->CreateEffect(L"ICtCp Tone Map (HDR -> SDR)", toneMap.put()));
        stats->SetInput(0, input.get());
        toneMap->SetInput(0, input.get());

        TEST("Bind_Accepted", SUCCEEDED(toneMap->Bind(L"SourcePeakNits", stats.get(), L"Max")));
        TEST("Bind_UnknownFieldRefused", FAILED(toneMap->Bind(L"SourcePeakNits", stats.get(), L"NoSuchField")));
        ID2D1Image* image = nullptr;
        const HRESULT realized = context->Realize(toneMap.get(), &image);
        TEST("Bind_Realize", SUCCEEDED(realized) && image);
        if (image) image->Release();
        float peak = 0.0f;
        toneMap->GetValueByName(L"SourcePeakNits", D2D1_PROPERTY_TYPE_FLOAT, reinterpret_cast<BYTE*>(&peak), sizeof(peak));
        TEST("Bind_ValueArrives", std::abs(peak - 240.0f) < 0.05f);
        if (std::abs(peak - 240.0f) >= 0.05f) printf("    (debug) SourcePeakNits = %.3f\n", peak);

        // A loop is refused, through an edge or a binding.
        TEST("Graph_EdgeCycleRefused", input.try_as<IEffectGraphNode>() && toneMap->SetInput(0, toneMap.get()) == EFFECTGRAPH_E_CYCLE);
        TEST("Graph_BindCycleRefused", stats->Bind(L"ClipNits", stats.get(), L"Max") == EFFECTGRAPH_E_CYCLE);

        // A node from another context, and a removed node.
        auto other = MakeContext();
        winrt::com_ptr<IEffectGraphNode> foreign;
        winrt::check_hresult(other->CreateEffect(L"Luminance Statistics", foreign.put()));
        TEST("Graph_ForeignNodeRefused", toneMap->SetInput(0, foreign.get()) == EFFECTGRAPH_E_WRONG_CONTEXT);
        TEST("Graph_Remove", SUCCEEDED(context->RemoveNode(stats.get())));
        wchar_t error[64];
        TEST("Graph_RemovedNodeSays", stats->GetError(error, 64) == EFFECTGRAPH_E_NODE_REMOVED &&
             context->Realize(stats.get(), nullptr) == EFFECTGRAPH_E_NODE_REMOVED);
    }

    struct DrawProbe
    {
        IEffectGraphContext* context;
        IEffectGraphNode* node;
        HRESULT nestedRealize{ S_OK };
        HRESULT nestedDraw{ S_OK };
    };

    void CALLBACK ProbeInsideDraw(ID2D1DeviceContext5*, void* callbackContext)
    {
        auto* probe = static_cast<DrawProbe*>(callbackContext);
        probe->nestedRealize = probe->context->Realize(probe->node, nullptr);
        probe->nestedDraw = probe->context->Draw(nullptr, ProbeInsideDraw, callbackContext);
    }

    SL_TEST_CASE(TestEffectGraphDrawAndLoad, Tag::Gpu)
    {
        printf("\n=== EffectGraph: draw sessions and saved graphs ===\n");
        auto context = MakeContext();
        winrt::com_ptr<IEffectGraphNode> stats;
        winrt::check_hresult(context->CreateEffect(L"Luminance Statistics", stats.put()));
        winrt::com_ptr<ID2D1DeviceContext5> dc;
        context->GetD2DDeviceContext(dc.put());
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED));
        winrt::com_ptr<ID2D1Bitmap1> target;
        winrt::check_hresult(dc->CreateBitmap(D2D1::SizeU(8, 8), nullptr, 0, &props, target.put()));
        DrawProbe probe{ context.get(), stats.get() };
        TEST("Draw_Runs", SUCCEEDED(context->Draw(target.get(), ProbeInsideDraw, &probe)));
        TEST("Draw_RefusesNesting", probe.nestedRealize == D2DERR_WRONG_STATE && probe.nestedDraw == D2DERR_WRONG_STATE);

        const auto fixture = FindFixture(L"test_cli_basic.json");
        TEST("Load_FixtureFound", !fixture.empty());
        if (fixture.empty()) return;
        TEST("Load_Graph", SUCCEEDED(context->LoadGraph(fixture.c_str())));
        wchar_t error[64];
        TEST("Load_OldNodesRemoved", stats->GetError(error, 64) == EFFECTGRAPH_E_NODE_REMOVED);
        winrt::com_ptr<IEffectGraphNode> source;
        TEST("Load_FindNode", context->FindNode(L"Gamut Source", source.put()) == S_OK && source);
        ID2D1Image* image = nullptr;
        TEST("Load_Realize", source && SUCCEEDED(context->Realize(source.get(), &image)) && image);
        if (image) image->Release();
    }
}
