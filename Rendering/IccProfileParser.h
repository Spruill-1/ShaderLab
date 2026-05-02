#pragma once

#include "pch_engine.h"
#include "../EngineExport.h"
#include "DisplayProfile.h"

namespace ShaderLab::Rendering
{
    // Data extracted from an ICC profile's monitor tags.
    struct IccProfileData
    {
        std::wstring description;
        ChromaticityXY primaryRed, primaryGreen, primaryBlue;
        ChromaticityXY whitePoint;
        float luminanceNits{ 0.0f };  // 0 means not present in profile
        bool valid{ false };
    };

    // Parses ICC profile binary files to extract display colorimetry data.
    class SHADERLAB_API IccProfileParser
    {
    public:
        static std::optional<IccProfileData> LoadFromFile(const std::wstring& filePath);
    };

    // Converts parsed ICC data into a full DisplayProfile.
    SHADERLAB_API DisplayProfile DisplayProfileFromIcc(const IccProfileData& icc);
}
