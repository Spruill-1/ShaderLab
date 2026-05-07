#include "pch_engine.h"
#include "GraphEvaluator.h"
#include "../Effects/ShaderCompiler.h"
#include "../Effects/BytecodeCache.h"
#include "MathExpression.h"

using namespace ShaderLab::Graph;

namespace ShaderLab::Rendering
{
    // ---- Phase 8 cache routing helpers (file-scope) -----------------------
    //
    // Build a BytecodeCompileRequest from a node's CustomEffectDefinition,
    // then synchronously fetch-or-compile via BytecodeCache. Centralizes
    // the canonicalize -> hash -> request pattern shared by:
    //   * ShaderLab built-in effects (PixelShader / D2D-tiled compute), and
    //   * D3D11 compute custom effects (the bridge's lazy compile).
    //
    // gpuBindableNames is filtered from def.parameters in stable index
    // order; today no built-in effect sets gpuBindable=true so the list is
    // empty and the bitset is 0 (= the baseline variant).
    namespace {
        Effects::BytecodeCacheResult CompileViaCache(
            const std::wstring& effectId,
            uint32_t            effectVersion,
            const std::wstring& hlslSource,
            const std::string&  target,
            uint32_t            macroBitset,
            const std::vector<std::string>& gpuBindableNames)
        {
            std::string canonical = Effects::CanonicalizeHlslSource(hlslSource);

            Effects::BytecodeCompileRequest req;
            req.key.sourceHash         = Effects::HashCanonicalSource(canonical);
            req.key.paramSignatureHash = Effects::HashParamSignature(gpuBindableNames);
            req.key.includeLibraryHash = Effects::IncludeLibraryHash();
            req.key.macroBitset        = macroBitset;
            req.key.entryPoint         = "main";
            req.key.target             = target;
            req.metadata.effectId      = effectId;
            req.metadata.version       = effectVersion;
            req.hlslSource             = std::move(canonical);
            req.gpuBindableParamNames  = gpuBindableNames;

            return Effects::BytecodeCache::Instance().GetOrCompile(std::move(req));
        }

        // Filter a definition's parameters to those flagged gpuBindable,
        // returning their UTF-8 names in declared order. Empty today
        // (no built-in effect has gpuBindable=true) but ready for
        // p8-migrate-ictcp.
        std::vector<std::string> ExtractGpuBindableNames(
            const ShaderLab::Graph::CustomEffectDefinition& def)
        {
            std::vector<std::string> names;
            names.reserve(def.parameters.size());
            for (const auto& p : def.parameters)
            {
                if (!p.gpuBindable) continue;
                std::string utf8;
                int needed = ::WideCharToMultiByte(
                    CP_UTF8, 0, p.name.data(), static_cast<int>(p.name.size()),
                    nullptr, 0, nullptr, nullptr);
                if (needed > 0)
                {
                    utf8.resize(static_cast<size_t>(needed));
                    ::WideCharToMultiByte(
                        CP_UTF8, 0, p.name.data(), static_cast<int>(p.name.size()),
                        utf8.data(), needed, nullptr, nullptr);
                }
                names.push_back(std::move(utf8));
            }
            return names;
        }
    }
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

        // Propagate dirty downstream in topological order. If any node is
        // dirty at the start of this frame (e.g. a Source whose video frame
        // just uploaded, or a node whose property changed), every node it
        // feeds is also dirty — they need to re-evaluate against the new
        // upstream output. Without this, analysis-only compute nodes
        // (Channel/Luminance/Chromaticity Statistics) freeze their fields
        // on the first frame because their own properties never change
        // even as the input image content does.
        for (uint32_t nodeId : order)
        {
            auto* n = graph.FindNode(nodeId);
            if (!n || !n->dirty) continue;
            for (const auto* edge : graph.GetOutputEdges(nodeId))
            {
                if (auto* dn = graph.FindNode(edge->destNodeId))
                    dn->dirty = true;
            }
        }

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
                // (by WIC image loading, Flood effect, or video provider).
                // Resolve property bindings (e.g., Clock → Video.Time).
                if (!node->propertyBindings.empty())
                {
                    std::map<std::wstring, PropertyValue> effectiveProps;
                    bool bindingsChanged = ResolveBindings(*node, graph, effectiveProps);
                    if (bindingsChanged)
                    {
                        for (const auto& [propName, binding] : node->propertyBindings)
                        {
                            auto eit = effectiveProps.find(propName);
                            if (eit != effectiveProps.end())
                                node->properties[propName] = eit->second;
                        }
                        node->dirty = true;
                    }
                }
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

                    // Write resolved binding values back for UI display.
                    if (bindingsChanged)
                    {
                        for (const auto& [propName, binding] : node->propertyBindings)
                        {
                            auto eit = effectiveProps.find(propName);
                            if (eit != effectiveProps.end())
                                node->properties[propName] = eit->second;
                        }
                    }

                    if (node->dirty || bindingsChanged)
                    {
                        ApplyProperties(effect, *node, effectiveProps);
                        node->dirty = false;
                    }

                    // The effect's output is an ID2D1Image. Take ownership so the
                    // image survives D2D's internal pipeline churn (input toggles,
                    // property reapply) until the effect itself is invalidated.
                    winrt::com_ptr<ID2D1Image> output;
                    effect->GetOutput(output.put());
                    m_outputCache[nodeId] = output;
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
                // D3D11 Compute Shader: defer dispatch until after all D2D effects are initialized.
                if (node->customEffect.has_value() &&
                    node->customEffect->shaderType == CustomShaderType::D3D11ComputeShader)
                {
                    // Resolve property bindings for D3D11 compute nodes.
                    if (!node->propertyBindings.empty())
                    {
                        std::map<std::wstring, PropertyValue> effectiveProps;
                        bool bindingsChanged = ResolveBindings(*node, graph, effectiveProps);
                        if (bindingsChanged)
                        {
                            for (const auto& [propName, binding] : node->propertyBindings)
                            {
                                auto eit = effectiveProps.find(propName);
                                if (eit != effectiveProps.end())
                                    node->properties[propName] = eit->second;
                            }
                            node->dirty = true;
                        }
                    }

                    auto inputs = graph.GetInputEdges(nodeId);
                    ID2D1Image* inputImage = nullptr;
                    if (!inputs.empty())
                    {
                        auto* srcNode = graph.FindNode(inputs[0]->sourceNodeId);
                        if (srcNode) inputImage = srcNode->cachedOutput;
                    }
                    bool hasImageOutput = !node->outputPins.empty();
                    // Image-producing compute: recompute when dirty or no cached output.
                    // Analysis-only compute: also recompute if no analysis fields yet.
                    bool needsCompute = node->dirty ||
                        (hasImageOutput && !node->cachedOutput) ||
                        (!hasImageOutput && node->analysisOutput.fields.empty());
                    if (inputImage && needsCompute)
                    {
                        // Phase 8: ensure the bridge effect exists for this
                        // node so ProcessDeferredCompute can drive it via
                        // ICustomComputeBridge. CreateOrGetEffect captures
                        // the impl into m_bridgeImplCache.
                        GetOrCreateEffect(dc, *node);

                        // For image-producing compute, pre-render the upstream
                        // D2D chain to a bitmap NOW while properties are fresh.
                        // This avoids D2D GPU caching returning stale data when
                        // ProcessDeferredCompute renders later inside BeginDraw.
                        winrt::com_ptr<ID2D1Bitmap1> preRendered;
                        if (hasImageOutput)
                            preRendered = PreRenderInputBitmap(dc, inputImage);
                        m_deferredCompute.push_back({ nodeId, inputImage, preRendered });
                    }
                    node->dirty = false;
                    if (!hasImageOutput)
                        node->cachedOutput = nullptr;
                    break;
                }

                // Parameter nodes: no HLSL, just expose property values as analysis output.
                if (node->customEffect.has_value() &&
                    node->customEffect->hlslSource.empty() &&
                    node->customEffect->analysisOutputType == AnalysisOutputType::Typed &&
                    !node->customEffect->analysisFields.empty())
                {
                    // Resolve property bindings for parameter/math/clock nodes.
                    if (!node->propertyBindings.empty())
                    {
                        std::map<std::wstring, PropertyValue> effectiveProps;
                        bool bindingsChanged = ResolveBindings(*node, graph, effectiveProps);
                        if (bindingsChanged)
                        {
                            for (const auto& [propName, binding] : node->propertyBindings)
                            {
                                auto eit = effectiveProps.find(propName);
                                if (eit != effectiveProps.end())
                                    node->properties[propName] = eit->second;
                            }
                        }
                    }

                    node->analysisOutput.type = AnalysisOutputType::Typed;
                    node->analysisOutput.fields.clear();

                    if (node->isClock)
                    {
                        // Clock nodes compute Time/Progress from internal clock state.
                        float startTime = 0.0f, stopTime = 10.0f;
                        auto stIt = node->properties.find(L"StartTime");
                        if (stIt != node->properties.end())
                            if (auto* f = std::get_if<float>(&stIt->second)) startTime = *f;
                        auto etIt = node->properties.find(L"StopTime");
                        if (etIt != node->properties.end())
                            if (auto* f = std::get_if<float>(&etIt->second)) stopTime = *f;
                        double duration = static_cast<double>(stopTime - startTime);
                        if (duration <= 0.0) duration = 1.0;

                        float currentTime = startTime + static_cast<float>(node->clockTime);
                        float progress = static_cast<float>(node->clockTime / duration);

                        for (const auto& fd : node->customEffect->analysisFields)
                        {
                            AnalysisFieldValue fv;
                            fv.name = fd.name;
                            fv.type = fd.type;
                            if (fd.name == L"Time") fv.components[0] = currentTime;
                            else if (fd.name == L"Progress") fv.components[0] = progress;
                            node->analysisOutput.fields.push_back(std::move(fv));
                        }
                    }
                    else if (node->customEffect.has_value() &&
                            !node->customEffect->shaderLabEffectId.empty() &&
                            node->customEffect->shaderLabEffectId == L"Math Expression")
                    {
                        // Numeric Expression node: evaluate the user-supplied
                        // formula with each declared float parameter (A, B, C, ...)
                        // bound by name. Inputs are dynamic — the parameter list
                        // on the node defines which variables exist.
                        std::wstring expr;
                        auto eIt = node->properties.find(L"Expression");
                        if (eIt != node->properties.end())
                            if (auto* s = std::get_if<std::wstring>(&eIt->second)) expr = *s;

                        std::vector<std::wstring> nameStrs;
                        std::vector<const wchar_t*> namePtrs;
                        std::vector<float> vals;
                        nameStrs.reserve(node->customEffect->parameters.size());
                        namePtrs.reserve(node->customEffect->parameters.size());
                        vals.reserve(node->customEffect->parameters.size());
                        for (const auto& p : node->customEffect->parameters)
                        {
                            if (p.name == L"Expression") continue;
                            if (p.typeName != L"float") continue;
                            float v = 0.0f;
                            auto it = node->properties.find(p.name);
                            if (it != node->properties.end())
                                if (auto* f = std::get_if<float>(&it->second)) v = *f;
                            nameStrs.push_back(p.name);
                            vals.push_back(v);
                        }
                        for (auto& n : nameStrs) namePtrs.push_back(n.c_str());

                        float result = 0.0f;
                        std::wstring err;
                        bool ok = EvaluateMathExpression(
                            expr, namePtrs.data(), vals.data(), vals.size(),
                            result, &err);
                        node->runtimeError = ok ? std::wstring{} : err;

                        AnalysisFieldValue fv;
                        fv.name = L"Result";
                        fv.type = AnalysisFieldType::Float;
                        fv.components[0] = result;
                        node->analysisOutput.fields.push_back(std::move(fv));
                    }
                    else if (node->customEffect.has_value() &&
                            !node->customEffect->shaderLabEffectId.empty() &&
                            node->customEffect->shaderLabEffectId == L"Random")
                    {
                        // Random Parameter Node: hash the Seed float into a
                        // uniform float in [0, 1). Pure function of the seed,
                        // so identical seeds reproduce identical output and
                        // any change to the seed (e.g. tick of an upstream
                        // Clock) yields a new well-mixed value. Uses a
                        // SplitMix64-style integer mixer on the bit pattern
                        // of the float — much better distribution than a
                        // raw `sinf(seed)` trick and stays deterministic
                        // across builds / architectures.
                        float seed = 0.0f;
                        auto sIt = node->properties.find(L"Seed");
                        if (sIt != node->properties.end())
                            if (auto* f = std::get_if<float>(&sIt->second)) seed = *f;

                        uint32_t bits = 0;
                        std::memcpy(&bits, &seed, sizeof(bits));
                        uint64_t z = static_cast<uint64_t>(bits) * 0x9E3779B97F4A7C15ULL
                                     + 0xBF58476D1CE4E5B9ULL;
                        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
                        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
                        z =  z ^ (z >> 31);
                        // Take the top 24 bits and divide into [0, 1) so the
                        // result has the full mantissa precision of a float.
                        const uint32_t mantissa = static_cast<uint32_t>(z >> 40);
                        const float result01 = static_cast<float>(mantissa) / 16777216.0f;

                        AnalysisFieldValue fv;
                        fv.name = L"Result";
                        fv.type = AnalysisFieldType::Float;
                        fv.components[0] = result01;
                        node->analysisOutput.fields.push_back(std::move(fv));
                    }
                    else
                    {
                        // Regular parameter nodes: copy property values into
                        // the analysis output fields. Supports the full
                        // PropertyValue variant — host-managed nodes (e.g.
                        // Working Space) write float2/float3/float4 plus
                        // bool / int / uint into properties keyed by field
                        // name and the evaluator unpacks them here.
                        for (const auto& fd : node->customEffect->analysisFields)
                        {
                            AnalysisFieldValue fv;
                            fv.name = fd.name;
                            fv.type = fd.type;
                            auto propIt = node->properties.find(fd.name);
                            if (propIt != node->properties.end())
                            {
                                const auto& pv = propIt->second;
                                if (auto* f = std::get_if<float>(&pv))
                                {
                                    fv.components[0] = *f;
                                }
                                else if (auto* b = std::get_if<bool>(&pv))
                                {
                                    fv.components[0] = *b ? 1.0f : 0.0f;
                                }
                                else if (auto* i = std::get_if<int32_t>(&pv))
                                {
                                    fv.components[0] = static_cast<float>(*i);
                                }
                                else if (auto* u = std::get_if<uint32_t>(&pv))
                                {
                                    fv.components[0] = static_cast<float>(*u);
                                }
                                else if (auto* v2 = std::get_if<winrt::Windows::Foundation::Numerics::float2>(&pv))
                                {
                                    fv.components[0] = v2->x;
                                    fv.components[1] = v2->y;
                                }
                                else if (auto* v3 = std::get_if<winrt::Windows::Foundation::Numerics::float3>(&pv))
                                {
                                    fv.components[0] = v3->x;
                                    fv.components[1] = v3->y;
                                    fv.components[2] = v3->z;
                                }
                                else if (auto* v4 = std::get_if<winrt::Windows::Foundation::Numerics::float4>(&pv))
                                {
                                    fv.components[0] = v4->x;
                                    fv.components[1] = v4->y;
                                    fv.components[2] = v4->z;
                                    fv.components[3] = v4->w;
                                }
                            }
                            node->analysisOutput.fields.push_back(std::move(fv));
                        }
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
                    auto gpuNames = ExtractGpuBindableNames(def);
                    auto cached = CompileViaCache(
                        node->name, /*effectVersion*/ 1u,
                        def.hlslSource, target, /*macroBitset*/ 0u, gpuNames);
                    if (cached.status == Effects::BytecodeStatus::Ready)
                    {
                        def.compiledBytecode = std::move(cached.bytecode);
                        CoCreateGuid(&def.shaderGuid);
                        node->dirty = true;
                        // Phase 8 eager precompile: kick off background
                        // compiles for the +N gpu-binding variants so
                        // they're warm if the user later wires a binding
                        // to a gpuBindable param. Idempotent (the cache
                        // dedupes by key); fires once per node per session
                        // since this branch runs only on !isCompiled().
                        if (!gpuNames.empty())
                        {
                            Effects::BytecodeCacheMetadata meta;
                            meta.effectId = node->name;
                            meta.version  = 1u;
                            Effects::BytecodeCache::Instance().PrecompileCommonShapes(
                                meta,
                                Effects::CanonicalizeHlslSource(def.hlslSource),
                                "main", target, gpuNames);
                        }
                    }
                    else
                    {
                        node->runtimeError = L"Auto-compile failed: " + cached.errorMessage;
                        node->cachedOutput = nullptr;
                        m_outputCache.erase(nodeId);
                        break;
                    }
                }

                ID2D1Effect* effect = GetOrCreateEffect(dc, *node);
                if (!effect)
                {
                    node->runtimeError = L"Failed to create D2D effect. Check effect registration.";
                    node->cachedOutput = nullptr;
                    m_outputCache.erase(nodeId);
                    break;
                }
                node->runtimeError.clear();

                if (node->customEffect.has_value() && node->customEffect->isCompiled())
                {
                    // Resolve property bindings every frame.
                    std::map<std::wstring, PropertyValue> effectiveProps;
                    bool bindingsChanged = ResolveBindings(*node, graph, effectiveProps);
                    bool wasDirty = node->dirty || bindingsChanged;

                    // Write resolved binding values back to node properties
                    // so the node graph UI shows live bound values.
                    if (bindingsChanged)
                    {
                        for (const auto& [propName, binding] : node->propertyBindings)
                        {
                            auto eit = effectiveProps.find(propName);
                            if (eit != effectiveProps.end())
                                node->properties[propName] = eit->second;
                        }
                    }

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
                    m_outputCache[nodeId] = output;
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
                        m_outputCache.erase(nodeId);
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
                        m_outputCache[nodeId] = output;
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

    bool GraphEvaluator::ProcessDeferredCompute(
        EffectGraph& graph, ID2D1DeviceContext5* dc)
    {
        if (m_deferredCompute.empty() || !dc) return false;

        bool imageComputeProduced = false;

        for (auto& deferred : m_deferredCompute)
        {
            auto* node = graph.FindNode(deferred.nodeId);
            if (!node || !deferred.inputImage) continue;
            if (!node->customEffect.has_value()) continue;

            // Phase 8: D3D11 compute custom effects (analysis-only and
            // image-producing alike) route through the bridge effect
            // captured during CreateOrGetEffect. The bridge is owned by
            // m_effectCache; the impl pointer is stashed in
            // m_bridgeImplCache.
            auto bit = m_bridgeImplCache.find(node->id);
            if (bit == m_bridgeImplCache.end() || !bit->second)
                continue;
            auto* bridge = bit->second;

            DispatchViaBridge(dc, *node, deferred.inputImage,
                deferred.preRenderedInput.get(), bridge);

            bool hasImageOutput = !node->outputPins.empty();
            if (hasImageOutput && node->cachedOutput)
            {
                imageComputeProduced = true;
                // Mark all downstream nodes dirty so they re-evaluate
                // on the next frame with the new image input.
                std::vector<uint32_t> queue = { node->id };
                for (size_t i = 0; i < queue.size(); ++i)
                {
                    for (const auto* edge : graph.GetOutputEdges(queue[i]))
                    {
                        auto* dn = graph.FindNode(edge->destNodeId);
                        if (dn && !dn->dirty)
                        {
                            dn->dirty = true;
                            queue.push_back(edge->destNodeId);
                        }
                    }
                }
            }
        }
        m_deferredCompute.clear();
        return true;
    }

    // -----------------------------------------------------------------------
    // Bridge-based D3D11 compute dispatch (Phase 8)
    // -----------------------------------------------------------------------
    //
    // Replaces the pre-Phase-8 DispatchUserD3D11Compute (analysis-only)
    // and DispatchImageCompute (image-producing) paths. Both flows now
    // go through CustomComputeBridgeEffect::Dispatch, which:
    //   * pre-renders the upstream D2D chain to an FP32 bitmap,
    //   * lazily compiles the shader on first call (or uses installed
    //     bytecode),
    //   * dispatches with optional u1 image-output binding,
    //   * reads back analysis floats and exposes the analysis SRV via
    //     IEngineComputeOutput for downstream Phase 8 GPU-binding
    //     consumers,
    //   * wraps the image-output texture as an ID2D1Bitmap1.

    void GraphEvaluator::DispatchViaBridge(
        ID2D1DeviceContext5* dc,
        EffectNode& node,
        ID2D1Image* inputImage,
        ID2D1Bitmap1* preRenderedInput,
        Effects::CustomComputeBridgeEffect* bridge)
    {
        if (!bridge) return;
        auto& def = node.customEffect.value();

        // Lazy compile: ProcessDeferredCompute runs inside BeginDraw so
        // we have a valid D3D11 device on the DC. If the host hasn't
        // already populated def.compiledBytecode (e.g. the editor or
        // an MCP /effect/compile call), compile via ShaderCompiler and
        // hand the bytecode to the bridge. Subsequent dispatches reuse
        // the installed shader.
        if (def.compiledBytecode.empty())
        {
            auto gpuNames = ExtractGpuBindableNames(def);
            auto cached = CompileViaCache(
                node.name, /*effectVersion*/ 1u,
                def.hlslSource, "cs_5_0", /*macroBitset*/ 0u, gpuNames);
            if (cached.status != Effects::BytecodeStatus::Ready)
            {
                node.runtimeError = cached.errorMessage.empty()
                    ? L"D3D11 compute shader compile failed"
                    : std::move(cached.errorMessage);
                return;
            }
            def.compiledBytecode = std::move(cached.bytecode);
            bridge->SetCompiledBytecode(def.compiledBytecode.data(),
                static_cast<UINT32>(def.compiledBytecode.size()));
            node.runtimeError.clear();
            // Phase 8 eager precompile (mirror of the ShaderLab built-in
            // case in EvaluateNode).
            if (!gpuNames.empty())
            {
                Effects::BytecodeCacheMetadata meta;
                meta.effectId = node.name;
                meta.version  = 1u;
                Effects::BytecodeCache::Instance().PrecompileCommonShapes(
                    meta,
                    Effects::CanonicalizeHlslSource(def.hlslSource),
                    "main", "cs_5_0", gpuNames);
            }
        }

        // Analysis float4 count: sum over typed-field pixel counts.
        UINT32 analysisFloat4Count = 0;
        for (const auto& f : def.analysisFields)
            analysisFloat4Count += f.pixelCount();

        // Image-output dimensions: DiagramSize / OutputSize, fallback
        // to upstream bounds. 0 / 0 = analysis-only (no u1 binding).
        UINT32 imageOutW = 0, imageOutH = 0;
        bool hasImageOutput = !node.outputPins.empty();
        if (hasImageOutput)
        {
            for (const auto& p : def.parameters)
            {
                if (p.name == L"DiagramSize" || p.name == L"OutputSize")
                {
                    auto it = node.properties.find(p.name);
                    if (it != node.properties.end())
                    {
                        if (auto* f = std::get_if<float>(&it->second))
                            imageOutW = imageOutH = static_cast<UINT32>((std::max)(*f, 64.0f));
                        else if (auto* u = std::get_if<uint32_t>(&it->second))
                            imageOutW = imageOutH = (std::max)(*u, 64u);
                    }
                    break;
                }
            }
            if (imageOutW == 0)
            {
                // Fall back to upstream input dimensions (96 DPI bounds
                // from the pre-rendered bitmap, if any, else the input
                // image's bounds).
                if (preRenderedInput)
                {
                    auto sz = preRenderedInput->GetPixelSize();
                    imageOutW = sz.width; imageOutH = sz.height;
                }
                else
                {
                    float oldDpiX, oldDpiY;
                    dc->GetDpi(&oldDpiX, &oldDpiY);
                    dc->SetDpi(96.0f, 96.0f);
                    D2D1_RECT_F bounds{};
                    dc->GetImageLocalBounds(inputImage, &bounds);
                    imageOutW = static_cast<UINT32>((std::min)(bounds.right - bounds.left, 8192.0f));
                    imageOutH = static_cast<UINT32>((std::min)(bounds.bottom - bounds.top, 8192.0f));
                    dc->SetDpi(oldDpiX, oldDpiY);
                }
                if (imageOutW < 64) imageOutW = 64;
                if (imageOutH < 64) imageOutH = 64;
            }
        }

        // Pack cbuffer (user portion only -- the runner prepends
        // Width/Height). Use the typed PackPropertyToCBuffer helper
        // (Phase 3) so HLSL `uint`/`int`/`bool` slots receive the
        // correctly converted scalar instead of a float bit pattern.
        std::vector<BYTE> cbBytes;
        if (!def.parameters.empty() && !def.compiledBytecode.empty())
        {
            winrt::com_ptr<ID3D11ShaderReflection> reflect;
            HRESULT hr = D3DReflect(
                def.compiledBytecode.data(), def.compiledBytecode.size(),
                IID_ID3D11ShaderReflection, reinterpret_cast<void**>(reflect.put()));
            if (SUCCEEDED(hr) && reflect)
            {
                auto* cbReflect = reflect->GetConstantBufferByIndex(0);
                D3D11_SHADER_BUFFER_DESC cbDesc{};
                if (cbReflect && SUCCEEDED(cbReflect->GetDesc(&cbDesc)) && cbDesc.Size > 8)
                {
                    UINT32 userSize = cbDesc.Size - 8;
                    cbBytes.assign(userSize, BYTE{ 0 });
                    for (UINT v = 0; v < cbDesc.Variables; ++v)
                    {
                        auto* var = cbReflect->GetVariableByIndex(v);
                        D3D11_SHADER_VARIABLE_DESC varDesc{};
                        if (!var || FAILED(var->GetDesc(&varDesc))) continue;
                        if (varDesc.StartOffset < 8) continue; // skip Width/Height

                        std::wstring varName(varDesc.Name, varDesc.Name + strlen(varDesc.Name));
                        auto propIt = node.properties.find(varName);
                        if (propIt == node.properties.end()) continue;

                        UINT32 destOff = varDesc.StartOffset - 8;
                        if (destOff >= cbBytes.size()) continue;
                        UINT32 remaining = static_cast<UINT32>(cbBytes.size() - destOff);

                        D3D11_SHADER_TYPE_DESC typeDesc{};
                        D3D_SHADER_VARIABLE_TYPE hlslType = D3D_SVT_FLOAT;
                        UINT cols = 1;
                        if (auto* typeInfo = var->GetType();
                            typeInfo && SUCCEEDED(typeInfo->GetDesc(&typeDesc)))
                        {
                            hlslType = typeDesc.Type;
                            cols = typeDesc.Columns;
                        }

                        Effects::PackPropertyToCBuffer(
                            cbBytes.data() + destOff, remaining,
                            hlslType, cols, propIt->second);
                    }
                }
            }
        }

        // Drive the dispatch through the bridge. The bridge handles
        // pre-rendering internally; if we have a pre-rendered bitmap
        // it's a cheap blit (same FP32 format). Use it directly when
        // available so D2D doesn't re-evaluate upstream effects with
        // potentially-different cached state.
        ID2D1Image* dispatchInput = preRenderedInput
            ? static_cast<ID2D1Image*>(preRenderedInput)
            : inputImage;

        std::vector<float> analysisFloats;
        HRESULT hr = bridge->Dispatch(
            dc, dispatchInput,
            cbBytes.empty() ? nullptr : cbBytes.data(),
            static_cast<UINT32>(cbBytes.size()),
            analysisFloat4Count,
            imageOutW, imageOutH,
            &analysisFloats);
        if (FAILED(hr))
        {
            node.runtimeError = std::format(L"Bridge dispatch failed 0x{:08X}",
                static_cast<uint32_t>(hr));
            return;
        }
        node.runtimeError.clear();

        // Unpack analysis floats into typed fields.
        if (analysisFloat4Count > 0)
        {
            node.analysisOutput.type = AnalysisOutputType::Typed;
            node.analysisOutput.fields.clear();
            UINT32 pixelOffset = 0;
            for (const auto& fd : def.analysisFields)
            {
                AnalysisFieldValue fv;
                fv.name = fd.name;
                fv.type = fd.type;

                UINT32 pc = fd.pixelCount();
                bool isArray = AnalysisFieldIsArray(fd.type);
                UINT32 cc = AnalysisFieldComponentCount(fd.type);

                if (!isArray)
                {
                    if (pixelOffset * 4 < analysisFloats.size())
                    {
                        for (UINT32 c = 0; c < cc && (pixelOffset * 4 + c) < analysisFloats.size(); ++c)
                            fv.components[c] = analysisFloats[pixelOffset * 4 + c];
                    }
                }
                else
                {
                    fv.arrayData.resize(fd.arrayLength * cc, 0.0f);
                    for (UINT32 i = 0; i < fd.arrayLength; ++i)
                    {
                        UINT32 base = (pixelOffset + i) * 4;
                        for (UINT32 c = 0; c < cc && (base + c) < analysisFloats.size(); ++c)
                            fv.arrayData[i * cc + c] = analysisFloats[base + c];
                    }
                }
                pixelOffset += pc;
                node.analysisOutput.fields.push_back(std::move(fv));
            }
        }

        // Image-producing nodes wire their output to the bridge's
        // wrapped bitmap. Downstream nodes consume node->cachedOutput
        // directly, same as for D2D-tiled compute / pixel-shader nodes.
        if (hasImageOutput)
            node.cachedOutput = bridge->GetImageOutput();
        else
            node.cachedOutput = nullptr;
    }

    // -----------------------------------------------------------------------
    // Effect cache management
    // -----------------------------------------------------------------------

    void GraphEvaluator::ReleaseCache()
    {
        m_effectCache.clear();
        m_outputCache.clear();
        m_customImplCache.clear();
        m_bridgeImplCache.clear();
        m_dummySourceBitmap = nullptr;
    }

    void GraphEvaluator::ReleaseCache(EffectGraph& graph)
    {
        ReleaseCache();
        // EffectNode::cachedOutput is a non-owning raw pointer whose lifetime is
        // owned by the caches we just cleared. Null every node's pointer so the
        // render path can't dereference a freed ID2D1Image.
        for (auto& node : const_cast<std::vector<EffectNode>&>(graph.Nodes()))
        {
            node.cachedOutput = nullptr;
            node.dirty = true;
        }
    }

    void GraphEvaluator::InvalidateNode(uint32_t nodeId)
    {
        m_effectCache.erase(nodeId);
        m_outputCache.erase(nodeId);
        m_customImplCache.erase(nodeId);
        m_bridgeImplCache.erase(nodeId);
        // Note: caller must also clear EffectNode::cachedOutput on the node
        // (the raw pointer it holds is now dangling). Prefer the graph-aware
        // overload below.
    }

    void GraphEvaluator::InvalidateNode(EffectGraph& graph, uint32_t nodeId)
    {
        InvalidateNode(nodeId);
        if (auto* node = graph.FindNode(nodeId))
        {
            node->cachedOutput = nullptr;
            node->dirty = true;
        }
    }

    void GraphEvaluator::ResolveSourceBindings(EffectGraph& graph)
    {
        for (auto& node : const_cast<std::vector<EffectNode>&>(graph.Nodes()))
        {
            if (node.type != NodeType::Source) continue;
            if (node.propertyBindings.empty()) continue;

            std::map<std::wstring, PropertyValue> effectiveProps;
            bool changed = ResolveBindings(node, graph, effectiveProps);
            if (changed)
            {
                for (const auto& [propName, binding] : node.propertyBindings)
                {
                    auto eit = effectiveProps.find(propName);
                    if (eit != effectiveProps.end())
                        node.properties[propName] = eit->second;
                }
                node.dirty = true;
            }
        }
    }

    void GraphEvaluator::UpdateNodeShader(uint32_t nodeId, const EffectNode& node)
    {
        if (!node.customEffect.has_value() || !node.customEffect->isCompiled())
            return;

        auto& def = node.customEffect.value();

        // D3D11 compute shaders: clear the bridge cache so the next
        // CreateOrGetEffect creates a fresh bridge with the new bytecode.
        if (def.shaderType == CustomShaderType::D3D11ComputeShader)
        {
            m_effectCache.erase(nodeId);
            m_bridgeImplCache.erase(nodeId);
            return;
        }
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
        bool isD3D11Compute = node.customEffect.has_value() &&
            node.customEffect->shaderType == CustomShaderType::D3D11ComputeShader;

        if (isD3D11Compute)
        {
            // Phase 8: D3D11 compute custom effects route through the
            // shared CustomComputeBridgeEffect. The bridge satisfies
            // D2D's "one input -> one output" contract via a passthrough
            // pixel shader; the actual D3D11 compute dispatch happens
            // out-of-band in ProcessDeferredCompute via QI for
            // ICustomComputeBridge. Single CLSID for all instances --
            // bytecode is set per-instance after CreateEffect via
            // SetCompiledBytecode. The bridge is registered once at
            // engine startup in RegisterEngineD2DEffects.
            clsid = Effects::CustomComputeBridgeEffect::CLSID_CustomComputeBridge;
        }
        else if ((node.type == NodeType::PixelShader || node.type == NodeType::ComputeShader) &&
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
        Effects::CustomComputeBridgeEffect::s_lastCreated = nullptr;

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
        else if (isD3D11Compute && Effects::CustomComputeBridgeEffect::s_lastCreated)
        {
            // Phase 8: bridge effect captured. Install pre-compiled
            // bytecode now if the host has it (compiled via
            // ShaderCompiler at MCP /effect/compile time or by the
            // editor on Ctrl+Enter). If not yet compiled, the
            // ProcessDeferredCompute pass will compile lazily on
            // first dispatch and call SetCompiledBytecode then.
            auto* bridge = Effects::CustomComputeBridgeEffect::s_lastCreated;
            m_bridgeImplCache[node.id] = bridge;
            if (node.customEffect.has_value() &&
                !node.customEffect->compiledBytecode.empty())
            {
                bridge->SetCompiledBytecode(
                    node.customEffect->compiledBytecode.data(),
                    static_cast<UINT32>(node.customEffect->compiledBytecode.size()));
            }
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
    // Pre-render upstream D2D chain to FP32 bitmap at 96 DPI
    // -----------------------------------------------------------------------

    winrt::com_ptr<ID2D1Bitmap1> GraphEvaluator::PreRenderInputBitmap(
        ID2D1DeviceContext5* dc, ID2D1Image* inputImage)
    {
        if (!dc || !inputImage) return nullptr;

        float oldDpiX, oldDpiY;
        dc->GetDpi(&oldDpiX, &oldDpiY);
        dc->SetDpi(96.0f, 96.0f);

        D2D1_RECT_F bounds{};
        dc->GetImageLocalBounds(inputImage, &bounds);
        uint32_t w = static_cast<uint32_t>((std::min)(bounds.right - bounds.left, 8192.0f));
        uint32_t h = static_cast<uint32_t>((std::min)(bounds.bottom - bounds.top, 8192.0f));
        if (w == 0 || h == 0) { dc->SetDpi(oldDpiX, oldDpiY); return nullptr; }

        winrt::com_ptr<ID2D1Bitmap1> bitmap;
        D2D1_BITMAP_PROPERTIES1 props{};
        props.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
        props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
        props.dpiX = 96.0f;
        props.dpiY = 96.0f;
        HRESULT hr = dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, bitmap.put());
        if (FAILED(hr)) { dc->SetDpi(oldDpiX, oldDpiY); return nullptr; }

        // Render inside a standalone BeginDraw/EndDraw pair so the D2D
        // effect chain is fully evaluated with current property values.
        winrt::com_ptr<ID2D1Image> prevTarget;
        dc->GetTarget(prevTarget.put());
        dc->SetTarget(bitmap.get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->DrawImage(inputImage, D2D1::Point2F(-bounds.left, -bounds.top));
        dc->EndDraw();
        dc->SetTarget(prevTarget.get());

        dc->SetDpi(oldDpiX, oldDpiY);
        return bitmap;
    }

}

