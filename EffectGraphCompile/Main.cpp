#include "pch_engine.h"
#include "../Effects/EffectBlob.h"
#include "../Engine/EngineBootstrap.h"
#include "../Engine/Utf8.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

// EffectGraphCompile: builds an effect blob from an effect saved by
// ShaderLab's Effect Designer, for a program to register with
// EffectGraphRegisterEffect. Run at the consuming project's build
// (EffectGraph.targets does it for EffectGraphEffect items).
namespace
{
    namespace fs = std::filesystem;
    using namespace ShaderLab;

    void PrintUsage()
    {
        std::fwprintf(stderr,
            L"Usage: EffectGraphCompile <effect.json> [--header <file.h>] [--symbol <name>]\n"
            L"                          [--out <file.bin>] [--gpu-bound <name>[,<name>...]]\n"
            L"  --header     write the blob as a C++ byte array\n"
            L"  --symbol     the array's name (default: c<FileName>Effect)\n"
            L"  --out        write the blob as raw bytes\n"
            L"  --gpu-bound  parameters the program binds to an analysis field; each\n"
            L"               combination gets a GPU-served build (at most %zu)\n",
            Effects::cMaxBlobGpuParameters);
    }

    bool ReadText(const fs::path& path, std::wstring& text)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
            static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF)
            bytes.erase(0, 3);
        text = Utf8ToWide(bytes);
        return !text.empty();
    }

    std::vector<std::wstring> SplitList(const std::wstring& list)
    {
        std::vector<std::wstring> items;
        size_t start = 0;
        while (start <= list.size())
        {
            const size_t end = list.find(L',', start);
            auto item = list.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            if (!item.empty()) items.push_back(std::move(item));
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
        return items;
    }

    // c + the file's stem in PascalCase + Effect: hdr_screenshot_tonemap.json
    // gives cHdrScreenshotTonemapEffect.
    std::string DefaultSymbol(const fs::path& input)
    {
        std::string symbol = "c";
        bool wordStart = true;
        for (wchar_t ch : input.stem().wstring())
        {
            const bool letter = (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z');
            const bool digit = ch >= L'0' && ch <= L'9';
            if (!letter && !digit) { wordStart = true; continue; }
            char c = static_cast<char>(ch);
            if (wordStart && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            symbol.push_back(c);
            wordStart = false;
        }
        return symbol + "Effect";
    }

    bool WriteFile(const fs::path& path, const void* data, size_t size)
    {
        std::ofstream out(path, std::ios::binary);
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        return static_cast<bool>(out);
    }
}

int wmain(int argc, wchar_t* argv[])
{
    fs::path input, header, raw;
    std::string symbol;
    Effects::EffectBlobOptions options;
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring arg = argv[i];
        auto value = [&]() -> const wchar_t* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (arg == L"--header") { if (auto v = value()) header = v; else { PrintUsage(); return 1; } }
        else if (arg == L"--out") { if (auto v = value()) raw = v; else { PrintUsage(); return 1; } }
        else if (arg == L"--symbol") { if (auto v = value()) symbol = WideToUtf8(v); else { PrintUsage(); return 1; } }
        else if (arg == L"--gpu-bound") { if (auto v = value()) options.gpuBoundParameters = SplitList(v); else { PrintUsage(); return 1; } }
        else if (!arg.starts_with(L"--") && input.empty()) input = arg;
        else { PrintUsage(); return 1; }
    }
    if (input.empty() || (header.empty() && raw.empty())) { PrintUsage(); return 1; }
    if (symbol.empty()) symbol = DefaultSymbol(input);

    winrt::init_apartment();
    // Joins the engine's worker threads on every return path.
    struct EngineServicesScope { ~EngineServicesScope() { Engine::ShutdownEngineServices(); } } engineServices;

    std::wstring json;
    if (!ReadText(input, json))
    {
        std::fwprintf(stderr, L"EffectGraphCompile: cannot read %ls\n", input.c_str());
        return 2;
    }
    const auto blob = Effects::BuildEffectBlob(json, input.wstring(), options);
    if (blob.builds.empty())
    {
        std::fwprintf(stderr, L"%ls: error: %ls\n", input.c_str(), blob.error.c_str());
        return 3;
    }
    if (!raw.empty())
    {
        const auto file = Effects::EffectBlobFile(blob);
        if (!WriteFile(raw, file.data(), file.size()))
        {
            std::fwprintf(stderr, L"EffectGraphCompile: cannot write %ls\n", raw.c_str());
            return 4;
        }
    }
    if (!header.empty())
    {
        const auto text = Effects::GenerateEffectHeader(blob, symbol, input.filename().wstring());
        if (!WriteFile(header, text.data(), text.size()))
        {
            std::fwprintf(stderr, L"EffectGraphCompile: cannot write %ls\n", header.c_str());
            return 4;
        }
    }
    std::wprintf(L"EffectGraphCompile: %ls -> %zu effect(s), %zu build(s), %.1f MB of shader code in %.2f MB\n",
                 input.filename().c_str(), blob.effects.size(), blob.builds.size(),
                 blob.BytecodeBytes() / 1e6, blob.StoredBytes() / 1e6);
    // The macros a generated header selects with.
    for (const auto& effect : blob.effects)
    {
        std::wprintf(L"  %ls\n    %hs\n", effect.name.c_str(), Effects::EffectMacro(effect.name).c_str());
        for (const auto& [option, labels] : effect.options)
        {
            std::wprintf(L"    %hs\n", Effects::OptionMacro(effect.name, option, L"ALL").c_str());
            for (const auto& label : labels)
                std::wprintf(L"    %hs\n", Effects::OptionMacro(effect.name, option, label).c_str());
        }
        for (const auto& layout : effect.gpuLayouts)
            std::wprintf(L"    %hs\n", Effects::LayoutMacro(effect.name, layout).c_str());
    }
    return 0;
}
