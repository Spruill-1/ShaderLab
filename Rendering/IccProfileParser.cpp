#include "pch_engine.h"
#include "IccProfileParser.h"

namespace
{
    uint32_t ReadBE32(const uint8_t* data)
    {
        return (static_cast<uint32_t>(data[0]) << 24)
             | (static_cast<uint32_t>(data[1]) << 16)
             | (static_cast<uint32_t>(data[2]) << 8)
             |  static_cast<uint32_t>(data[3]);
    }

    float ReadS15Fixed16(const uint8_t* data)
    {
        uint32_t raw = ReadBE32(data);
        return static_cast<int32_t>(raw) / 65536.0f;
    }

    // Read an XYZ type tag: 4-byte type signature + 4-byte reserved, then 3 × s15Fixed16.
    bool ReadXYZTag(const uint8_t* data, uint32_t offset, uint32_t size,
                    float& X, float& Y, float& Z)
    {
        // XYZ type needs at least 20 bytes: 4 sig + 4 reserved + 3×4 values
        if (size < 20)
            return false;

        const uint8_t* tag = data + offset;
        X = ReadS15Fixed16(tag + 8);
        Y = ReadS15Fixed16(tag + 12);
        Z = ReadS15Fixed16(tag + 16);
        return true;
    }

    ShaderLab::Rendering::ChromaticityXY XyzToXy(float X, float Y, float Z)
    {
        float sum = X + Y + Z;
        if (sum < 1e-8f)
            return { 0.0f, 0.0f };
        return { X / sum, Y / sum };
    }

    // Signature constants for tags we care about.
    constexpr uint32_t kTagDesc = 0x64657363; // 'desc'
    constexpr uint32_t kTagRXYZ = 0x7258595A; // 'rXYZ'
    constexpr uint32_t kTagGXYZ = 0x6758595A; // 'gXYZ'
    constexpr uint32_t kTagBXYZ = 0x6258595A; // 'bXYZ'
    constexpr uint32_t kTagWtpt = 0x77747074; // 'wtpt'
    constexpr uint32_t kTagLumi = 0x6C756D69; // 'lumi'

    // Parse 'desc' tag — handles ICC v2 (textDescriptionType) and v4 (multiLocalizedUnicodeType).
    std::wstring ParseDescriptionTag(const uint8_t* data, uint32_t offset, uint32_t size)
    {
        if (size < 12)
            return {};

        const uint8_t* tag = data + offset;
        uint32_t typeSig = ReadBE32(tag);

        // ICC v2: 'desc' type — ASCII string at offset 8, with length at offset 8.
        if (typeSig == 0x64657363) // 'desc'
        {
            uint32_t strLen = ReadBE32(tag + 8);
            if (strLen == 0 || 12 + strLen > size)
                return {};
            // ASCII → wstring
            std::string ascii(reinterpret_cast<const char*>(tag + 12), strLen);
            // Trim null terminator if present.
            if (!ascii.empty() && ascii.back() == '\0')
                ascii.pop_back();
            return std::wstring(ascii.begin(), ascii.end());
        }

        // ICC v4: 'mluc' (multi-localized Unicode) type.
        if (typeSig == 0x6D6C7563) // 'mluc'
        {
            if (size < 16)
                return {};
            uint32_t recordCount = ReadBE32(tag + 8);
            // uint32_t recordSize  = ReadBE32(tag + 12); // should be 12
            if (recordCount == 0 || size < 16 + 12)
                return {};
            // First record: offset 16 = lang(2) + country(2) + strLength(4) + strOffset(4)
            uint32_t strLength = ReadBE32(tag + 20);
            uint32_t strOffset = ReadBE32(tag + 24);
            if (strOffset + strLength > size || strLength < 2)
                return {};
            // UTF-16 BE
            uint32_t charCount = strLength / 2;
            std::wstring result;
            result.reserve(charCount);
            for (uint32_t i = 0; i < charCount; ++i)
            {
                uint16_t ch = (static_cast<uint16_t>(tag[strOffset + i * 2]) << 8)
                            |  static_cast<uint16_t>(tag[strOffset + i * 2 + 1]);
                if (ch == 0) break;
                result.push_back(static_cast<wchar_t>(ch));
            }
            return result;
        }

        return {};
    }

    // Detect well-known gamut from chromaticity primaries.
    ShaderLab::Rendering::GamutId DetectGamut(
        const ShaderLab::Rendering::ChromaticityXY& r,
        const ShaderLab::Rendering::ChromaticityXY& g,
        const ShaderLab::Rendering::ChromaticityXY& b)
    {
        using namespace ShaderLab::Rendering;

        auto close = [](float a, float b, float tol = 0.02f) { return std::abs(a - b) < tol; };

        // sRGB / BT.709: R(0.64,0.33) G(0.30,0.60) B(0.15,0.06)
        if (close(r.x, 0.64f) && close(r.y, 0.33f) &&
            close(g.x, 0.30f) && close(g.y, 0.60f) &&
            close(b.x, 0.15f) && close(b.y, 0.06f))
            return GamutId::sRGB;

        // DCI-P3: R(0.680,0.320) G(0.265,0.690) B(0.150,0.060)
        if (close(r.x, 0.680f) && close(r.y, 0.320f) &&
            close(g.x, 0.265f) && close(g.y, 0.690f) &&
            close(b.x, 0.150f) && close(b.y, 0.060f))
            return GamutId::DCI_P3;

        // BT.2020: R(0.708,0.292) G(0.170,0.797) B(0.131,0.046)
        if (close(r.x, 0.708f) && close(r.y, 0.292f) &&
            close(g.x, 0.170f) && close(g.y, 0.797f) &&
            close(b.x, 0.131f) && close(b.y, 0.046f))
            return GamutId::BT2020;

        return GamutId::Custom;
    }
}

namespace ShaderLab::Rendering
{
    std::optional<IccProfileData> IccProfileParser::LoadFromFile(const std::wstring& filePath)
    {
        // Read entire file into memory.
        std::ifstream file(filePath, std::ios::binary | std::ios::ate);
        if (!file.is_open())
            return std::nullopt;

        auto fileSize = file.tellg();
        if (fileSize < 132)
            return std::nullopt;

        std::vector<uint8_t> buf(static_cast<size_t>(fileSize));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(buf.data()), fileSize);
        if (!file)
            return std::nullopt;

        const uint8_t* data = buf.data();
        const size_t dataSize = buf.size();

        // Validate profile size header.
        uint32_t profileSize = ReadBE32(data);
        if (profileSize > dataSize)
            return std::nullopt;

        // Tag count at offset 128.
        uint32_t tagCount = ReadBE32(data + 128);
        if (128 + 4 + tagCount * 12 > dataSize)
            return std::nullopt;

        IccProfileData result{};
        bool hasRed = false, hasGreen = false, hasBlue = false, hasWhite = false;

        for (uint32_t i = 0; i < tagCount; ++i)
        {
            const uint8_t* entry = data + 132 + i * 12;
            uint32_t sig    = ReadBE32(entry);
            uint32_t offset = ReadBE32(entry + 4);
            uint32_t size   = ReadBE32(entry + 8);

            if (offset + size > dataSize)
                continue;

            float X, Y, Z;

            switch (sig)
            {
            case kTagDesc:
                result.description = ParseDescriptionTag(data, offset, size);
                break;

            case kTagRXYZ:
                if (ReadXYZTag(data, offset, size, X, Y, Z))
                {
                    result.primaryRed = XyzToXy(X, Y, Z);
                    hasRed = true;
                }
                break;

            case kTagGXYZ:
                if (ReadXYZTag(data, offset, size, X, Y, Z))
                {
                    result.primaryGreen = XyzToXy(X, Y, Z);
                    hasGreen = true;
                }
                break;

            case kTagBXYZ:
                if (ReadXYZTag(data, offset, size, X, Y, Z))
                {
                    result.primaryBlue = XyzToXy(X, Y, Z);
                    hasBlue = true;
                }
                break;

            case kTagWtpt:
                if (ReadXYZTag(data, offset, size, X, Y, Z))
                {
                    result.whitePoint = XyzToXy(X, Y, Z);
                    hasWhite = true;
                }
                break;

            case kTagLumi:
                if (ReadXYZTag(data, offset, size, X, Y, Z))
                {
                    // Y component is luminance in cd/m².
                    result.luminanceNits = Y;
                }
                break;
            }
        }

        result.valid = hasRed && hasGreen && hasBlue && hasWhite;
        if (!result.valid)
            return std::nullopt;

        return result;
    }

    DisplayProfile DisplayProfileFromIcc(const IccProfileData& icc)
    {
        DisplayProfile p{};

        // Luminance: use ICC value or default to 270 nits.
        float lum = (icc.luminanceNits > 0.0f) ? icc.luminanceNits : 270.0f;

        p.caps.maxLuminanceNits = lum;
        p.caps.hdrEnabled = (lum > 400.0f);
        p.caps.bitsPerColor = p.caps.hdrEnabled ? 10u : 8u;
        p.caps.colorSpace = p.caps.hdrEnabled
            ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
            : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        p.caps.sdrWhiteLevelNits = 80.0f;
        p.caps.minLuminanceNits = p.caps.hdrEnabled ? 0.05f : 0.5f;
        p.caps.maxFullFrameLuminanceNits = (std::min)(lum, lum * 0.8f + 100.0f);

        p.primaryRed   = icc.primaryRed;
        p.primaryGreen = icc.primaryGreen;
        p.primaryBlue  = icc.primaryBlue;
        p.whitePoint   = icc.whitePoint;

        p.gamut = DetectGamut(icc.primaryRed, icc.primaryGreen, icc.primaryBlue);
        p.profileName = icc.description.empty() ? L"ICC Profile" : icc.description;
        p.isSimulated = true;

        return p;
    }
}
