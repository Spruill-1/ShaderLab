#include "pch_engine.h"
#include "SourceKind.h"
#include "EffectNode.h"

namespace ShaderLab::Graph
{
    namespace
    {
        bool HasTrueMarker(const EffectNode& node, const wchar_t* marker)
        {
            auto it = node.properties.find(marker);
            if (it == node.properties.end()) return false;
            const auto* value = std::get_if<bool>(&it->second);
            return value && *value;
        }
    }

    SourceKind ClassifySource(const EffectNode& node)
    {
        if (node.type != NodeType::Source) return SourceKind::None;
        if (HasTrueMarker(node, cDesktopDuplicationMarker)) return SourceKind::DesktopDuplication;
        if (HasTrueMarker(node, cGraphicsCaptureMarker))    return SourceKind::GraphicsCapture;
        if (HasTrueMarker(node, cVideoMarker))              return SourceKind::Video;
        if (node.mediaPath.has_value() && !node.effectClsid.has_value()) return SourceKind::Image;
        if (node.effectClsid.has_value() && IsEqualGUID(*node.effectClsid, CLSID_D2D1Flood))
            return SourceKind::Flood;
        return SourceKind::None;
    }
}
