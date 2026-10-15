#include "pch_engine.h"
#include "../TestSupport.h"

#include "../../Graph/GraphModel.h"
#include "../../Graph/GraphLayout.h"
#include "../../Graph/ValueFormat.h"
#include "Graph/GraphEdits.h"

#include <cstdio>

// Canvas auto-layout and the node classifications the canvas draws by.
namespace ShaderLab::Tests
{
    namespace
    {
        Graph::EffectNode MakeNode(const wchar_t* name, uint32_t inputs, uint32_t outputs)
        {
            Graph::EffectNode node;
            node.name = name;
            node.type = Graph::NodeType::BuiltInEffect;
            for (uint32_t i = 0; i < inputs; ++i)
                node.inputPins.push_back({ std::format(L"In{}", i), i });
            for (uint32_t i = 0; i < outputs; ++i)
                node.outputPins.push_back({ L"Out", i });
            return node;
        }

        bool Near(float a, float b) { return std::abs(a - b) < 1e-4f; }
    }

    SL_TEST_CASE(TestAutoLayout, Tag::Fast)
    {
        printf("\n=== Auto Layout ===\n");

        TEST("AutoLayout_EmptyGraph",
             Graph::ComputeAutoLayout(Graph::GraphModel{}, [](const Graph::EffectNode&) { return 10.0f; })
                 .value_or(std::unordered_map<uint32_t, winrt::Windows::Foundation::Numerics::float2>{ { 1, {} } })
                 .empty());

        // a1 -> b, a2 -> c, b -> d, c -> d, and d's Gain bound to p. a1
        // sits below a2, so c (fed by a2) goes above b.
        Graph::GraphModel graph;
        auto lower = MakeNode(L"a1", 0, 1);
        ShaderLab::Graph::SetNodePosition(lower, { 0.0f, 500.0f });
        auto upper = MakeNode(L"a2", 0, 1);
        ShaderLab::Graph::SetNodePosition(upper, { 0.0f, 100.0f });
        const uint32_t a1 = graph.AddNode(std::move(lower));
        const uint32_t a2 = graph.AddNode(std::move(upper));
        const uint32_t b = graph.AddNode(MakeNode(L"b", 1, 1));
        const uint32_t c = graph.AddNode(MakeNode(L"c", 1, 1));
        auto sink = MakeNode(L"d", 2, 1);
        ShaderLab::Graph::SetNodeProperty(sink, L"Gain", 1.0f);
        const uint32_t d = graph.AddNode(std::move(sink));
        auto parameter = MakeNode(L"p", 0, 0);
        parameter.analysisOutput.type = Graph::AnalysisOutputType::Typed;
        const uint32_t p = graph.AddNode(std::move(parameter));
        graph.Connect(a1, 0, b, 0);
        graph.Connect(a2, 0, c, 0);
        graph.Connect(b, 0, d, 0);
        graph.Connect(c, 0, d, 1);
        Graph::PropertyBinding binding;
        binding.sources.push_back(Graph::ComponentSource{ p, L"Value", 0, 0 });
        graph.FindNode(d)->propertyBindings[L"Gain"] = binding;

        // Every node is 100 tall but b, which is 40; rows are 30 apart.
        const auto height = [&](const Graph::EffectNode& node) { return node.id == b ? 40.0f : 100.0f; };
        const auto layout = Graph::ComputeAutoLayout(graph, height);
        TEST("AutoLayout_Placed", layout.has_value() && layout->size() == 6);
        if (!layout || layout->size() != 6) return;
        const auto& at = *layout;

        // Columns by depth: {a1, a2, p}, {b, c}, {d}.
        TEST("AutoLayout_FirstColumn",
             Near(at.at(a1).x, 60.0f) && Near(at.at(a2).x, 60.0f) && Near(at.at(p).x, 60.0f));
        TEST("AutoLayout_SecondColumn", Near(at.at(b).x, 310.0f) && Near(at.at(c).x, 310.0f));
        TEST("AutoLayout_BindingCountsAsDependency", Near(at.at(d).x, 560.0f));

        // The first column is the tallest (3 x 130), so it starts at 60.
        std::vector<float> firstColumnY{ at.at(a1).y, at.at(a2).y, at.at(p).y };
        std::sort(firstColumnY.begin(), firstColumnY.end());
        TEST("AutoLayout_FirstColumnStacked",
             Near(firstColumnY[0], 60.0f) && Near(firstColumnY[1], 190.0f) && Near(firstColumnY[2], 320.0f));
        // {b, c} is 200 tall, centred on 390, and ordered by upstream y.
        TEST("AutoLayout_OrderedByUpstreamY", Near(at.at(c).y, 155.0f) && Near(at.at(b).y, 285.0f));
        TEST("AutoLayout_LastColumnCentred", Near(at.at(d).y, 190.0f));

        // A dependency loop through a binding has no left-to-right order.
        Graph::PropertyBinding loop;
        loop.sources.push_back(Graph::ComponentSource{ d, L"Value", 0, 0 });
        auto* aNode = graph.FindNode(a1);
        ShaderLab::Graph::SetNodeProperty(*aNode, L"Gain", 1.0f);
        aNode->propertyBindings[L"Gain"] = loop;
        TEST("AutoLayout_CycleLeavesPositions", !Graph::ComputeAutoLayout(graph, height).has_value());
    }

    SL_TEST_CASE(TestNodeClassification, Tag::Fast)
    {
        printf("\n=== Node Classification ===\n");

        Graph::EffectNode parameter;
        parameter.type = Graph::NodeType::PixelShader;
        parameter.customEffect.emplace();
        parameter.customEffect->nodeKind = Graph::NodeKind::ConstantParameter;
        TEST("Classify_ParameterNode", parameter.IsParameterNode() && !parameter.IsDataOnlyEffect());

        Graph::EffectNode source = parameter;
        source.type = Graph::NodeType::Source;
        TEST("Classify_SourceIsNeverParameter", !source.IsParameterNode());

        Graph::EffectNode statistics;
        statistics.type = Graph::NodeType::ComputeShader;
        statistics.customEffect.emplace();
        statistics.customEffect->hlslSource = L"[numthreads(8,8,1)] void main() {}";
        TEST("Classify_DataOnlyShader", statistics.IsDataOnlyEffect() && !statistics.IsParameterNode());
        statistics.outputPins.push_back({ L"Output", 0 });
        TEST("Classify_ImageShaderIsNotDataOnly", !statistics.IsDataOnlyEffect());

        Graph::EffectNode labelled;
        labelled.customEffect.emplace();
        Graph::ParameterDefinition mode;
        mode.name = L"Mode";
        mode.enumLabels = { L"Off", L"On" };
        labelled.customEffect->parameters.push_back(mode);
        TEST("EnumLabel_Rounds", Graph::EnumLabelFor(labelled, L"Mode", 0.6f).value_or(L"") == L"On");
        TEST("EnumLabel_OutOfRange", !Graph::EnumLabelFor(labelled, L"Mode", 2.0f).has_value());
        TEST("EnumLabel_UnlabelledParameter", !Graph::EnumLabelFor(labelled, L"Gain", 0.0f).has_value());
    }
}
