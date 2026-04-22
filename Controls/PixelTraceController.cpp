#include "pch.h"
#include "PixelTraceController.h"

namespace ShaderLab::Controls
{
    // -----------------------------------------------------------------------
    // Initialization
    // -----------------------------------------------------------------------

    void PixelTraceController::Initialize(ID3D11Device* device)
    {
        m_device.copy_from(device);
    }

    // -----------------------------------------------------------------------
    // Shared bitmap creation
    // -----------------------------------------------------------------------

    bool PixelTraceController::EnsureBitmaps(ID2D1DeviceContext* dc)
    {
        if (m_targetBitmap && m_readbackBitmap)
            return true;

        // 1×1 render target (float4).
        D2D1_BITMAP_PROPERTIES1 targetProps = {};
        targetProps.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;

        HRESULT hr = dc->CreateBitmap(
            D2D1::SizeU(1, 1),
            nullptr, 0,
            targetProps,
            m_targetBitmap.put());
        if (FAILED(hr)) return false;

        // 1×1 CPU-readable staging bitmap.
        D2D1_BITMAP_PROPERTIES1 readbackProps = {};
        readbackProps.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        readbackProps.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

        hr = dc->CreateBitmap(
            D2D1::SizeU(1, 1),
            nullptr, 0,
            readbackProps,
            m_readbackBitmap.put());
        if (FAILED(hr))
        {
            m_targetBitmap = nullptr;
            return false;
        }

        return true;
    }

    // -----------------------------------------------------------------------
    // Single-pixel readback
    // -----------------------------------------------------------------------

    bool PixelTraceController::ReadPixel(
        ID2D1DeviceContext* dc,
        ID2D1Image* image,
        uint32_t pixelX,
        uint32_t pixelY,
        InspectedPixel& out)
    {
        if (!dc || !image) return false;
        if (!EnsureBitmaps(dc)) return false;

        // Save and set DPI to 96 so that 1 DIP = 1 pixel in the source rect,
        // and the 1×1 pixel target bitmap covers exactly 1×1 DIPs.
        float oldDpiX, oldDpiY;
        dc->GetDpi(&oldDpiX, &oldDpiY);
        dc->SetDpi(96.0f, 96.0f);

        float srcX = static_cast<float>(pixelX);
        float srcY = static_cast<float>(pixelY);

        // Draw the source image so the desired pixel lands at (0,0).
        winrt::com_ptr<ID2D1Image> prevTarget;
        dc->GetTarget(prevTarget.put());

        dc->SetTarget(m_targetBitmap.get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->DrawImage(
            image,
            D2D1::Point2F(0, 0),
            D2D1::RectF(srcX, srcY, srcX + 1.0f, srcY + 1.0f),
            D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
            D2D1_COMPOSITE_MODE_SOURCE_COPY);
        dc->EndDraw();
        dc->SetTarget(prevTarget.get());
        dc->SetDpi(oldDpiX, oldDpiY);

        // Copy to CPU-readable bitmap.
        D2D1_POINT_2U destPoint = { 0, 0 };
        D2D1_RECT_U   srcRect  = { 0, 0, 1, 1 };
        HRESULT hr = m_readbackBitmap->CopyFromBitmap(&destPoint, m_targetBitmap.get(), &srcRect);
        if (FAILED(hr)) return false;

        // Map & read float4.
        D2D1_MAPPED_RECT mapped{};
        hr = m_readbackBitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped);
        if (FAILED(hr)) return false;

        auto* pixels = reinterpret_cast<const float*>(mapped.bits);
        out.scR = pixels[0];
        out.scG = pixels[1];
        out.scB = pixels[2];
        out.scA = pixels[3];

        m_readbackBitmap->Unmap();

        // sRGB conversion.
        out.sR = LinearToSRGB(out.scR);
        out.sG = LinearToSRGB(out.scG);
        out.sB = LinearToSRGB(out.scB);
        out.sA = static_cast<uint8_t>((std::min)(255.0f, (std::max)(0.0f, out.scA * 255.0f)));

        // HDR10 PQ conversion.
        out.pqR = LinearToPQ(out.scR);
        out.pqG = LinearToPQ(out.scG);
        out.pqB = LinearToPQ(out.scB);

        // Luminance.
        out.luminanceNits = ComputeLuminance(out.scR, out.scG, out.scB);

        return true;
    }

    // -----------------------------------------------------------------------
    // Recursive trace builder
    // -----------------------------------------------------------------------

    PixelTraceNode PixelTraceController::BuildTraceNode(
        ID2D1DeviceContext* dc,
        const Graph::EffectGraph& graph,
        uint32_t nodeId,
        uint32_t pixelX,
        uint32_t pixelY)
    {
        PixelTraceNode result;
        result.nodeId = nodeId;

        const auto* node = graph.FindNode(nodeId);
        if (!node) return result;

        result.nodeName = node->name;

        // Read this node's pixel value from its cached output.
        if (node->cachedOutput)
            ReadPixel(dc, node->cachedOutput, pixelX, pixelY, result.pixel);

        result.pixel.x = pixelX;
        result.pixel.y = pixelY;
        result.pixel.nodeId = nodeId;

        // Copy analysis output for compute/analysis nodes.
        if (node->analysisOutput.type == Graph::AnalysisOutputType::Typed &&
            !node->analysisOutput.fields.empty())
        {
            result.hasAnalysisOutput = true;
            result.analysisFields = node->analysisOutput.fields;
        }

        // Recurse backward through input edges.
        auto inputEdges = graph.GetInputEdges(nodeId);
        for (const auto* edge : inputEdges)
        {
            auto child = BuildTraceNode(dc, graph, edge->sourceNodeId, pixelX, pixelY);
            child.inputPin = edge->destPin;

            if (edge->destPin < node->inputPins.size())
                child.pinName = node->inputPins[edge->destPin].name;
            else
                child.pinName = std::format(L"[{}]", edge->destPin);

            result.inputs.push_back(std::move(child));
        }

        // Sort children by input pin index for deterministic order.
        std::sort(result.inputs.begin(), result.inputs.end(),
            [](const PixelTraceNode& a, const PixelTraceNode& b)
            {
                return a.inputPin < b.inputPin;
            });

        return result;
    }

    // -----------------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------------

    bool PixelTraceController::BuildTrace(
        ID2D1DeviceContext* dc,
        const Graph::EffectGraph& graph,
        uint32_t targetNodeId,
        float normalizedX,
        float normalizedY,
        uint32_t imageWidth,
        uint32_t imageHeight)
    {
        m_hasTrace = false;
        if (!dc || !m_device) return false;
        if (imageWidth == 0 || imageHeight == 0) return false;

        // Convert normalized [0,1] coordinates to pixel position.
        uint32_t pixelX = static_cast<uint32_t>(
            (std::min)(normalizedX * static_cast<float>(imageWidth),
                       static_cast<float>(imageWidth - 1)));
        uint32_t pixelY = static_cast<uint32_t>(
            (std::min)(normalizedY * static_cast<float>(imageHeight),
                       static_cast<float>(imageHeight - 1)));

        // Store tracking position.
        m_trackNormX = normalizedX;
        m_trackNormY = normalizedY;

        m_root = BuildTraceNode(dc, graph, targetNodeId, pixelX, pixelY);
        m_hasTrace = true;
        return true;
    }

    void PixelTraceController::SetTrackPosition(float normX, float normY)
    {
        m_trackNormX = normX;
        m_trackNormY = normY;
    }

    bool PixelTraceController::ReTrace(
        ID2D1DeviceContext* dc,
        const Graph::EffectGraph& graph,
        uint32_t targetNodeId,
        uint32_t imageWidth,
        uint32_t imageHeight)
    {
        return BuildTrace(dc, graph, targetNodeId,
                          m_trackNormX, m_trackNormY,
                          imageWidth, imageHeight);
    }

    // -----------------------------------------------------------------------
    // Color-space conversion helpers
    // -----------------------------------------------------------------------

    uint8_t PixelTraceController::LinearToSRGB(float linear)
    {
        float c = (std::max)(0.0f, (std::min)(1.0f, linear));

        float srgb;
        if (c <= 0.0031308f)
            srgb = c * 12.92f;
        else
            srgb = 1.055f * std::powf(c, 1.0f / 2.4f) - 0.055f;

        return static_cast<uint8_t>(srgb * 255.0f + 0.5f);
    }

    float PixelTraceController::LinearToPQ(float linear)
    {
        float Y = (std::max)(0.0f, linear * 80.0f / 10000.0f);

        constexpr float m1 = 2610.0f / 16384.0f;
        constexpr float m2 = 2523.0f / 4096.0f * 128.0f;
        constexpr float c1 = 3424.0f / 4096.0f;
        constexpr float c2 = 2413.0f / 4096.0f * 32.0f;
        constexpr float c3 = 2392.0f / 4096.0f * 32.0f;

        float Ym1 = std::powf(Y, m1);
        float pq = std::powf((c1 + c2 * Ym1) / (1.0f + c3 * Ym1), m2);
        return pq;
    }

    float PixelTraceController::ComputeLuminance(float r, float g, float b)
    {
        return (0.2126f * r + 0.7152f * g + 0.0722f * b) * 80.0f;
    }
}
