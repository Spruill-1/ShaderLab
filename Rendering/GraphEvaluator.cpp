#include "pch.h"
#include "GraphEvaluator.h"
#include "../Effects/ShaderCompiler.h"

using namespace ShaderLab::Graph;

namespace ShaderLab::Rendering
{
    // -----------------------------------------------------------------------
    // Main evaluation entry point
    // -----------------------------------------------------------------------

    ID2D1Image* GraphEvaluator::Evaluate(EffectGraph& graph, ID2D1DeviceContext5* dc)
    {
        if (graph.IsEmpty() || !dc)
            return nullptr;

        // Effects created on the previous frame are now fully initialized.
        m_justCreated.clear();

        // Get topological ordering (sources first → output last).
        std::vector<uint32_t> order;
        try
        {
            order = graph.TopologicalSort();
        }
        catch (const std::logic_error&)
        {
            // Graph has a cycle -- cannot evaluate.
            return nullptr;
        }

        ID2D1Image* finalOutput = nullptr;

        for (uint32_t nodeId : order)
        {
            EffectNode* node = graph.FindNode(nodeId);
            if (!node)
                continue;

            // Skip nodes not needed by any visible output.
            if (!node->needed)
            {
                node->dirty = false;
                continue;
            }

            switch (node->type)
            {
            case NodeType::Source:
            {
                // Source nodes have their cachedOutput set externally
                // (by WIC image loading or Flood effect in Step 8).
                // Nothing to do here -- just ensure it's marked clean.
                node->dirty = false;
                break;
            }

            case NodeType::BuiltInEffect:
            {
                ID2D1Effect* effect = GetOrCreateEffect(dc, *node);
                if (effect)
                {
                    WireInputs(effect, *node, graph);

                    // Resolve property bindings every frame.
                    std::map<std::wstring, PropertyValue> effectiveProps;
                    bool bindingsChanged = ResolveBindings(*node, graph, effectiveProps);

                    if (node->dirty || bindingsChanged)
                    {
                        ApplyProperties(effect, *node, effectiveProps);
                        node->dirty = false;
                    }

                    // The effect's output is an ID2D1Image.
                    winrt::com_ptr<ID2D1Image> output;
                    effect->GetOutput(output.put());
                    node->cachedOutput = output.get();

                    // For analysis effects (e.g., Histogram), force computation
                    // by drawing the output, then read back the result property.
                    if (node->effectClsid.has_value() &&
                        IsEqualGUID(node->effectClsid.value(), CLSID_D2D1Histogram))
                    {
                        ReadHistogramOutput(dc, effect, *node);
                    }
                }
                break;
            }

            case NodeType::PixelShader:
            case NodeType::ComputeShader:
            {
                // Image Statistics: CPU-side analysis (no shader needed).
                if (node->customEffect.has_value() &&
                    node->customEffect->shaderLabEffectId == L"Image Statistics" &&
                    node->dirty)
                {
                    // Find the upstream input image.
                    auto inputs = graph.GetInputEdges(nodeId);
                    ID2D1Image* inputImage = nullptr;
                    if (!inputs.empty())
                    {
                        auto* srcNode = graph.FindNode(inputs[0]->sourceNodeId);
                        if (srcNode) inputImage = srcNode->cachedOutput;
                    }
                    if (inputImage)
                        ComputeImageStatistics(dc, *node, inputImage);
                    node->dirty = false;
                    break;
                }

                // Parameter nodes: no HLSL, just expose property values as analysis output.
                if (node->customEffect.has_value() &&
                    node->customEffect->hlslSource.empty() &&
                    node->customEffect->analysisOutputType == AnalysisOutputType::Typed &&
                    !node->customEffect->analysisFields.empty())
                {
                    node->analysisOutput.type = AnalysisOutputType::Typed;
                    node->analysisOutput.fields.clear();
                    for (const auto& fd : node->customEffect->analysisFields)
                    {
                        AnalysisFieldValue fv;
                        fv.name = fd.name;
                        fv.type = fd.type;
                        auto propIt = node->properties.find(fd.name);
                        if (propIt != node->properties.end())
                        {
                            if (auto* f = std::get_if<float>(&propIt->second))
                                fv.components[0] = *f;
                        }
                        node->analysisOutput.fields.push_back(std::move(fv));
                    }
                    node->cachedOutput = nullptr;
                    node->dirty = false;
                    break;
                }

                // Auto-compile ShaderLab effects that have HLSL but no bytecode.
                if (node->customEffect.has_value() &&
                    !node->customEffect->isCompiled() &&
                    !node->customEffect->hlslSource.empty())
                {
                    auto& def = node->customEffect.value();
                    std::string target = (def.shaderType == CustomShaderType::PixelShader)
                        ? "ps_5_0" : "cs_5_0";
                    std::string hlslUtf8(def.hlslSource.begin(), def.hlslSource.end());
                    for (auto& ch : hlslUtf8)
                        if (ch == '\r') ch = '\n';
                    auto result = Effects::ShaderCompiler::CompileFromString(hlslUtf8, "ShaderLabEffect", "main", target);
                    if (result.succeeded)
                    {
                        auto* blob = result.bytecode.get();
                        def.compiledBytecode.resize(blob->GetBufferSize());
                        memcpy(def.compiledBytecode.data(), blob->GetBufferPointer(), blob->GetBufferSize());
                        CoCreateGuid(&def.shaderGuid);
                        node->dirty = true;
                    }
                    else
                    {
                        node->runtimeError = L"Auto-compile failed: " + result.ErrorMessage();
                        node->cachedOutput = nullptr;
                        break;
                    }
                }

                ID2D1Effect* effect = GetOrCreateEffect(dc, *node);
                if (!effect)
                {
                    node->runtimeError = L"Failed to create D2D effect. Check effect registration.";
                    node->cachedOutput = nullptr;
                    break;
                }
                node->runtimeError.clear();

                if (node->customEffect.has_value() && node->customEffect->isCompiled())
                {
                    // Resolve property bindings every frame.
                    std::map<std::wstring, PropertyValue> effectiveProps;
                    bool bindingsChanged = ResolveBindings(*node, graph, effectiveProps);
                    bool wasDirty = node->dirty || bindingsChanged;

                    if (wasDirty)
                    {
                        ApplyCustomEffect(effect, *node, effectiveProps);

                        // Force-upload the cbuffer directly to the GPU.
                        auto implIt = m_customImplCache.find(node->id);
                        if (implIt != m_customImplCache.end())
                        {
                            if (node->type == NodeType::PixelShader && implIt->second.pixelImpl)
                                implIt->second.pixelImpl->ForceUploadConstantBuffer();
                            else if (node->type == NodeType::ComputeShader && implIt->second.computeImpl)
                                implIt->second.computeImpl->ForceUploadConstantBuffer();
                        }

                        node->dirty = false;
                    }

                    WireInputs(effect, *node, graph);

                    // For source effects (no declared inputs), feed a dummy bitmap
                    // so D2D has valid input for MapInputRectsToOutputRect sizing.
                    if (node->customEffect->inputNames.empty())
                    {
                        EnsureDummySourceBitmap(dc);
                        if (m_dummySourceBitmap)
                            effect->SetInput(0, m_dummySourceBitmap.get());
                    }

                    // Force D2D to re-render by toggling input 0.
                    {
                        winrt::com_ptr<ID2D1Image> savedInput;
                        effect->GetInput(0, savedInput.put());
                        if (savedInput)
                        {
                            effect->SetInput(0, nullptr);
                            effect->SetInput(0, savedInput.get());
                        }
                    }

                    winrt::com_ptr<ID2D1Image> output;
                    effect->GetOutput(output.put());
                    node->cachedOutput = output.get();

                    // Read back analysis data only when the node was dirty
                    // (avoids expensive BeginDraw/DrawImage/EndDraw + CPU readback every frame).
                    // Also defer for effects created this frame.
                    if (wasDirty &&
                        node->customEffect->analysisOutputType == AnalysisOutputType::Typed &&
                        !node->customEffect->analysisFields.empty() &&
                        node->cachedOutput &&
                        m_justCreated.find(node->id) == m_justCreated.end())
                    {
                        ReadCustomAnalysisOutput(dc, *node);
                    }
                }
                else
                {
                    if (node->customEffect.has_value() && !node->customEffect->isCompiled())
                    {
                        node->runtimeError = L"Shader not compiled. Open in Effect Designer and compile.";
                        node->cachedOutput = nullptr;
                    }
                    else
                    {
                        WireInputs(effect, *node, graph);
                        if (node->dirty)
                        {
                            ApplyProperties(effect, *node, node->properties);
                            node->dirty = false;
                        }
                        winrt::com_ptr<ID2D1Image> output;
                        effect->GetOutput(output.put());
                        node->cachedOutput = output.get();
                    }
                }
                break;
            }

            case NodeType::Output:
            {
                // The output node simply passes through the first input.
                node->cachedOutput = nullptr;
                auto inputs = graph.GetInputEdges(nodeId);
                if (!inputs.empty())
                {
                    const EffectNode* srcNode = graph.FindNode(inputs[0]->sourceNodeId);
                    if (srcNode && srcNode->cachedOutput)
                    {
                        node->cachedOutput = srcNode->cachedOutput;
                        finalOutput = node->cachedOutput;
                    }
                }
                node->dirty = false;
                break;
            }
            }
        }

        // Newly created effects need a second evaluation pass —
        // D2D requires one full render cycle to initialize the transform
        // pipeline before the output is valid. Mark them dirty for next frame.
        if (!m_justCreated.empty())
        {
            for (uint32_t id : m_justCreated)
            {
                auto* n = graph.FindNode(id);
                if (n) n->dirty = true;
            }
        }

        return finalOutput;
    }

    // -----------------------------------------------------------------------
    // Effect cache management
    // -----------------------------------------------------------------------

    void GraphEvaluator::ReleaseCache()
    {
        m_effectCache.clear();
        m_customImplCache.clear();
        m_dummySourceBitmap = nullptr;
    }

    void GraphEvaluator::InvalidateNode(uint32_t nodeId)
    {
        m_effectCache.erase(nodeId);
        m_customImplCache.erase(nodeId);
    }

    void GraphEvaluator::UpdateNodeShader(uint32_t nodeId, const EffectNode& node)
    {
        if (!node.customEffect.has_value() || !node.customEffect->isCompiled())
            return;

        auto& def = node.customEffect.value();
        auto implIt = m_customImplCache.find(nodeId);
        if (implIt == m_customImplCache.end())
            return; // First compile — next Evaluate() will create the effect.

        // Check if input count changed (structural change requires recreate).
        UINT32 newIC = static_cast<UINT32>(
            (std::max)(size_t(1), def.inputNames.size()));
        auto effectIt = m_effectCache.find(nodeId);
        if (effectIt != m_effectCache.end())
        {
            UINT32 curIC = effectIt->second->GetInputCount();
            if (curIC != newIC)
            {
                InvalidateNode(nodeId);
                return;
            }
        }

        // Non-structural recompile: update bytecode in place.
        if (node.type == NodeType::PixelShader && implIt->second.pixelImpl)
        {
            implIt->second.pixelImpl->SetShaderGuid(def.shaderGuid);
            implIt->second.pixelImpl->LoadShaderBytecode(
                def.compiledBytecode.data(),
                static_cast<UINT32>(def.compiledBytecode.size()));
        }
        else if (node.type == NodeType::ComputeShader && implIt->second.computeImpl)
        {
            implIt->second.computeImpl->SetShaderGuid(def.shaderGuid);
            implIt->second.computeImpl->LoadShaderBytecode(
                def.compiledBytecode.data(),
                static_cast<UINT32>(def.compiledBytecode.size()));
            implIt->second.computeImpl->SetThreadGroupSize(
                def.threadGroupX, def.threadGroupY, def.threadGroupZ);
        }
    }

    // -----------------------------------------------------------------------
    // D2D effect creation / retrieval
    // -----------------------------------------------------------------------

    ID2D1Effect* GraphEvaluator::GetOrCreateEffect(
        ID2D1DeviceContext5* dc,
        const EffectNode& node)
    {
        // Check the cache first.
        auto it = m_effectCache.find(node.id);
        if (it != m_effectCache.end())
            return it->second.get();

        // For custom effects, use the per-definition shaderGuid as the CLSID
        // and register with the exact number of inputs.
        GUID clsid{};
        if ((node.type == NodeType::PixelShader || node.type == NodeType::ComputeShader) &&
            node.customEffect.has_value() && node.customEffect->isCompiled())
        {
            clsid = node.customEffect->shaderGuid;
            // D2D custom effects require at least 1 input. Source effects
            // (empty inputNames) get a hidden input fed by a dummy bitmap.
            UINT32 inputCount = static_cast<UINT32>(
                (std::max)(size_t(1), node.customEffect->inputNames.size()));

            // Register this specific CLSID if not already registered.
            winrt::com_ptr<ID2D1Factory> factory;
            dc->GetFactory(factory.put());
            auto factory1 = factory.as<ID2D1Factory1>();
            if (factory1)
            {
                HRESULT regHr;
                if (node.type == NodeType::PixelShader)
                    regHr = Effects::CustomPixelShaderEffect::RegisterWithInputCount(
                        factory1.get(), clsid, inputCount);
                else
                    regHr = Effects::CustomComputeShaderEffect::RegisterWithInputCount(
                        factory1.get(), clsid, inputCount);

                // S_OK = registered, D2DERR_ALREADY_REGISTERED-style = already done (also fine).
                if (FAILED(regHr) && regHr != static_cast<HRESULT>(0x88990004L))
                {
                    OutputDebugStringW(std::format(
                        L"[CustomFX] RegisterWithInputCount node {} inputs={} hr=0x{:08X}\n",
                        node.id, inputCount, static_cast<uint32_t>(regHr)).c_str());
                }
            }
        }
        else if (node.effectClsid.has_value())
        {
            clsid = node.effectClsid.value();
        }
        else
        {
            return nullptr;
        }

        // Clear thread-local impl pointers before CreateEffect.
        Effects::CustomPixelShaderEffect::s_lastCreated = nullptr;
        Effects::CustomComputeShaderEffect::s_lastCreated = nullptr;

        winrt::com_ptr<ID2D1Effect> effect;
        // Set pending input count BEFORE CreateEffect so the constructor
        // initializes m_inputCount correctly for SetSingleTransformNode.
        if (node.customEffect.has_value())
        {
            // Always at least 1 for D2D (source effects get a dummy input).
            UINT32 ic = static_cast<UINT32>(
                (std::max)(size_t(1), node.customEffect->inputNames.size()));
            if (node.type == NodeType::PixelShader)
                Effects::CustomPixelShaderEffect::s_pendingInputCount = ic;
            else if (node.type == NodeType::ComputeShader)
                Effects::CustomComputeShaderEffect::s_pendingInputCount = ic;
        }

        HRESULT hr = dc->CreateEffect(clsid, effect.put());

        // Clear pending counts.
        Effects::CustomPixelShaderEffect::s_pendingInputCount = 0;
        Effects::CustomComputeShaderEffect::s_pendingInputCount = 0;

        if (FAILED(hr))
        {
            wchar_t guidStr[64]{};
            StringFromGUID2(clsid, guidStr, 64);
            OutputDebugStringW(std::format(
                L"[CustomFX] CreateEffect({}) FAILED hr=0x{:08X}\n",
                guidStr, static_cast<uint32_t>(hr)).c_str());
            return nullptr;
        }

        // Capture custom effect impl for host-side API.
        if (node.type == NodeType::PixelShader && Effects::CustomPixelShaderEffect::s_lastCreated)
        {
            auto* impl = Effects::CustomPixelShaderEffect::s_lastCreated;
            m_customImplCache[node.id] = { impl, nullptr };

            // For zero-input source effects, set fixed output size from properties.
            if (node.customEffect.has_value() && node.customEffect->inputNames.empty())
            {
                UINT32 outW = 512, outH = 512;
                // Look for OutputSize/DiagramSize/PlateSize/GradSize/PatternSize/PatchSize/ScopeSize property.
                for (const auto& [key, val] : node.properties)
                {
                    if (key.find(L"Size") != std::wstring::npos ||
                        key.find(L"size") != std::wstring::npos)
                    {
                        if (auto* f = std::get_if<float>(&val))
                        {
                            outW = outH = static_cast<UINT32>(*f);
                            break;
                        }
                    }
                }
                // Special case: PatchSize for Color Checker (6 cols × 4 rows).
                auto patchIt = node.properties.find(L"PatchSize");
                if (patchIt != node.properties.end())
                {
                    if (auto* f = std::get_if<float>(&patchIt->second))
                    {
                        outW = static_cast<UINT32>(*f * 6);
                        outH = static_cast<UINT32>(*f * 4);
                    }
                }
                impl->SetFixedOutputSize(outW, outH);
            }
        }
        else if (node.type == NodeType::ComputeShader && Effects::CustomComputeShaderEffect::s_lastCreated)
        {
            m_customImplCache[node.id] = { nullptr, Effects::CustomComputeShaderEffect::s_lastCreated };
        }

        auto* raw = effect.get();
        m_effectCache[node.id] = std::move(effect);
        m_justCreated.insert(node.id);
        return raw;
    }

    // -----------------------------------------------------------------------
    // Property application
    // -----------------------------------------------------------------------

    void GraphEvaluator::ApplyProperties(
        ID2D1Effect* effect,
        const EffectNode& node,
        const std::map<std::wstring, PropertyValue>& effectiveProps)
    {
        if (!effect)
            return;

        // D2D built-in effects use indexed properties (0, 1, 2, ...).
        // The property map in EffectNode uses string keys. We map numeric
        // string keys ("0", "1", ...) directly to D2D property indices.
        // Named keys are resolved through the effect's property name table.
        for (const auto& [key, value] : effectiveProps)
        {
            // Try to parse the key as a numeric index first.
            uint32_t index = UINT32_MAX;
            if (!key.empty() && key[0] >= L'0' && key[0] <= L'9')
            {
                size_t pos = 0;
                unsigned long parsed = std::stoul(key, &pos);
                if (pos == key.size())
                    index = static_cast<uint32_t>(parsed);
            }

            // If not numeric, look up the property by name.
            if (index == UINT32_MAX)
            {
                index = effect->GetPropertyIndex(key.c_str());
                if (index == UINT32_MAX)
                    continue;
            }

            // Set the property value based on its variant type.
            std::visit([effect, index](auto&& v)
            {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, float>)
                {
                    effect->SetValue(index, v);
                }
                else if constexpr (std::is_same_v<T, int32_t>)
                {
                    effect->SetValue(index, v);
                }
                else if constexpr (std::is_same_v<T, uint32_t>)
                {
                    effect->SetValue(index, v);
                }
                else if constexpr (std::is_same_v<T, bool>)
                {
                    effect->SetValue(index, static_cast<BOOL>(v));
                }
                else if constexpr (std::is_same_v<T, std::wstring>)
                {
                    // String properties are rare in D2D effects; skip.
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>)
                {
                    D2D1_VECTOR_2F vec{ v.x, v.y };
                    effect->SetValue(index, vec);
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>)
                {
                    D2D1_VECTOR_3F vec{ v.x, v.y, v.z };
                    effect->SetValue(index, vec);
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>)
                {
                    D2D1_VECTOR_4F vec{ v.x, v.y, v.z, v.w };
                    effect->SetValue(index, vec);
                }
                else if constexpr (std::is_same_v<T, D2D1_MATRIX_5X4_F>)
                {
                    effect->SetValue(index, D2D1_PROPERTY_TYPE_MATRIX_5X4,
                        reinterpret_cast<const BYTE*>(&v), sizeof(v));
                }
                else if constexpr (std::is_same_v<T, std::vector<float>>)
                {
                    effect->SetValue(index,
                        reinterpret_cast<const BYTE*>(v.data()),
                        static_cast<UINT32>(v.size() * sizeof(float)));
                }
            }, value);
        }
    }

    // -----------------------------------------------------------------------
    // Input wiring
    // -----------------------------------------------------------------------

    void GraphEvaluator::WireInputs(
        ID2D1Effect* effect,
        const EffectNode& destNode,
        const EffectGraph& graph)
    {
        if (!effect)
            return;

        auto inputEdges = graph.GetInputEdges(destNode.id);

        // Track which pins are connected so we can clear stale ones.
        UINT32 totalInputs = effect->GetInputCount();
        std::vector<bool> connected(totalInputs, false);

        for (const auto* edge : inputEdges)
        {
            if (edge->destPin >= totalInputs)
                continue;

            const EffectNode* srcNode = graph.FindNode(edge->sourceNodeId);
            if (srcNode && srcNode->cachedOutput)
            {
                effect->SetInput(edge->destPin, srcNode->cachedOutput);
                connected[edge->destPin] = true;
            }
        }

        // Clear any inputs that are no longer connected.
        for (UINT32 i = 0; i < totalInputs; ++i)
        {
            if (!connected[i])
                effect->SetInput(i, nullptr);
        }
    }

    // -----------------------------------------------------------------------
    // Property binding resolution
    // -----------------------------------------------------------------------

    // Helper: resolve a single ComponentSource to a float value.
    static bool ResolveComponentSource(
        const ComponentSource& src,
        const EffectGraph& graph,
        float& outValue)
    {
        const EffectNode* srcNode = graph.FindNode(src.sourceNodeId);
        if (!srcNode || srcNode->analysisOutput.type != AnalysisOutputType::Typed)
            return false;

        for (const auto& fv : srcNode->analysisOutput.fields)
        {
            if (fv.name != src.sourceFieldName) continue;

            if (AnalysisFieldIsArray(fv.type))
            {
                uint32_t stride = AnalysisFieldComponentCount(fv.type);
                uint32_t flatIdx = src.sourceIndex * stride + (std::min)(src.sourceComponent, stride - 1);
                if (flatIdx < fv.arrayData.size())
                {
                    outValue = fv.arrayData[flatIdx];
                    return true;
                }
            }
            else
            {
                uint32_t comp = (std::min)(src.sourceComponent, 3u);
                outValue = fv.components[comp];
                return true;
            }
            return false;
        }
        return false;
    }

    bool GraphEvaluator::ResolveBindings(
        EffectNode& node,
        const EffectGraph& graph,
        std::map<std::wstring, PropertyValue>& effectiveProps)
    {
        // Start with authored properties.
        effectiveProps = node.properties;

        if (node.propertyBindings.empty())
            return false;

        bool anyChanged = false;

        for (const auto& [propName, binding] : node.propertyBindings)
        {
            auto propIt = effectiveProps.find(propName);
            if (propIt == effectiveProps.end()) continue;

            // Whole-array mode.
            if (binding.wholeArray)
            {
                const EffectNode* srcNode = graph.FindNode(binding.wholeArraySourceNodeId);
                if (!srcNode || srcNode->analysisOutput.type != AnalysisOutputType::Typed)
                    continue;
                for (const auto& fv : srcNode->analysisOutput.fields)
                {
                    if (fv.name == binding.wholeArraySourceFieldName && AnalysisFieldIsArray(fv.type))
                    {
                        effectiveProps[propName] = fv.arrayData;
                        anyChanged = true;
                        break;
                    }
                }
                continue;
            }

            // Per-component mode.
            if (binding.sources.empty()) continue;

            PropertyValue newVal;
            bool resolved = false;

            std::visit([&](auto&& existing)
            {
                using T = std::decay_t<decltype(existing)>;

                if constexpr (std::is_same_v<T, float>)
                {
                    if (!binding.sources.empty() && binding.sources[0].has_value())
                    {
                        float v = 0;
                        if (ResolveComponentSource(*binding.sources[0], graph, v))
                        { newVal = v; resolved = true; }
                    }
                }
                else if constexpr (std::is_same_v<T, int32_t>)
                {
                    if (!binding.sources.empty() && binding.sources[0].has_value())
                    {
                        float v = 0;
                        if (ResolveComponentSource(*binding.sources[0], graph, v))
                        { newVal = static_cast<int32_t>(v); resolved = true; }
                    }
                }
                else if constexpr (std::is_same_v<T, uint32_t>)
                {
                    if (!binding.sources.empty() && binding.sources[0].has_value())
                    {
                        float v = 0;
                        if (ResolveComponentSource(*binding.sources[0], graph, v))
                        { newVal = static_cast<uint32_t>(v); resolved = true; }
                    }
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>)
                {
                    auto result = existing; // start from authored
                    bool any = false;
                    for (uint32_t c = 0; c < 2 && c < binding.sources.size(); ++c)
                    {
                        if (binding.sources[c].has_value())
                        {
                            float v = 0;
                            if (ResolveComponentSource(*binding.sources[c], graph, v))
                            { (&result.x)[c] = v; any = true; }
                        }
                    }
                    if (any) { newVal = result; resolved = true; }
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>)
                {
                    auto result = existing;
                    bool any = false;
                    for (uint32_t c = 0; c < 3 && c < binding.sources.size(); ++c)
                    {
                        if (binding.sources[c].has_value())
                        {
                            float v = 0;
                            if (ResolveComponentSource(*binding.sources[c], graph, v))
                            { (&result.x)[c] = v; any = true; }
                        }
                    }
                    if (any) { newVal = result; resolved = true; }
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>)
                {
                    auto result = existing;
                    bool any = false;
                    for (uint32_t c = 0; c < 4 && c < binding.sources.size(); ++c)
                    {
                        if (binding.sources[c].has_value())
                        {
                            float v = 0;
                            if (ResolveComponentSource(*binding.sources[c], graph, v))
                            { (&result.x)[c] = v; any = true; }
                        }
                    }
                    if (any) { newVal = result; resolved = true; }
                }
            }, propIt->second);

            if (resolved)
            {
                effectiveProps[propName] = newVal;
                anyChanged = true;
            }
        }

        return anyChanged;
    }

    // -----------------------------------------------------------------------
    // Custom effect application
    // -----------------------------------------------------------------------

    void GraphEvaluator::ApplyCustomEffect(
        ID2D1Effect* effect,
        EffectNode& node,
        const std::map<std::wstring, PropertyValue>& effectiveProps)
    {
        if (!node.customEffect.has_value()) return;
        auto& def = node.customEffect.value();

        auto implIt = m_customImplCache.find(node.id);
        if (implIt == m_customImplCache.end())
            return;

        // Load bytecode only if not already loaded (bytecode doesn't change
        // on property updates, only on recompile/update-in-graph).
        bool needsShaderLoad = false;
        if (node.type == NodeType::PixelShader && implIt->second.pixelImpl)
        {
            if (implIt->second.pixelImpl->NeedsShaderLoad())
            {
                implIt->second.pixelImpl->SetShaderGuid(def.shaderGuid);
                implIt->second.pixelImpl->LoadShaderBytecode(
                    def.compiledBytecode.data(),
                    static_cast<UINT32>(def.compiledBytecode.size()));
                needsShaderLoad = true;
            }
        }
        else if (node.type == NodeType::ComputeShader && implIt->second.computeImpl)
        {
            implIt->second.computeImpl->SetShaderGuid(def.shaderGuid);
            implIt->second.computeImpl->LoadShaderBytecode(
                def.compiledBytecode.data(),
                static_cast<UINT32>(def.compiledBytecode.size()));
            implIt->second.computeImpl->SetThreadGroupSize(
                def.threadGroupX, def.threadGroupY, def.threadGroupZ);
        }

        // Reflect the bytecode to discover cbuffer layout.
        auto reflection = Effects::ShaderCompiler::Reflect(def.compiledBytecode);
        if (!reflection.constantBuffers.empty())
        {
            auto& cb = reflection.constantBuffers[0];
            std::vector<BYTE> cbData(cb.sizeBytes, 0);

            // Pack each property into the cbuffer at the reflected offset.
            for (const auto& var : cb.variables)
            {
                // Skip system-injected variables for compute shaders.
                // _TileOffset (int2, offset 0) is populated per-tile in
                // CalculateThreadgroups. Shaders use Source.GetDimensions()
                // for image size instead of a cbuffer variable.
                if (node.type == NodeType::ComputeShader &&
                    var.name == L"_TileOffset")
                {
                    continue;
                }

                auto propIt = effectiveProps.find(
                    std::wstring(var.name.begin(), var.name.end()));
                if (propIt == effectiveProps.end()) continue;

                std::visit([&](const auto& v)
                {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, float> || std::is_same_v<T, int32_t> ||
                                  std::is_same_v<T, uint32_t> || std::is_same_v<T, bool>)
                    {
                        if (var.offset + sizeof(T) <= cbData.size())
                            memcpy(cbData.data() + var.offset, &v, sizeof(T));
                    }
                    else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>)
                    {
                        if (var.offset + sizeof(float) * 2 <= cbData.size())
                            memcpy(cbData.data() + var.offset, &v, sizeof(float) * 2);
                    }
                    else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>)
                    {
                        if (var.offset + sizeof(float) * 3 <= cbData.size())
                            memcpy(cbData.data() + var.offset, &v, sizeof(float) * 3);
                    }
                    else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>)
                    {
                        if (var.offset + sizeof(float) * 4 <= cbData.size())
                            memcpy(cbData.data() + var.offset, &v, sizeof(float) * 4);
                    }
                }, propIt->second);
            }

            // Set the packed cbuffer on the concrete impl.
            if (node.type == NodeType::PixelShader && implIt->second.pixelImpl)
            {
                implIt->second.pixelImpl->SetConstantBufferData(cbData.data(), static_cast<UINT32>(cbData.size()));

                // Update fixed output size for zero-input source effects.
                if (node.customEffect->inputNames.empty())
                {
                    UINT32 outW = 512, outH = 512;
                    for (const auto& [key, val] : effectiveProps)
                    {
                        if (key.find(L"Size") != std::wstring::npos ||
                            key.find(L"size") != std::wstring::npos)
                        {
                            if (auto* f = std::get_if<float>(&val))
                            { outW = outH = static_cast<UINT32>(*f); break; }
                        }
                    }
                    auto patchIt = effectiveProps.find(L"PatchSize");
                    if (patchIt != effectiveProps.end())
                    {
                        if (auto* f = std::get_if<float>(&patchIt->second))
                        { outW = static_cast<UINT32>(*f * 6); outH = static_cast<UINT32>(*f * 4); }
                    }
                    implIt->second.pixelImpl->SetFixedOutputSize(outW, outH);
                }
            }
            else if (node.type == NodeType::ComputeShader && implIt->second.computeImpl)
                implIt->second.computeImpl->SetConstantBufferData(cbData.data(), static_cast<UINT32>(cbData.size()));
        }
    }

    // -----------------------------------------------------------------------
    // Dummy source bitmap for zero-input source effects
    // -----------------------------------------------------------------------

    void GraphEvaluator::EnsureDummySourceBitmap(ID2D1DeviceContext5* dc)
    {
        if (m_dummySourceBitmap) return;

        // Create a bitmap sized to the max source effect output (512x512 default).
        // The actual output rect comes from MapInputRectsToOutputRect which uses
        // SetFixedOutputSize, but D2D needs the input bitmap to be at least as large.
        D2D1_BITMAP_PROPERTIES1 props = {};
        props.pixelFormat = { DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
        dc->CreateBitmap(D2D1::SizeU(2048, 2048), nullptr, 0, props, m_dummySourceBitmap.put());
    }

    // -----------------------------------------------------------------------
    // Analysis effect readback
    // -----------------------------------------------------------------------

    void GraphEvaluator::ReadHistogramOutput(
        ID2D1DeviceContext5* dc,
        ID2D1Effect* effect,
        EffectNode& node)
    {
        if (!node.cachedOutput) return;

        // Force D2D to compute the histogram by drawing the effect output.
        // The histogram processes the entire input, so we need a target large
        // enough and must actually draw the image (not just a 1x1 region).
        winrt::com_ptr<ID2D1Image> prevTarget;
        dc->GetTarget(prevTarget.put());

        // Get the input image bounds to size the temp target appropriately.
        D2D1_RECT_F bounds{};
        dc->GetImageLocalBounds(node.cachedOutput, &bounds);
        uint32_t w = static_cast<uint32_t>((std::min)(bounds.right - bounds.left, 4096.0f));
        uint32_t h = static_cast<uint32_t>((std::min)(bounds.bottom - bounds.top, 4096.0f));
        if (w == 0) w = 1;
        if (h == 0) h = 1;

        // Recreate temp target if size changed.
        if (!m_analysisTarget || m_analysisTargetW != w || m_analysisTargetH != h ||
            m_analysisTargetFormat != DXGI_FORMAT_B8G8R8A8_UNORM)
        {
            m_analysisTarget = nullptr;
            D2D1_BITMAP_PROPERTIES1 props = {};
            props.pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED };
            props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
            HRESULT hr = dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, m_analysisTarget.put());
            if (FAILED(hr)) { dc->SetTarget(prevTarget.get()); return; }
            m_analysisTargetW = w;
            m_analysisTargetH = h;
            m_analysisTargetFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        }

        dc->SetTarget(m_analysisTarget.get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->DrawImage(node.cachedOutput, D2D1::Point2F(-bounds.left, -bounds.top));
        dc->EndDraw();
        dc->SetTarget(prevTarget.get());

        // Read the channel selector for labeling.
        uint32_t channelIdx = 0;
        auto chanIt = node.properties.find(L"ChannelSelect");
        if (chanIt != node.properties.end())
        {
            if (auto* pv = std::get_if<uint32_t>(&chanIt->second))
                channelIdx = *pv;
        }
        static const std::wstring channelNames[] = { L"Red", L"Green", L"Blue", L"Alpha" };

        // Query the actual output data size.
        UINT32 dataSize = effect->GetValueSize(D2D1_HISTOGRAM_PROP_HISTOGRAM_OUTPUT);
        if (dataSize == 0) return;

        uint32_t numBins = dataSize / sizeof(float);
        std::vector<float> histData(numBins, 0.0f);

        HRESULT hr = effect->GetValue(
            D2D1_HISTOGRAM_PROP_HISTOGRAM_OUTPUT,
            reinterpret_cast<BYTE*>(histData.data()),
            dataSize);

        if (SUCCEEDED(hr))
        {
            node.analysisOutput.type = AnalysisOutputType::Histogram;
            node.analysisOutput.data = std::move(histData);
            node.analysisOutput.channelIndex = channelIdx;
            node.analysisOutput.label = (channelIdx < 4)
                ? channelNames[channelIdx] + L" Histogram"
                : L"Histogram";
        }
        else
        {
            node.analysisOutput.type = AnalysisOutputType::None;
            node.analysisOutput.data.clear();
        }
    }

    // -----------------------------------------------------------------------
    // Custom analysis readback
    // -----------------------------------------------------------------------

    void GraphEvaluator::ReadCustomAnalysisOutput(
        ID2D1DeviceContext5* dc,
        EffectNode& node)
    {
        if (!node.cachedOutput || !node.customEffect.has_value()) return;
        auto& def = node.customEffect.value();
        if (def.analysisFields.empty()) return;

        uint32_t totalPixels = def.totalAnalysisPixels();
        if (totalPixels == 0) return;

        // Force D2D to evaluate the compute effect by drawing its output.
        winrt::com_ptr<ID2D1Image> prevTarget;
        dc->GetTarget(prevTarget.put());

        D2D1_RECT_F bounds{};
        dc->GetImageLocalBounds(node.cachedOutput, &bounds);
        uint32_t w = static_cast<uint32_t>((std::min)(bounds.right - bounds.left, 4096.0f));
        uint32_t h = static_cast<uint32_t>((std::min)(bounds.bottom - bounds.top, 4096.0f));
        if (w == 0) w = 1;
        if (h == 0) h = 1;

        if (!m_analysisTarget || m_analysisTargetW != w || m_analysisTargetH != h ||
            m_analysisTargetFormat != DXGI_FORMAT_R32G32B32A32_FLOAT)
        {
            m_analysisTarget = nullptr;
            D2D1_BITMAP_PROPERTIES1 props = {};
            props.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
            props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
            HRESULT hr = dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, m_analysisTarget.put());
            if (FAILED(hr)) { dc->SetTarget(prevTarget.get()); return; }
            m_analysisTargetW = w;
            m_analysisTargetH = h;
            m_analysisTargetFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
        }

        dc->SetTarget(m_analysisTarget.get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->DrawImage(node.cachedOutput, D2D1::Point2F(-bounds.left, -bounds.top));
        HRESULT drawHr = dc->EndDraw();
        dc->SetTarget(prevTarget.get());
        if (FAILED(drawHr)) return;

        // Create a CPU-readable bitmap to read back analysis pixels.
        winrt::com_ptr<ID2D1Bitmap1> cpuBitmap;
        D2D1_BITMAP_PROPERTIES1 cpuProps = {};
        cpuProps.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        cpuProps.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

        uint32_t readW = (std::max)(totalPixels, 1u);
        HRESULT hr = dc->CreateBitmap(D2D1::SizeU(readW, 1), nullptr, 0, cpuProps, cpuBitmap.put());
        if (FAILED(hr)) return;

        D2D1_POINT_2U dest = { 0, 0 };
        D2D1_RECT_U srcRect = { 0, 0, readW, 1 };
        hr = cpuBitmap->CopyFromBitmap(&dest, m_analysisTarget.get(), &srcRect);
        if (FAILED(hr)) return;

        D2D1_MAPPED_RECT mapped{};
        hr = cpuBitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped);
        if (FAILED(hr)) return;

        // Unpack typed fields from the pixel row.
        node.analysisOutput.type = AnalysisOutputType::Typed;
        node.analysisOutput.fields.clear();

        const float* pixels = reinterpret_cast<const float*>(mapped.bits);
        uint32_t pixelOffset = 0;

        for (const auto& fieldDesc : def.analysisFields)
        {
            AnalysisFieldValue fv;
            fv.name = fieldDesc.name;
            fv.type = fieldDesc.type;
            uint32_t pc = fieldDesc.pixelCount();

            if (!AnalysisFieldIsArray(fieldDesc.type))
            {
                // Scalar: read one pixel's worth of components.
                uint32_t cc = AnalysisFieldComponentCount(fieldDesc.type);
                for (uint32_t c = 0; c < cc && c < 4; ++c)
                    fv.components[c] = pixels[(pixelOffset) * 4 + c];
            }
            else if (fieldDesc.type == AnalysisFieldType::FloatArray)
            {
                // FloatArray: 4 floats packed per pixel.
                fv.arrayData.resize(fieldDesc.arrayLength, 0.0f);
                for (uint32_t i = 0; i < fieldDesc.arrayLength; ++i)
                {
                    uint32_t pix = pixelOffset + i / 4;
                    uint32_t comp = i % 4;
                    fv.arrayData[i] = pixels[pix * 4 + comp];
                }
            }
            else
            {
                // Float2Array, Float3Array, Float4Array: 1 pixel per element.
                uint32_t cc = AnalysisFieldComponentCount(fieldDesc.type);
                fv.arrayData.resize(fieldDesc.arrayLength * cc, 0.0f);
                for (uint32_t i = 0; i < fieldDesc.arrayLength; ++i)
                {
                    for (uint32_t c = 0; c < cc; ++c)
                        fv.arrayData[i * cc + c] = pixels[(pixelOffset + i) * 4 + c];
                }
            }

            pixelOffset += pc;
            node.analysisOutput.fields.push_back(std::move(fv));
        }

        cpuBitmap->Unmap();
    }

    // -----------------------------------------------------------------------
    // CPU-side image statistics
    // -----------------------------------------------------------------------

    void GraphEvaluator::ComputeImageStatistics(
        ID2D1DeviceContext5* dc,
        EffectNode& node,
        ID2D1Image* inputImage)
    {
        if (!dc || !inputImage) return;

        // Get input image bounds.
        D2D1_RECT_F bounds{};
        dc->GetImageLocalBounds(inputImage, &bounds);
        uint32_t w = static_cast<uint32_t>((std::min)(bounds.right - bounds.left, 8192.0f));
        uint32_t h = static_cast<uint32_t>((std::min)(bounds.bottom - bounds.top, 8192.0f));
        if (w == 0 || h == 0) return;

        // Render input to a GPU target bitmap.
        winrt::com_ptr<ID2D1Bitmap1> gpuTarget;
        D2D1_BITMAP_PROPERTIES1 gpuProps = {};
        gpuProps.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        gpuProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
        HRESULT hr = dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, gpuProps, gpuTarget.put());
        if (FAILED(hr)) return;

        winrt::com_ptr<ID2D1Image> prevTarget;
        dc->GetTarget(prevTarget.put());
        dc->SetTarget(gpuTarget.get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->DrawImage(inputImage, D2D1::Point2F(-bounds.left, -bounds.top));
        dc->EndDraw();
        dc->SetTarget(prevTarget.get());

        // Create CPU-readable bitmap and copy.
        winrt::com_ptr<ID2D1Bitmap1> cpuBitmap;
        D2D1_BITMAP_PROPERTIES1 cpuProps = {};
        cpuProps.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        cpuProps.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        hr = dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, cpuProps, cpuBitmap.put());
        if (FAILED(hr)) return;

        D2D1_POINT_2U dest = { 0, 0 };
        D2D1_RECT_U srcRect = { 0, 0, w, h };
        hr = cpuBitmap->CopyFromBitmap(&dest, gpuTarget.get(), &srcRect);
        if (FAILED(hr)) return;

        D2D1_MAPPED_RECT mapped{};
        hr = cpuBitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped);
        if (FAILED(hr)) return;

        // Read channel selection from properties.
        uint32_t channel = 0;
        {
            auto it = node.properties.find(L"Channel");
            if (it != node.properties.end())
                if (auto* f = std::get_if<float>(&it->second)) channel = static_cast<uint32_t>(*f);
        }
        bool nonzeroOnly = true;
        {
            auto it = node.properties.find(L"NonzeroOnly");
            if (it != node.properties.end())
                if (auto* f = std::get_if<float>(&it->second)) nonzeroOnly = *f > 0.5f;
        }

        // Single-pass scan over all pixels.
        float vMin = 1e30f, vMax = -1e30f, vSum = 0;
        uint64_t totalSamples = 0, totalScanned = 0, nonzeroCount = 0;

        for (uint32_t y = 0; y < h; ++y)
        {
            const float* row = reinterpret_cast<const float*>(
                reinterpret_cast<const uint8_t*>(mapped.bits) + y * mapped.pitch);
            for (uint32_t x = 0; x < w; ++x)
            {
                float r = row[x * 4 + 0];
                float g = row[x * 4 + 1];
                float b = row[x * 4 + 2];
                float a = row[x * 4 + 3];

                float v = 0;
                switch (channel)
                {
                case 1: v = r; break;
                case 2: v = g; break;
                case 3: v = b; break;
                case 4: v = a; break;
                default: v = 0.2126f * r + 0.7152f * g + 0.0722f * b; break;
                }

                totalScanned++;
                bool isNonzero = std::abs(v) > 0.0001f;
                if (isNonzero) nonzeroCount++;
                if (nonzeroOnly && !isNonzero) continue;

                vMin = (std::min)(vMin, v);
                vMax = (std::max)(vMax, v);
                vSum += v;
                totalSamples++;
            }
        }

        cpuBitmap->Unmap();

        float vMean = (totalSamples > 0) ? vSum / static_cast<float>(totalSamples) : 0.0f;
        float vNonzero = (totalScanned > 0) ? static_cast<float>(nonzeroCount) / static_cast<float>(totalScanned) : 0.0f;
        if (totalSamples == 0) { vMin = 0; vMax = 0; }

        // Populate analysis output.
        node.analysisOutput.type = AnalysisOutputType::Typed;
        node.analysisOutput.fields.clear();

        auto addField = [&](const std::wstring& name, float value) {
            AnalysisFieldValue fv;
            fv.name = name;
            fv.type = AnalysisFieldType::Float;
            fv.components[0] = value;
            node.analysisOutput.fields.push_back(std::move(fv));
        };

        addField(L"Min", vMin);
        addField(L"Max", vMax);
        addField(L"Mean", vMean);
        addField(L"Samples", static_cast<float>(totalSamples));
        addField(L"Nonzero%", vNonzero);
    }
}
