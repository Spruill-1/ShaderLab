#pragma once

// Wire frame codec for the MCP broker pipe (stdio-migration Step 4).
//
// Layout, little-endian throughout:
//
//   [u32 totalLen][u32 channelId][u64 seq][body bytes ...]
//    ^ prefix      ^------- header -------^^--- payload --^
//
// totalLen counts everything AFTER the prefix (12 header bytes + body).
// The header travels IN THE CLEAR — the hub legitimately routes on
// {channelId, seq} — while the body is sealed by McpCrypto once a
// session is established. That split is deliberately explicit in the
// type (clear FrameHeader vs opaque body) so it cannot drift: nothing
// the hub needs is ever inside the sealed portion, and nothing sealed
// is ever readable by the hub.
//
// Frames are capped at 64 MB. A 4K inline capture is ~33 MB of base64;
// 8K would be ~130 MB, so producers must cap resolution server-side —
// the codec fails EXPLICITLY (Oversize) rather than desyncing the
// stream by half-consuming a giant length prefix.

#include "pch_engine.h"
#include "../../EngineExport.h"

#include <cstdint>
#include <vector>
#include <span>

namespace ShaderLab::Mcp
{
    inline constexpr size_t kFrameHeaderBytes = 12;              // channelId + seq
    inline constexpr size_t kMaxFrameBytes = 64ull * 1024 * 1024; // totalLen ceiling

    struct FrameHeader
    {
        uint32_t channelId{ 0 };
        uint64_t seq{ 0 };
    };

    struct Frame
    {
        FrameHeader          header;   // clear — the hub routes on this
        std::vector<uint8_t> body;     // sealed once a session key exists
    };

    // Appends the encoded frame to `out`. Returns false (appending
    // nothing) if the body would exceed the frame cap.
    SHADERLAB_API bool EncodeFrame(const Frame& frame, std::vector<uint8_t>& out);

    enum class FrameDecodeStatus : uint8_t
    {
        Ok,            // one complete frame decoded; `consumed` bytes eaten
        NeedMoreData,  // prefix or payload incomplete; nothing consumed
        Oversize,      // declared totalLen exceeds kMaxFrameBytes — the
                       //   stream is poisoned; the caller must drop the
                       //   connection, not try to resynchronize
        Malformed,     // declared totalLen too small to hold the header
    };

    struct FrameDecodeResult
    {
        FrameDecodeStatus status{ FrameDecodeStatus::NeedMoreData };
        Frame             frame;        // valid only when status == Ok
        size_t            consumed{ 0 }; // bytes eaten from the buffer front
    };

    // Attempts to decode one frame from the front of `data`. Never
    // consumes on any status other than Ok, so the caller's accumulation
    // buffer stays coherent across partial reads.
    SHADERLAB_API FrameDecodeResult TryDecodeFrame(std::span<const uint8_t> data);
}
