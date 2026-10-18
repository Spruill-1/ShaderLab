#include "pch_engine.h"
#include "../TestSupport.h"
#include "Effects/EffectRegistry.h"
#include "Effects/ShaderLabEffects.h"
#include "Engine/Utf8.h"
#include "Graph/ClockModel.h"
#include "Graph/GraphEdits.h"
#include "Graph/GraphModel.h"
#include "Rendering/GraphEvaluator.h"
#include "Rendering/MathExpression.h"
#include "Rendering/SourceRuntime.h"
#include "Rendering/Evaluator/ParameterNodes.h"
#include "Support/ExprTkEvaluator.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// Numeric Expression nodes, evaluated by ExprTk, which ShaderLab registers
// through the engine's evaluator hook.
namespace ShaderLab::Tests
{
    namespace
    {
        using namespace ShaderLab::Graph;
        using namespace ShaderLab::Rendering::Evaluator;


        EffectNode ParameterNode(NodeKind kind, std::vector<AnalysisFieldDescriptor> fields)
        {
            EffectNode node;
            node.type = NodeType::ComputeShader;
            CustomEffectDefinition def;
            def.nodeKind = kind;
            def.analysisFields = std::move(fields);
            node.customEffect = std::move(def);
            return node;
        }


        float FieldValue(const std::vector<AnalysisFieldValue>& fields, const wchar_t* name, uint32_t component = 0)
        {
            for (const auto& field : fields)
                if (field.name == name) return field.components[component];
            return std::numeric_limits<float>::quiet_NaN();
        }

        SL_TEST_CASE(TestExpressionParameterNode, Tag::Fast)
        {
            auto expression = ParameterNode(NodeKind::Expression, { { L"Result", AnalysisFieldType::Float } });
            expression.customEffect->parameters = {
                ParameterDefinition{ L"Expression", L"string", std::wstring(L"") },
                ParameterDefinition{ L"A", L"float", 0.0f },
                ParameterDefinition{ L"B", L"float", 0.0f },
            };
            expression.properties = { { L"Expression", std::wstring(L"A * 2 + B") }, { L"A", 3.0f }, { L"B", 1.0f } };
            TEST("ParameterNode_ExpressionEvaluates",
                 FieldValue(ParameterNodeFields(expression), L"Result") == 7.0f && expression.runtimeError.empty());
            ShaderLab::Graph::SetNodeProperty(expression, L"Expression", std::wstring(L"A * ("));
            ParameterNodeFields(expression);
            TEST("ParameterNode_ExpressionErrorReported", !expression.runtimeError.empty());
            ShaderLab::Graph::SetNodeProperty(expression, L"Expression", std::wstring(L"A \u00D7 2"));
            ParameterNodeFields(expression);
            TEST("ParameterNode_ExpressionRejectsNonAscii",
                 expression.runtimeError.find(L"ASCII") != std::wstring::npos);
        }
    }

    SL_TEST_CASE(TestMathNodes, Tag::Gpu)
    {
        printf("\n=== Numeric Expression Node ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();

        // Effect descriptor exists.
        auto* desc = registry.FindByName(L"Numeric Expression");
        TEST("DescriptorExists", desc != nullptr);
        if (!desc) return;
        TEST("StableEffectId", desc->effectId == L"Math Expression");

        auto baseNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
        TEST("DefaultHasOnlyA",
            baseNode.properties.count(L"A") == 1 &&
            baseNode.properties.count(L"B") == 0 &&
            baseNode.properties.count(L"Expression") == 1);

        // Each entry: expression + (name,value) pairs to set + expected result.
        struct Case {
            const wchar_t* name;
            const wchar_t* expression;
            std::vector<std::pair<const wchar_t*, float>> inputs;
            float expected;
        };
        Case cases[] = {
            { L"Identity",         L"A",                 {{L"A", 7.5f}},                            7.5f },
            { L"AddTwoInputs",     L"A + B",             {{L"A", 3.0f},{L"B", 4.0f}},               7.0f },
            { L"SubtractMultiply", L"(A - B) * C",       {{L"A", 10.0f},{L"B", 4.0f},{L"C", 2.0f}}, 12.0f },
            { L"MaxOfFour",        L"max(A, B, C, D)",   {{L"A", 1.0f},{L"B", 9.0f},{L"C", 3.0f},{L"D", 7.0f}}, 9.0f },
            { L"FiveInputAvg",     L"(A + B + C + D + E) / 5",
                                   {{L"A", 1.0f},{L"B", 2.0f},{L"C", 3.0f},{L"D", 4.0f},{L"E", 5.0f}}, 3.0f },
            { L"SinPi",            L"sin(pi)",           {{L"A", 0.0f}},                            0.0f },
            { L"Conditional",      L"if(A > B, A, B)",   {{L"A", 5.0f},{L"B", 3.0f}},               5.0f },
        };

        for (const auto& c : cases)
        {
            ShaderLab::Graph::GraphModel g;
            ShaderLab::Effects::SourceNodeFactory sf;
            auto node = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            // Add as many inputs as required (default has only A).
            for (const auto& [pname, pval] : c.inputs)
            {
                if (node.properties.find(pname) == node.properties.end())
                {
                    ShaderLab::Graph::ParameterDefinition pd;
                    pd.name = pname; pd.typeName = L"float"; pd.defaultValue = 0.0f;
                    pd.minValue = -100000.0f; pd.maxValue = 100000.0f; pd.step = 0.1f;
                    node.customEffect->parameters.push_back(std::move(pd));
                    ShaderLab::Graph::SetNodeProperty(node, pname, 0.0f);
                }
                ShaderLab::Graph::SetNodeProperty(node, pname, pval);
            }
            ShaderLab::Graph::SetNodeProperty(node, L"Expression", std::wstring(c.expression));

            auto id = g.AddNode(std::move(node));
            PrepareSourcesAndEvaluate(g, sf);

            auto* mn = g.FindNode(id);
            bool ok = false;
            float got = 0.0f;
            if (mn) {
                for (const auto& f : mn->analysisOutput.fields) {
                    if (f.name == L"Result") { got = f.components[0]; break; }
                }
                ok = std::abs(got - c.expected) < 0.01f && mn->runtimeError.empty();
            }
            std::string testName = "Eval_";
            testName += ShaderLab::WideToUtf8(c.name);
            TEST(testName.c_str(), ok);
        }

        // Parse error path.
        {
            ShaderLab::Graph::GraphModel g;
            ShaderLab::Effects::SourceNodeFactory sf;
            auto node = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            ShaderLab::Graph::SetNodeProperty(node, L"Expression", std::wstring(L"A + + +"));
            auto id = g.AddNode(std::move(node));
            PrepareSourcesAndEvaluate(g, sf);
            auto* mn = g.FindNode(id);
            TEST("ParseErrorReported", mn && !mn->runtimeError.empty());
        }

        // Loops are refused: an expression runs on the render thread.
        for (const wchar_t* loopExpression : {
                 L"for (var i := 0; i < 3; i += 1) { A; }",
                 L"while (A < 0) { A; }",
                 L"repeat A; until (A > -1);" })
        {
            ShaderLab::Graph::GraphModel g;
            ShaderLab::Effects::SourceNodeFactory sf;
            auto node = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            ShaderLab::Graph::SetNodeProperty(node, L"Expression", std::wstring(loopExpression));
            auto id = g.AddNode(std::move(node));
            PrepareSourcesAndEvaluate(g, sf);
            auto* mn = g.FindNode(id);
            std::string testName = "LoopRefused_";
            for (const wchar_t* p = loopExpression; *p && *p != L' '; ++p) testName += static_cast<char>(*p);
            TEST(testName.c_str(), mn && !mn->runtimeError.empty());
        }

        // Add/remove inputs at the graph level (mirrors the UI flow).
        {
            ShaderLab::Graph::GraphModel g;
            auto node = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            auto id = g.AddNode(std::move(node));
            auto* n = g.FindNode(id);

            auto addInput = [&](const wchar_t* name) {
                ShaderLab::Graph::ParameterDefinition pd;
                pd.name = name; pd.typeName = L"float"; pd.defaultValue = 0.0f;
                pd.minValue = -100000.0f; pd.maxValue = 100000.0f; pd.step = 0.1f;
                n->customEffect->parameters.push_back(std::move(pd));
                ShaderLab::Graph::SetNodeProperty(*n, name, 0.0f);
            };
            addInput(L"B");
            addInput(L"C");
            TEST("AddInputs",
                n->properties.count(L"B") == 1 && n->properties.count(L"C") == 1);

            // Remove B.
            std::erase_if(n->customEffect->parameters,
                [](const auto& p) { return p.name == L"B"; });
            n->properties.erase(L"B");
            g.UnbindProperty(id, L"B");
            TEST("RemoveInput",
                n->properties.count(L"B") == 0 &&
                n->properties.count(L"A") == 1 && n->properties.count(L"C") == 1);
        }

        // JSON round-trip with custom inputs.
        {
            ShaderLab::Graph::GraphModel g;
            auto node = ShaderLab::Effects::ShaderLabEffects::CreateNode(*desc);
            // Add B and C, set values + expression.
            for (const wchar_t* name : { L"B", L"C" })
            {
                ShaderLab::Graph::ParameterDefinition pd;
                pd.name = name; pd.typeName = L"float"; pd.defaultValue = 0.0f;
                pd.minValue = -100000.0f; pd.maxValue = 100000.0f; pd.step = 0.1f;
                node.customEffect->parameters.push_back(std::move(pd));
                ShaderLab::Graph::SetNodeProperty(node, name, 0.0f);
            }
            ShaderLab::Graph::SetNodeProperty(node, L"A", 1.0f);
            ShaderLab::Graph::SetNodeProperty(node, L"B", 2.0f);
            ShaderLab::Graph::SetNodeProperty(node, L"C", 3.0f);
            ShaderLab::Graph::SetNodeProperty(node, L"Expression", std::wstring(L"A + B * C"));
            auto id = g.AddNode(std::move(node));

            auto json = g.ToJson();
            auto g2 = ShaderLab::Graph::GraphModel::FromJson(json);

            ShaderLab::Effects::SourceNodeFactory sf;
            PrepareSourcesAndEvaluate(g2, sf);
            auto* mn = g2.FindNode(id);
            float got = 0.0f;
            if (mn) {
                for (const auto& f : mn->analysisOutput.fields)
                    if (f.name == L"Result") { got = f.components[0]; break; }
            }
            TEST("JsonRoundTripInputsAndExpression",
                mn && std::abs(got - 7.0f) < 0.01f); // 1 + 2*3 = 7
        }
    }

    // The engine has no expression language of its own: the runner registered
    // ExprTk, and without a registration every expression fails with a reason.
    SL_TEST_CASE(TestMathExpressionHook, Tag::Fast)
    {
        printf("\n=== Numeric Expression evaluator hook ===\n");
        const wchar_t* names[] = { L"A", L"B" };
        const float values[] = { 3.0f, 4.0f };
        float result = 0.0f;
        std::wstring error;
        TEST("MathHook_Registered", Rendering::EvaluateMathExpression(L"sqrt(A*A + B*B)", names, values, 2, result, &error) &&
             result == 5.0f);

        Rendering::SetMathExpressionEvaluator(nullptr);
        result = 1.0f;
        error.clear();
        const bool evaluated = Rendering::EvaluateMathExpression(L"A + B", names, values, 2, result, &error);
        Rendering::SetMathExpressionEvaluator(&Support::EvaluateWithExprTk);
        TEST("MathHook_UnsetFails", !evaluated && result == 0.0f && !error.empty());
    }
}
