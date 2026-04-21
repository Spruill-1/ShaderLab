#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "Rendering/RenderEngine.h"

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
        // Parse flags from command line AND environment variables.
        // Environment vars work reliably with packaged WinUI 3 apps.
        std::wstring cmdLine = GetCommandLineW();
        bool autoMcp = (cmdLine.find(L"--mcp") != std::wstring::npos);
        auto devicePref = ::ShaderLab::Rendering::DevicePreference::Default;
        if (cmdLine.find(L"--warp") != std::wstring::npos)
            devicePref = ::ShaderLab::Rendering::DevicePreference::Warp;
        else if (cmdLine.find(L"--gpu") != std::wstring::npos)
            devicePref = ::ShaderLab::Rendering::DevicePreference::Hardware;

        // Also check environment variables (reliable for packaged apps).
        wchar_t envBuf[16]{};
        if (GetEnvironmentVariableW(L"SHADERLAB_MCP", envBuf, 16) > 0)
            autoMcp = (envBuf[0] == L'1' || envBuf[0] == L't' || envBuf[0] == L'T');
        if (GetEnvironmentVariableW(L"SHADERLAB_WARP", envBuf, 16) > 0)
            devicePref = ::ShaderLab::Rendering::DevicePreference::Warp;
        if (GetEnvironmentVariableW(L"SHADERLAB_GPU", envBuf, 16) > 0)
            devicePref = ::ShaderLab::Rendering::DevicePreference::Hardware;

        auto mw = make<MainWindow>();
        auto* impl = winrt::get_self<MainWindow>(mw);
        if (autoMcp)
            impl->SetAutoStartMcp(true);
        impl->SetDevicePreference(devicePref);

        window = mw;
        window.Activate();
    }
}
