#include "pch_engine.h"
#include "../TestSupport.h"
#include "Effects/ShaderLabEffects.h"
#include "Graph/GraphEdits.h"
#include "Graph/GraphModel.h"
#include "Graph/GraphUiSnapshot.h"
#include "Graph/ParameterVisibility.h"

#include <atomic>
#include <cstdio>
#include <map>
#include <vector>

// ShaderLab's views of the graph: the Properties panel's visibility rule and
// the per-frame UI snapshot.
namespace ShaderLab::Tests
{
    SL_TEST_CASE(TestParameterVisibility, Tag::Fast)
    {
        printf("\n=== Parameter Visibility ===\n");
        std::map<std::wstring, Graph::PropertyValue> props{
            { L"Mode", 2u }, { L"Gain", 0.5f }, { L"On", true }, { L"Name", std::wstring(L"x") } };

        TEST("VisibleWhen_EmptyIsVisible", Graph::EvaluateVisibleWhen(L"", props));
        TEST("VisibleWhen_EqualHolds", Graph::EvaluateVisibleWhen(L"Mode == 2", props));
        TEST("VisibleWhen_EqualFails", !Graph::EvaluateVisibleWhen(L"Mode == 1", props));
        TEST("VisibleWhen_NotEqual", Graph::EvaluateVisibleWhen(L"Mode != 1", props));
        TEST("VisibleWhen_LessEqual", Graph::EvaluateVisibleWhen(L"Gain <= 0.5", props));
        TEST("VisibleWhen_Greater", !Graph::EvaluateVisibleWhen(L"Gain > 0.5", props));
        TEST("VisibleWhen_BoolAsNumber", Graph::EvaluateVisibleWhen(L"On == 1", props));
        TEST("VisibleWhen_NoSpaces", Graph::EvaluateVisibleWhen(L"Mode>=2", props));
        TEST("VisibleWhen_TrailingTextIgnored", Graph::EvaluateVisibleWhen(L"Mode == 2abc", props));
        TEST("VisibleWhen_BadNumberFailsOpen", Graph::EvaluateVisibleWhen(L"Mode == abc", props));
        TEST("VisibleWhen_OutOfRangeFailsOpen", Graph::EvaluateVisibleWhen(L"Mode == 1e99", props));
        TEST("VisibleWhen_NoOperatorFailsOpen", Graph::EvaluateVisibleWhen(L"Mode", props));
        TEST("VisibleWhen_MissingParamFailsOpen", Graph::EvaluateVisibleWhen(L"Other == 7", props));
        TEST("VisibleWhen_StringParamFailsOpen", Graph::EvaluateVisibleWhen(L"Name == 7", props));

        Graph::EffectNode plain;
        ShaderLab::Graph::SetNodeProperty(plain, L"Anything", 1.0f);
        TEST("Shown_NoCustomEffectShowsAll", Graph::IsParameterShown(plain, L"Anything"));

        Graph::EffectNode custom;
        custom.customEffect.emplace();
        Graph::ParameterDefinition mode;
        mode.name = L"Mode";
        Graph::ParameterDefinition gain;
        gain.name = L"Gain";
        gain.visibleWhen = L"Mode == 1";
        custom.customEffect->parameters = { mode, gain };
        custom.properties = { { L"Mode", 2u }, { L"Gain", 0.5f }, { L"Hidden", 1.0f } };
        TEST("Shown_DeclaredParam", Graph::IsParameterShown(custom, L"Mode"));
        TEST("Shown_UndeclaredHidden", !Graph::IsParameterShown(custom, L"Hidden"));
        TEST("Shown_ConditionFalseHidden", !Graph::IsParameterShown(custom, L"Gain"));
        ShaderLab::Graph::SetNodeProperty(custom, L"Mode", 1u);
        TEST("Shown_ConditionTrueShown", Graph::IsParameterShown(custom, L"Gain"));
    }

    SL_TEST_CASE(TestSnapshot, Tag::Fast)
    {
        printf("\n=== GraphUiSnapshot ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        ShaderLab::Graph::GraphModel g;
        auto srcDesc = registry.FindByName(L"Gamut Source");
        auto dstDesc = registry.FindByName(L"Gamut Source");
        auto srcId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*srcDesc));
        auto dstId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*dstDesc));
        g.Connect(srcId, 0, dstId, 0);

        // Stash a fake cachedOutput on a node to confirm the snapshot strips
        // it (raw ID2D1Image* must never escape into UI code).
        auto* live = g.FindNode(srcId);
        live->cachedOutput = reinterpret_cast<ID2D1Image*>(static_cast<uintptr_t>(0xDEADBEEFu));
        live->runtimeError = L"oops";
        ShaderLab::Graph::MarkNodeChanged(*live);

        auto snap = ShaderLab::Graph::BuildGraphUiSnapshot(
            g, /*previewId*/ dstId, /*graphGen*/ 7, /*frameGen*/ 99);
        TEST("BuildSnapshot returns non-null",        snap != nullptr);
        TEST("Snapshot graphGeneration",              snap->graphGeneration == 7);
        TEST("Snapshot frameGeneration",              snap->frameGeneration == 99);
        TEST("Snapshot previewNodeId",                snap->previewNodeId == dstId);
        TEST("Snapshot node count matches live",      snap->nodes.size() == 2);
        TEST("Snapshot edge count matches live",      snap->edges.size() == 1);
        TEST("Snapshot lookup by id works",           snap->FindNode(srcId) != nullptr);
        TEST("Snapshot lookup unknown id is null",    snap->FindNode(99999u) == nullptr);
        TEST("Snapshot strips cachedOutput",          snap->FindNode(srcId)->cachedOutput == nullptr);
        TEST("Snapshot preserves runtimeError",       snap->FindNode(srcId)->runtimeError == L"oops");
        TEST("Snapshot preserves dirty flag",         snap->FindNode(srcId)->dirty);
        TEST("Snapshot edge mirrors connection",
             snap->edges[0].sourceNodeId == srcId && snap->edges[0].destNodeId == dstId);
        TEST("Snapshot carries topological order",
             snap->topologicalOrder == std::vector<uint32_t>{ srcId, dstId });

        // Mutating the live graph after snapshot must not affect the snap.
        g.RemoveNode(srcId);
        TEST("Live mutation does not affect snapshot",
             snap->nodes.size() == 2 && snap->FindNode(srcId) != nullptr);

        // atomic<shared_ptr> publication round-trip.
        std::atomic<std::shared_ptr<const ShaderLab::Graph::GraphUiSnapshot>> latest{ snap };
        auto loaded = latest.load();
        TEST("atomic<shared_ptr> publication round-trips", loaded == snap);
    }
}
