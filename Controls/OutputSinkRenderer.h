#pragma once

#include "pch.h"
#include "OutputSinkRenderState.h"
#include "../Rendering/PipelineFormat.h"

namespace ShaderLab::Controls
{
    // Render thread: draws `image` at its own size (up to 8192 per side) into
    // the sink's free buffer, reallocating the pair when the size changes,
    // and publishes the frame. Returns the failing HRESULT, or S_FALSE when
    // there was nothing to draw.
    HRESULT RenderOutputSink(OutputSinkRenderState& sink, ID2D1Image* image, ID2D1DeviceContext* dc,
                             ID3D11Device* device, const Rendering::PipelineFormat& format);
}
