#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "Rendering/RenderEngine.h"
#include "Rendering/GraphEvaluator.h"
#include "Graph/EffectGraph.h"
#include "Effects/SourceNodeFactory.h"
#include "Effects/CustomPixelShaderEffect.h"
#include "Effects/CustomComputeShaderEffect.h"
#include <ShlObj.h>
#pragma comment(lib, "shell32.lib")

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::ShaderLab::implementation
{
    App::App()
    {
#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
        {
            if (IsDebuggerPresent())
            {
                auto errorMessage = e.Message();
                __debugbreak();
            }
        });
#endif
    }

    void App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& e)
    {
        // Parse flags from command line.
        std::wstring cmdLine = GetCommandLineW();
        bool autoMcp = (cmdLine.find(L"--mcp") != std::wstring::npos);
        auto devicePref = ::ShaderLab::Rendering::DevicePreference::Default;
        if (cmdLine.find(L"--warp") != std::wstring::npos)
            devicePref = ::ShaderLab::Rendering::DevicePreference::Warp;
        else if (cmdLine.find(L"--gpu") != std::wstring::npos)
            devicePref = ::ShaderLab::Rendering::DevicePreference::Hardware;

        // Also check environment variables.
        wchar_t envBuf[16]{};
        if (GetEnvironmentVariableW(L"SHADERLAB_MCP", envBuf, 16) > 0)
            autoMcp = (envBuf[0] == L'1' || envBuf[0] == L't' || envBuf[0] == L'T');
        if (GetEnvironmentVariableW(L"SHADERLAB_WARP", envBuf, 16) > 0)
            devicePref = ::ShaderLab::Rendering::DevicePreference::Warp;

        // Also check config file: %LOCALAPPDATA%\ShaderLab\config.json
        // Format: {"mcp": true, "renderer": "warp"|"gpu"|"default"}
        {
            // Try multiple paths: real LOCALAPPDATA via SHGetKnownFolderPath,
            // and the env var (which may be virtualized in packaged apps).
            std::vector<std::wstring> configPaths;

            // Real LOCALAPPDATA via shell API.
            PWSTR realAppData = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &realAppData)))
            {
                configPaths.push_back(std::wstring(realAppData) + L"\\ShaderLab\\config.json");
                CoTaskMemFree(realAppData);
            }

            // Env var fallback.
            wchar_t appData[MAX_PATH]{};
            if (GetEnvironmentVariableW(L"LOCALAPPDATA", appData, MAX_PATH) > 0)
            {
                auto envPath = std::wstring(appData) + L"\\ShaderLab\\config.json";
                if (configPaths.empty() || configPaths[0] != envPath)
                    configPaths.push_back(envPath);
            }

            for (const auto& configPath : configPaths)
            {
                HANDLE hFile = CreateFileW(configPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                    nullptr, OPEN_EXISTING, 0, nullptr);
                if (hFile != INVALID_HANDLE_VALUE)
                {
                    char buf[1024]{};
                    DWORD bytesRead = 0;
                    ReadFile(hFile, buf, sizeof(buf) - 1, &bytesRead, nullptr);
                    CloseHandle(hFile);
                    std::string json(buf, bytesRead);
                    if (json.find("\"mcp\"") != std::string::npos &&
                        json.find("true") != std::string::npos)
                        autoMcp = true;
                    if (json.find("\"warp\"") != std::string::npos)
                        devicePref = ::ShaderLab::Rendering::DevicePreference::Warp;
                    else if (json.find("\"gpu\"") != std::string::npos)
                        devicePref = ::ShaderLab::Rendering::DevicePreference::Hardware;
                    break;
                }
            }
        }

        // ---- CLI Mode: headless graph evaluation ----
        if (cmdLine.find(L"--cli") != std::wstring::npos)
        {
            RunCliMode(cmdLine, static_cast<int>(devicePref));
            Application::Current().Exit();
            return;
        }

        auto mw = make<MainWindow>();
        auto* impl = winrt::get_self<MainWindow>(mw);
        if (autoMcp)
            impl->SetAutoStartMcp(true);
        impl->SetDevicePreference(devicePref);

        window = mw;
        window.Activate();
    }

    void App::RunCliMode(const std::wstring& cmdLine, int devicePrefInt)
    {
        auto devicePref = static_cast<::ShaderLab::Rendering::DevicePreference>(devicePrefInt);
        // Attach or allocate a console for output.
        if (!AttachConsole(ATTACH_PARENT_PROCESS))
            AllocConsole();
        FILE* fOut = nullptr;
        freopen_s(&fOut, "CONOUT$", "w", stdout);

        // Parse --graph and --output paths.
        auto extractArg = [&](const std::wstring& flag) -> std::wstring {
            auto pos = cmdLine.find(flag);
            if (pos == std::wstring::npos) return std::wstring();
            pos += flag.size();
            while (pos < cmdLine.size() && cmdLine[pos] == L' ') pos++;
            if (pos >= cmdLine.size()) return std::wstring();
            if (cmdLine[pos] == L'"')
            {
                auto end = cmdLine.find(L'"', pos + 1);
                return (end != std::wstring::npos) ? cmdLine.substr(pos + 1, end - pos - 1) : std::wstring();
            }
            auto end = cmdLine.find(L' ', pos);
            return (end != std::wstring::npos) ? cmdLine.substr(pos, end - pos) : cmdLine.substr(pos);
        };

        auto graphPath = extractArg(L"--graph");
        auto outputDir = extractArg(L"--output");
        auto adapterName = extractArg(L"--adapter");

        if (graphPath.empty())
        {
            wprintf(L"Usage: ShaderLab.exe --cli --graph <path.json> --output <dir> [--adapter warp|default]\n");
            return;
        }
        if (outputDir.empty()) outputDir = L".";

        // Handle --adapter flag.
        if (!adapterName.empty())
        {
            if (adapterName == L"warp" || adapterName == L"WARP")
                devicePref = ::ShaderLab::Rendering::DevicePreference::Warp;
            else if (adapterName != L"default")
            {
                // Try to find adapter by name substring.
                auto adapters = ::ShaderLab::Rendering::RenderEngine::EnumerateAdapters();
                for (const auto& a : adapters)
                {
                    if (a.name.find(adapterName) != std::wstring::npos)
                    {
                        devicePref = ::ShaderLab::Rendering::DevicePreference::Adapter;
                        // Store LUID for later — need to set on engine.
                        break;
                    }
                }
            }
        }

        wprintf(L"[CLI] Loading graph: %s\n", graphPath.c_str());

        // Load graph JSON.
        std::wstring graphJson;
        {
            HANDLE hf = CreateFileW(graphPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, 0, nullptr);
            if (hf == INVALID_HANDLE_VALUE)
            {
                wprintf(L"[CLI] Error: cannot open graph file\n");
                return;
            }
            DWORD size = GetFileSize(hf, nullptr);
            std::string utf8(size, '\0');
            DWORD read = 0;
            ReadFile(hf, utf8.data(), size, &read, nullptr);
            CloseHandle(hf);
            graphJson = winrt::to_hstring(utf8).c_str();
        }

        // Create headless D3D/D2D device stack.
        ::ShaderLab::Rendering::RenderEngine engine;
        // Initialize without a swap chain panel — just device resources.
        // We need a minimal init path. Use the internal CreateDeviceResources.
        // For now, use a simpler approach: create device directly.
        UINT d3dFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        winrt::com_ptr<ID3D11Device> baseDevice;
        winrt::com_ptr<ID3D11DeviceContext> baseCtx;
        D3D_DRIVER_TYPE driverType = (devicePref == ::ShaderLab::Rendering::DevicePreference::Warp)
            ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
        HRESULT hr = D3D11CreateDevice(nullptr, driverType, nullptr, d3dFlags,
            featureLevels, ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION, baseDevice.put(), nullptr, baseCtx.put());
        if (FAILED(hr))
        {
            wprintf(L"[CLI] Error: D3D11CreateDevice failed 0x%08X\n", (uint32_t)hr);
            return;
        }

        // Create D2D factory and device.
        winrt::com_ptr<ID2D1Factory7> d2dFactory;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory7), reinterpret_cast<void**>(d2dFactory.put()));

        winrt::com_ptr<IDXGIDevice> dxgiDev;
        baseDevice->QueryInterface(dxgiDev.put());
        winrt::com_ptr<ID2D1Device6> d2dDevice;
        d2dFactory->CreateDevice(dxgiDev.as<IDXGIDevice>().get(),
            reinterpret_cast<ID2D1Device**>(d2dDevice.put()));
        winrt::com_ptr<ID2D1DeviceContext5> dc;
        d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            reinterpret_cast<ID2D1DeviceContext**>(dc.put()));

        // Register custom D2D effects.
        winrt::com_ptr<ID2D1Factory1> factory1;
        d2dFactory->QueryInterface(factory1.put());
        if (factory1)
        {
            ::ShaderLab::Effects::CustomPixelShaderEffect::RegisterEffect(factory1.get());
            ::ShaderLab::Effects::CustomComputeShaderEffect::RegisterEffect(factory1.get());
        }

        wprintf(L"[CLI] Device created (%s)\n",
            driverType == D3D_DRIVER_TYPE_WARP ? L"WARP" : L"Hardware");

        // Load and evaluate graph.
        auto graph = ::ShaderLab::Graph::EffectGraph::FromJson(winrt::hstring(graphJson));
        graph.MarkAllDirty();

        // Prepare source nodes.
        ::ShaderLab::Effects::SourceNodeFactory sourceFactory;
        for (auto& node : const_cast<std::vector<::ShaderLab::Graph::EffectNode>&>(graph.Nodes()))
        {
            if (node.type == ::ShaderLab::Graph::NodeType::Source)
            {
                try {
                    sourceFactory.PrepareSourceNode(node, dc.get(), 0.0,
                        baseDevice.as<ID3D11Device5>().get(),
                        baseCtx.as<ID3D11DeviceContext4>().get());
                } catch (...) {
                    wprintf(L"[CLI] Warning: failed to prepare source node %d\n", node.id);
                }
            }
        }

        // Evaluate.
        ::ShaderLab::Rendering::GraphEvaluator evaluator;
        evaluator.Evaluate(graph, dc.get());
        if (graph.HasDirtyNodes())
            evaluator.Evaluate(graph, dc.get());

        // Save output for each node that has cachedOutput.
        // Create output directory if needed.
        CreateDirectoryW(outputDir.c_str(), nullptr);

        uint32_t saved = 0;
        for (const auto& node : graph.Nodes())
        {
            if (!node.cachedOutput) continue;
            if (node.type == ::ShaderLab::Graph::NodeType::Output ||
                node.outputPins.empty()) // leaf nodes
            {
                // Render to bitmap.
                D2D1_RECT_F bounds{};
                dc->GetImageLocalBounds(node.cachedOutput, &bounds);
                uint32_t w = static_cast<uint32_t>(bounds.right - bounds.left);
                uint32_t h = static_cast<uint32_t>(bounds.bottom - bounds.top);
                if (w == 0 || h == 0 || w > 8192 || h > 8192) continue;

                winrt::com_ptr<ID2D1Bitmap1> renderBmp;
                D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_TARGET,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, bmpProps, renderBmp.put());
                if (!renderBmp) continue;

                winrt::com_ptr<ID2D1Image> oldTarget;
                dc->GetTarget(oldTarget.put());
                dc->SetTarget(renderBmp.get());
                dc->BeginDraw();
                dc->Clear(D2D1::ColorF(D2D1::ColorF::Black));
                dc->DrawImage(node.cachedOutput);
                dc->EndDraw();
                dc->SetTarget(oldTarget.get());

                // CPU readback.
                winrt::com_ptr<ID2D1Bitmap1> cpuBmp;
                D2D1_BITMAP_PROPERTIES1 cpuProps = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, cpuProps, cpuBmp.put());
                if (!cpuBmp) continue;
                cpuBmp->CopyFromBitmap(nullptr, renderBmp.get(), nullptr);

                D2D1_MAPPED_RECT mapped{};
                cpuBmp->Map(D2D1_MAP_OPTIONS_READ, &mapped);

                // Write PNG via WIC.
                winrt::com_ptr<IWICImagingFactory> wic;
                CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()));
                if (!wic) { cpuBmp->Unmap(); continue; }

                std::wstring outPath = std::wstring(outputDir) + L"\\" +
                    std::format(L"node_{}.png", node.id);
                winrt::com_ptr<IWICStream> stream;
                wic->CreateStream(stream.put());
                stream->InitializeFromFilename(outPath.c_str(), GENERIC_WRITE);

                winrt::com_ptr<IWICBitmapEncoder> encoder;
                wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put());
                encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);

                winrt::com_ptr<IWICBitmapFrameEncode> frame;
                encoder->CreateNewFrame(frame.put(), nullptr);
                frame->Initialize(nullptr);
                frame->SetSize(w, h);
                WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
                frame->SetPixelFormat(&fmt);
                frame->WritePixels(h, mapped.pitch, mapped.pitch * h,
                    const_cast<BYTE*>(mapped.bits));
                frame->Commit();
                encoder->Commit();

                cpuBmp->Unmap();
                wprintf(L"[CLI] Saved: %s (%dx%d)\n", outPath.c_str(), w, h);
                saved++;
            }
        }

        wprintf(L"[CLI] Done. %d images saved.\n", saved);
    }
}
