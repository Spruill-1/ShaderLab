#include "pch_engine.h"
#include "../TestSupport.h"
#include "Engine/Api/EffectGraph.h"
#include "Engine/Api/EffectGraphInternal.h"
#include "Engine/Utf8.h"
#include "Mcp/EngineMcpRoutes.h"
#include "Mcp/McpRouter.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

// The MCP routes driving an EffectGraph context, as ShaderLab's hosts do.
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


    // A texture on `device`, the test device unless given.
    winrt::com_ptr<ID3D11Texture2D> MakeTexture(DXGI_FORMAT format, UINT bind, const void* texels, UINT pitch,
                                                ID3D11Device* device = nullptr)
    {
        if (!device) device = g_d3dDevice.get();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = cWidth; desc.Height = cHeight; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = format; desc.SampleDesc.Count = 1; desc.BindFlags = bind;
        D3D11_SUBRESOURCE_DATA init{ texels, pitch, 0 };
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(device->CreateTexture2D(&desc, texels ? &init : nullptr, texture.put()));
        return texture;
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

    // An MCP route that replaces the graph does it through the context, so
    // node interfaces held from before see their node as removed.
    SL_TEST_CASE(TestEffectGraphReplacedOverMcp, Tag::Gpu | Tag::Mcp)
    {
        printf("\n=== EffectGraph: a graph replaced over MCP ===\n");
        struct ContextSink : ShaderLab::Mcp::IEngineCommandSink
        {
            IEffectGraphContextInternal* context{ nullptr };
            ShaderLab::Mcp::Response Dispatch(
                std::function<ShaderLab::Mcp::Response(ShaderLab::Mcp::EngineContext&)> closure) override
            {
                ShaderLab::Mcp::EngineContext engineContext(*context);
                return closure(engineContext);
            }
        };
        auto context = MakeContext();
        auto internal = context.as<IEffectGraphContextInternal>();
        ContextSink sink;
        sink.context = internal.get();
        ShaderLab::McpRouter router;
        ShaderLab::Mcp::RegisterEngineRoutes(router, sink);

        auto texels = GreyTexels(1.0f, 3.0f);
        auto texture = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, texels.data(), cWidth * 8);
        winrt::com_ptr<IEffectGraphNode> input, stats;
        winrt::check_hresult(context->CreateTextureInput(texture.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR, input.put()));
        winrt::check_hresult(context->CreateEffect(L"Luminance Statistics", stats.put()));
        stats->SetInput(0, input.get());
        TEST("Replace_RendersBefore", SUCCEEDED(context->Realize(stats.get(), nullptr)) &&
             std::abs(FieldOf(stats.get(), L"Max") - 240.0f) < 0.05f);

        const auto cleared = router.RouteRequest(L"POST", L"/graph/clear", "");
        wchar_t error[64];
        TEST("Replace_ClearSucceeds", cleared.statusCode == 200 && internal->Model().Nodes().empty());
        TEST("Replace_ClearRemovesNodes", stats->GetError(error, 64) == EFFECTGRAPH_E_NODE_REMOVED &&
             context->Realize(stats.get(), nullptr) == EFFECTGRAPH_E_NODE_REMOVED);
        TEST("Replace_ClearRemovesInputs",
             input.as<IEffectGraphInput>()->SetTexture(texture.get(), EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR) ==
                 EFFECTGRAPH_E_NODE_REMOVED);

        // A node the next graph gives the same id is not the old interface's.
        winrt::com_ptr<IEffectGraphNode> fresh;
        winrt::check_hresult(context->CreateEffect(L"Luminance Statistics", fresh.put()));
        const std::string graphJson = ShaderLab::WideToUtf8(std::wstring(internal->Model().ToJson()));
        const auto loaded = router.RouteRequest(L"POST", L"/graph/load", graphJson);
        TEST("Replace_LoadSucceeds", loaded.statusCode == 200 && internal->Model().Nodes().size() == 1);
        TEST("Replace_LoadRemovesNodes", fresh->GetError(error, 64) == EFFECTGRAPH_E_NODE_REMOVED);
        winrt::com_ptr<IEffectGraphNode> found;
        TEST("Replace_LoadedNodeFound", internal->NodeFor(internal->Model().Nodes()[0].id, found.put()) == S_OK &&
             found && found.get() != fresh.get());
    }
}
