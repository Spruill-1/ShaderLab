#pragma once

#include "pch.h"
#include "../Graph/EffectNode.h"

// Canvas geometry of one node: its box, pins and inline controls. A pure
// function of the node, used by the canvas paint and hit-tests.
namespace ShaderLab::Controls
{
    constexpr float cNodeWidth = 180.0f;
    constexpr float cPinRadius = 6.0f;
    constexpr float cNodeCornerRadius = 6.0f;

    enum class PinKind : uint8_t { ImageIn, ImageOut, DataIn, DataOut };

    constexpr bool IsDataPin(PinKind kind) { return kind == PinKind::DataIn || kind == PinKind::DataOut; }
    constexpr bool IsOutputPin(PinKind kind) { return kind == PinKind::ImageOut || kind == PinKind::DataOut; }

    // A pin on a node. A data pin carries its property (input) or analysis
    // field (output) name, which is what a binding records.
    struct PinRef
    {
        uint32_t nodeId{ 0 };
        PinKind kind{ PinKind::ImageOut };
        uint32_t index{ 0 };
        std::wstring name;
    };

    struct NodeVisual
    {
        uint32_t nodeId{ 0 };
        D2D1_RECT_F bounds{};
        float headerHeight{ 28.0f };
        float pinSpacing{ 20.0f };

        // Pin centres, by kind.
        std::vector<D2D1_POINT_2F> imageInputPins;
        std::vector<D2D1_POINT_2F> imageOutputPins;
        std::vector<D2D1_POINT_2F> dataInputPins;
        std::vector<D2D1_POINT_2F> dataOutputPins;

        // Data pin names: the bindable property keys shown on the left and
        // the analysis field names on the right, in pin order.
        std::vector<std::wstring> dataInputNames;
        std::vector<std::wstring> dataOutputNames;

        bool isParameterNode{ false };
        bool isClockNode{ false };
        bool isDataOnly{ false };
        D2D1_RECT_F sliderRect{};       // parameter nodes
        D2D1_RECT_F playButtonRect{};   // clock nodes

        const std::vector<D2D1_POINT_2F>& Pins(PinKind kind) const;
        std::optional<D2D1_POINT_2F> Pin(PinKind kind, uint32_t index) const;
        // The data pin called `name`, if the node shows it.
        std::optional<D2D1_POINT_2F> DataPin(PinKind kind, const std::wstring& name) const;
        // The property or field name of a data pin; empty for image pins.
        std::wstring PinName(PinKind kind, uint32_t index) const;

        // Moves the box, every pin and the inline controls.
        void Translate(float dx, float dy);
    };

    NodeVisual ComputeNodeVisual(const Graph::EffectNode& node);

    // An edge's shape: a cubic bezier leaving and entering its pins
    // horizontally. Paint and hit-testing both use it.
    struct EdgeCurve
    {
        D2D1_POINT_2F start{}, cp1{}, cp2{}, end{};

        static EdgeCurve Between(D2D1_POINT_2F from, D2D1_POINT_2F to)
        {
            const float dx = (to.x - from.x) * 0.4f;
            return { from, { from.x + dx, from.y }, { to.x - dx, to.y }, to };
        }
    };

    // A drawn property binding: one per distinct (source node, field) a
    // property reads.
    struct DataEdge
    {
        uint32_t sourceNodeId{ 0 };
        std::wstring field;
        uint32_t destNodeId{ 0 };
        std::wstring property;
        D2D1_POINT_2F start{};
        D2D1_POINT_2F end{};
    };

    // Calls fn(const DataEdge&) for every binding whose two data pins are laid out.
    template <class Fn>
    void ForEachDataEdge(const std::vector<Graph::EffectNode>& nodes,
                         const std::unordered_map<uint32_t, NodeVisual>& visuals, Fn&& fn)
    {
        for (const auto& node : nodes)
        {
            auto dest = visuals.find(node.id);
            if (dest == visuals.end()) continue;
            for (const auto& [property, binding] : node.propertyBindings)
            {
                const auto end = dest->second.DataPin(PinKind::DataIn, property);
                if (!end) continue;

                std::vector<std::pair<uint32_t, std::wstring>> sources;
                if (binding.wholeArray)
                    sources.emplace_back(binding.wholeArraySourceNodeId, binding.wholeArraySourceFieldName);
                for (const auto& source : binding.sources)
                {
                    if (!source.has_value()) continue;
                    std::pair<uint32_t, std::wstring> key{ source->sourceNodeId, source->sourceFieldName };
                    if (std::find(sources.begin(), sources.end(), key) == sources.end())
                        sources.push_back(std::move(key));
                }

                for (auto& [sourceId, field] : sources)
                {
                    auto src = visuals.find(sourceId);
                    if (src == visuals.end()) continue;
                    if (const auto start = src->second.DataPin(PinKind::DataOut, field))
                        fn(DataEdge{ sourceId, std::move(field), node.id, property, *start, *end });
                }
            }
        }
    }
}
