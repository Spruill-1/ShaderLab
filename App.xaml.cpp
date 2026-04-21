#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "Rendering/RenderEngine.h"
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

        auto mw = make<MainWindow>();
        auto* impl = winrt::get_self<MainWindow>(mw);
        if (autoMcp)
            impl->SetAutoStartMcp(true);
        impl->SetDevicePreference(devicePref);

        window = mw;
        window.Activate();
    }
}
