#include "pch_engine.h"
#include "GraphPatch.h"

#include "Graph/GraphModel.h"
#include "Rendering/GraphEvaluator.h"
#include "Effects/SourceNodeFactory.h"
#include "Graph/GraphEdits.h"

#include <algorithm>

namespace ShaderLab::Mcp::Routes
{
    namespace
    {
        std::string Where(const char* list, uint32_t index)
        {
            return std::format("{}[{}]", list, index);
        }

        // A node reference: a number is an id, a string a ref or an id in digits.
        std::optional<PatchNodeRef> ReadNodeRef(const WDJ::IJsonValue& value, const std::string& where, std::string& error)
        {
            PatchNodeRef ref;
            if (value.ValueType() == WDJ::JsonValueType::Number)
            {
                const double number = value.GetNumber();
                if (number < 0.0 || number > static_cast<double>(UINT32_MAX) || std::floor(number) != number)
                {
                    SetArgError(error, where + " must be a node id or ref");
                    return std::nullopt;
                }
                ref.id = static_cast<uint32_t>(number);
                return ref;
            }
            if (value.ValueType() == WDJ::JsonValueType::String)
            {
                ref.ref = std::wstring(value.GetString());
                return ref;
            }
            SetArgError(error, "Expected number or string for " + where);
            return std::nullopt;
        }

        std::optional<WDJ::JsonObject> EntryAt(const WDJ::JsonArray& list, uint32_t index, const std::string& where, std::string& error)
        {
            const auto value = list.GetAt(index);
            if (value.ValueType() == WDJ::JsonValueType::Object)
                return value.GetObject();
            SetArgError(error, where + " must be an object");
            return std::nullopt;
        }

        std::optional<PatchNode> ParseNode(const WDJ::JsonObject& entry, const std::string& where, std::string& error)
        {
            PatchNode node;
            auto effect = ReadStringArg(entry, L"effect", error);
            if (!effect)
            {
                SetArgError(error, where + " missing 'effect'");
                return std::nullopt;
            }
            node.effect = *effect;
            std::string argError;
            node.args.filePath     = ReadStringArg(entry, L"filePath", argError).value_or(L"");
            ReadUintArgs(entry, node.args, argError);
            if (entry.HasKey(L"ref") && entry.GetNamedValue(L"ref").ValueType() == WDJ::JsonValueType::String)
                node.ref = std::wstring(entry.GetNamedString(L"ref"));
            if (entry.HasKey(L"properties") &&
                entry.GetNamedValue(L"properties").ValueType() == WDJ::JsonValueType::Object)
            {
                for (const auto& member : entry.GetNamedObject(L"properties"))
                {
                    const std::wstring key(member.Key());
                    std::string valueError;
                    auto value = ToUntypedValue(member.Value(), valueError);
                    if (!value)
                    {
                        SetArgError(argError, "property '" + WideToUtf8(key) + "': " + valueError);
                        break;
                    }
                    node.properties.emplace_back(key, std::move(*value));
                }
            }
            if (!argError.empty())
            {
                SetArgError(error, where + " " + argError);
                return std::nullopt;
            }
            return node;
        }

        std::optional<PatchEdge> ParseEdge(const WDJ::JsonObject& entry, const std::string& where, std::string& error)
        {
            if (!entry.HasKey(L"from") || !entry.HasKey(L"to"))
            {
                SetArgError(error, where + " missing 'from' or 'to'");
                return std::nullopt;
            }
            auto from = ReadNodeRef(entry.GetNamedValue(L"from"), "edge source", error);
            auto to = ReadNodeRef(entry.GetNamedValue(L"to"), "edge dest", error);
            std::string pinError;
            const uint32_t fromPin = ReadUintArg(entry, L"fromPin", pinError).value_or(0);
            const uint32_t toPin = ReadUintArg(entry, L"toPin", pinError).value_or(0);
            if (!pinError.empty())
                SetArgError(error, where + " " + pinError);
            if (!from || !to || !pinError.empty())
                return std::nullopt;
            return PatchEdge{ *from, *to, fromPin, toPin };
        }

        std::optional<PatchBinding> ParseBinding(const WDJ::JsonObject& entry, const std::string& where, std::string& error)
        {
            if (!entry.HasKey(L"node") || !entry.HasKey(L"property") || !entry.HasKey(L"from"))
            {
                SetArgError(error, where + " missing one of node/property/from");
                return std::nullopt;
            }
            PatchBinding binding;
            auto node = ReadNodeRef(entry.GetNamedValue(L"node"), "binding node", error);
            std::string argError;
            auto property = ReadStringArg(entry, L"property", argError);
            binding.component = ReadUintArg(entry, L"component", argError).value_or(0);
            if (!argError.empty())
            {
                SetArgError(error, where + " " + argError);
                return std::nullopt;
            }
            if (!node || !property)
                return std::nullopt;
            binding.node = *node;
            binding.property = *property;

            // `from` is "ref.fieldName" or { "node": ..., "field": ... }.
            const auto from = entry.GetNamedValue(L"from");
            if (from.ValueType() == WDJ::JsonValueType::String)
            {
                const std::wstring text(from.GetString());
                const auto dot = text.find(L'.');
                if (dot == std::wstring::npos)
                {
                    SetArgError(error, where + " 'from' must be 'ref.fieldName'");
                    return std::nullopt;
                }
                binding.from.ref = text.substr(0, dot);
                binding.field = text.substr(dot + 1);
                return binding;
            }
            if (from.ValueType() == WDJ::JsonValueType::Object)
            {
                const auto fromObject = from.GetObject();
                std::string fieldError;
                auto field = ReadStringArg(fromObject, L"field", fieldError);
                if (!fromObject.HasKey(L"node") || !field)
                {
                    SetArgError(error, where + " 'from' object missing node/field");
                    return std::nullopt;
                }
                auto source = ReadNodeRef(fromObject.GetNamedValue(L"node"), "binding source", error);
                if (!source)
                    return std::nullopt;
                binding.from = *source;
                binding.field = *field;
                return binding;
            }
            SetArgError(error, where + " 'from' must be string or object");
            return std::nullopt;
        }

        // Undoes a partly applied patch.
        class PatchJournal
        {
        public:
            explicit PatchJournal(Graph::GraphModel& graph)
                : m_graph(graph), m_edgesBefore(graph.Edges()) {}

            void NodeCreated(uint32_t id) { m_created.push_back(id); }

            // Call before binding a property on nodeId.
            void BindingTouched(uint32_t nodeId)
            {
                if (std::ranges::find(m_created, nodeId) != m_created.end() || m_bindingsBefore.contains(nodeId))
                    return;
                if (const auto* node = m_graph.FindNode(nodeId))
                    m_bindingsBefore.emplace(nodeId, node->propertyBindings);
            }

            void Rollback(EngineContext& ctx)
            {
                for (auto it = m_created.rbegin(); it != m_created.rend(); ++it)
                {
                    m_graph.RemoveNode(*it);
                    ctx.Evaluator().InvalidateNode(*it);
                }

                // Edges between existing nodes the patch replaced or added.
                const std::vector<Graph::EffectEdge> now = m_graph.Edges();
                for (const auto& edge : now)
                    if (std::ranges::find(m_edgesBefore, edge) == m_edgesBefore.end())
                        m_graph.Disconnect(edge.sourceNodeId, edge.sourcePin, edge.destNodeId, edge.destPin);
                for (const auto& edge : m_edgesBefore)
                    if (std::ranges::find(m_graph.Edges(), edge) == m_graph.Edges().end())
                        m_graph.Connect(edge.sourceNodeId, edge.sourcePin, edge.destNodeId, edge.destPin);

                for (auto& [nodeId, bindings] : m_bindingsBefore)
                {
                    if (auto* node = m_graph.FindNode(nodeId))
                    {
                        node->propertyBindings = std::move(bindings);
                        Graph::MarkNodeChanged(*node);
                    }
                }

                // Close the providers the removed sources opened.
                ctx.Sources().PruneOrphans(m_graph.Nodes());
            }

        private:
            Graph::GraphModel&                    m_graph;
            const std::vector<Graph::EffectEdge>   m_edgesBefore;
            std::vector<uint32_t>                  m_created;
            std::map<uint32_t, std::map<std::wstring, Graph::PropertyBinding>> m_bindingsBefore;
        };

        std::optional<uint32_t> Resolve(const PatchNodeRef& ref, const std::map<std::wstring, uint32_t>& refToId,
            const char* what, std::string& error)
        {
            if (ref.id)
                return ref.id;
            if (auto it = refToId.find(ref.ref); it != refToId.end())
                return it->second;
            if (auto id = ParseUintText(ref.ref))
                return id;
            error = std::string("Unknown ") + what + " ref: " + WideToUtf8(ref.ref);
            return std::nullopt;
        }
    }

    std::optional<GraphPatch> ParseGraphPatch(const WDJ::JsonObject& body, std::string& error)
    {
        GraphPatch patch;
        std::string listError;
        auto nodes = ReadArrayArg(body, L"nodes", listError);
        auto edges = ReadArrayArg(body, L"edges", listError);
        auto bindings = ReadArrayArg(body, L"bindings", listError);
        if (!listError.empty())
        {
            error = listError;
            return std::nullopt;
        }

        for (uint32_t i = 0; nodes && i < nodes->Size(); ++i)
        {
            const std::string where = Where("nodes", i);
            auto entry = EntryAt(*nodes, i, where, error);
            auto node = entry ? ParseNode(*entry, where, error) : std::nullopt;
            if (!node) return std::nullopt;
            patch.nodes.push_back(std::move(*node));
        }
        for (uint32_t i = 0; edges && i < edges->Size(); ++i)
        {
            const std::string where = Where("edges", i);
            auto entry = EntryAt(*edges, i, where, error);
            auto edge = entry ? ParseEdge(*entry, where, error) : std::nullopt;
            if (!edge) return std::nullopt;
            patch.edges.push_back(std::move(*edge));
        }
        for (uint32_t i = 0; bindings && i < bindings->Size(); ++i)
        {
            const std::string where = Where("bindings", i);
            auto entry = EntryAt(*bindings, i, where, error);
            auto binding = entry ? ParseBinding(*entry, where, error) : std::nullopt;
            if (!binding) return std::nullopt;
            patch.bindings.push_back(std::move(*binding));
        }
        return patch;
    }

    PatchResult ApplyGraphPatch(EngineContext& ctx, const GraphPatch& patch)
    {
        auto& graph = ctx.Model();
        PatchResult result;
        PatchJournal journal(graph);
        auto fail = [&](std::string error) -> PatchResult
        {
            journal.Rollback(ctx);
            PatchResult failed;
            failed.error = std::move(error);
            return failed;
        };

        for (uint32_t i = 0; i < patch.nodes.size(); ++i)
        {
            const auto& entry = patch.nodes[i];
            auto created = Effects::CreateNodeByName(entry.effect, entry.args, graph);
            if (!created.node)
                return fail(Where("nodes", i) + " " + WideToUtf8(created.error));
            // Written before the node is added, so client values override the defaults.
            for (const auto& [key, value] : entry.properties)
                if (auto error = Graph::WriteProperty(*created.node, key, value); !error.empty())
                    return fail(WideToUtf8(error));

            const uint32_t id = AddCreatedNode(ctx, created);
            journal.NodeCreated(id);
            result.nodeIds.push_back(id);
            if (!entry.ref.empty())
                result.refToId[entry.ref] = id;
        }

        for (uint32_t i = 0; i < patch.edges.size(); ++i)
        {
            const auto& edge = patch.edges[i];
            std::string error;
            const auto sourceId = Resolve(edge.from, result.refToId, "edge source", error);
            const auto destId = sourceId ? Resolve(edge.to, result.refToId, "edge dest", error) : std::nullopt;
            if (!sourceId || !destId)
                return fail(error);
            if (!graph.Connect(*sourceId, edge.fromPin, *destId, edge.toPin))
                return fail(Where("edges", i) + " Connect failed (cycle or invalid pin)");
        }

        for (uint32_t i = 0; i < patch.bindings.size(); ++i)
        {
            const auto& binding = patch.bindings[i];
            std::string error;
            const auto destId = Resolve(binding.node, result.refToId, "binding node", error);
            const auto sourceId = destId ? Resolve(binding.from, result.refToId, "binding source", error) : std::nullopt;
            if (!destId || !sourceId)
                return fail(error);
            journal.BindingTouched(*destId);
            const auto bindError = graph.BindProperty(*destId, binding.property, *sourceId, binding.field, binding.component);
            if (!bindError.empty())
                return fail(Where("bindings", i) + ": " + WideToUtf8(bindError));
        }
        return result;
    }
}
