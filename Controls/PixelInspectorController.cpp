#include "pch.h"
#include "PixelInspectorController.h"

namespace ShaderLab::Controls
{
    // -----------------------------------------------------------------------
    // InspectedPixel format strings
    // -----------------------------------------------------------------------

    std::wstring InspectedPixel::ScRGBString() const
    {
        return std::format(L"scRGB: ({:.4f}, {:.4f}, {:.4f}, {:.4f})", scR, scG, scB, scA);
    }

    std::wstring InspectedPixel::SRGBString() const
    {
        return std::format(L"sRGB: ({}, {}, {}, {})", sR, sG, sB, sA);
    }

    std::wstring InspectedPixel::HDR10String() const
    {
        return std::format(L"PQ: ({:.4f}, {:.4f}, {:.4f})", pqR, pqG, pqB);
    }

    std::wstring InspectedPixel::LuminanceString() const
    {
        return std::format(L"Luminance: {:.1f} cd/m\u00B2", luminanceNits);
    }

    std::wstring InspectedPixel::PositionString() const
    {
        return std::format(L"Pixel: ({}, {})", x, y);
    }

    // -----------------------------------------------------------------------
    // Initialization
    // -----------------------------------------------------------------------

    void PixelInspectorController::Initialize(ID3D11Device* device)
    {
        m_device.copy_from(device);
        if (m_device)
            m_device->GetImmediateContext(m_deviceContext.put());
    }

    // -----------------------------------------------------------------------
    // Pixel inspection
    // -----------------------------------------------------------------------

    bool PixelInspectorController::InspectPixel(
        ID2D1DeviceContext* dc,
        const Graph::EffectGraph& graph,
        uint32_t nodeId,
        uint32_t pixelX,
        uint32_t pixelY)
    {
        m_hasPixel = false;
        if (!dc || !m_device) return false;

        const auto* node = graph.FindNode(nodeId);
        if (!node || !node->cachedOutput) return false;

        // Render the pixel region to a small CPU-readable bitmap.
        // Strategy: create a target bitmap, draw the image offset so the
        // desired pixel lands at (0,0), then map for CPU read.
        winrt::com_ptr<ID2D1Bitmap1> targetBitmap;

        D2D1_BITMAP_PROPERTIES1 targetProps = {};
        targetProps.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;

        HRESULT hr = dc->CreateBitmap(
            D2D1::SizeU(1, 1),
            nullptr, 0,
            targetProps,
            targetBitmap.put());
        if (FAILED(hr)) return false;

        // Draw the image to the target bitmap.
        winrt::com_ptr<ID2D1Image> prevTarget;
        dc->GetTarget(prevTarget.put());

        // Set DPI to 96 so 1 DIP = 1 pixel in the source rect.
        float oldDpiX, oldDpiY;
        dc->GetDpi(&oldDpiX, &oldDpiY);
        dc->SetDpi(96.0f, 96.0f);

        dc->SetTarget(targetBitmap.get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->DrawImage(
            node->cachedOutput,
            D2D1::Point2F(0, 0),
            D2D1::RectF(
                static_cast<float>(pixelX),
                static_cast<float>(pixelY),
                static_cast<float>(pixelX + 1),
                static_cast<float>(pixelY + 1)),
            D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
            D2D1_COMPOSITE_MODE_SOURCE_COPY);
        dc->EndDraw();
        dc->SetTarget(prevTarget.get());
        dc->SetDpi(oldDpiX, oldDpiY);

        // Create a CPU-readable bitmap and copy from the target.
        winrt::com_ptr<ID2D1Bitmap1> readbackBitmap;

        D2D1_BITMAP_PROPERTIES1 readbackProps = {};
        readbackProps.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        readbackProps.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

        hr = dc->CreateBitmap(
            D2D1::SizeU(1, 1),
            nullptr, 0,
            readbackProps,
            readbackBitmap.put());
        if (FAILED(hr)) return false;

        D2D1_POINT_2U destPoint = { 0, 0 };
        D2D1_RECT_U srcRect = { 0, 0, 1, 1 };
        hr = readbackBitmap->CopyFromBitmap(&destPoint, targetBitmap.get(), &srcRect);
        if (FAILED(hr)) return false;

        // Map the bitmap for CPU read.
        D2D1_MAPPED_RECT mapped{};
        hr = readbackBitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped);
        if (FAILED(hr)) return false;

        // Read the RGBA float values.
        auto* pixels = reinterpret_cast<const float*>(mapped.bits);
        m_lastPixel.scR = pixels[0];
        m_lastPixel.scG = pixels[1];
        m_lastPixel.scB = pixels[2];
        m_lastPixel.scA = pixels[3];

        readbackBitmap->Unmap();

        // Convert to sRGB.
        m_lastPixel.sR = LinearToSRGB(m_lastPixel.scR);
        m_lastPixel.sG = LinearToSRGB(m_lastPixel.scG);
        m_lastPixel.sB = LinearToSRGB(m_lastPixel.scB);
        m_lastPixel.sA = static_cast<uint8_t>((std::min)(255.0f, (std::max)(0.0f, m_lastPixel.scA * 255.0f)));

        // Convert to PQ.
        m_lastPixel.pqR = LinearToPQ(m_lastPixel.scR);
        m_lastPixel.pqG = LinearToPQ(m_lastPixel.scG);
        m_lastPixel.pqB = LinearToPQ(m_lastPixel.scB);

        // Compute luminance.
        m_lastPixel.luminanceNits = ComputeLuminance(m_lastPixel.scR, m_lastPixel.scG, m_lastPixel.scB);

        // Position.
        m_lastPixel.x = pixelX;
        m_lastPixel.y = pixelY;
        m_lastPixel.nodeId = nodeId;

        m_hasPixel = true;
        return true;
    }

    // -----------------------------------------------------------------------
    // Tracking
    // -----------------------------------------------------------------------

    void PixelInspectorController::SetTrackPosition(uint32_t x, uint32_t y, uint32_t nodeId)
    {
        m_trackX = x;
        m_trackY = y;
        m_trackNodeId = nodeId;
    }

    bool PixelInspectorController::ReInspect(
        ID2D1DeviceContext* dc,
        const Graph::EffectGraph& graph)
    {
        if (m_trackNodeId == 0) return false;
        return InspectPixel(dc, graph, m_trackNodeId, m_trackX, m_trackY);
    }

    // -----------------------------------------------------------------------
    // Staging texture
    // -----------------------------------------------------------------------

    bool PixelInspectorController::EnsureStagingTexture(uint32_t width, uint32_t height)
    {
        if (m_stagingTexture && m_stagingWidth >= width && m_stagingHeight >= height)
            return true;

        m_stagingTexture = nullptr;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc = { 1, 0 };
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, m_stagingTexture.put());
        if (FAILED(hr)) return false;

        m_stagingWidth = width;
        m_stagingHeight = height;
        return true;
    }

    // -----------------------------------------------------------------------
    // Color space conversion helpers
    // -----------------------------------------------------------------------

    uint8_t PixelInspectorController::LinearToSRGB(float linear)
    {
        // Clamp to [0, 1] for sRGB.
        float c = (std::max)(0.0f, (std::min)(1.0f, linear));

        // sRGB transfer function.
        float srgb;
        if (c <= 0.0031308f)
            srgb = c * 12.92f;
        else
            srgb = 1.055f * std::powf(c, 1.0f / 2.4f) - 0.055f;

        return static_cast<uint8_t>(srgb * 255.0f + 0.5f);
    }

    float PixelInspectorController::LinearToPQ(float linear)
    {
        // ST.2084 PQ EOTF inverse.
        // Input is scene-referred linear light (1.0 = 80 cd/m²).
        // Scale to absolute luminance: scRGB 1.0 = 80 nits.
        float Y = (std::max)(0.0f, linear * 80.0f / 10000.0f);

        // PQ constants.
        constexpr float m1 = 2610.0f / 16384.0f;
        constexpr float m2 = 2523.0f / 4096.0f * 128.0f;
        constexpr float c1 = 3424.0f / 4096.0f;
        constexpr float c2 = 2413.0f / 4096.0f * 32.0f;
        constexpr float c3 = 2392.0f / 4096.0f * 32.0f;

        float Ym1 = std::powf(Y, m1);
        float pq = std::powf((c1 + c2 * Ym1) / (1.0f + c3 * Ym1), m2);
        return pq;
    }

    float PixelInspectorController::ComputeLuminance(float r, float g, float b)
    {
        // BT.709 luminance coefficients, scaled by scRGB reference white (80 nits).
        return (0.2126f * r + 0.7152f * g + 0.0722f * b) * 80.0f;
    }
}
