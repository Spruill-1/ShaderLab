#pragma once

// ShaderLab's version. EffectGraph has its own (Engine/Api/EffectGraphVersion.h),
// and so does the saved-graph format (Graph::cGraphFormatVersion).
// Increment on each release:
//   MAJOR: breaking changes
//   MINOR: new features
//   PATCH: bug fixes

namespace ShaderLab
{
    constexpr uint32_t VersionMajor = 2;
    constexpr uint32_t VersionMinor = 0;
    constexpr uint32_t VersionPatch = 0;

    // Human-readable version string.
    constexpr const wchar_t* VersionString = L"2.0.0";
}
