#pragma once

#include "pch.h"

namespace ShaderLab::Controls
{
    // One allocation of an output window's double buffer: two textures at the
    // image's size and the render context's targets on them. Never changed
    // once made; a resize makes a new one.
    struct SinkBuffers
    {
        uint32_t width{ 0 };
        uint32_t height{ 0 };
        winrt::com_ptr<ID3D11Texture2D> textures[2];
        winrt::com_ptr<ID2D1Bitmap1>    renderTargets[2];   // TARGET, render-thread context only
    };

    // A finished frame: which buffer of which allocation holds it.
    struct SinkFrame
    {
        std::shared_ptr<const SinkBuffers> buffers;
        int32_t  index{ 0 };
        uint64_t version{ 0 };
    };

    // What an OutputWindow (UI thread) shares with the render worker, which
    // never touches the window itself.
    //
    // The worker copies MainWindow's sink list under its mutex and holds each
    // shared_ptr for the frame, so closing a window (Close sets `closed`, the
    // window drops its reference) never frees a sink mid-render.
    //
    // The worker draws into the buffer the latest frame does not use and then
    // publishes a new SinkFrame; the UI loads it once and blits from that
    // buffer set, which its shared_ptr keeps alive through a resize. Both
    // contexts belong to one multithreaded D2D device, which serializes their
    // GPU work, so the worker writing a buffer the UI read a frame ago is
    // ordered after that read.
    struct OutputSinkRenderState
    {
        uint32_t nodeId{ 0 };   // the Output node shown

        std::mutex viewMutex;
        bool closed{ false };   // under viewMutex

        // Render thread only: the current allocation.
        std::shared_ptr<const SinkBuffers> renderBuffers;
        // Written by the render thread, read by the UI.
        std::atomic<std::shared_ptr<const SinkFrame>> published;

        // UI thread only: source bitmaps on the UI context over `uiBuffers`.
        std::shared_ptr<const SinkBuffers> uiBuffers;
        winrt::com_ptr<ID2D1Bitmap1>       uiSources[2];
    };
}
