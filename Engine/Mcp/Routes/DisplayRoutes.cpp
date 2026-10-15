#include "pch_engine.h"
#include "RouteSupport.h"
#include "../McpRouter.h"
#include "../McpSerialize.h"

#include "../../../Graph/GraphModel.h"
#include "../../../Rendering/DisplayMonitor.h"
#include "../../../Rendering/DisplayProfile.h"
#include "../../../Rendering/IccProfileParser.h"
#include "../../../Rendering/WorkingSpaceSync.h"
#include "../../../Version.h"

#include <filesystem>

// The display the pipeline targets: live capabilities, the simulated
// profile and the preset list.
namespace ShaderLab::Mcp::Routes
{
    namespace
    {
        void WriteGamut(JsonWriter& json, const char* key, const Rendering::DisplayProfile& profile)
        {
            auto pair = [&json](const char* name, const Rendering::ChromaticityXY& xy)
            {
                json.Key(name).BeginArray().Fixed(xy.x, 4).Fixed(xy.y, 4).EndArray();
            };
            json.Key(key).BeginObject();
            pair("red", profile.primaryRed);
            pair("green", profile.primaryGreen);
            pair("blue", profile.primaryBlue);
            json.EndObject();
        }

        // GET /display/info: app version, pipeline format, and the display's
        // capabilities with the active (possibly simulated) and live gamuts.
        // monitorStatus is present when the last display query failed, so the
        // caps are defaults rather than measurements.
        void RegisterDisplayInfo(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"GET", L"/display/info",
                [&sink](const std::wstring&, const std::wstring&, const std::string&) -> Response
                {
                    return sink.Dispatch([](EngineContext& ctx) -> Response
                    {
                        const auto profile = ctx.displayMonitor->ActiveProfile();
                        const auto live = ctx.displayMonitor->LiveProfile();
                        const auto caps = ctx.displayMonitor->CachedCapabilities();
                        const std::wstring formatName = ctx.getPipelineFormatName
                            ? ctx.getPipelineFormatName() : std::wstring(L"unknown");
                        const auto monitorError = ctx.displayMonitor->LastError();

                        JsonWriter json;
                        json.BeginObject()
                            .Key("appVersion").String(::ShaderLab::VersionString)
                            .Key("graphFormatVersion").Int(::ShaderLab::GraphFormatVersion)
                            .Key("pipeline").String(formatName)
                            .Key("display").BeginObject()
                                .Key("hdr").Bool(caps.hdrEnabled)
                                .Key("maxNits").Fixed(caps.maxLuminanceNits, 0)
                                .Key("sdrWhiteNits").Fixed(caps.sdrWhiteLevelNits, 0)
                                .Key("simulated").Bool(profile.isSimulated)
                                .Key("profileName").String(profile.profileName);
                        WriteGamut(json, "activeGamut", profile);
                        WriteGamut(json, "monitorGamut", live);
                        if (!monitorError.empty())
                            json.Key("monitorStatus").String(monitorError);
                        json.EndObject().EndObject();
                        return Json(200, json.Take());
                    });
                });
        }

        // GET /display/profiles: every preset with its index, the active and
        // live profiles, and the last loaded ICC profile when the host keeps one.
        void RegisterGetDisplayProfiles(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"GET", L"/display/profiles",
                [&sink](const std::wstring&, const std::wstring&, const std::string&) -> Response
                {
                    return sink.Dispatch([](EngineContext& ctx) -> Response
                    {
                        const auto presets = Rendering::AllPresets();
                        JsonWriter json;
                        json.BeginObject().Key("presets").BeginArray();
                        for (size_t i = 0; i < presets.size(); ++i)
                            json.BeginObject().Key("index").Int(i)
                                .Key("profile").Raw(DisplayProfileToJson(presets[i])).EndObject();
                        json.EndArray()
                            .Key("active").Raw(DisplayProfileToJson(ctx.displayMonitor->ActiveProfile()))
                            .Key("live").Raw(DisplayProfileToJson(ctx.displayMonitor->LiveProfile()))
                            .Key("isSimulated").Bool(ctx.displayMonitor->IsSimulated());
                        if (ctx.getLoadedIccProfile)
                            if (auto icc = ctx.getLoadedIccProfile())
                                json.Key("loadedIcc").Raw(DisplayProfileToJson(*icc));
                        json.EndObject();
                        return Json(200, json.Take());
                    });
                });
        }

        std::optional<Rendering::DisplayProfile> PresetByName(const std::wstring& name)
        {
            using namespace Rendering;
            if (name == L"PresetSrgbSdr"     || name == L"sRGB SDR (80 nits)")                         return PresetSrgbSdr();
            if (name == L"PresetSrgb270"     || name == L"sRGB SDR (270 nits, typical laptop)")        return PresetSrgb270();
            if (name == L"PresetAdobeRGB"    || name == L"Adobe RGB (1998)")                           return PresetAdobeRGB();
            if (name == L"PresetP3_600"      || name == L"DCI-P3 HDR (600 nits, MacBook Pro-class)")   return PresetP3_600();
            if (name == L"PresetP3_1000"     || name == L"DCI-P3 HDR (1000 nits, reference monitor)")  return PresetP3_1000();
            if (name == L"PresetBT2020_1000" || name == L"BT.2020 HDR (1000 nits, HDR TV)")            return PresetBT2020_1000();
            if (name == L"PresetBT2020_4000" || name == L"BT.2020 HDR (4000 nits, mastering)")         return PresetBT2020_4000();
            return std::nullopt;
        }

        // A custom profile from { name?, hdrEnabled?, sdrWhiteNits?, peakNits,
        // minNits?, maxFullFrameNits?, primaryRed/Green/Blue?, whitePoint?, gamut? }.
        std::optional<Rendering::DisplayProfile> CustomProfile(const WDJ::JsonObject& custom, std::string& error)
        {
            using namespace Rendering;
            DisplayProfile p{};
            p.isSimulated = true;
            std::string argError;
            p.profileName = ReadStringArg(custom, L"name", argError).value_or(L"Custom MCP profile");
            p.caps.hdrEnabled = ReadBoolArg(custom, L"hdrEnabled", argError).value_or(false);
            const auto peakNits = ReadNumberArg(custom, L"peakNits", argError);
            const auto sdrWhite = ReadNumberArg(custom, L"sdrWhiteNits", argError);
            const auto minNits = ReadNumberArg(custom, L"minNits", argError);
            const auto fullFrame = ReadNumberArg(custom, L"maxFullFrameNits", argError);
            const auto gamutName = ReadStringArg(custom, L"gamut", argError);
            if (!argError.empty())
            {
                error = "custom: " + argError;
                return std::nullopt;
            }
            if (!peakNits)
            {
                error = "custom profile requires 'peakNits'";
                return std::nullopt;
            }
            p.caps.sdrWhiteLevelNits = sdrWhite ? static_cast<float>(*sdrWhite) : (p.caps.hdrEnabled ? 203.0f : 80.0f);
            p.caps.maxLuminanceNits = static_cast<float>(*peakNits);
            p.caps.minLuminanceNits = minNits ? static_cast<float>(*minNits) : 0.5f;
            p.caps.maxFullFrameLuminanceNits = fullFrame ? static_cast<float>(*fullFrame) : p.caps.maxLuminanceNits;

            auto readChroma = [&](const wchar_t* key, ChromaticityXY& xy) -> bool
            {
                if (!custom.HasKey(key)) return true;
                const auto value = custom.GetNamedValue(key);
                if (value.ValueType() != WDJ::JsonValueType::Array) return false;
                const auto pair = value.GetArray();
                if (pair.Size() != 2 ||
                    pair.GetAt(0).ValueType() != WDJ::JsonValueType::Number ||
                    pair.GetAt(1).ValueType() != WDJ::JsonValueType::Number)
                    return false;
                xy.x = static_cast<float>(pair.GetNumberAt(0));
                xy.y = static_cast<float>(pair.GetNumberAt(1));
                return true;
            };
            if (!readChroma(L"primaryRed", p.primaryRed) || !readChroma(L"primaryGreen", p.primaryGreen) ||
                !readChroma(L"primaryBlue", p.primaryBlue) || !readChroma(L"whitePoint", p.whitePoint))
            {
                error = "primaries / whitePoint must be 2-element arrays";
                return std::nullopt;
            }

            // The gamut follows the primaries unless named, so a plain-sRGB
            // custom is not stamped wide-gamut below.
            p.gamut = DetectGamut(p.primaryRed, p.primaryGreen, p.primaryBlue);
            if (gamutName)
            {
                const auto& gn = *gamutName;
                if      (gn == L"sRGB")                                            p.gamut = GamutId::sRGB;
                else if (gn == L"DCI-P3" || gn == L"P3" || gn == L"DCI_P3")        p.gamut = GamutId::DCI_P3;
                else if (gn == L"BT.2020" || gn == L"BT2020" || gn == L"Rec2020") p.gamut = GamutId::BT2020;
                else                                                               p.gamut = GamutId::Custom;
            }
            // Keeps activeColorMode, the *Supported flags and bitsPerColor
            // consistent with hdrEnabled, as the Working Space node reads them.
            StampSimulatedColorMode(p);
            return p;
        }

        struct ProfileRequest
        {
            Rendering::DisplayProfile profile;
            bool fromIcc{ false };
        };

        // The profile a /display/profile body asks for: exactly one of
        // preset, presetIndex, iccPath or custom.
        std::optional<ProfileRequest> ReadProfileRequest(const WDJ::JsonObject& args, std::string& error)
        {
            const int given = int(args.HasKey(L"preset")) + int(args.HasKey(L"presetIndex")) +
                              int(args.HasKey(L"iccPath")) + int(args.HasKey(L"custom"));
            if (given != 1)
            {
                error = "Specify exactly one of: preset, presetIndex, iccPath, custom";
                return std::nullopt;
            }

            std::string argError;
            if (auto name = ReadStringArg(args, L"preset", argError))
            {
                auto preset = PresetByName(*name);
                if (!preset)
                {
                    error = "Unknown preset: " + WideToUtf8(*name);
                    return std::nullopt;
                }
                return ProfileRequest{ *preset };
            }
            if (auto index = ReadUintArg(args, L"presetIndex", argError))
            {
                const auto presets = Rendering::AllPresets();
                if (*index >= presets.size())
                {
                    error = std::format("presetIndex out of range (0-{})", presets.size() - 1);
                    return std::nullopt;
                }
                return ProfileRequest{ presets[*index] };
            }
            if (auto path = ReadStringArg(args, L"iccPath", argError))
            {
                std::error_code ec;
                if (!std::filesystem::exists(*path, ec))
                {
                    error = "ICC file not found: " + WideToUtf8(*path);
                    return std::nullopt;
                }
                auto parsed = Rendering::IccProfileParser::LoadFromFile(*path);
                if (!parsed.has_value() || !parsed->valid)
                {
                    error = "Failed to parse ICC profile: " + WideToUtf8(*path);
                    return std::nullopt;
                }
                return ProfileRequest{ Rendering::DisplayProfileFromIcc(*parsed), true };
            }
            if (auto custom = ReadObjectArg(args, L"custom", argError))
            {
                auto profile = CustomProfile(*custom, error);
                if (!profile) return std::nullopt;
                return ProfileRequest{ *profile };
            }
            error = argError;
            return std::nullopt;
        }

        // POST /display/profile: simulate a profile. Working Space nodes
        // follow it before OnDisplayProfileChanged fires.
        void RegisterSetDisplayProfile(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"POST", L"/display/profile",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    WDJ::JsonObject args{ nullptr };
                    if (!ParseObjectBody(body, args))
                        return Error(400, "Invalid JSON body");
                    std::string argError;
                    const auto request = ReadProfileRequest(args, argError);
                    if (!request)
                        return Error(400, argError);

                    return sink.Dispatch([&](EngineContext& ctx) -> Response
                    {
                        if (request->fromIcc && ctx.setLoadedIccProfile)
                            ctx.setLoadedIccProfile(request->profile);
                        ctx.displayMonitor->SetSimulatedProfile(request->profile);
                        Rendering::UpdateWorkingSpaceNodes(*ctx.graph, ctx.displayMonitor->ActiveProfile());
                        sink.OnDisplayProfileChanged();

                        const auto active = ctx.displayMonitor->ActiveProfile();
                        return Json(200, JsonWriter().BeginObject()
                            .Key("ok").Bool(true)
                            .Key("applied").String(active.profileName)
                            .Key("hdrEnabled").Bool(active.caps.hdrEnabled)
                            .Key("peakNits").Fixed(active.caps.maxLuminanceNits, 2)
                            .EndObject().Take());
                    });
                });
        }

        // POST /display/profile/clear: back to the live display.
        void RegisterClearDisplayProfile(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"POST", L"/display/profile/clear",
                [&sink](const std::wstring&, const std::wstring&, const std::string&) -> Response
                {
                    return sink.Dispatch([&sink](EngineContext& ctx) -> Response
                    {
                        ctx.displayMonitor->ClearSimulatedProfile();
                        Rendering::UpdateWorkingSpaceNodes(*ctx.graph, ctx.displayMonitor->ActiveProfile());
                        sink.OnDisplayProfileChanged();
                        return Json(200, R"({"ok":true,"isSimulated":false})");
                    });
                });
        }
    }

    void RegisterDisplayRoutes(McpRouter& router, IEngineCommandSink& sink)
    {
        RegisterDisplayInfo(router, sink);
        RegisterGetDisplayProfiles(router, sink);
        RegisterSetDisplayProfile(router, sink);
        RegisterClearDisplayProfile(router, sink);
    }
}
