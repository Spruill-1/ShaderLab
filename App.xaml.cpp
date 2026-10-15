#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "Rendering/RenderEngine.h"
#include "Rendering/EffectGraphFile.h"
#include "Graph/PropertyWrite.h"
#include <ShlObj.h>
#include <fstream>
#pragma comment(lib, "shell32.lib")

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace
{
    // Applies one config.json:
    //   {"mcp": true | false, "renderer": "warp" | "gpu" | "default"}
    // Returns false when the file is missing or is not a JSON object.
    bool ApplyConfigFile(const std::wstring& path, bool& autoMcp,
                         ::ShaderLab::Rendering::DevicePreference& devicePref)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        winrt::Windows::Data::Json::JsonObject config{ nullptr };
        if (!winrt::Windows::Data::Json::JsonObject::TryParse(winrt::to_hstring(text), config))
        {
            OutputDebugStringW((L"[ShaderLab] config is not a JSON object: " + path + L"\n").c_str());
            return false;
        }
        if (config.HasKey(L"mcp"))
        {
            const auto value = config.GetNamedValue(L"mcp");
            if (value.ValueType() == winrt::Windows::Data::Json::JsonValueType::Boolean)
                autoMcp = value.GetBoolean();
        }
        const std::wstring renderer{ config.GetNamedString(L"renderer", L"") };
        if (renderer == L"warp")
            devicePref = ::ShaderLab::Rendering::DevicePreference::Warp;
        else if (renderer == L"gpu")
            devicePref = ::ShaderLab::Rendering::DevicePreference::Hardware;
        return true;
    }
}

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
        // Parse flags from command line. The MCP session starts with the
        // window unless SHADERLAB_MCP=0 or config.json's "mcp": false says not.
        std::wstring cmdLine = GetCommandLineW();
        bool autoMcp = true;
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

        // Also check %LOCALAPPDATA%\ShaderLab\config.json.
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
                if (ApplyConfigFile(configPath, autoMcp, devicePref))
                    break;
            }
        }

        // ---- Reap mode: scan %TEMP% for orphaned ShaderLab media dirs ----
        // and delete any whose .heartbeat (or directory mtime, for legacy
        // dirs without a heartbeat) is older than the staleness threshold.
        // Useful for CI / scripted regression: no UI, no prompt, exits
        // with the count of dirs reaped.
        if (cmdLine.find(L"--reap-now") != std::wstring::npos)
        {
            uint64_t staleSec = ::ShaderLab::Rendering::cStaleMediaDirSec;

            // Optional --reap-stale-sec=<N> override for tests that don't
            // want to wait through the default window.
            {
                auto p = cmdLine.find(L"--reap-stale-sec=");
                if (p != std::wstring::npos)
                {
                    p += wcslen(L"--reap-stale-sec=");
                    auto end = cmdLine.find(L' ', p);
                    if (auto value = ::ShaderLab::Graph::ParseNumber(cmdLine.substr(p, end - p)); value && *value >= 0.0)
                        staleSec = static_cast<uint64_t>(*value);
                }
            }

            // Allocate / use stderr for status reporting.
            if (!AttachConsole(ATTACH_PARENT_PROCESS))
                AllocConsole();
            FILE* fp = nullptr;
            freopen_s(&fp, "CONOUT$", "w", stderr);

            const auto stale = ::ShaderLab::Rendering::FindStaleMediaDirs(
                ::ShaderLab::Rendering::TempDirectory(), staleSec);
            const auto reap = ::ShaderLab::Rendering::ReapMediaDirs(stale);
            for (const auto& [path, reason] : reap.failures)
                fwprintf(stderr, L"[reap] FAIL   %s (%hs)\n", path.c_str(), reason.c_str());
            const uint32_t reaped = static_cast<uint32_t>(reap.deleted);
            fwprintf(stderr, L"[reap] done; %u folder%s removed\n",
                reaped, reaped == 1 ? L"" : L"s");
            ExitProcess(reaped);
            return;
        }

        auto mw = make<MainWindow>();
        auto* impl = winrt::get_self<MainWindow>(mw);
        impl->SetAutoStartMcp(autoMcp);
        impl->SetDevicePreference(devicePref);

        // File activation: when the user double-clicks a .effectgraph
        // file in Explorer, the OS launches us with the file path on
        // the command line. We just scan for it here -- proper
        // Microsoft.Windows.AppLifecycle activation routing isn't
        // worth the WinAppSDK reunion glue for a single argument.
        {
            int argc = 0;
            wchar_t** argv = ::CommandLineToArgvW(GetCommandLineW(), &argc);
            if (argv)
            {
                for (int i = 1; i < argc; ++i)
                {
                    std::wstring a = argv[i];
                    // Skip our own flags.
                    if (a.starts_with(L"--")) continue;
                    auto lower = a;
                    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
                    if (lower.ends_with(L".effectgraph"))
                    {
                        impl->SetPendingOpenPath(a);
                        break;
                    }
                }
                ::LocalFree(argv);
            }
        }

        window = mw;
        window.Activate();
    }
}
