#include "pch_engine.h"
#include "../TestSupport.h"
#include "Support/EffectPlugins.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

// ShaderLab's effect plugin discovery. The build copies TestEffectPlugin.dll
// to Effects\ beside this runner.
namespace
{
    using namespace ShaderLab::Tests;


    std::filesystem::path RunnerDir()
    {
        wchar_t buffer[MAX_PATH]{};
        ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        return std::filesystem::path(buffer).parent_path();
    }

    // ShaderLab's discovery, unpackaged: Effects\ beside the executable, and
    // nothing else. Tagged Last: it loads the plugin into the
    // process, and a case that loaded it first leaves it at S_FALSE.
    SL_TEST_CASE(TestShaderLabPluginDiscovery, Tag::Gpu | Tag::Last)
    {
        printf("\n=== ShaderLab: finding effect plugins ===\n");
        const auto folders = ShaderLab::Support::EffectPluginFolders();
        TEST("Discovery_ExecutableFolderOnly", folders.size() == 1 && folders[0] == RunnerDir() / L"Effects");
        const auto loads = ShaderLab::Support::LoadEffectPlugins();
        const bool found = std::any_of(loads.begin(), loads.end(), [](const auto& load) {
            return load.path.filename() == L"TestEffectPlugin.dll" && (load.result == S_OK || load.result == S_FALSE);
        });
        TEST("Discovery_LoadsTestPlugin", found);
        for (const auto& load : loads)
            printf("  [info] %ls -> 0x%08X\n", load.path.filename().c_str(), static_cast<unsigned>(load.result));

        // Any folder a host names, such as the user effects folder: each DLL
        // goes to the loader and anything else is left alone. A non-plugin DLL
        // is refused and unloaded, so the folder can be deleted afterwards.
        const ScopedTempDir dir(L"ShaderLabPluginFolderTest");
        std::error_code ec;
        std::filesystem::create_directories(dir.Path(), ec);
        wchar_t system[MAX_PATH]{};
        ::GetSystemDirectoryW(system, MAX_PATH);
        std::filesystem::copy_file(std::filesystem::path(system) / L"version.dll", dir.Path() / L"NotAPlugin.dll", ec);
        std::ofstream(dir.Path() / L"notes.txt") << "not a DLL";
        const auto folderLoads = ShaderLab::Support::LoadEffectPluginsIn(dir.Path());
        TEST("Discovery_FolderLoadsItsDlls", folderLoads.size() == 1 &&
             folderLoads[0].path.filename() == L"NotAPlugin.dll" &&
             folderLoads[0].result == HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND));
        TEST("Discovery_MissingFolderLoadsNone", ShaderLab::Support::LoadEffectPluginsIn(dir.Path() / L"none").empty());
    }
}
