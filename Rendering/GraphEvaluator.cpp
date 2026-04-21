#include "pch.h"
#include "GraphEvaluator.h"

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

                    if (node->dirty)
                    {
                        ApplyProperties(effect, *node);
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
                ID2D1Effect* effect = GetOrCreateEffect(dc, *node);
                if (!effect)
                {
                    node->runtimeError = L"Failed to create D2D effect. Check effect registration.";
                    break;
                }
                node->runtimeError.clear();

                if (node->customEffect.has_value() && node->customEffect->isCompiled())
                {
                    if (node->dirty)
                    {
                        ApplyCustomEffect(effect, *node);

                        // Force-upload the cbuffer directly to the GPU.
                        // D2D won't call PrepareForRender for host-side cbuffer changes,
                        // so we push it via DrawInfo immediately.
                        auto implIt = m_customImplCache.find(node->id);
                        if (implIt != m_customImplCache.end())
                        {
                            if (node->type == NodeType::PixelShader && implIt->second.pixelImpl)
                                implIt->second.pixelImpl->ForceUploadConstantBuffer();
                            else if (node->type == NodeType::ComputeShader && implIt->second.computeImpl)
                                implIt->second.computeImpl->ForceUploadConstantBuffer();
                        }

                        // Force D2D to re-render by toggling input 0.
                        // D2D caches effect output and skips re-rendering if it sees
                        // no changes. Briefly nulling input 0 signals a change.
                        winrt::com_ptr<ID2D1Image> savedInput;
                        effect->GetInput(0, savedInput.put());
                        effect->SetInput(0, nullptr);
                        effect->SetInput(0, savedInput.get());

                        node->dirty = false;
                    }

                    WireInputs(effect, *node, graph);

                    winrt::com_ptr<ID2D1Image> output;
                    effect->GetOutput(output.put());
                    node->cachedOutput = output.get();
                }
                else
                {
                    if (node->customEffect.has_value() && !node->customEffect->isCompiled())
                        node->runtimeError = L"Shader not compiled. Open in Effect Designer and compile.";

                    WireInputs(effect, *node, graph);
                    if (node->dirty)
                    {
                        ApplyProperties(effect, *node);
                        node->dirty = false;
                    }
                    winrt::com_ptr<ID2D1Image> output;
                    effect->GetOutput(output.put());
                    node->cachedOutput = output.get();
                }
                break;
            }

            case NodeType::Output:
            {
                // The output node simply passes through the first input.
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

        return finalOutput;
    }

    // -----------------------------------------------------------------------
    // Effect cache management
    // -----------------------------------------------------------------------

    void GraphEvaluator::ReleaseCache()
    {
        m_effectCache.clear();
        m_customImplCache.clear();
    }

    void GraphEvaluator::InvalidateNode(uint32_t nodeId)
    {
        m_effectCache.erase(nodeId);
        m_customImplCache.erase(nodeId);
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
            UINT32 ic = static_cast<UINT32>(node.customEffect->inputNames.size());
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
            m_customImplCache[node.id] = { Effects::CustomPixelShaderEffect::s_lastCreated, nullptr };
        }
        else if (node.type == NodeType::ComputeShader && Effects::CustomComputeShaderEffect::s_lastCreated)
        {
            m_customImplCache[node.id] = { nullptr, Effects::CustomComputeShaderEffect::s_lastCreated };
        }

        auto* raw = effect.get();
        m_effectCache[node.id] = std::move(effect);
        return raw;
    }

    // -----------------------------------------------------------------------
    // Property application
    // -----------------------------------------------------------------------

    void GraphEvaluator::ApplyProperties(ID2D1Effect* effect, const EffectNode& node)
    {
        if (!effect)
            return;

        // D2D built-in effects use indexed properties (0, 1, 2, ...).
        // The property map in EffectNode uses string keys. We map numeric
        // string keys ("0", "1", ...) directly to D2D property indices.
        // Named keys are resolved through the effect's property name table.
        for (const auto& [key, value] : node.properties)
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
    // Custom effect application
    // -----------------------------------------------------------------------

    void GraphEvaluator::ApplyCustomEffect(ID2D1Effect* effect, EffectNode& node)
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
                auto propIt = node.properties.find(
                    std::wstring(var.name.begin(), var.name.end()));
                if (propIt == node.properties.end()) continue;

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
                implIt->second.pixelImpl->SetConstantBufferData(cbData.data(), static_cast<UINT32>(cbData.size()));
            else if (node.type == NodeType::ComputeShader && implIt->second.computeImpl)
                implIt->second.computeImpl->SetConstantBufferData(cbData.data(), static_cast<UINT32>(cbData.size()));
        }
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
        if (!m_analysisTarget || m_analysisTargetW != w || m_analysisTargetH != h)
        {
            m_analysisTarget = nullptr;
            D2D1_BITMAP_PROPERTIES1 props = {};
            props.pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED };
            props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
            HRESULT hr = dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, m_analysisTarget.put());
            if (FAILED(hr)) { dc->SetTarget(prevTarget.get()); return; }
            m_analysisTargetW = w;
            m_analysisTargetH = h;
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
}
