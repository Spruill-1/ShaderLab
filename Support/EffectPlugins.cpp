#include "pch_engine.h"
#include "EffectPlugins.h"
#include "Engine/Api/EffectGraphPlugin.h"

#include <appmodel.h>
#include <winrt/Windows.ApplicationModel.h>

namespace ShaderLab::Support
{
    namespace
    {
        constexpr const wchar_t* cPluginFolder = L"Effects";

        bool IsPackagedProcess()
        {
            UINT32 length = 0;
            return ::GetCurrentPackageFullName(&length, nullptr) != APPMODEL_ERROR_NO_PACKAGE;
        }

        std::filesystem::path ExecutableDir()
        {
            wchar_t buffer[MAX_PATH]{};
            ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
            return std::filesystem::path(buffer).parent_path();
        }
    }

    std::vector<std::filesystem::path> EffectPluginFolders()
    {
        std::vector<std::filesystem::path> folders;
        if (!IsPackagedProcess())
        {
            folders.push_back(ExecutableDir() / cPluginFolder);
            return folders;
        }
        const auto app = winrt::Windows::ApplicationModel::Package::Current();
        folders.push_back(std::filesystem::path(std::wstring(app.InstalledPath())) / cPluginFolder);
        for (const auto& dependency : app.Dependencies())
            folders.push_back(std::filesystem::path(std::wstring(dependency.InstalledPath())) / cPluginFolder);
        return folders;
    }

    std::vector<EffectPluginLoad> LoadEffectPluginsIn(const std::filesystem::path& folder)
    {
        std::vector<EffectPluginLoad> loads;
        std::error_code ec;
        std::vector<std::filesystem::path> dlls;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
            if (entry.is_regular_file(ec) && _wcsicmp(entry.path().extension().c_str(), L".dll") == 0)
                dlls.push_back(entry.path());
        std::sort(dlls.begin(), dlls.end());
        for (const auto& dll : dlls)
            loads.push_back({ dll, EffectGraphLoadPlugin(dll.c_str()) });
        return loads;
    }

    std::vector<EffectPluginLoad> LoadEffectPlugins()
    {
        std::vector<EffectPluginLoad> loads;
        for (const auto& folder : EffectPluginFolders())
        {
            auto found = LoadEffectPluginsIn(folder);
            loads.insert(loads.end(), found.begin(), found.end());
        }
        return loads;
    }
}
