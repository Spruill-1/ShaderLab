#include "pch.h"
#include "NodeLayout.h"
#include "Graph/GraphModel.h"
#include "Graph/ParameterVisibility.h"

namespace ShaderLab::Controls
{
    namespace
    {
        void Offset(D2D1_RECT_F& rect, float dx, float dy)
        {
            rect.left += dx; rect.right += dx;
            rect.top += dy;  rect.bottom += dy;
        }
    }

    const std::vector<D2D1_POINT_2F>& NodeVisual::Pins(PinKind kind) const
    {
        switch (kind)
        {
        case PinKind::ImageIn:  return imageInputPins;
        case PinKind::ImageOut: return imageOutputPins;
        case PinKind::DataIn:   return dataInputPins;
        default:                return dataOutputPins;
        }
    }

    std::optional<D2D1_POINT_2F> NodeVisual::Pin(PinKind kind, uint32_t index) const
    {
        const auto& pins = Pins(kind);
        if (index >= pins.size()) return std::nullopt;
        return pins[index];
    }

    std::optional<D2D1_POINT_2F> NodeVisual::DataPin(PinKind kind, const std::wstring& name) const
    {
        const auto& names = kind == PinKind::DataIn ? dataInputNames : dataOutputNames;
        const auto& pins = Pins(kind);
        for (size_t i = 0; i < names.size() && i < pins.size(); ++i)
            if (names[i] == name) return pins[i];
        return std::nullopt;
    }

    std::wstring NodeVisual::PinName(PinKind kind, uint32_t index) const
    {
        if (!IsDataPin(kind)) return {};
        const auto& names = kind == PinKind::DataIn ? dataInputNames : dataOutputNames;
        return index < names.size() ? names[index] : std::wstring{};
    }

    void NodeVisual::Translate(float dx, float dy)
    {
        Offset(bounds, dx, dy);
        for (auto* pins : { &imageInputPins, &imageOutputPins, &dataInputPins, &dataOutputPins })
            for (auto& p : *pins) { p.x += dx; p.y += dy; }
        if (isParameterNode) Offset(sliderRect, dx, dy);
        if (isClockNode) Offset(playButtonRect, dx, dy);
    }

    NodeVisual ComputeNodeVisual(const Graph::EffectNode& node)
    {
        NodeVisual v;
        v.nodeId = node.id;
        v.isParameterNode = node.IsParameterNode();
        v.isClockNode = node.IsClock();
        v.isDataOnly = node.IsDataOnlyEffect();

        // Data inputs: the bindable properties currently shown.
        for (const auto& [key, value] : node.properties)
            if (Graph::IsParameterShown(node, key) && Graph::GraphModel::IsBindablePropertyType(value))
                v.dataInputNames.push_back(key);

        // Data outputs: the declared analysis fields, or for a node without a
        // definition (a video source), the fields it currently publishes.
        if (node.customEffect.has_value() &&
            node.customEffect->analysisOutputType == Graph::AnalysisOutputType::Typed)
        {
            for (const auto& field : node.customEffect->analysisFields)
                v.dataOutputNames.push_back(field.name);
        }
        else if (node.analysisOutput.type == Graph::AnalysisOutputType::Typed)
        {
            for (const auto& field : node.analysisOutput.fields)
                v.dataOutputNames.push_back(field.name);
        }

        const auto dataInputCount = static_cast<uint32_t>(v.dataInputNames.size());
        const auto dataOutputCount = static_cast<uint32_t>(v.dataOutputNames.size());
        const auto maxImagePins = static_cast<uint32_t>((std::max)(node.inputPins.size(), node.outputPins.size()));

        float imageBodyHeight = (std::max)(maxImagePins * v.pinSpacing + 10.0f, 40.0f);
        float dataBodyHeight = 0.0f;
        if (dataInputCount > 0 || dataOutputCount > 0)
            dataBodyHeight = 8.0f + (std::max)(dataInputCount, dataOutputCount) * v.pinSpacing;

        // Parameter nodes have no image pins but an inline slider (and a play
        // button on a clock); a data-only shader without image inputs has no
        // image section either.
        float sliderHeight = 0.0f;
        if (v.isParameterNode)
        {
            imageBodyHeight = 4.0f;
            sliderHeight = v.isClockNode ? 48.0f : 28.0f;
        }
        if (v.isDataOnly && node.inputPins.empty())
            imageBodyHeight = 4.0f;

        const float x = node.position.x;
        const float y = node.position.y;
        v.bounds = { x, y, x + cNodeWidth, y + v.headerHeight + imageBodyHeight + sliderHeight + dataBodyHeight };

        const float pinStartY = y + v.headerHeight + 15.0f;
        for (uint32_t i = 0; i < node.inputPins.size(); ++i)
            v.imageInputPins.push_back({ x, pinStartY + i * v.pinSpacing });
        for (uint32_t i = 0; i < node.outputPins.size(); ++i)
            v.imageOutputPins.push_back({ x + cNodeWidth, pinStartY + i * v.pinSpacing });

        if (v.isParameterNode)
        {
            const float sliderY = y + v.headerHeight + imageBodyHeight;
            if (v.isClockNode)
            {
                // Play button on the left, seek slider beside it.
                v.playButtonRect = { x + 12.0f, sliderY + 2.0f, x + 32.0f, sliderY + 18.0f };
                v.sliderRect = { x + 36.0f, sliderY + 2.0f, x + cNodeWidth - 12.0f, sliderY + 18.0f };
            }
            else
            {
                v.sliderRect = { x + 12.0f, sliderY + 4.0f, x + cNodeWidth - 12.0f, sliderY + sliderHeight - 4.0f };
            }
        }

        const float dataStartY = y + v.headerHeight + imageBodyHeight + sliderHeight + 4.0f;
        for (uint32_t i = 0; i < dataInputCount; ++i)
            v.dataInputPins.push_back({ x, dataStartY + i * v.pinSpacing });
        for (uint32_t i = 0; i < dataOutputCount; ++i)
            v.dataOutputPins.push_back({ x + cNodeWidth, dataStartY + i * v.pinSpacing });

        return v;
    }
}
