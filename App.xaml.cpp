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
        // Parse command-line flags.
        std::wstring cmdLine = GetCommandLineW();
        bool autoMcp = (cmdLine.find(L"--mcp") != std::wstring::npos);
        auto devicePref = ::ShaderLab::Rendering::DevicePreference::Default;
        if (cmdLine.find(L"--warp") != std::wstring::npos)
            devicePref = ::ShaderLab::Rendering::DevicePreference::Warp;
        else if (cmdLine.find(L"--gpu") != std::wstring::npos)
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
