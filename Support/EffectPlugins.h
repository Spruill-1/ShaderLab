#pragma once
#include <filesystem>
#include <vector>
#include <windows.h>

namespace ShaderLab::Support
{
    struct EffectPluginLoad
    {
        std::filesystem::path path;
        HRESULT result{ S_OK };   // EffectGraphLoadPlugin's
    };

    // The Effects\ folders a ShaderLab install brings: in a packaged process,
    // the app's package and every package it depends on (an optional package
    // of effects, from anyone); unpackaged, beside the executable.
    std::vector<std::filesystem::path> EffectPluginFolders();

    // Loads every .dll in `folder`, in name order. A missing folder loads none.
    std::vector<EffectPluginLoad> LoadEffectPluginsIn(const std::filesystem::path& folder);

    // LoadEffectPluginsIn for each of EffectPluginFolders. Every ShaderLab
    // program calls it once at startup, after RegisterShaderLabSupport and
    // before any graph evaluates, and also loads the plugins in the folders it
    // reads user effects from.
    std::vector<EffectPluginLoad> LoadEffectPlugins();
}
