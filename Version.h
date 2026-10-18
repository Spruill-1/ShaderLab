#pragma once

// ShaderLab's version. EffectGraph has its own (Engine/Api/EffectGraphVersion.h),
// and so does the saved-graph format (Graph::cGraphFormatVersion).
// Increment on each release:
//   MAJOR: breaking changes
//   MINOR: new features
//   PATCH: bug fixes

namespace ShaderLab
{
    constexpr uint32_t VersionMajor = 1;
    constexpr uint32_t VersionMinor = 9;
    constexpr uint32_t VersionPatch = 0;

    // Human-readable version string.
    constexpr const wchar_t* VersionString = L"1.9.0";
}
