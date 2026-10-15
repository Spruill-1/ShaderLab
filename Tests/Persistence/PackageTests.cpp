#include "pch_engine.h"
#include "../TestSupport.h"
#include "Graph/GraphModel.h"
#include "Rendering/EffectGraphFile.h"
#include "Rendering/GraphEvaluator.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// .effectgraph packages: save, progress, failure and untrusted input.
namespace
{
    using namespace ShaderLab::Tests;

    // .effectgraph packages: saving over an existing file reuses media that
    // did not change (same name, size, CRC-32), in place when it can.
    SL_TEST_CASE(TestEffectGraphPackageSave, Tag::Fast)
    {
        printf("\n=== .effectgraph save: unchanged media is reused ===\n");
        namespace fs = std::filesystem;
        using ShaderLab::Rendering::EffectGraphFile;
        const ScopedTempDir tempDir(L"ShaderLabPackageTest");
        const fs::path dir = tempDir.Path();
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        const fs::path pkg = dir / L"graph.effectgraph";

        auto writeFile = [](const fs::path& path, const std::vector<uint8_t>& bytes) {
            std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        };
        auto readFile = [](const fs::path& path) {
            std::ifstream file(path, std::ios::binary);
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        };
        // a.bin: 64 MB of xorshift noise (incompressible, like video); b.bin: 1 MB of zeros.
        std::vector<uint8_t> noiseBytes(64u << 20), zeroBytes(1u << 20, 0);
        uint32_t rngState = 0x9E3779B9u;
        for (auto& value : noiseBytes)
        {
            rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
            value = static_cast<uint8_t>(rngState);
        }
        writeFile(dir / L"a.bin", noiseBytes);
        writeFile(dir / L"b.bin", zeroBytes);
        std::vector<EffectGraphFile::MediaEntry> media = {
            { L"media/a.bin", (dir / L"a.bin").wstring() }, { L"media/b.bin", (dir / L"b.bin").wstring() } };

        // Load the package and compare the graph and both media files.
        auto loadsBack = [&](const std::wstring& json, const std::vector<uint8_t>& expectedA) {
            auto loaded = EffectGraphFile::Load(pkg.wstring(), (dir / L"extract").wstring());
            if (!loaded || loaded->graphJson != json) return false;
            auto entryA = loaded->mediaMap.find(L"media://a.bin"), entryB = loaded->mediaMap.find(L"media://b.bin");
            return entryA != loaded->mediaMap.end() && entryB != loaded->mediaMap.end() &&
                   readFile(entryA->second) == expectedA && readFile(entryB->second) == zeroBytes;
        };

        EffectGraphFile::SaveStats s1, s2, s3, s4, s5;
        const auto firstSaveStart = std::chrono::steady_clock::now();
        const bool ok1 = EffectGraphFile::Save(pkg.wstring(), L"{\"v\":1}", media, {}, &s1);
        const auto firstSaveEnd = std::chrono::steady_clock::now();
        TEST("Package_FirstSaveWritesAllMedia", ok1 && s1.mediaWritten == 2 && s1.mediaUnchanged == 0 && !s1.inPlace);
        TEST("Package_FirstSaveLoadsBack", loadsBack(L"{\"v\":1}", noiseBytes));

        // Re-save with only the graph changed.
        const auto before = readFile(pkg);
        const auto resaveStart = std::chrono::steady_clock::now();
        const bool ok2 = EffectGraphFile::Save(pkg.wstring(), L"{\"v\":2, \"longer\": true}", media, {}, &s2);
        const auto resaveEnd = std::chrono::steady_clock::now();
        const auto after = readFile(pkg);
        const size_t prefix = noiseBytes.size();   // a.bin is stored first, so its bytes lie inside this prefix
        TEST("Package_ResaveReusesUnchangedMediaInPlace", ok2 && s2.mediaUnchanged == 2 && s2.mediaWritten == 0 && s2.inPlace);
        TEST("Package_ResaveWritesOnlyTheTail", s2.bytesWritten < 64 * 1024);
        TEST("Package_ResaveLeavesMediaBytesUntouched",
             before.size() > prefix && after.size() > prefix && std::equal(before.begin(), before.begin() + prefix, after.begin()));
        TEST("Package_ResaveLoadsBack", loadsBack(L"{\"v\":2, \"longer\": true}", noiseBytes));
        printf("  [info] 65 MB of media: first save %.0f ms, re-save %.0f ms (%llu bytes written)\n",
               std::chrono::duration<double, std::milli>(firstSaveEnd - firstSaveStart).count(),
               std::chrono::duration<double, std::milli>(resaveEnd - resaveStart).count(),
               static_cast<unsigned long long>(s2.bytesWritten));

        // Same size, one byte different: only the CRC can tell.
        auto editedNoise = noiseBytes; editedNoise[editedNoise.size() / 2] ^= 0x5A;
        writeFile(dir / L"a.bin", editedNoise);
        const bool ok3 = EffectGraphFile::Save(pkg.wstring(), L"{\"v\":3}", media, {}, &s3);
        TEST("Package_ChangedMediaCaughtByCrc", ok3 && s3.mediaWritten == 1 && s3.mediaUnchanged == 1 && !s3.inPlace);
        TEST("Package_ChangedMediaLoadsBack", loadsBack(L"{\"v\":3}", editedNoise));

        // Reordered media: unchanged, but no longer an in-order prefix, so the
        // entries are copied into a new file.
        std::vector<EffectGraphFile::MediaEntry> swapped = { media[1], media[0] };
        const bool ok4 = EffectGraphFile::Save(pkg.wstring(), L"{\"v\":4}", swapped, {}, &s4);
        TEST("Package_ReorderedMediaCopied", ok4 && s4.mediaUnchanged == 2 && s4.mediaWritten == 0 && !s4.inPlace);
        TEST("Package_ReorderedMediaLoadsBack", loadsBack(L"{\"v\":4}", editedNoise));

        // A file at the path that is not a package is simply replaced.
        writeFile(pkg, std::vector<uint8_t>{ 'n', 'o', 't', ' ', 'a', ' ', 'z', 'i', 'p' });
        const bool ok5 = EffectGraphFile::Save(pkg.wstring(), L"{\"v\":5}", media, {}, &s5);
        TEST("Package_NonPackageAtPathReplaced", ok5 && s5.mediaWritten == 2 && loadsBack(L"{\"v\":5}", editedNoise));
        TEST("Package_NoTempFileLeftBehind", !fs::exists(pkg.wstring() + L".saving"));

        // Opt-in timing of a real package: SHADERLAB_PACKAGE_BENCH=<path>.
        // Prints timings only; asserts nothing.
        if (char bench[MAX_PATH]{}; ::GetEnvironmentVariableA("SHADERLAB_PACKAGE_BENCH", bench, MAX_PATH) > 0 && *bench)
        {
            const fs::path src(bench), copy = dir / L"bench.effectgraph";
            fs::copy_file(src, copy, fs::copy_options::overwrite_existing, ec);
            auto loaded = ec ? std::nullopt : EffectGraphFile::Load(copy.wstring(), (dir / L"bench_extract").wstring());
            if (loaded)
            {
                std::vector<EffectGraphFile::MediaEntry> benchMedia;
                for (const auto& [token, extracted] : loaded->mediaMap)
                    benchMedia.push_back({ L"media/" + token.substr(8), extracted });   // strip "media://"
                // Save 0 goes to a new path (nothing to reuse), save 1 over the
                // copy as found, save 2 is the everyday re-save.
                for (int i = 0; i < 3; ++i)
                {
                    EffectGraphFile::SaveStats benchStats;
                    const fs::path target = (i == 0) ? dir / L"bench_fresh.effectgraph" : copy;
                    const auto saveStart = std::chrono::steady_clock::now();
                    const bool saved = EffectGraphFile::Save(target.wstring(), loaded->graphJson, benchMedia, {}, &benchStats);
                    printf("  [info] bench save %d: %s in %.0f ms -- media unchanged %u, written %u, in place %s, %.1f MB written\n",
                           i, saved ? "ok" : "FAILED",
                           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - saveStart).count(),
                           benchStats.mediaUnchanged, benchStats.mediaWritten, benchStats.inPlace ? "yes" : "no",
                           benchStats.bytesWritten / 1048576.0);
                }
            }
            else printf("  [info] bench: could not copy or load %s\n", bench);
        }

        fs::remove_all(dir, ec);   // best-effort cleanup
    }

    // .effectgraph files are shared, so Load treats them as untrusted: a media
    // entry may not name a path outside the extraction directory, and a
    // deflated entry may not inflate past its declared size.
    SL_TEST_CASE(TestEffectGraphPackageUntrustedInput, Tag::Fast)
    {
        printf("\n=== .effectgraph load: untrusted archive input ===\n");
        namespace fs = std::filesystem;
        using ShaderLab::Rendering::EffectGraphFile;
        const ScopedTempDir tempDir(L"ShaderLabUntrustedPackageTest");
        const fs::path dir = tempDir.Path();
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        const fs::path extractRoot = dir / L"extract";

        // One ZIP local entry: header, name, payload. Load walks local headers only.
        auto appendEntry = [](std::vector<uint8_t>& zip, const std::string& name, uint16_t method,
                              const std::vector<uint8_t>& payload, uint32_t uncompressedSize) {
            auto put16 = [&](uint16_t v) { zip.push_back(static_cast<uint8_t>(v)); zip.push_back(static_cast<uint8_t>(v >> 8)); };
            auto put32 = [&](uint32_t v) { put16(static_cast<uint16_t>(v)); put16(static_cast<uint16_t>(v >> 16)); };
            put32(0x04034b50); put16(20); put16(1 << 11); put16(method); put16(0); put16(0);
            put32(0); put32(static_cast<uint32_t>(payload.size())); put32(uncompressedSize);
            put16(static_cast<uint16_t>(name.size())); put16(0);
            zip.insert(zip.end(), name.begin(), name.end());
            zip.insert(zip.end(), payload.begin(), payload.end());
        };
        auto storedEntry = [&](std::vector<uint8_t>& zip, const std::string& name, const std::string& text) {
            appendEntry(zip, name, 0, std::vector<uint8_t>(text.begin(), text.end()), static_cast<uint32_t>(text.size()));
        };
        // Raw DEFLATE holding one stored block, so the test needs no compressor.
        auto deflateStoredBlock = [](const std::vector<uint8_t>& data) {
            const uint16_t length = static_cast<uint16_t>(data.size());
            std::vector<uint8_t> out = { 0x01, static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8),
                static_cast<uint8_t>(~length), static_cast<uint8_t>(~length >> 8) };
            out.insert(out.end(), data.begin(), data.end());
            return out;
        };
        auto load = [&](std::vector<uint8_t> zip) {
            zip.resize(zip.size() + 22, 0);   // room for an end-of-central-directory record
            const fs::path pkg = dir / L"test.effectgraph";
            std::ofstream(pkg, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(zip.data()), zip.size());
            return EffectGraphFile::Load(pkg.wstring(), extractRoot.wstring());
        };

        // Instrument: a well-formed archive loads and extracts.
        {
            std::vector<uint8_t> zip;
            storedEntry(zip, "graph.json", "{}");
            storedEntry(zip, "media/ok.txt", "fine");
            auto loaded = load(zip);
            TEST("UntrustedPackage_WellFormedLoads", loaded.has_value() && loaded->mediaMap.count(L"media://ok.txt") == 1);
        }

        for (const char* escapingName : { "media/../escape.txt", "media/..\\escape.txt", "media/sub/x.txt", "media/C:x.txt", "media/NUL.txt" })
        {
            std::vector<uint8_t> zip;
            storedEntry(zip, "graph.json", "{}");
            storedEntry(zip, escapingName, "payload");
            const bool refused = !load(zip).has_value();
            TEST((std::string("UntrustedPackage_Rejects ") + escapingName).c_str(), refused);
        }
        TEST("UntrustedPackage_NothingWrittenOutsideExtractDir",
             !fs::exists(extractRoot / L"escape.txt") && !fs::exists(dir / L"escape.txt"));

        std::vector<uint8_t> hundredBytes(100);
        for (size_t i = 0; i < hundredBytes.size(); ++i) hundredBytes[i] = static_cast<uint8_t>(i);
        {
            std::vector<uint8_t> zip;
            storedEntry(zip, "graph.json", "{}");
            appendEntry(zip, "media/inflated.bin", 8, deflateStoredBlock(hundredBytes), 100);
            auto loaded = load(zip);
            bool matches = false;
            if (loaded)
            {
                auto it = loaded->mediaMap.find(L"media://inflated.bin");
                if (it != loaded->mediaMap.end())
                {
                    std::ifstream file(it->second, std::ios::binary);
                    matches = std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()) == hundredBytes;
                }
            }
            TEST("UntrustedPackage_DeflatedEntryInflates", matches);
        }
        {
            std::vector<uint8_t> zip;
            storedEntry(zip, "graph.json", "{}");
            appendEntry(zip, "media/oversized.bin", 8, deflateStoredBlock(hundredBytes), 10);
            TEST("UntrustedPackage_InflatePastDeclaredSizeRefused", !load(zip).has_value());
        }
        {
            std::vector<uint8_t> zip;
            storedEntry(zip, "graph.json", "{}");
            appendEntry(zip, "media/bomb.bin", 8, deflateStoredBlock(hundredBytes), 0xFFFFFFF0u);
            TEST("UntrustedPackage_ImplausibleDeclaredSizeRefused", !load(zip).has_value());
        }
        fs::remove_all(dir, ec);
    }

    // .effectgraph save progress reaches the total, and a save that fails
    // part way (here an exception from the progress callback) releases its
    // file handles and temp file, so the next save succeeds.
    SL_TEST_CASE(TestEffectGraphPackageSaveProgressAndFailure, Tag::Fast)
    {
        printf("\n=== .effectgraph save: progress and failure cleanup ===\n");
        namespace fs = std::filesystem;
        using ShaderLab::Rendering::EffectGraphFile;
        const ScopedTempDir tempDir(L"ShaderLabPackageProgressTest");
        const fs::path dir = tempDir.Path();
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        const fs::path pkg = dir / L"graph.effectgraph";
        const std::wstring tempPath = pkg.wstring() + L".saving";
        for (const wchar_t* name : { L"a.bin", L"b.bin" })
        {
            std::vector<uint8_t> bytes(256u << 10);
            for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(i * 7 + name[0]);
            std::ofstream(dir / name, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }
        std::vector<EffectGraphFile::MediaEntry> media = {
            { L"media/a.bin", (dir / L"a.bin").wstring() }, { L"media/b.bin", (dir / L"b.bin").wstring() } };

        struct Step { uint32_t current, total; std::wstring message; };
        auto recordInto = [](std::vector<Step>& steps)
        {
            return [&steps](uint32_t current, uint32_t total, const std::wstring& message)
            {
                steps.push_back({ current, total, message });
                return true;
            };
        };
        // Steps never go backwards, the last one is the total, and every step
        // up to the total is reported (none skipped) when skipsAllowed is false.
        auto progressIsComplete = [](const std::vector<Step>& steps, bool skipsAllowed)
        {
            if (steps.empty() || steps.back().current != steps.back().total) return false;
            for (size_t i = 0; i < steps.size(); ++i)
            {
                if (steps[i].total != steps.back().total) return false;
                if (i > 0 && steps[i].current < steps[i - 1].current) return false;
                if (!skipsAllowed && steps[i].current != i + 1) return false;
            }
            return true;
        };

        // A fresh save writes each file; one step per check and per write.
        std::vector<Step> freshSteps;
        EffectGraphFile::SaveStats freshStats;
        const bool freshOk = EffectGraphFile::Save(pkg.wstring(), L"{\"v\":1}", media, recordInto(freshSteps), &freshStats);
        TEST("PackageProgress_FreshSaveReachesTotal", freshOk && progressIsComplete(freshSteps, false));
        const bool writeStepsAdvance = freshSteps.size() >= 4 &&
            freshSteps[2].message.starts_with(L"Writing") && freshSteps[3].message.starts_with(L"Writing") &&
            freshSteps[3].current == freshSteps[2].current + 1;
        TEST("PackageProgress_AdvancesPerWrittenFile", writeStepsAdvance);

        // An in-place re-save skips the write steps but still ends at the total.
        std::vector<Step> resaveSteps;
        EffectGraphFile::SaveStats resaveStats;
        const bool resaveOk = EffectGraphFile::Save(pkg.wstring(), L"{\"v\":2}", media, recordInto(resaveSteps), &resaveStats);
        TEST("PackageProgress_InPlaceSaveReachesTotal", resaveOk && resaveStats.inPlace && progressIsComplete(resaveSteps, true));

        // Throw from the callback at a chosen message, as an allocation failure
        // part way through a save would.
        struct SaveAborted {};
        auto throwAt = [](const wchar_t* prefix)
        {
            return [prefix](uint32_t, uint32_t, const std::wstring& message) -> bool
            {
                if (message.starts_with(prefix)) throw SaveAborted{};
                return true;
            };
        };
        auto saveThrows = [&](const std::wstring& json, const std::vector<EffectGraphFile::MediaEntry>& entries, const wchar_t* prefix)
        {
            try
            {
                EffectGraphFile::Save(pkg.wstring(), json, entries, throwAt(prefix));
            }
            catch (const SaveAborted&)
            {
                return true;
            }
            return false;
        };
        auto loadsBack = [&](const std::wstring& json)
        {
            auto loaded = EffectGraphFile::Load(pkg.wstring(), (dir / L"extract").wstring());
            return loaded && loaded->graphJson == json;
        };

        // A failure while writing a new file: the old file and temp file handles close.
        std::vector<EffectGraphFile::MediaEntry> reordered = { media[1], media[0] };
        const bool threwWriting = saveThrows(L"{\"v\":3}", reordered, L"Copying");
        TEST("PackageFailure_WritePhaseThrows", threwWriting);
        TEST("PackageFailure_TempFileRemoved", !fs::exists(tempPath));
        TEST("PackageFailure_OldFileIntact", loadsBack(L"{\"v\":2}"));
        EffectGraphFile::SaveStats afterWriteFailure;
        const bool savedAfterWriteFailure = EffectGraphFile::Save(pkg.wstring(), L"{\"v\":4}", reordered, {}, &afterWriteFailure);
        TEST("PackageFailure_NextSaveSucceeds", savedAfterWriteFailure && !afterWriteFailure.inPlace && loadsBack(L"{\"v\":4}"));

        // A failure during an in-place tail write: the writable handle closes.
        const bool threwInPlace = saveThrows(L"{\"v\":5}", reordered, L"graph.json");
        TEST("PackageFailure_InPlaceThrows", threwInPlace);
        EffectGraphFile::SaveStats afterInPlaceFailure;
        const bool savedAfterInPlaceFailure = EffectGraphFile::Save(pkg.wstring(), L"{\"v\":6}", reordered, {}, &afterInPlaceFailure);
        TEST("PackageFailure_NextInPlaceSaveSucceeds",
             savedAfterInPlaceFailure && afterInPlaceFailure.inPlace && loadsBack(L"{\"v\":6}"));

        fs::remove_all(dir, ec);   // best-effort cleanup
    }
}
