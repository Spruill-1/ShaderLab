#include "pch_engine.h"
#include "McpFrame.h"

namespace ShaderLab::Mcp
{
    namespace
    {
        void PutU32(std::vector<uint8_t>& out, uint32_t v)
        {
            out.push_back(static_cast<uint8_t>(v));
            out.push_back(static_cast<uint8_t>(v >> 8));
            out.push_back(static_cast<uint8_t>(v >> 16));
            out.push_back(static_cast<uint8_t>(v >> 24));
        }

        void PutU64(std::vector<uint8_t>& out, uint64_t v)
        {
            for (int i = 0; i < 8; ++i)
                out.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }

        uint32_t GetU32(const uint8_t* p)
        {
            return static_cast<uint32_t>(p[0])
                 | (static_cast<uint32_t>(p[1]) << 8)
                 | (static_cast<uint32_t>(p[2]) << 16)
                 | (static_cast<uint32_t>(p[3]) << 24);
        }

        uint64_t GetU64(const uint8_t* p)
        {
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i)
                v |= static_cast<uint64_t>(p[i]) << (8 * i);
            return v;
        }
    }

    bool EncodeFrame(const Frame& frame, std::vector<uint8_t>& out)
    {
        const uint64_t totalLen = kFrameHeaderBytes + frame.body.size();
        if (totalLen > kMaxFrameBytes)
            return false;

        out.reserve(out.size() + 4 + static_cast<size_t>(totalLen));
        PutU32(out, static_cast<uint32_t>(totalLen));
        PutU32(out, frame.header.channelId);
        PutU64(out, frame.header.seq);
        out.insert(out.end(), frame.body.begin(), frame.body.end());
        return true;
    }

    FrameDecodeResult TryDecodeFrame(std::span<const uint8_t> data)
    {
        FrameDecodeResult r;
        if (data.size() < 4)
            return r;   // NeedMoreData

        const uint32_t totalLen = GetU32(data.data());
        if (totalLen > kMaxFrameBytes)
        {
            r.status = FrameDecodeStatus::Oversize;
            return r;
        }
        if (totalLen < kFrameHeaderBytes)
        {
            r.status = FrameDecodeStatus::Malformed;
            return r;
        }
        if (data.size() < 4ull + totalLen)
            return r;   // NeedMoreData — payload incomplete

        const uint8_t* p = data.data() + 4;
        r.frame.header.channelId = GetU32(p);
        r.frame.header.seq = GetU64(p + 4);
        const size_t bodyLen = totalLen - kFrameHeaderBytes;
        r.frame.body.assign(p + kFrameHeaderBytes, p + kFrameHeaderBytes + bodyLen);
        r.consumed = 4ull + totalLen;
        r.status = FrameDecodeStatus::Ok;
        return r;
    }
}
