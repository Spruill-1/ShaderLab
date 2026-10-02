#include "pch_engine.h"
#include "GraphEvaluator.h"
#include "../Effects/ShaderCompiler.h"
#include "../Effects/BytecodeCache.h"
#include "../Effects/ShaderVariants.h"
#include "../Effects/Performance.h"
#include "../Effects/IEngineComputeOutput.h"
#include "MathExpression.h"
#include "../Effects/ShaderLabEffects.h"

using namespace ShaderLab::Graph;

namespace
{
    // A resolved binding must never carry a non-finite value into
    // node->properties: that map is what /graph/save serialises, and a NaN
    // lands in the document as 1.#QNAN -- an .effectgraph no JSON parser will
    // load. Analysis fields legitimately carry NaN (one bad texel is enough)
    // and binding one to a property is a recipe this project recommends, so
    // the guard belongs at the write-back.
    //
    // Covers EVERY float-bearing alternative, not just scalar float: a float2
    // binding was still corrupting documents while the scalar path was
    // guarded, which is the same partial-sweep mistake twice over.
    bool IsFinitePropertyValue(const ::ShaderLab::Graph::PropertyValue& v)
    {
        using namespace winrt::Windows::Foundation::Numerics;
        if (const auto* f = std::get_if<float>(&v))   return std::isfinite(*f);
        if (const auto* f2 = std::get_if<float2>(&v)) return std::isfinite(f2->x) && std::isfinite(f2->y);
        if (const auto* f3 = std::get_if<float3>(&v))
            return std::isfinite(f3->x) && std::isfinite(f3->y) && std::isfinite(f3->z);
        if (const auto* f4 = std::get_if<float4>(&v))
            return std::isfinite(f4->x) && std::isfinite(f4->y) && std::isfinite(f4->z) && std::isfinite(f4->w);
        if (const auto* vec = std::get_if<std::vector<float>>(&v))
        {
            for (float e : *vec) if (!std::isfinite(e)) return false;
            return true;
        }
        if (const auto* m = std::get_if<D2D1_MATRIX_5X4_F>(&v))
        {
            for (int i = 0; i < 20; ++i)
                if (!std::isfinite(reinterpret_cast<const float*>(m)[i])) return false;
            return true;
        }
        return true;   // int / uint / bool / wstring cannot be non-finite
    }
}

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
        // The cache request for one variant of a definition. Option names go
        // with every compile, the baseline included: SHADERLAB_OPTION needs
        // _SLOPT_<name>_MODE defined even for the generic build.
        Effects::BytecodeCompileRequest MakeCompileRequest(
            const std::wstring& effectId,
            uint32_t            effectVersion,
            std::string         canonical,
            const std::string&  target,
            uint32_t            macroBitset,
            const std::vector<std::string>& gpuBindableNames,
            uint64_t            optionKey,
            const std::vector<std::string>& optionNames)
        {
            Effects::BytecodeCompileRequest req;
            req.key.sourceHash         = Effects::HashCanonicalSource(canonical);
            req.key.paramSignatureHash = Effects::HashParamSignature(gpuBindableNames, optionNames);
            req.key.includeLibraryHash = Effects::IncludeLibraryHash();
            req.key.macroBitset        = macroBitset;
            req.key.optionKey          = optionKey;
            req.key.entryPoint         = "main";
            req.key.target             = target;
            req.metadata.effectId      = effectId;
            req.metadata.version       = effectVersion;
            req.hlslSource             = std::move(canonical);
            req.gpuBindableParamNames  = gpuBindableNames;
            req.optionParamNames       = optionNames;
            return req;
        }

        Effects::BytecodeCacheResult CompileViaCache(
            const std::wstring& effectId,
            uint32_t            effectVersion,
            const std::wstring& hlslSource,
            const std::string&  target,
            uint32_t            macroBitset,
            const std::vector<std::string>& gpuBindableNames,
            bool                async,
            uint64_t            optionKey = 0,
            const std::vector<std::string>& optionNames = {},
            bool                urgent = false,
            Effects::BytecodeCompileKey* outKey = nullptr)
        {
            auto request = MakeCompileRequest(effectId, effectVersion,
                Effects::CanonicalizeHlslSource(hlslSource), target,
                macroBitset, gpuBindableNames, optionKey, optionNames);
            if (outKey) *outKey = request.key;

            auto& cache = Effects::BytecodeCache::Instance();
            if (async)
            {
                // Take what the cache has, or queue the compile and report Pending.
                auto existing = cache.TryGet(request.key);
                if (existing.status == Effects::BytecodeStatus::Ready ||
                    existing.status == Effects::BytecodeStatus::Failed)
                    return existing;
                cache.RequestCompile(std::move(request), urgent);   // idempotent while Pending
                Effects::BytecodeCacheResult pending;
                pending.status = Effects::BytecodeStatus::Pending;
                return pending;
            }
            return cache.GetOrCompile(std::move(request));
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

        // Re-dirty nodes whose wanted variant finished compiling.
        PollVariantCompiles(graph);

        // NOTE on deferred-compute ownership:
        //   `DeferredCompute::inputImages` holds owning `winrt::com_ptr<ID2D1Image>`
        //   entries (not raw pointers), so even if a subsequent Evaluate
        //   pass within the same render iteration rebuilds the producing
        //   effect, this pass's entry keeps the image chain alive. We do
        //   NOT clear m_deferredCompute here.
        //
        //   We used to clear at the top of Evaluate to prevent a UAF from
        //   stale raw pointers across a two-pass render iteration. With
        //   owning refs in place, that defence is moot -- and clearing here
        //   actually breaks the steady-state flow: pass 1 pushes a deferred
        //   entry while the node is dirty + fields are stale, pass 2 finds
        //   `dirty=false` and `analysisOutput.fields` already populated
        //   from a previous runEval, so it does NOT re-push. Clearing
        //   between the passes then leaves m_deferredCompute empty and
        //   ProcessDeferredCompute dispatches nothing -- set-property on
        //   an upstream Source has no visible effect on a downstream
        //   compute analysis node.
        //
        //   m_deferredCompute is drained + cleared at the bottom of
        //   ProcessDeferredCompute; the source-not-ready early return
        //   above (in ProcessDeferredCompute) also clears it.

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

        // Collect any async analysis readbacks that have landed since the
        // last pass. This marks their bound consumers dirty, so it has to run
        // BEFORE the walk for them to be re-evaluated in this pass.
        if (!m_asyncReadbackPending.empty())
            PollAsyncReadbacks(graph);

        // Dirty propagation is PULL-based, done at each node's visit below
        // rather than in a pass up front.
        //
        // It used to be a push: walk the order once, and for every node
        // already dirty, mark its successors. That misses two things, and the
        // second one served stale numbers silently for as long as it existed.
        //
        //  1. A property binding is an invalidation edge too, but not a graph
        //     edge. A Clock node has NO output pins and NO edges -- it drives
        //     consumers purely through bindings -- so a push along edges
        //     leaves everything it animates marked clean.
        //
        //  2. A node can become dirty DURING the main loop, when ResolveBindings
        //     sees a bound value move. A pass that ran before the loop cannot
        //     know about that, and by the next frame the flag has already been
        //     cleared -- so the change never propagates on any frame.
        //
        // Together those meant an analysis node downstream of animated content
        // dispatched exactly once, on the frame its analysisOutput was still
        // empty, and then reported cached forever while its input kept moving.
        // Its fields stayed plausible and wrong. CLAUDE.md warns about exactly
        // this shape; here it was structural.
        //
        // Pulling instead is correct by construction: topological order means
        // every predecessor -- edge or binding -- has finished its visit, so
        // m_outputChangedThisEval is final for all of them.
        m_outputChangedThisEval.clear();

        for (uint32_t nodeId : order)
        {
            EffectNode* node = graph.FindNode(nodeId);
            if (!node)
                continue;

            // Did anything this node reads change earlier in the pass?
            if (!node->dirty)
            {
                for (const auto* edge : graph.GetInputEdges(nodeId))
                {
                    if (m_outputChangedThisEval.count(edge->sourceNodeId))
                    { node->dirty = true; break; }
                }
            }
            if (!node->dirty)
            {
                for (const auto& [propName, binding] : node->propertyBindings)
                {
                    if (binding.wholeArray)
                    {
                        if (m_outputChangedThisEval.count(binding.wholeArraySourceNodeId))
                        { node->dirty = true; break; }
                        continue;
                    }
                    for (const auto& src : binding.sources)
                    {
                        if (src.has_value() &&
                            m_outputChangedThisEval.count(src->sourceNodeId))
                        { node->dirty = true; break; }
                    }
                    if (node->dirty) break;
                }
            }

            // Record BEFORE the visit for the start-of-frame case; the binding
            // sites below add themselves when they discover a change mid-visit.
            if (node->dirty)
                m_outputChangedThisEval.insert(nodeId);

            // Skip nodes not needed by any visible output -- but KEEP their
            // pending change. Clearing `dirty` here threw away an edit made
            // while the node was hidden, so switching the preview to it showed
            // stale output; that is why every edit site used to MarkAllDirty.
            // EffectGraph::HasDirtyNodes ignores unneeded nodes, so a parked
            // change does not keep the host rendering.
            // A node waits on its baseline only while it has none and is rendered.
            if (node->compilePending &&
                (!node->needed || !node->customEffect.has_value() || node->customEffect->isCompiled()))
                node->compilePending = false;

            if (!node->needed)
                continue;

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
                            if (eit == effectiveProps.end()) continue;
                            if (!IsFinitePropertyValue(eit->second))
                            {
                                node->runtimeError = L"binding '" + propName +
                                    L"' resolved to a non-finite value; previous kept";
                                continue;
                            }
                            node->properties[propName] = eit->second;
                        }
                        node->dirty = true;
                        m_outputChangedThisEval.insert(nodeId);
                    }
                }
                node->dirty = false;
                break;
            }

            case NodeType::BuiltInEffect:
            {
                ID2D1Effect* effect = GetOrCreateEffect(dc, *node);
                // Creation is retried on every visit whatever `dirty` says, so
                // a failure must not leave the flag set: a node stuck dirty
                // keeps HasDirtyNodes() true and costs every frame a second
                // full Evaluate plus the frozen post-dispatch pass.
                if (!effect)
                    node->dirty = false;
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
                            if (eit == effectiveProps.end()) continue;
                            if (!IsFinitePropertyValue(eit->second))
                            {
                                node->runtimeError = L"binding '" + propName +
                                    L"' resolved to a non-finite value; previous kept";
                                continue;
                            }
                            node->properties[propName] = eit->second;
                        }
                    }

                    const bool wasDirty = node->dirty || bindingsChanged;
                    // A binding that moved changes this node's output, so
                    // downstream consumers have to learn about it too.
                    if (bindingsChanged) m_outputChangedThisEval.insert(nodeId);
                    if (wasDirty)
                    {
                        ApplyProperties(effect, *node, effectiveProps);
                        node->dirty = false;
                    }

                    // Clean-subgraph caching: enable D2D's output cache;
                    // drop it when content changed (upstream in-place
                    // texture updates arrive as node->dirty via the dirty
                    // walks, which D2D itself cannot see).
                    UpdateEffectCachePolicy(effect, nodeId, wasDirty);

                    // The effect's output is an ID2D1Image. Take ownership so the
                    // image survives D2D's internal pipeline churn (input toggles,
                    // property reapply) until the effect itself is invalidated.
                    winrt::com_ptr<ID2D1Image> output;
                    effect->GetOutput(output.put());
                    m_outputCache[nodeId] = output;
                    node->cachedOutput = output.get();

                    // For analysis effects (e.g., Histogram), force computation
                    // by drawing the output, then read back the result property.
                    // Only when something changed -- it used to redraw the whole
                    // image on every pass (2-3 per frame) of a static graph.
                    // And never in the frozen post-dispatch pass: that runs
                    // INSIDE the host's BeginDraw, so the helper's own
                    // BeginDraw nests and fails (D2DERR_WRONG_STATE). Leave
                    // the node dirty instead and read it on the next frame.
                    if (wasDirty && node->effectClsid.has_value() &&
                        IsEqualGUID(node->effectClsid.value(), CLSID_D2D1Histogram))
                    {
                        if (m_deferredComputeFrozen)
                            node->dirty = true;
                        else
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
                                {
                                    if (IsFinitePropertyValue(eit->second))
                                        node->properties[propName] = eit->second;
                                    else
                                        node->runtimeError = L"binding '" + propName +
                                            L"' resolved to a non-finite value; previous kept";
                                }
                            }
                            node->dirty = true;
                            m_outputChangedThisEval.insert(nodeId);
                        }
                    }

                    auto inputs = graph.GetInputEdges(nodeId);
                    // Collect all upstream input images, in pin-index order
                    // (matching customEffect.inputNames). The bridge binds
                    // them at t0..t(N-1).
                    std::vector<winrt::com_ptr<ID2D1Image>> inputImages;
                    {
                        // Find max destPin so we can size by declared input
                        // count. Slots not connected get nullptr (the bridge
                        // returns E_INVALIDARG, surfaced as a runtime error).
                        uint32_t maxPin = 0;
                        for (const auto* e : inputs)
                            if (e->destPin >= maxPin) maxPin = e->destPin + 1;
                        // Also bound by declared input count if known.
                        uint32_t declaredCount = node->customEffect.has_value()
                            ? static_cast<uint32_t>(node->customEffect->inputNames.size())
                            : 0;
                        uint32_t totalSlots = (std::max)(maxPin, declaredCount);
                        if (totalSlots == 0 && !inputs.empty()) totalSlots = 1;
                        inputImages.assign(totalSlots, {});
                        for (const auto* e : inputs)
                        {
                            auto* srcNode = graph.FindNode(e->sourceNodeId);
                            if (srcNode && srcNode->cachedOutput &&
                                e->destPin < inputImages.size())
                            {
                                inputImages[e->destPin].copy_from(srcNode->cachedOutput);
                            }
                        }
                    }
                    // A GENERATOR declares no image inputs: its output is a
                    // function of its parameters alone (a lookup table, say).
                    // The compute path assumes an input 0 throughout -- the
                    // dispatch gate below, the deferred queue, the bridge's
                    // pre-render -- so feed it the 1x1 zero placeholder. That
                    // placeholder is never dirty, which is exactly the point:
                    // the generator re-dispatches only when one of its own
                    // properties moves, and otherwise serves its cached output.
                    if (node->customEffect->inputNames.empty() && inputImages.empty())
                    {
                        EnsureAnalysisDummy(dc);
                        if (m_analysisDummyBitmap)
                            inputImages.emplace_back().copy_from(
                                static_cast<ID2D1Image*>(m_analysisDummyBitmap.get()));
                    }
                    // An unwired lookup input gets the same placeholder, as on
                    // the pixel path: its alpha of 0 is how the shader reads
                    // "no table". A wired pin whose producer has no output yet
                    // stays empty.
                    if (const uint32_t lookupCount = node->customEffect->lookupInputCount; lookupCount > 0)
                    {
                        const uint32_t declared = static_cast<uint32_t>(node->customEffect->inputNames.size());
                        const uint32_t firstLookup = declared - (std::min)(lookupCount, declared);
                        for (uint32_t pin = firstLookup; pin < declared && pin < inputImages.size(); ++pin)
                        {
                            const bool wired = std::any_of(inputs.begin(), inputs.end(),
                                [pin](const auto* edge) { return edge->destPin == pin; });
                            if (wired) continue;
                            EnsureAnalysisDummy(dc);
                            if (m_analysisDummyBitmap)
                                inputImages[pin].copy_from(static_cast<ID2D1Image*>(m_analysisDummyBitmap.get()));
                        }
                    }
                    ID2D1Image* primaryInput = inputImages.empty() ? nullptr : inputImages[0].get();
                    bool hasImageOutput = !node->outputPins.empty();
                    // Image-producing compute: recompute when dirty or no cached output.
                    // Analysis-only compute: also recompute if no analysis fields yet.
                    //
                    // GPU-binding freshness chain (replaces the Phase 8c
                    // redispatch-every-frame-under-skip-readback rule): a
                    // compute consumer must redispatch when one of its
                    // binding SOURCES is queued for dispatch this cycle —
                    // its dispatch reads the source's analysis SRV, whose
                    // content is about to change. When no source is
                    // dispatching, the SRV retains last cycle's values and
                    // an identical redispatch would produce identical
                    // output, so dirty-gating is safe. (The old rule
                    // redispatched EVERY compute node EVERY frame, which
                    // both burned GPU on static graphs and — because each
                    // image-producing dispatch transitively dirties its
                    // downstream — permanently defeated clean-subgraph
                    // output caching.) The topological order includes
                    // binding dependencies, so sources are visited before
                    // their consumers.
                    bool bindingSourceQueued = false;
                    for (const auto& [bpName, b] : node->propertyBindings)
                    {
                        if (b.wholeArray)
                        {
                            if (m_queuedComputeThisEval.count(b.wholeArraySourceNodeId))
                            { bindingSourceQueued = true; break; }
                        }
                        else
                        {
                            for (const auto& s : b.sources)
                            {
                                if (s.has_value() &&
                                    m_queuedComputeThisEval.count(s->sourceNodeId))
                                { bindingSourceQueued = true; break; }
                            }
                        }
                        if (bindingSourceQueued) break;
                    }
                    bool needsCompute = node->dirty ||
                        bindingSourceQueued ||
                        (hasImageOutput && !node->cachedOutput) ||
                        (!hasImageOutput && node->analysisOutput.fields.empty());
                    // Queued at most ONCE per evaluate cycle. m_queuedComputeThisEval
                    // lives until ProcessDeferredCompute drains the queue, so on
                    // the host's second pass a node whose binding source was
                    // queued in the first saw bindingSourceQueued again and was
                    // pushed twice -- two dispatches, two pre-renders, two
                    // readbacks. The dispatch reads the node's state at
                    // ProcessDeferredCompute time, so one entry is always
                    // current.
                    //
                    // No pre-render here any more. This used to render every
                    // input of an image-producing node into a NEW full-size FP32
                    // bitmap (133 MB at 4K) right here, "while properties are
                    // fresh" -- but ProcessDeferredCompute already had to discard
                    // that snapshot whenever the producer was dispatched in the
                    // same pass (it predates the dispatch), and the render it
                    // does instead, inside the draw session, is the fresh one by
                    // construction. It is now the only one, into a persistent
                    // per-producer target.
                    if (primaryInput && needsCompute && !m_deferredComputeFrozen &&
                        !m_queuedComputeThisEval.count(nodeId))
                    {
                        // Phase 8: ensure the bridge effect exists for this
                        // node so ProcessDeferredCompute can drive it via
                        // ICustomComputeBridge. CreateOrGetEffect captures
                        // the impl into m_bridgeImplCache.
                        GetOrCreateEffect(dc, *node);
                        m_deferredCompute.push_back({ nodeId, std::move(inputImages) });
                        m_queuedComputeThisEval.insert(nodeId);
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
                                {
                                    if (IsFinitePropertyValue(eit->second))
                                        node->properties[propName] = eit->second;
                                    else
                                        node->runtimeError = L"binding '" + propName +
                                            L"' resolved to a non-finite value; previous kept";
                                }
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

                        // Quantise to the emit rate, with the SAME rule the
                        // tick uses to decide whether to dirty. If these two
                        // disagreed, a consumer could be woken for a tick and
                        // then read a value from between ticks -- the output
                        // would creep every frame while claiming to be
                        // rate-limited.
                        float updateRate = 0.0f;
                        auto urIt = node->properties.find(L"UpdateRate");
                        if (urIt != node->properties.end())
                            if (auto* f = std::get_if<float>(&urIt->second)) updateRate = *f;

                        double quantized = node->clockTime;
                        if (updateRate > 0.0f)
                        {
                            const double step = 1.0 / static_cast<double>(updateRate);
                            quantized = std::floor(node->clockTime / step) * step;
                        }

                        float currentTime = startTime + static_cast<float>(quantized);
                        float progress = static_cast<float>(quantized / duration);

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
                    // Urgent: without its baseline the node has no output, so
                    // it goes ahead of queued option variants. Those are queued
                    // later, in ApplyCustomEffect, once the GPU bindings are known.
                    auto cached = CompileViaCache(
                        node->name, /*effectVersion*/ 1u,
                        def.hlslSource, target, /*macroBitset*/ 0u, gpuNames, m_asyncCompile,
                        /*optionKey*/ 0, Effects::SpecializedOptionNames(def), /*urgent*/ true);
                    if (cached.status == Effects::BytecodeStatus::Pending)
                    {
                        // No output yet; the node stays dirty so the next frame looks again.
                        node->compilePending = true;
                        node->cachedOutput = nullptr;
                        m_outputCache.erase(nodeId);
                        break;
                    }
                    node->compilePending = false;
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
                        // Skipped for an effect with option variants: those are
                        // precompiled for the binding shape in use, and these
                        // option-generic shapes would only be fallbacks.
                        if (!gpuNames.empty() && Effects::SpecializedOptionNames(def).empty())
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
                        // Retried on every visit regardless; see BuiltInEffect.
                        node->dirty = false;
                        break;
                    }
                }

                ID2D1Effect* effect = GetOrCreateEffect(dc, *node);
                if (!effect)
                {
                    node->runtimeError = L"Failed to create D2D effect. Check effect registration.";
                    node->cachedOutput = nullptr;
                    m_outputCache.erase(nodeId);
                    node->dirty = false;   // retried on every visit regardless
                    break;
                }
                node->runtimeError.clear();

                if (node->customEffect.has_value() && node->customEffect->isCompiled())
                {
                    // Resolve property bindings every frame.
                    std::map<std::wstring, PropertyValue> effectiveProps;
                    bool bindingsChanged = ResolveBindings(*node, graph, effectiveProps);
                    bool wasDirty = node->dirty || bindingsChanged;
                    // A binding that moved changes this node's output, so
                    // downstream consumers have to learn about it too.
                    if (bindingsChanged) m_outputChangedThisEval.insert(nodeId);

                    // Inject host-driven output dimensions for shaders that
                    // declare OutputW / OutputH cbuffer fields. D2D pads input
                    // textures to atlas allocation sizes (typically 4096x4096),
                    // so HLSL `Texture2D::GetDimensions()` is unreliable for
                    // anything that needs the true output rect (e.g. Split
                    // Comparisons center-of-image pivot). The shader uses
                    // these values instead.
                    // By property name: declared parameters and hidden defaults
                    // both land in node->properties.
                    const bool declaresOutputDims =
                        node->customEffect->shaderType == Graph::CustomShaderType::PixelShader &&
                        (node->properties.count(L"OutputW") || node->properties.count(L"OutputH"));

                    // Tell a shader with an InputMask field which pins carry an
                    // image; the rest hold the placeholder.
                    if (node->customEffect->shaderType == Graph::CustomShaderType::PixelShader &&
                        node->properties.count(L"InputMask"))
                    {
                        uint32_t inputMask = 0;
                        for (const auto* edge : graph.GetInputEdges(nodeId))
                        {
                            const auto* sourceNode = graph.FindNode(edge->sourceNodeId);
                            if (sourceNode && sourceNode->cachedOutput && edge->destPin < 32)
                                inputMask |= 1u << edge->destPin;
                        }
                        effectiveProps[L"InputMask"] = static_cast<float>(inputMask);
                        node->properties[L"InputMask"] = static_cast<float>(inputMask);
                    }
                    if (declaresOutputDims)
                    {
                        auto inputs = graph.GetInputEdges(nodeId);
                        if (!inputs.empty() && dc)
                        {
                            // Compute the *union* of all input bounds, mirroring
                            // CustomPixelShaderEffect::MapInputRectsToOutputRect
                            // (which produces the actual D2D output rect this
                            // shader runs over). Using only inputs[0] would
                            // misreport OutputW/H whenever inputs have different
                            // bounds (e.g. Split Comparison fed a 4K and a 2K
                            // input), causing the shader's coordinate math --
                            // including UV normalization -- to use a smaller
                            // virtual canvas than the actual output rect.
                            //
                            // Per-input bounds (ImageAW/H, ImageBW/H ...) are
                            // also captured so the shader can compensate for
                            // D2D's atlas padding -- a D2D pixel-shader output
                            // bitmap may live inside e.g. a 4096x4096 atlas
                            // even when its content rect is only 1920x1080.
                            // Sampling [0,1] would otherwise read the padding.
                            // Only flip DPI when the context isn't already
                            // at 96 — a real DPI change here invalidates
                            // every D2D1_PROPERTY_CACHED intermediate in
                            // the context (caches are DPI-referenced), and
                            // this block runs per-eval for dims-declaring
                            // nodes. The render context is pinned at 96
                            // (RenderEngine), so this is normally a no-op.
                            float oldDpiX = 0, oldDpiY = 0;
                            dc->GetDpi(&oldDpiX, &oldDpiY);
                            const bool dpiFlip =
                                (oldDpiX != 96.0f || oldDpiY != 96.0f);
                            if (dpiFlip)
                                dc->SetDpi(96.0f, 96.0f);
                            float unionLeft = (std::numeric_limits<float>::max)();
                            float unionTop = (std::numeric_limits<float>::max)();
                            float unionRight = -(std::numeric_limits<float>::max)();
                            float unionBottom = -(std::numeric_limits<float>::max)();
                            bool anyValid = false;
                            // Per-input content widths/heights, indexed by
                            // destination pin (so input #0 = ImageA, #1 = ImageB,
                            // ...). graph.GetInputEdges() returns edges in
                            // arbitrary order, so we have to scatter into a
                            // pin-indexed vector explicitly.
                            std::vector<std::pair<float, float>> perInputWH(4, { 0.0f, 0.0f });
                            for (const auto& edge : inputs)
                            {
                                auto* srcNode = graph.FindNode(edge->sourceNodeId);
                                if (!srcNode || !srcNode->cachedOutput) continue;
                                D2D1_RECT_F b{};
                                if (FAILED(dc->GetImageLocalBounds(srcNode->cachedOutput, &b)))
                                    continue;
                                float bw = b.right - b.left;
                                float bh = b.bottom - b.top;
                                if (bw <= 0 || bh <= 0) continue;
                                unionLeft   = (std::min)(unionLeft,   b.left);
                                unionTop    = (std::min)(unionTop,    b.top);
                                unionRight  = (std::max)(unionRight,  b.right);
                                unionBottom = (std::max)(unionBottom, b.bottom);
                                anyValid = true;
                                if (edge->destPin < perInputWH.size())
                                    perInputWH[edge->destPin] = { bw, bh };
                            }
                            if (dpiFlip)
                                dc->SetDpi(oldDpiX, oldDpiY);
                            if (anyValid)
                            {
                                float w = unionRight - unionLeft;
                                float h = unionBottom - unionTop;
                                if (w > 0 && h > 0)
                                {
                                    auto wIt = effectiveProps.find(L"OutputW");
                                    auto hIt = effectiveProps.find(L"OutputH");
                                    bool changed =
                                        (wIt == effectiveProps.end() ||
                                         !std::holds_alternative<float>(wIt->second) ||
                                         std::get<float>(wIt->second) != w) ||
                                        (hIt == effectiveProps.end() ||
                                         !std::holds_alternative<float>(hIt->second) ||
                                         std::get<float>(hIt->second) != h);
                                    effectiveProps[L"OutputW"] = w;
                                    effectiveProps[L"OutputH"] = h;
                                    node->properties[L"OutputW"] = w;
                                    node->properties[L"OutputH"] = h;

                                    // Write per-input ImageAW/H / ImageBW/H /
                                    // ImageCW/H / ImageDW/H -- but only if the
                                    // shader actually declares those params.
                                    static const wchar_t* kSlots[][2] = {
                                        { L"ImageAW", L"ImageAH" },
                                        { L"ImageBW", L"ImageBH" },
                                        { L"ImageCW", L"ImageCH" },
                                        { L"ImageDW", L"ImageDH" },
                                    };
                                    auto declaresParam = [&](const std::wstring& name) {
                                        for (const auto& p : node->customEffect->parameters)
                                            if (p.name == name) return true;
                                        return false;
                                    };
                                    for (size_t i = 0; i < perInputWH.size() && i < std::size(kSlots); ++i)
                                    {
                                        const wchar_t* nW = kSlots[i][0];
                                        const wchar_t* nH = kSlots[i][1];
                                        if (!declaresParam(nW) || !declaresParam(nH)) continue;
                                        // Fall back to the union dimensions when the input's
                                        // cachedOutput is briefly nullptr (e.g. first frame
                                        // after a compute branch is wired in -- deferred
                                        // compute hasn't populated the wrapper bitmap yet).
                                        // The shader's content/atlas math then degenerates
                                        // to "atlas == content" which is correct for compute
                                        // outputs (the typical case for null cachedOutput).
                                        float iw = perInputWH[i].first;
                                        float ih = perInputWH[i].second;
                                        if (iw <= 0 || ih <= 0) { iw = w; ih = h; }
                                        auto iwIt = effectiveProps.find(nW);
                                        auto ihIt = effectiveProps.find(nH);
                                        bool slotChanged =
                                            (iwIt == effectiveProps.end() ||
                                             !std::holds_alternative<float>(iwIt->second) ||
                                             std::get<float>(iwIt->second) != iw) ||
                                            (ihIt == effectiveProps.end() ||
                                             !std::holds_alternative<float>(ihIt->second) ||
                                             std::get<float>(ihIt->second) != ih);
                                        effectiveProps[nW] = iw;
                                        effectiveProps[nH] = ih;
                                        node->properties[nW] = iw;
                                        node->properties[nH] = ih;
                                        if (slotChanged) changed = true;
                                    }

                                    if (changed) wasDirty = true;
                                }
                            }
                        }
                    }

                    // Write resolved binding values back to node properties
                    // so the node graph UI shows live bound values.
                    if (bindingsChanged)
                    {
                        for (const auto& [propName, binding] : node->propertyBindings)
                        {
                            auto eit = effectiveProps.find(propName);
                            if (eit == effectiveProps.end()) continue;
                            if (!IsFinitePropertyValue(eit->second))
                            {
                                node->runtimeError = L"binding '" + propName +
                                    L"' resolved to a non-finite value; previous kept";
                                continue;
                            }
                            node->properties[propName] = eit->second;
                        }
                    }

                    if (wasDirty)
                    {
                        ApplyCustomEffect(effect, *node, effectiveProps, graph, dc);

                        // Force-upload the cbuffer directly to the GPU, with the
                        // shader its layout was reflected from.
                        auto implIt = m_customImplCache.find(node->id);
                        if (implIt != m_customImplCache.end())
                        {
                            if (node->type == NodeType::PixelShader && implIt->second.pixelImpl)
                            {
                                if (FAILED(implIt->second.pixelImpl->ForceLoadShader()))
                                    node->runtimeError = L"Loading the pixel shader variant failed";
                                implIt->second.pixelImpl->ForceUploadConstantBuffer();
                            }
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

                    // Custom effects upload their cbuffer directly to the
                    // GPU (bypassing the D2D property system), so D2D has
                    // no idea a re-render is needed when one changes. Two
                    // manual invalidation mechanisms, by mode:
                    //   * caching ON: UpdateEffectCachePolicy drops this
                    //     node's D2D1_PROPERTY_CACHED intermediate on
                    //     wasDirty — the next pull finds no cache and must
                    //     re-execute, picking up the fresh cbuffer. This
                    //     touches ONLY this node's cache.
                    //   * caching OFF: legacy input-0 detach/reattach
                    //     toggle. NOT used when caching is on because
                    //     detaching the consumer of an upstream effect's
                    //     output releases that upstream's cached
                    //     intermediate as collateral — one dirty custom
                    //     node per frame (any animated graph) then defeats
                    //     caching for its whole input chain.
                    if (wasDirty && !Performance::IsEffectOutputCachingEnabled())
                    {
                        winrt::com_ptr<ID2D1Image> savedInput;
                        effect->GetInput(0, savedInput.put());
                        if (savedInput)
                        {
                            effect->SetInput(0, nullptr);
                            effect->SetInput(0, savedInput.get());
                        }
                    }

                    UpdateEffectCachePolicy(effect, nodeId, wasDirty);

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
                        node->dirty = false;
                    }
                    else
                    {
                        WireInputs(effect, *node, graph);
                        const bool wasDirtyFallback = node->dirty;
                        if (wasDirtyFallback)
                        {
                            ApplyProperties(effect, *node, node->properties);
                            node->dirty = false;
                        }
                        UpdateEffectCachePolicy(effect, nodeId, wasDirtyFallback);
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

        // P7 / first-frame safety: if any Source node still has a null
        // cachedOutput, the chain isn't ready (e.g., video provider was
        // just created but hasn't decoded its first frame; image source
        // hasn't loaded; capture provider awaiting first frame). Walking
        // a downstream effect's GetImageLocalBounds chain through a null
        // upstream input AVs deep inside d2d1.dll. Skip this frame --
        // the next worker iteration will retry once the source is
        // populated. We DO clear m_deferredCompute so the entries (which
        // hold raw ID2D1Image* pointers to potentially stale objects)
        // don't pile up across frames.
        for (const auto& n : graph.Nodes())
        {
            if (n.type == NodeType::Source && !n.cachedOutput)
            {
                m_deferredCompute.clear();
                m_queuedComputeThisEval.clear();
                return false;
            }
        }

        bool imageComputeProduced = false;
        m_dispatchesLastFrame = 0;

        // Phase 8c: build the per-frame "needs CPU readback" set. When
        // the skip-readback feature flag is OFF, the set is treated as
        // covering every node (preserves pre-Phase-8c behavior). When
        // ON, a node lands in the set if either:
        //   (1) it is hinted by the host via SetCpuAnalysisInterest
        //       (UI selected node, MCP target, etc.), or
        //   (2) at least one downstream property binding that consumes
        //       this node's analysis output is NOT served via the GPU
        //       SRV path (CanServeBindingViaGpu returned false).
        // Nodes not in the set skip the CopyResource + Map round-trip;
        // their `analysisOutput.fields` retains last-frame values.
        const bool skipFlag = Performance::IsSkipUnneededCpuReadbackEnabled();
        std::unordered_set<uint32_t> needsReadback;
        if (skipFlag)
        {
            // Throttle host-hint readbacks (selected node, canvas value
            // labels) to Performance::CpuAnalysisHintThrottleMs. CPU-
            // routed bindings (added below) are unaffected because the
            // consumer reads `analysisOutput.fields` every frame.
            const auto throttle = std::chrono::milliseconds(
                Performance::CpuAnalysisHintThrottleMs());
            const auto nowTs = std::chrono::steady_clock::now();
            for (uint32_t id : m_cpuAnalysisInterest)
            {
                auto it = m_lastHintReadbackTime.find(id);
                const bool freshHint = (it == m_lastHintReadbackTime.end());
                const bool dueAgain = !freshHint && (nowTs - it->second) >= throttle;
                if (freshHint || dueAgain || throttle.count() == 0)
                    needsReadback.insert(id);
            }
            // Add every source whose binding cannot be GPU-served. These
            // are the consumers actual data dependencies and run every
            // frame regardless of throttle.
            for (const auto& consumerNode : graph.Nodes())
            {
                for (const auto& [propName, binding] : consumerNode.propertyBindings)
                {
                    // The skip must match the mode the consumer is ACTUALLY
                    // compiled in, not merely the mode that is possible.
                    // Texture mode only engages once the producer has published
                    // a bitmap (see ApplyCustomEffect); before that the consumer
                    // is still cbuffer-bound and genuinely needs the CPU value.
                    // Skipping on the possible mode would starve exactly the
                    // frames that have no texture yet -- a silent wrong value,
                    // not a slow one.
                    const GpuBindMode mode =
                        CanServeBindingViaGpu(consumerNode, propName, binding, graph);
                    // Lane 3 has to be ASKED for before the dispatch that
                    // fills it -- the producer creates nothing until then.
                    // Requesting it does not yet make the binding served; see
                    // IsGpuBindModeWired.
                    if (mode == GpuBindMode::Texture)
                    {
                        for (const auto& srcOpt : binding.sources)
                        {
                            if (!srcOpt.has_value()) continue;
                            const uint32_t pid = srcOpt->sourceNodeId;
                            m_analysisTextureWanted.insert(pid);
                            auto bIt = m_bridgeImplCache.find(pid);
                            if (bIt == m_bridgeImplCache.end() || !bIt->second) continue;
                            winrt::com_ptr<Effects::IEngineComputeTexture> iect;
                            if (SUCCEEDED(bIt->second->QueryInterface(
                                    __uuidof(Effects::IEngineComputeTexture), iect.put_void())))
                                iect->RequestAnalysisTexture();
                        }
                    }
                    bool served = (mode == GpuBindMode::StructuredBuffer);
                    if (mode == GpuBindMode::Texture)
                    {
                        served = true;
                        for (const auto& srcOpt : binding.sources)
                            if (srcOpt.has_value() && !AnalysisBitmap(srcOpt->sourceNodeId))
                                served = false;
                    }
                    // A consumer on a fallback build while its variant compiles
                    // reads the cbuffer, so it still needs this frame's value.
                    if (served && !IsBindingGpuApplied(consumerNode, propName))
                        served = false;
                    if (served) continue;
                    for (const auto& srcOpt : binding.sources)
                    {
                        if (srcOpt.has_value())
                            needsReadback.insert(srcOpt->sourceNodeId);
                    }
                }
            }
        }
        auto isReadbackNeeded = [&](uint32_t nodeId) -> bool
        {
            return !skipFlag || needsReadback.count(nodeId) > 0;
        };

        // Pre-render each distinct upstream image ONCE per pass into an FP32
        // bitmap the bridge can bind as a texture, and share it across every
        // deferred consumer of that image. This is the ONLY pre-render: it
        // runs inside the draw session and after any producer in the same
        // pass has been dispatched, so it is fresh by construction.
        //
        // Inline SetTarget + Clear + DrawImage (no nested BeginDraw, which
        // would fail inside the outer session); Flush forces the commands
        // out before the bridge's D3D11 compute reads.
        //
        // Targets persist across frames in m_sharedPreRenderCache, keyed by
        // the producing node + pin; `sharedPreRender` memoizes per pass by
        // image, so fan-out renders once.
        std::unordered_map<ID2D1Image*, ID2D1Bitmap1*> sharedPreRender;
        const uint64_t kNoProducerKey = ~0ull;

        auto preRenderShared = [&](ID2D1Image* inputImage, uint64_t cacheKey,
                                   HRESULT* failHr) -> ID2D1Bitmap1*
        {
            auto it = sharedPreRender.find(inputImage);
            if (it != sharedPreRender.end()) return it->second;

            float oldDpiX = 0, oldDpiY = 0;
            dc->GetDpi(&oldDpiX, &oldDpiY);
            dc->SetDpi(96.0f, 96.0f);

            D2D1_RECT_F bounds{};
            // GetImageLocalBounds walks the input image's effect chain back
            // to its source bitmap. If any upstream effect has a null/stale
            // input -- which can happen on the FIRST frame after a graph
            // load or GPU switch when a video source provider hasn't
            // decoded its first frame yet -- d2d1.dll AVs deep inside.
            // Wrap defensively and treat any failure as "not yet ready"
            // so the next frame can retry once upstream has its bitmap.
            HRESULT bhr = E_FAIL;
            try
            {
                bhr = dc->GetImageLocalBounds(inputImage, &bounds);
            }
            catch (...)
            {
                // Deliberate: "not ready yet", retried next frame (see above).
                dc->SetDpi(oldDpiX, oldDpiY);
                return nullptr;
            }
            D2D1_RECT_L px{};
            if (SUCCEEDED(bhr))
                bhr = Effects::SnapComputeInputRect(bounds, px);
            if (FAILED(bhr))
            {
                dc->SetDpi(oldDpiX, oldDpiY);
                if (failHr) *failHr = bhr;
                return nullptr;
            }
            const UINT32 w = static_cast<UINT32>(px.right - px.left);
            const UINT32 h = static_cast<UINT32>(px.bottom - px.top);

            // Already an FP32 bitmap covering exactly its bounds -- the image
            // output of an upstream compute node, or the generator
            // placeholder. The bridge binds it directly; copying it would
            // buy nothing. Safe for freshness because a producer in this pass
            // was dispatched earlier in topological order, so its texture
            // already holds this pass's pixels.
            {
                winrt::com_ptr<ID2D1Bitmap1> asBitmap;
                if (SUCCEEDED(inputImage->QueryInterface(asBitmap.put())) && asBitmap)
                {
                    const auto fmt = asBitmap->GetPixelFormat();
                    const auto sz  = asBitmap->GetPixelSize();
                    if (fmt.format == DXGI_FORMAT_R32G32B32A32_FLOAT &&
                        px.left == 0 && px.top == 0 && sz.width == w && sz.height == h)
                    {
                        dc->SetDpi(oldDpiX, oldDpiY);
                        sharedPreRender[inputImage] = asBitmap.get();
                        return asBitmap.get();
                    }
                }
            }

            // Reuse the persistent target when its size still matches.
            auto& cacheBitmap = m_sharedPreRenderCache[cacheKey];
            if (!cacheBitmap.bitmap || cacheBitmap.width != w || cacheBitmap.height != h)
            {
                D2D1_BITMAP_PROPERTIES1 bp{};
                bp.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
                bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
                bp.dpiX = 96.0f;
                bp.dpiY = 96.0f;
                winrt::com_ptr<ID2D1Bitmap1> newBmp;
                HRESULT hr = dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, bp, newBmp.put());
                if (FAILED(hr))
                {
                    dc->SetDpi(oldDpiX, oldDpiY);
                    if (failHr) *failHr = hr;
                    return nullptr;
                }
                cacheBitmap.bitmap = std::move(newBmp);
                cacheBitmap.width  = w;
                cacheBitmap.height = h;
            }

            // Integer origin: see SnapComputeInputRect for why a fractional
            // one is wrong.
            winrt::com_ptr<ID2D1Image> prevTarget;
            dc->GetTarget(prevTarget.put());
            dc->SetTarget(cacheBitmap.bitmap.get());
            dc->Clear(D2D1::ColorF(0, 0, 0, 0));
            dc->DrawImage(inputImage, D2D1::Point2F(
                -static_cast<float>(px.left), -static_cast<float>(px.top)));
            dc->SetTarget(prevTarget.get());
            dc->Flush();
            dc->SetDpi(oldDpiX, oldDpiY);

            auto* ptr = cacheBitmap.bitmap.get();
            sharedPreRender[inputImage] = ptr;
            return ptr;
        };

        // Nodes whose image output is (re)produced by THIS pass. An input fed
        // by one of them must NOT use the Evaluate-time pre-render: that
        // bitmap was captured before the producer was dispatched, so it holds
        // the PREVIOUS evaluation's pixels. Symptom is a clean one-mutation
        // lag -- a CS-Img consuming a CS-Img reports the value its upstream
        // had before the last set-property, silently and at HTTP 200.
        // m_deferredCompute is built in topological order (see
        // docs/architecture/topological-evaluation.md), so by the time a
        // consumer is reached its producer has already been dispatched and a
        // re-render here picks up fresh pixels.
        std::unordered_set<uint32_t> producedThisPass;
        for (const auto& d : m_deferredCompute)
            producedThisPass.insert(d.nodeId);

        // ...and everything DOWNSTREAM of them. Checking only the immediate
        // producer fixes compute -> compute but leaves the lag alive one hop
        // further out: a pixel shader between two compute nodes is not in the
        // deferred set, yet its D2D output still resolves through a producer
        // that has not been dispatched at Evaluate time, so the consumer's
        // snapshot of it is a frame stale. Measured on
        // ICtCp Tone Map -> Nit Map -> ICtCp Highlight Desaturation: the
        // consumer reported 1.400438 / 1.400438 / 0.772682 while its input read
        // 1.401232 / 0.772924 / 2.435596 -- each value one mutation behind.
        // Data-only consumers never had this because they do not pre-render at
        // all, which is why the direct-edge test passed and this did not.
        std::unordered_set<uint32_t> staleThisPass = producedThisPass;
        std::vector<uint32_t> reach(producedThisPass.begin(), producedThisPass.end());
        for (size_t qi = 0; qi < reach.size(); ++qi)
            for (const auto* e : graph.GetOutputEdges(reach[qi]))
                if (e && staleThisPass.insert(e->destNodeId).second)
                    reach.push_back(e->destNodeId);

        for (auto& deferred : m_deferredCompute)
        {
            auto* node = graph.FindNode(deferred.nodeId);
            if (!node || deferred.inputImages.empty() || !deferred.inputImages[0]) continue;
            if (!node->customEffect.has_value()) continue;

            auto bit = m_bridgeImplCache.find(node->id);
            if (bit == m_bridgeImplCache.end() || !bit->second)
                continue;
            auto* bridge = bit->second;

            // Resolve each input from the graph NOW rather than trusting the
            // image recorded at Evaluate time: a later Evaluate pass in the
            // same cycle can rebuild an upstream effect, and a producer
            // dispatched earlier in this pass has just replaced its output.
            // The recorded image is the fallback for a slot whose producer
            // currently has no output.
            std::vector<ID2D1Bitmap1*> preRendered(deferred.inputImages.size(), nullptr);
            std::vector<ID2D1Image*>   inputRaw(deferred.inputImages.size(), nullptr);
            std::unordered_map<uint32_t, const Graph::EffectEdge*> edgeByPin;
            for (const auto* e : graph.GetInputEdges(deferred.nodeId))
                if (e) edgeByPin[e->destPin] = e;

            HRESULT preRenderHr = S_OK;
            size_t failedPin = 0;
            for (size_t i = 0; i < deferred.inputImages.size(); ++i)
            {
                uint64_t key = kNoProducerKey;
                ID2D1Image* img = deferred.inputImages[i].get();
                auto eit = edgeByPin.find(static_cast<uint32_t>(i));
                if (eit != edgeByPin.end())
                {
                    key = (static_cast<uint64_t>(eit->second->sourceNodeId) << 32) |
                          eit->second->sourcePin;
                    if (auto* src = graph.FindNode(eit->second->sourceNodeId);
                        src && src->cachedOutput)
                        img = src->cachedOutput;
                }
                if (!img) continue;
                inputRaw[i] = img;
                HRESULT hr = S_OK;
                preRendered[i] = preRenderShared(img, key, &hr);
                if (FAILED(hr) && SUCCEEDED(preRenderHr)) { preRenderHr = hr; failedPin = i; }
            }
            if (preRenderHr == E_BOUNDS)
            {
                // An unbounded image (Flood, Tile, Border, Turbulence) cannot
                // be copied into a texture; it used to become a 1 GiB 8192^2
                // FP32 allocation. Say so instead of dispatching on garbage.
                node->runtimeError = std::format(
                    L"Input {} has unbounded extent (Flood, Tile, Border or Turbulence?). "
                    L"Crop it before feeding a compute effect.", failedPin);
                continue;
            }
            if (preRenderHr == D2DERR_MAX_TEXTURE_SIZE_EXCEEDED)
            {
                node->runtimeError = std::format(
                    L"Input {} is larger than {} px, the largest texture a compute effect can read. "
                    L"Scale or crop it first.", failedPin, D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION);
                continue;
            }

            const bool readback = isReadbackNeeded(node->id);
            // Exact per-node attribution: this dispatch is its own D3D11
            // submission, so bracketing it measures this node and nothing else.
            //
            // NOTE the non-flushing EndNode. The flushing overload exists for
            // spans around DIRECT2D work, which may not have been emitted yet
            // when the span closes. A compute dispatch is not D2D work, so a
            // flush here buys nothing -- and it would flush the context in the
            // middle of ProcessDeferredCompute's own target juggling, inside a
            // draw session this codebase already documents as fragile.
            // Async readback only in the GUI's live loop (skip-readback on)
            // and only when the host opted in; see Performance.h.
            const bool asyncReadback = readback && skipFlag &&
                Performance::IsAsyncAnalysisReadbackEnabled();
            bridge->SetAsyncReadback(asyncReadback);
            if (m_gpuTimer) m_gpuTimer->BeginNode(node->id);
            DispatchViaBridge(dc, graph, *node, inputRaw,
                preRendered, bridge, readback);
            if (m_gpuTimer) m_gpuTimer->EndNode(node->id);
            if (asyncReadback && bridge->HasPendingReadback())
                m_asyncReadbackPending.insert(node->id);
            ++m_dispatchesLastFrame;

            // Phase 8c: record the timestamp for hinted nodes whose
            // readback actually ran this frame so the throttle window
            // starts now. Bindings-driven readbacks (consumer needs CPU
            // value) intentionally don't update this map -- they fire
            // every frame regardless of throttle.
            if (readback && skipFlag && m_cpuAnalysisInterest.count(node->id))
                m_lastHintReadbackTime[node->id] = std::chrono::steady_clock::now();

            // Lane 3: the copy pass has just run inside DispatchViaBridge, so
            // the texture now holds this frame's values. Wrapping it as a D2D
            // bitmap is what makes it attachable as an effect input.
            if (m_analysisTextureWanted.count(node->id))
                EnsureAnalysisBitmap(node->id, dc);

            // Consumers BOUND to this node's analysis values are stale now too,
            // and no edge reaches them: a Luminance Statistics feeding a
            // tone mapper's SourcePeakNits has no image output at all.
            // Without this the consumer evaluated with whatever the binding
            // held BEFORE this dispatch -- a frame late on a live graph, and
            // on a static one (a still image, no clock) forever: headless
            // rendered a capture with SourcePeakNits 2712 (the saved value)
            // while the statistics said 608. It hid in any graph where some
            // other edge happened to dirty the consumer.
            // The caller's frozen post-dispatch Evaluate re-runs the marked
            // nodes this frame, so the value is used the frame it is made.
            MarkBindingConsumersDirty(graph, node->id);

            bool hasImageOutput = !node->outputPins.empty();
            if (hasImageOutput && node->cachedOutput)
            {
                imageComputeProduced = true;
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
        m_queuedComputeThisEval.clear();

        // Drop pre-render targets whose producer has left the graph. Each is
        // a full-size FP32 surface; keying by node id already stops the
        // per-edit leak, this stops the per-deleted-node one.
        for (auto it = m_sharedPreRenderCache.begin(); it != m_sharedPreRenderCache.end();)
        {
            const uint64_t key = it->first;
            const bool orphan = key != kNoProducerKey &&
                !graph.FindNode(static_cast<uint32_t>(key >> 32));
            if (orphan) it = m_sharedPreRenderCache.erase(it);
            else ++it;
        }

        // Per-frame: a consumer removed from the graph must stop the producer
        // paying for lane 3. The producer's own RequestAnalysisTexture latch
        // is intentionally sticky -- tearing the texture down and rebuilding
        // it on alternate frames would cost more than keeping it.
        m_analysisTextureWanted.clear();
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

    GraphEvaluator::BytecodeId GraphEvaluator::IdOfBytecode(const std::vector<uint8_t>& b)
    {
        BytecodeId id{};
        if (b.size() >= 20 && b[0] == 'D' && b[1] == 'X' && b[2] == 'B' && b[3] == 'C')
        {
            std::memcpy(id.data(), b.data() + 4, id.size());
            return id;
        }
        uint64_t h = 1469598103934665603ull;
        for (uint8_t c : b) { h ^= c; h *= 1099511628211ull; }
        std::memcpy(id.data(), &h, sizeof(h));
        const uint64_t sz = b.size();
        std::memcpy(id.data() + 8, &sz, sizeof(sz));
        return id;
    }

    GUID GraphEvaluator::ShaderGuidFor(const GUID& definitionGuid, const std::vector<uint8_t>& bytecode)
    {
        static_assert(sizeof(GUID) == sizeof(BytecodeId));
        const BytecodeId id = IdOfBytecode(bytecode);
        GUID guid = definitionGuid;
        auto* guidBytes = reinterpret_cast<uint8_t*>(&guid);
        for (size_t index = 0; index < id.size(); ++index)
            guidBytes[index] ^= id[index];
        return guid;
    }

    std::shared_ptr<const std::vector<uint8_t>> GraphEvaluator::GetVariantBytecode(
        EffectNode& node, const std::wstring& effectId, uint32_t effectVersion,
        const std::string& target, uint32_t bits, uint64_t options, bool urgent,
        std::optional<Effects::BytecodeCompileKey>& pendingKey)
    {
        const auto& def = node.customEffect.value();
        if (def.compiledBytecode.empty()) return nullptr;
        // Keyed by the BASELINE's checksum: the variant is a pure function of
        // the source the baseline was compiled from plus the bitset, so a
        // recompile (new baseline) can never be served an old variant.
        const VariantKey key{ IdOfBytecode(def.compiledBytecode), bits, options };
        auto it = m_variantMemo.find(key);
        if (it != m_variantMemo.end()) return it->second;

        auto gpuNames = ExtractGpuBindableNames(def);
        Effects::BytecodeCompileKey cacheKey;
        auto variant = CompileViaCache(effectId, effectVersion,
                                       def.hlslSource, target, bits, gpuNames, m_asyncCompile,
                                       options, Effects::SpecializedOptionNames(def),
                                       urgent, &cacheKey);
        if (variant.status == Effects::BytecodeStatus::Pending && !pendingKey)
            pendingKey = cacheKey;
        if (variant.status != Effects::BytecodeStatus::Ready || variant.bytecode.empty())
            return nullptr;   // not memoized: a background compile may finish later
        if (m_variantMemo.size() > 256) m_variantMemo.clear();
        auto bytes = std::make_shared<const std::vector<uint8_t>>(std::move(variant.bytecode));
        m_variantMemo[key] = bytes;
        return bytes;
    }

    GraphEvaluator::VariantChoice GraphEvaluator::SelectVariant(
        EffectNode& node, const std::wstring& effectId, uint32_t effectVersion,
        const std::string& target, uint32_t bits, uint64_t options)
    {
        // The wanted variant is urgent; fallbacks queue behind it. The node is
        // re-dirtied when the first pending one in this order finishes.
        std::optional<Effects::BytecodeCompileKey> pendingKey;
        VariantChoice choice{ GetVariantBytecode(node, effectId, effectVersion, target, bits, options,
                                                 /*urgent*/ true, pendingKey),
                              bits, options };
        // Keep the GPU routing without the options.
        if (!choice.bytes && options != 0 && bits != 0)
        {
            choice.bytes = GetVariantBytecode(node, effectId, effectVersion, target, bits, 0,
                                              /*urgent*/ false, pendingKey);
            if (choice.bytes) choice.options = 0;
        }
        // Drop the GPU routing, keeping the options.
        if (!choice.bytes && bits != 0)
        {
            choice.bits = 0;
            if (options != 0)
                choice.bytes = GetVariantBytecode(node, effectId, effectVersion, target, 0, options,
                                                  /*urgent*/ false, pendingKey);
        }
        if (pendingKey)
            m_wantedVariant[node.id] = *pendingKey;
        // Nothing specialised is ready: the generic baseline.
        if (!choice.bytes || choice.bytes->empty())
            choice.options = 0;
        return choice;
    }

    uint32_t GraphEvaluator::IntendedGpuBits(const EffectNode& node, bool pixel) const
    {
        // The binding shape the node will run once its sources have output:
        // every single-source binding on a gpu-bindable parameter, in the
        // stage's GPU mode. The per-frame plan reports 0 until then.
        if (!Performance::IsGpuBindingsEnabled() || !node.customEffect.has_value()) return 0;
        uint32_t bits = 0, gpuBindableIndex = 0;
        const uint32_t mode = pixel ? 2u : 1u;
        for (const auto& p : node.customEffect->parameters)
        {
            if (!p.gpuBindable) continue;
            const uint32_t paramIndex = gpuBindableIndex++;
            auto bindingIt = node.propertyBindings.find(p.name);
            if (bindingIt == node.propertyBindings.end()) continue;
            const auto& binding = bindingIt->second;
            if (binding.wholeArray || binding.sources.size() != 1 || !binding.sources[0].has_value()) continue;
            bits |= mode << (2u * paramIndex);
        }
        return bits;
    }

    void GraphEvaluator::QueueOptionVariants(EffectNode& node, const std::string& target, uint32_t gpuBits)
    {
        // Interactive hosts only; headless compiles just what it renders.
        if (!m_asyncCompile || !node.customEffect.has_value()) return;
        const auto& def = node.customEffect.value();
        // Wait for the baseline so it never queues behind these.
        if (def.compiledBytecode.empty()) return;
        // The baseline's identity changes with any edit that changes the compiled shader.
        // Toggling a parameter's "specialize" flag leaves the baseline unchanged,
        // so the specialised names are part of the check too.
        const BytecodeId baseline = IdOfBytecode(def.compiledBytecode);
        auto optionNames = Effects::SpecializedOptionNames(def);
        auto& precompile = m_optionPrecompile[node.id];
        if (precompile.baseline == baseline && precompile.gpuBits == gpuBits &&
            precompile.optionNames == optionNames &&
            (precompile.done || !precompile.keys.empty()))
            return;   // already queued for this baseline, binding shape and option set
        precompile = OptionPrecompile{};
        precompile.baseline    = baseline;
        precompile.gpuBits     = gpuBits;
        precompile.optionNames = optionNames;
        node.variantsCompiling = 0;

        auto optionKeys = Effects::AllOptionKeys(def);
        if (optionKeys.empty()) { precompile.done = true; return; }   // nothing specialised, or too many
        // The combination the node shows now goes first.
        const uint64_t currentKey = Effects::OptionKeyFor(def, node.properties);
        std::stable_partition(optionKeys.begin(), optionKeys.end(),
            [&](uint64_t optionKey) { return optionKey == currentKey; });

        const auto gpuNames    = ExtractGpuBindableNames(def);
        const std::string canonical = Effects::CanonicalizeHlslSource(def.hlslSource);
        auto& cache = Effects::BytecodeCache::Instance();
        for (uint64_t optionKey : optionKeys)
        {
            auto request = MakeCompileRequest(node.name, 1u, canonical, target,
                                              gpuBits, gpuNames, optionKey, optionNames);
            precompile.keys.push_back(request.key);
            // TryGet first: it loads from the disk cache, where RequestCompile would recompile.
            if (cache.TryGet(request.key).status == Effects::BytecodeStatus::NotRequested)
                cache.RequestCompile(std::move(request), /*urgent*/ optionKey == currentKey);
        }
    }

    void GraphEvaluator::PollVariantCompiles(EffectGraph& graph)
    {
        auto& cache = Effects::BytecodeCache::Instance();
        for (auto it = m_wantedVariant.begin(); it != m_wantedVariant.end();)
        {
            if (cache.GetStatus(it->second) == Effects::BytecodeStatus::Pending) { ++it; continue; }
            // Ready, failed or evicted: evaluate the node again.
            if (auto* node = graph.FindNode(it->first)) node->dirty = true;
            it = m_wantedVariant.erase(it);
        }
        for (auto it = m_optionPrecompile.begin(); it != m_optionPrecompile.end();)
        {
            auto* node = graph.FindNode(it->first);
            if (!node) { it = m_optionPrecompile.erase(it); continue; }
            auto& precompile = it->second;
            if (!precompile.done)
            {
                uint32_t pending = 0;
                for (const auto& key : precompile.keys)
                    if (cache.GetStatus(key) == Effects::BytecodeStatus::Pending) ++pending;
                node->variantsCompiling = pending;
                if (pending == 0) precompile.done = true;
            }
            ++it;
        }
        for (auto it = m_pixelLoadedVariant.begin(); it != m_pixelLoadedVariant.end();)
        {
            if (graph.FindNode(it->first)) ++it;
            else it = m_pixelLoadedVariant.erase(it);
        }
        for (auto it = m_appliedGpuBits.begin(); it != m_appliedGpuBits.end();)
        {
            if (graph.FindNode(it->first)) ++it;
            else it = m_appliedGpuBits.erase(it);
        }
    }

    bool GraphEvaluator::IsBindingGpuApplied(const EffectNode& consumer, const std::wstring& paramName) const
    {
        auto applied = m_appliedGpuBits.find(consumer.id);
        if (applied == m_appliedGpuBits.end() || !consumer.customEffect.has_value()) return false;
        uint32_t gpuBindableIndex = 0;
        for (const auto& param : consumer.customEffect->parameters)
        {
            if (!param.gpuBindable) continue;
            if (param.name == paramName) return ((applied->second >> (2u * gpuBindableIndex)) & 3u) != 0;
            ++gpuBindableIndex;
        }
        return false;
    }

    void GraphEvaluator::ForgetVariantState(uint32_t nodeId)
    {
        m_appliedGpuBits.erase(nodeId);
        m_pixelLoadedVariant.erase(nodeId);
        m_optionPrecompile.erase(nodeId);
        m_wantedVariant.erase(nodeId);
    }

    std::shared_ptr<const GraphEvaluator::ReflectedCb> GraphEvaluator::ReflectComputeCb(
        const std::vector<uint8_t>& bytes)
    {
        const BytecodeId id = IdOfBytecode(bytes);
        auto it = m_computeReflectMemo.find(id);
        if (it != m_computeReflectMemo.end()) return it->second;

        auto out = std::make_shared<ReflectedCb>();
        winrt::com_ptr<ID3D11ShaderReflection> reflect;
        if (SUCCEEDED(D3DReflect(bytes.data(), bytes.size(),
                IID_ID3D11ShaderReflection, reinterpret_cast<void**>(reflect.put()))) && reflect)
        {
            auto* cbReflect = reflect->GetConstantBufferByIndex(0);
            D3D11_SHADER_BUFFER_DESC cbDesc{};
            if (cbReflect && SUCCEEDED(cbReflect->GetDesc(&cbDesc)))
            {
                out->sizeBytes = cbDesc.Size;
                for (UINT v = 0; v < cbDesc.Variables; ++v)
                {
                    auto* var = cbReflect->GetVariableByIndex(v);
                    D3D11_SHADER_VARIABLE_DESC varDesc{};
                    if (!var || FAILED(var->GetDesc(&varDesc))) continue;
                    ReflectedCbVar rv;
                    rv.name.assign(varDesc.Name, varDesc.Name + strlen(varDesc.Name));
                    rv.offset = varDesc.StartOffset;
                    D3D11_SHADER_TYPE_DESC typeDesc{};
                    if (auto* typeInfo = var->GetType();
                        typeInfo && SUCCEEDED(typeInfo->GetDesc(&typeDesc)))
                    {
                        rv.type = typeDesc.Type;
                        rv.cols = typeDesc.Columns;
                    }
                    out->vars.push_back(std::move(rv));
                }
            }
        }
        if (m_computeReflectMemo.size() > 256) m_computeReflectMemo.clear();
        m_computeReflectMemo[id] = out;
        return out;
    }

    void GraphEvaluator::UnpackAnalysisFloats(EffectNode& node, const std::vector<float>& analysisFloats)
    {
        const auto& def = node.customEffect.value();
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
    }

    void GraphEvaluator::MarkBindingConsumersDirty(EffectGraph& graph, uint32_t producerId)
    {
        std::vector<uint32_t> staleQueue;
        for (const auto& consumer : graph.Nodes())
        {
            bool bound = false;
            for (const auto& [propName, binding] : consumer.propertyBindings)
            {
                if (binding.wholeArray && binding.wholeArraySourceNodeId == producerId)
                    bound = true;
                for (const auto& src : binding.sources)
                    if (src.has_value() && src->sourceNodeId == producerId)
                        bound = true;
                if (bound) break;
            }
            if (bound) staleQueue.push_back(consumer.id);
        }
        for (size_t i = 0; i < staleQueue.size(); ++i)
        {
            auto* sn = graph.FindNode(staleQueue[i]);
            if (!sn) continue;
            sn->dirty = true;
            for (const auto* edge : graph.GetOutputEdges(staleQueue[i]))
            {
                auto* dn = graph.FindNode(edge->destNodeId);
                if (dn && !dn->dirty)
                {
                    dn->dirty = true;
                    staleQueue.push_back(edge->destNodeId);
                }
            }
        }
    }

    bool GraphEvaluator::PollAsyncReadbacks(EffectGraph& graph)
    {
        bool any = false;
        for (auto it = m_asyncReadbackPending.begin(); it != m_asyncReadbackPending.end();)
        {
            const uint32_t id = *it;
            auto* node = graph.FindNode(id);
            auto bIt = m_bridgeImplCache.find(id);
            if (!node || !node->customEffect.has_value() ||
                bIt == m_bridgeImplCache.end() || !bIt->second)
            {
                it = m_asyncReadbackPending.erase(it);
                continue;
            }
            std::vector<float> floats;
            if (bIt->second->PollAnalysisReadback(floats) && !floats.empty())
            {
                UnpackAnalysisFloats(*node, floats);
                MarkBindingConsumersDirty(graph, id);
                any = true;
            }
            if (!bIt->second->HasPendingReadback())
                it = m_asyncReadbackPending.erase(it);
            else
                ++it;
        }
        return any;
    }

    void GraphEvaluator::EnsureAnalysisDummy(ID2D1DeviceContext5* dc)
    {
        if (m_analysisDummyBitmap || !dc) return;
        // 1x1 and zero-filled. A shader reading it gets 0, which is the same
        // thing an unbound cbuffer slot would give -- and it is never actually
        // read, because a parameter only compiles to texture mode when a real
        // bitmap is attached.
        const float zero[4]{ 0, 0, 0, 0 };
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_IGNORE));
        dc->CreateBitmap(D2D1::SizeU(1, 1), zero, sizeof(zero), &props,
                         m_analysisDummyBitmap.put());
    }

    ID2D1Bitmap1* GraphEvaluator::EnsureAnalysisBitmap(
        uint32_t producerNodeId, ID2D1DeviceContext5* dc)
    {
        if (!dc) return nullptr;
        auto bIt = m_bridgeImplCache.find(producerNodeId);
        if (bIt == m_bridgeImplCache.end() || !bIt->second) return nullptr;

        winrt::com_ptr<Effects::IEngineComputeTexture> iect;
        if (FAILED(bIt->second->QueryInterface(
                __uuidof(Effects::IEngineComputeTexture), iect.put_void())))
            return nullptr;

        winrt::com_ptr<ID3D11Texture2D> tex;
        if (FAILED(iect->GetAnalysisTexture(tex.put())) || !tex)
            return nullptr;   // not filled yet -- next frame

        // Reuse the wrapper while it still points at the same texture. The
        // bitmap is a VIEW, not a copy: the copy pass writes the texture and
        // the bitmap sees it, so recreating per frame would buy nothing and
        // cost an allocation on every dispatch.
        auto cIt = m_analysisBitmapCache.find(producerNodeId);
        auto sIt = m_analysisBitmapSource.find(producerNodeId);
        if (cIt != m_analysisBitmapCache.end() && cIt->second &&
            sIt != m_analysisBitmapSource.end() && sIt->second == tex.get())
            return cIt->second.get();

        winrt::com_ptr<IDXGISurface> surface;
        if (FAILED(tex->QueryInterface(__uuidof(IDXGISurface), surface.put_void())))
            return nullptr;

        // R32G32B32A32_FLOAT, matching the texture: the analysis values are
        // absolute nits and similar magnitudes, and FP16 would quantise a
        // 620-nit peak to half-nit steps for no reason -- the whole lane
        // exists to avoid losing precision on the way to the shader.
        // IGNORE alpha: these are data, not colour, and a premultiplied
        // interpretation would let D2D scale the .x channel by .w.
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_IGNORE));
        winrt::com_ptr<ID2D1Bitmap1> bmp;
        if (FAILED(dc->CreateBitmapFromDxgiSurface(surface.get(), &props, bmp.put())))
            return nullptr;

        m_analysisBitmapSource[producerNodeId] = tex.get();
        m_analysisBitmapCache[producerNodeId] = bmp;
        return bmp.get();
    }

    void GraphEvaluator::DispatchViaBridge(
        ID2D1DeviceContext5* dc,
        const EffectGraph& graph,
        EffectNode& node,
        const std::vector<ID2D1Image*>& inputImages,
        const std::vector<ID2D1Bitmap1*>& preRenderedInputs,
        Effects::CustomComputeBridgeEffect* bridge,
        bool readbackToCpu)
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
            // Urgent; option variants are queued below once the bindings are known.
            auto cached = CompileViaCache(
                node.name, /*effectVersion*/ 1u,
                def.hlslSource, "cs_5_0", /*macroBitset*/ 0u, gpuNames, m_asyncCompile,
                /*optionKey*/ 0, Effects::SpecializedOptionNames(def), /*urgent*/ true);
            if (cached.status == Effects::BytecodeStatus::Pending)
            {
                // Skip this dispatch and look again next frame.
                node.compilePending = true;
                node.dirty = true;
                return;
            }
            node.compilePending = false;
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
            // Eager precompile of the GPU-binding shapes, as in EvaluateNode.
            if (!gpuNames.empty() && Effects::SpecializedOptionNames(def).empty())
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

        // -----------------------------------------------------------------
        // Phase 8 GPU-binding plan
        // -----------------------------------------------------------------
        // Walk gpuBindable params in declaration order. For each one
        // that has a binding from an upstream effect implementing
        // IEngineComputeOutput, capture (paramIndex, slot, srv) and
        // set the corresponding bit in macroBitset. Slot follows the
        // convention: gpuBindable param at index i binds to t<i+1>
        // (t0 is reserved for the input texture). HLSL authors using
        // SHADERLAB_GPU_BUFFER must pick consistent slot numbers.
        struct GpuBindingEntry {
            uint32_t                                 gpuBindableIndex;
            uint32_t                                 slot;
            uint32_t                                 fieldIndex;   // float4 index in upstream's analysis SRV
            std::wstring                             paramName;    // for _SLIdx_<name> cbuffer slot lookup
            winrt::com_ptr<ID3D11ShaderResourceView> srv;
        };
        std::vector<GpuBindingEntry> bindingPlan;
        uint32_t macroBitset = 0;
        const std::vector<uint8_t>* reflectBytecode = &def.compiledBytecode;
        std::shared_ptr<const std::vector<uint8_t>> variantBytecode;

        if (Performance::IsGpuBindingsEnabled())
        {
            uint32_t gpuBindableIdx = 0;
            for (const auto& p : def.parameters)
            {
                if (!p.gpuBindable)
                    continue;
                const uint32_t thisGpuIdx = gpuBindableIdx++;

                auto bIt = node.propertyBindings.find(p.name);
                if (bIt == node.propertyBindings.end())
                    continue;
                const auto& binding = bIt->second;
                if (binding.wholeArray ||
                    binding.sources.empty() ||
                    !binding.sources[0].has_value())
                    continue;

                uint32_t srcId = binding.sources[0]->sourceNodeId;
                // Discover via m_bridgeImplCache (the bridge impl
                // pointer captured at CreateEffect time). D2D's outer
                // ID2D1Effect's QueryInterface doesn't delegate
                // arbitrary IIDs to the impl, so we use the impl
                // pointer directly. The bridge implements
                // IEngineComputeOutput by delegation to its internal
                // D3D11ComputeRunner.
                auto bridgeIt = m_bridgeImplCache.find(srcId);
                if (bridgeIt == m_bridgeImplCache.end() || !bridgeIt->second)
                    continue;
                winrt::com_ptr<Effects::IEngineComputeOutput> ieco;
                if (FAILED(bridgeIt->second->QueryInterface(
                    __uuidof(Effects::IEngineComputeOutput), ieco.put_void())))
                    continue;

                winrt::com_ptr<ID3D11ShaderResourceView> srv;
                if (FAILED(ieco->GetAnalysisSrv(srv.put())) || !srv)
                    continue;

                // Compute the upstream field's float4 index in the
                // analysis SRV. The upstream's CustomEffectDefinition
                // declares analysis fields in order; each field
                // occupies pixelCount() float4 slots (floats / float2 /
                // float3 / float4 = 1 slot each; arrays = arrayLength
                // * 1 slot per element). Walk the prefix-sum until we
                // hit the bound source field name.
                const EffectNode* srcNode = graph.FindNode(srcId);
                if (!srcNode || !srcNode->customEffect.has_value())
                    continue;
                uint32_t fieldIndex = 0;
                bool fieldFound = false;
                for (const auto& fd : srcNode->customEffect->analysisFields)
                {
                    if (fd.name == binding.sources[0]->sourceFieldName)
                    {
                        fieldIndex += binding.sources[0]->sourceIndex;
                        fieldFound = true;
                        break;
                    }
                    fieldIndex += fd.pixelCount();
                }
                if (!fieldFound)
                    continue;

                bindingPlan.push_back({
                    thisGpuIdx,
                    thisGpuIdx + 1u,
                    fieldIndex,
                    p.name,
                    std::move(srv) });
                // Mode 1 (StructuredBuffer) for a compute consumer, in the
                // low bit of this param's 2-bit field. See BytecodeCache.h.
                macroBitset |= (1u << (2u * thisGpuIdx));
                // (telemetry already bumped by ResolveBindings; no
                // double-count here.)
            }

        }

        // Pick the variant for the GPU-routed bindings (_SLPARAM_<name>_GPU=1)
        // and specialised options. The generic baseline is the last fallback
        // and is always correct (cbuffer values).
        QueueOptionVariants(node, "cs_5_0", IntendedGpuBits(node, /*pixel*/ false));
        {
            const uint64_t optionKey = Effects::OptionKeyFor(def, node.properties);
            if (macroBitset != 0 || optionKey != 0)
            {
                const auto choice = SelectVariant(node, node.name, 1u, "cs_5_0", macroBitset, optionKey);
                if (macroBitset != 0 && choice.bits == 0)
                    bindingPlan.clear();
                macroBitset     = choice.bits;
                variantBytecode = choice.bytes;
                if (variantBytecode)
                    reflectBytecode = variantBytecode.get();
            }
        }
        m_appliedGpuBits[node.id] = macroBitset;

        // Install whatever the plan implies -- baseline or variant -- before
        // every dispatch. The bridge compares by DXBC checksum, so this is a
        // no-op unless the plan changed. (It used to swap the variant in
        // here and the baseline back after the dispatch, which recreated
        // the compute shader twice per frame.)
        if (!reflectBytecode->empty())
            bridge->SetCompiledBytecode(reflectBytecode->data(),
                static_cast<UINT32>(reflectBytecode->size()));

        // Analysis float4 count: sum over typed-field pixel counts.
        UINT32 analysisFloat4Count = 0;
        for (const auto& f : def.analysisFields)
            analysisFloat4Count += f.pixelCount();

        // Image-output dimensions: DiagramSize / OutputSize for square
        // viewers, or OutputWidth + OutputHeight for non-square viewers,
        // fallback to upstream input bounds. 0/0 = analysis-only (no
        // u1 binding).
        UINT32 imageOutW = 0, imageOutH = 0;
        bool hasImageOutput = !node.outputPins.empty();
        if (hasImageOutput)
        {
            // Pass 1: explicit OutputWidth + OutputHeight (non-square).
            UINT32 explicitW = 0, explicitH = 0;
            for (const auto& [key, val] : node.properties)
            {
                if (key == L"OutputWidth")
                {
                    if (auto* f = std::get_if<float>(&val))
                        explicitW = (*f >= 1.0f) ? static_cast<UINT32>(*f) : 0;
                    else if (auto* u = std::get_if<uint32_t>(&val))
                        explicitW = *u;
                }
                else if (key == L"OutputHeight")
                {
                    if (auto* f = std::get_if<float>(&val))
                        explicitH = (*f >= 1.0f) ? static_cast<UINT32>(*f) : 0;
                    else if (auto* u = std::get_if<uint32_t>(&val))
                        explicitH = *u;
                }
            }
            if (explicitW > 0 && explicitH > 0)
            {
                imageOutW = explicitW;
                imageOutH = explicitH;
            }
            // Pass 2: square sizing via DiagramSize / OutputSize.
            if (imageOutW == 0)
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
            }
            if (imageOutW == 0)
            {
                // Fall back to upstream input #0 dimensions (96 DPI bounds
                // from the pre-rendered bitmap, if any, else the input
                // image's bounds). For multi-input shaders we still drive
                // dispatch sizing from t0 -- the contract is that all
                // inputs match dimensions.
                ID2D1Bitmap1* primaryPreRendered =
                    preRenderedInputs.empty() ? nullptr : preRenderedInputs[0];
                ID2D1Image* primaryInput =
                    inputImages.empty() ? nullptr : inputImages[0];
                if (primaryPreRendered)
                {
                    auto sz = primaryPreRendered->GetPixelSize();
                    imageOutW = sz.width; imageOutH = sz.height;
                }
                else if (primaryInput)
                {
                    float oldDpiX, oldDpiY;
                    dc->GetDpi(&oldDpiX, &oldDpiY);
                    dc->SetDpi(96.0f, 96.0f);
                    D2D1_RECT_F bounds{};
                    dc->GetImageLocalBounds(primaryInput, &bounds);
                    D2D1_RECT_L px{};
                    if (SUCCEEDED(Effects::SnapComputeInputRect(bounds, px)))
                    {
                        imageOutW = static_cast<UINT32>(px.right - px.left);
                        imageOutH = static_cast<UINT32>(px.bottom - px.top);
                    }
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
        // Reflection runs against the variant bytecode if we swapped;
        // GPU-bound params simply have no cbuffer slot in the variant
        // and PackPropertyToCBuffer skips them naturally.
        std::vector<BYTE> cbBytes;
        if (!def.parameters.empty() && !reflectBytecode->empty())
        {
            // Layout memoized per bytecode (it never changes for given bytes).
            auto cb = ReflectComputeCb(*reflectBytecode);
            if (cb && cb->sizeBytes > 8)
            {
                cbBytes.assign(cb->sizeBytes - 8, BYTE{ 0 });
                for (const auto& var : cb->vars)
                {
                    if (var.offset < 8) continue; // skip Width/Height
                    const UINT32 destOff = var.offset - 8;
                    if (destOff >= cbBytes.size()) continue;
                    const UINT32 remaining = static_cast<UINT32>(cbBytes.size() - destOff);

                    // Phase 8 GPU-binding index slot. Variant
                    // bytecode declares a `uint _SLIdx_<paramName>;`
                    // for each gpuBindable param routed via SRV.
                    // The host packs the upstream's float4 index
                    // here so the consumer's shader reads the right
                    // slot via _SLBuf_<name>[_SLIdx_<name>].
                    if (var.name.starts_with(L"_SLIdx_"))
                    {
                        const std::wstring paramName = var.name.substr(7);
                        for (const auto& be : bindingPlan)
                        {
                            if (be.paramName == paramName)
                            {
                                uint32_t idx = be.fieldIndex;
                                if (remaining >= sizeof(uint32_t))
                                    memcpy(cbBytes.data() + destOff, &idx, sizeof(uint32_t));
                                break;
                            }
                        }
                        continue;
                    }

                    auto propIt = node.properties.find(var.name);
                    if (propIt == node.properties.end()) continue;
                    Effects::PackPropertyToCBuffer(
                        cbBytes.data() + destOff, remaining,
                        var.type, var.cols, propIt->second);
                }
            }
        }

        // Drive the dispatch through the bridge. The bridge handles
        // pre-rendering internally; if we have pre-rendered bitmaps
        // it's a cheap blit (same FP32 format). Use them directly when
        // available so D2D doesn't re-evaluate upstream effects with
        // potentially-different cached state.
        std::vector<ID2D1Image*> dispatchInputs(inputImages.size(), nullptr);
        for (size_t i = 0; i < inputImages.size(); ++i)
        {
            ID2D1Bitmap1* preRendered = (i < preRenderedInputs.size())
                ? preRenderedInputs[i] : nullptr;
            dispatchInputs[i] = preRendered
                ? static_cast<ID2D1Image*>(preRendered)
                : inputImages[i];
        }

        // Phase 8 GPU bindings: register each upstream SRV with its
        // declared t-slot. Bridge clears these at the end of Dispatch.
        for (const auto& e : bindingPlan)
            bridge->SetGpuBinding(e.slot, e.srv.get());

        // Image-producing per-pixel computes need dispatch dims that
        // cover the full output. analysis-only and "fixed-size loop"
        // image producers (e.g. CIE Histogram, Vectorscope) keep (1,1,1)
        // because their shader does its own internal iteration over the
        // source pixels into groupshared accumulators.
        //
        // Heuristic for "single group" vs "per-pixel tile":
        //   * Effects whose descriptor declares a `DiagramSize` /
        //     `OutputSize` parameter are visualization viewers that
        //     compute their output independently of the source resolution.
        //     Their shader assumes ONE thread group covers the whole
        //     output. Tiling them across the output (W/tx, H/ty)
        //     dispatches the same redundant histogram-of-the-whole-image
        //     N times -- on a 4K source feeding a 512x512 CIE Histogram
        //     that's a 256x amplification of an already heavy inner
        //     loop. Keep dispatch at (1,1,1) for these.
        //   * Otherwise tile per-pixel (the ICtCp Tone Map style: each
        //     thread group covers a [numthreads] tile of the output).
        if (hasImageOutput && imageOutW > 0 && imageOutH > 0 &&
            def.threadGroupX > 0 && def.threadGroupY > 0)
        {
            // Discriminator: single-group scatter shaders use a large
            // thread group (commonly 32x32 = 1024) and dispatch (1,1,1);
            // per-pixel-tile shaders use a small group (commonly 8x8 = 64)
            // and dispatch (W/tx, H/ty, 1). Threshold at 256 cleanly
            // separates the two conventions in the current catalog.
            //
            // The DiagramSize / OutputSize parameter names ALSO mark fixed-
            // output viewers (CIE Histogram, etc.) that pre-date the
            // generic threshold; keep that check for descriptors that use
            // an 8x8 group but still want single-group dispatch (none
            // exist today, but the check is cheap and safe).
            const bool largeGroup = (def.threadGroupX * def.threadGroupY) >= 256;
            bool isFixedSizeViewer = largeGroup;
            for (const auto& p : def.parameters)
            {
                if (p.name == L"DiagramSize" || p.name == L"OutputSize")
                {
                    isFixedSizeViewer = true;
                    break;
                }
            }
            const bool perPixelTiling =
                !isFixedSizeViewer &&
                def.threadGroupX * def.threadGroupY >= 4 &&
                def.threadGroupY >= 2;
            if (perPixelTiling)
            {
                UINT32 dx = (imageOutW + def.threadGroupX - 1) / def.threadGroupX;
                UINT32 dy = (imageOutH + def.threadGroupY - 1) / def.threadGroupY;
                bridge->SetDispatchDims(dx, dy, 1);
            }
        }

        std::vector<float> analysisFloats;
        // Phase 8c: pass nullptr for outAnalysisFloats when readback is
        // not needed; the bridge interprets that as "skip the Map" and
        // returns an empty `floats` vector. The structured-buffer SRV
        // is still populated on the GPU side for downstream consumers.
        HRESULT hr = bridge->Dispatch(
            dc,
            dispatchInputs.data(),
            static_cast<UINT32>(dispatchInputs.size()),
            cbBytes.empty() ? nullptr : cbBytes.data(),
            static_cast<UINT32>(cbBytes.size()),
            analysisFloat4Count,
            imageOutW, imageOutH,
            readbackToCpu ? &analysisFloats : nullptr);

        if (!readbackToCpu)
            Performance::IncrementSkippedCpuReadbacks();

        if (FAILED(hr))
        {
            node.runtimeError = std::format(L"Bridge dispatch failed 0x{:08X}",
                static_cast<uint32_t>(hr));
            return;
        }
        node.runtimeError.clear();

        // Unpack analysis floats into typed fields. Phase 8c: only
        // overwrite when readback actually ran -- skip-readback frames
        // leave `node.analysisOutput.fields` at the previous-frame
        // values (or empty if never populated). Hosts that need fresh
        // values must include the node in
        // `GraphEvaluator::SetCpuAnalysisInterest` (see Performance.h).
        // An async readback returns nothing now -- PollAsyncReadbacks unpacks
        // it when it lands -- and a failed Map returns nothing too; neither
        // may overwrite the previous values with zeros.
        if (readbackToCpu && analysisFloat4Count > 0 && !analysisFloats.empty())
            UnpackAnalysisFloats(node, analysisFloats);

        // Image-producing nodes wire their output to the bridge's
        // wrapped bitmap. Downstream nodes consume node->cachedOutput
        // directly, same as for D2D-tiled compute / pixel-shader nodes.
        if (hasImageOutput)
            node.cachedOutput = bridge->GetImageOutput();
        else
            node.cachedOutput = nullptr;
    }

    // Phase 8c: predicate matching the GPU-routability checks used inside
    // DispatchViaBridge (see bindingPlan construction). Conservative:
    // any condition that would cause DispatchViaBridge to skip GPU
    // routing for the binding returns false here, so the pre-pass
    // counts the source as needing CPU readback. Keep this in sync
    // with the bindingPlan construction in DispatchViaBridge -- if a
    // new GPU-routability requirement is added there, mirror it here.
    GraphEvaluator::GpuBindMode GraphEvaluator::CanServeBindingViaGpu(
        const EffectNode&         consumer,
        const std::wstring&       paramName,
        const Graph::PropertyBinding& binding,
        const EffectGraph&        graph) const
    {
        using Mode = GpuBindMode;
        if (!Performance::IsGpuBindingsEnabled())
            return Mode::None;
        if (!consumer.customEffect.has_value())
            return Mode::None;

        // The lane depends on what the CONSUMER can bind.
        //   D3D11 compute -> lane 1: CustomComputeBridgeEffect controls its own
        //     dispatch, so it can bind an upstream SRV at any t-slot.
        //   Pixel shader  -> lane 3: Direct2D owns the draw and maps only a
        //     transform's INPUTS to t0, t1, ..., so the values have to arrive
        //     as an image. (CreateResourceTexture is not a way round this: it
        //     takes const BYTE* CPU memory and would reintroduce the readback.)
        // Anything else -- D2D-tiled, built-in D2D -- has no GPU-side route.
        Mode mode = Mode::None;
        switch (consumer.customEffect->shaderType)
        {
        case Graph::CustomShaderType::D3D11ComputeShader: mode = Mode::StructuredBuffer; break;
        case Graph::CustomShaderType::PixelShader:        mode = Mode::Texture;          break;
        default:                                          return Mode::None;
        }
        // Single-component bindings only -- multi-source per-component
        // packing always goes CPU.
        if (binding.wholeArray ||
            binding.sources.empty() ||
            binding.sources.size() > 1 ||
            !binding.sources[0].has_value())
            return Mode::None;
        // Target parameter must be flagged gpuBindable.
        const auto& def = consumer.customEffect.value();
        const Graph::ParameterDefinition* paramDef = nullptr;
        for (const auto& p : def.parameters)
        {
            if (p.name == paramName) { paramDef = &p; break; }
        }
        if (!paramDef || !paramDef->gpuBindable)
            return Mode::None;
        // Source bridge must exist and expose IEngineComputeOutput. We
        // don't actually call GetAnalysisSrv here (it can fail before
        // first dispatch); presence of the bridge in m_bridgeImplCache
        // and a customEffect on the source is the predicate. The
        // pre-pass runs before DispatchViaBridge during the same
        // ProcessDeferredCompute, so by the time the consumer
        // dispatches, the upstream's runner has produced its SRV.
        const uint32_t srcId = binding.sources[0]->sourceNodeId;
        auto bridgeIt = m_bridgeImplCache.find(srcId);
        if (bridgeIt == m_bridgeImplCache.end() || !bridgeIt->second)
            return Mode::None;
        const EffectNode* srcNode = graph.FindNode(srcId);
        if (!srcNode || !srcNode->customEffect.has_value())
            return Mode::None;
        // Source field must exist on the upstream's analysisFields.
        const auto& srcFieldName = binding.sources[0]->sourceFieldName;
        bool fieldFound = false;
        for (const auto& fd : srcNode->customEffect->analysisFields)
        {
            if (fd.name == srcFieldName) { fieldFound = true; break; }
        }
        return fieldFound ? mode : Mode::None;
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
        // The lane-3 wrappers alias textures owned by the bridges being
        // dropped here; keeping them would leave D2D bitmaps over freed D3D
        // resources.
        m_analysisBitmapCache.clear();
        m_analysisBitmapSource.clear();
        m_analysisTextureWanted.clear();
        m_analysisDummyBitmap = nullptr;
        m_sharedPreRenderCache.clear();
        m_asyncReadbackPending.clear();
        m_histogramTarget = nullptr;
        m_analysisTarget = nullptr;
        m_analysisCpuBitmap = nullptr;
        m_lastHintReadbackTime.clear();
        m_cacheEnabled.clear();
        m_queuedComputeThisEval.clear();
        m_dummySourceBitmap = nullptr;
        m_pixelLoadedVariant.clear();
        m_appliedGpuBits.clear();
        m_optionPrecompile.clear();
        m_wantedVariant.clear();

        // P7: also drop any deferred-compute entries that the previous
        // Evaluate left pending for ProcessDeferredCompute. They hold raw
        // ID2D1Image* pointers to the cachedOutputs we just released; if
        // we don't clear here, the next ProcessDeferredCompute call (from
        // the post-SwitchAdapter worker, or any path that resets the
        // device) deref's freed bitmaps and AVs in d2d1.dll.
        m_deferredCompute.clear();
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
            node.variantsCompiling = 0;
        }
    }

    void GraphEvaluator::InvalidateNode(uint32_t nodeId)
    {
        m_effectCache.erase(nodeId);
        m_outputCache.erase(nodeId);
        m_customImplCache.erase(nodeId);
        m_bridgeImplCache.erase(nodeId);
        m_cacheEnabled.erase(nodeId);
        m_asyncReadbackPending.erase(nodeId);
        ForgetVariantState(nodeId);
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
            node->variantsCompiling = 0;
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
                    {
                        if (IsFinitePropertyValue(eit->second))
                            node.properties[propName] = eit->second;
                        else
                            node.runtimeError = L"binding '" + propName +
                                L"' resolved to a non-finite value; previous kept";
                    }
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
            m_cacheEnabled.erase(nodeId);
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
            implIt->second.pixelImpl->SetShaderGuid(ShaderGuidFor(def.shaderGuid, def.compiledBytecode));
            implIt->second.pixelImpl->LoadShaderBytecode(
                def.compiledBytecode.data(),
                static_cast<UINT32>(def.compiledBytecode.size()));
            m_pixelLoadedVariant[nodeId] = IdOfBytecode(def.compiledBytecode);
        }
        else if (node.type == NodeType::ComputeShader && implIt->second.computeImpl)
        {
            implIt->second.computeImpl->SetShaderGuid(ShaderGuidFor(def.shaderGuid, def.compiledBytecode));
            implIt->second.computeImpl->LoadShaderBytecode(
                def.compiledBytecode.data(),
                static_cast<UINT32>(def.compiledBytecode.size()));
            implIt->second.computeImpl->SetThreadGroupSize(
                def.threadGroupX, def.threadGroupY, def.threadGroupZ);
        }

        // The effect instance survives the bytecode swap, so any cached
        // output intermediate is now stale — drop it. The next Evaluate
        // re-enables caching after the fresh render.
        if (effectIt != m_effectCache.end())
        {
            auto cacheIt = m_cacheEnabled.find(nodeId);
            if (cacheIt != m_cacheEnabled.end() && cacheIt->second)
            {
                effectIt->second->SetValue(D2D1_PROPERTY_CACHED, FALSE);
                cacheIt->second = false;
                ++m_cacheInvalidations;
            }
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
            // D2D custom effects require at least 1 input. Source effects
            // (empty inputNames) get a hidden input fed by a dummy bitmap.
            // Declared image inputs plus one reserved pin per gpu-bindable
            // parameter (pixel shaders only). See TotalInputCount.
            UINT32 inputCount = TotalInputCount(node, node.customEffect.value());
            // A CLSID registers once, so each input count gets its own.
            clsid = node.customEffect->shaderGuid;
            clsid.Data1 ^= inputCount;

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
            // Must match the count used at registration exactly -- D2D sizes
            // the transform's input array from this.
            UINT32 ic = TotalInputCount(node, node.customEffect.value());
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
            // Split the pins: the first DeclaredInputCount are image inputs,
            // the rest are reserved for analysis textures. The impl needs this
            // to compute output bounds correctly -- see SetImageInputCount.
            if (node.customEffect.has_value())
                impl->SetImageInputCount(GeometricInputCount(node.customEffect.value()));
            m_customImplCache[node.id] = { impl, nullptr };

            // For zero-input source effects, set fixed output size from properties.
            if (node.customEffect.has_value() && node.customEffect->inputNames.empty())
            {
                UINT32 outW = 512, outH = 512;
                // Look for OutputSize/DiagramSize/PlateSize/GradSize/PatternSize/PatchSize property.
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

        // Re-setting an input D2D already holds may count as a topology
        // change and drop the effect's D2D1_PROPERTY_CACHED intermediate,
        // so only call SetInput when the pointer actually differs.
        auto setInputIfChanged = [effect](UINT32 pin, ID2D1Image* desired)
        {
            winrt::com_ptr<ID2D1Image> current;
            effect->GetInput(pin, current.put());
            if (current.get() != desired)
                effect->SetInput(pin, desired);
        };

        for (const auto* edge : inputEdges)
        {
            if (edge->destPin >= totalInputs)
                continue;

            const EffectNode* srcNode = graph.FindNode(edge->sourceNodeId);
            if (srcNode && srcNode->cachedOutput)
            {
                setInputIfChanged(edge->destPin, srcNode->cachedOutput);
                connected[edge->destPin] = true;
            }
        }

        // Clear inputs that are no longer connected -- but STOP at the
        // declared image inputs. The pins past that point carry lane-3
        // analysis textures, which are attached by the binding pass rather
        // than by a graph edge; clearing them here would unbind the analysis
        // texture on every frame, and the shader would sample an empty pin
        // while still compiled to read from it.
        UINT32 clearLimit = totalInputs;
        UINT32 firstLookup = totalInputs;
        if (destNode.type == NodeType::PixelShader && destNode.customEffect.has_value())
        {
            clearLimit = (std::min)(totalInputs,
                                    DeclaredInputCount(destNode.customEffect.value()));
            firstLookup = (std::min)(clearLimit,
                                     GeometricInputCount(destNode.customEffect.value()));
        }
        for (UINT32 i = 0; i < clearLimit; ++i)
        {
            if (connected[i]) continue;
            // An unwired LOOKUP pin gets the 1x1 zero placeholder rather than
            // null: D2D renders nothing at all for an effect with a null input,
            // so leaving an optional table pin empty would blank the node. The
            // placeholder's alpha of 0 is how the shader reads "no table".
            // (ApplyCustomEffect creates it before this runs.)
            // So do a variadic node's unconnected pins, shown or not.
            const bool variadic = destNode.customEffect.has_value() && destNode.customEffect->variadicInputs;
            ID2D1Image* fill = nullptr;
            if ((i >= firstLookup || variadic) && m_analysisDummyBitmap)
                fill = static_cast<ID2D1Image*>(m_analysisDummyBitmap.get());
            setInputIfChanged(i, fill);
        }
    }

    // -----------------------------------------------------------------------
    // Clean-subgraph output caching policy
    // -----------------------------------------------------------------------

    void GraphEvaluator::UpdateEffectCachePolicy(
        ID2D1Effect* effect, uint32_t nodeId, bool contentDirty)
    {
        if (!effect)
            return;

        bool& enabled = m_cacheEnabled[nodeId];   // default-inserts false

        if (!Performance::IsEffectOutputCachingEnabled())
        {
            // Kill switch: release any held intermediate and stop caching.
            if (enabled)
            {
                effect->SetValue(D2D1_PROPERTY_CACHED, FALSE);
                enabled = false;
            }
            return;
        }

        if (contentDirty)
        {
            // Content changed through a channel D2D can't see (in-place
            // texture update or direct cbuffer upload). Drop the cache and
            // STAY uncached while dirty: an uncached effect re-executes on
            // every pull, which both picks up the fresh cbuffer and — the
            // hysteresis part — avoids touching the CACHED property again
            // next frame. Measured: any per-frame property transition on a
            // consumer (input toggle OR the CACHED off->on poke) causes
            // D2D to drop its PRODUCERS' cached intermediates as
            // collateral, so a per-frame-dirty node (an animated split)
            // must generate ZERO property traffic to let its upstream
            // caches survive. Cache re-enables one frame after the node
            // goes clean.
            if (enabled)
            {
                effect->SetValue(D2D1_PROPERTY_CACHED, FALSE);
                enabled = false;
                ++m_cacheInvalidations;
            }
            return;
        }

        if (!enabled)
        {
            effect->SetValue(D2D1_PROPERTY_CACHED, TRUE);
            enabled = true;
        }
    }

    // -----------------------------------------------------------------------
    // Property binding resolution
    // -----------------------------------------------------------------------

    // Exact-equality compare for PropertyValue. std::variant's operator==
    // is unusable here because D2D1_MATRIX_5X4_F has no operator==; the
    // WinRT numerics are compared componentwise for the same reason.
    // Exact float compare is intentional: binding sources are
    // deterministic frame to frame, so "unchanged" means bit-identical.
    static bool PropertyValuesEqual(
        const Graph::PropertyValue& a, const Graph::PropertyValue& b)
    {
        namespace num = winrt::Windows::Foundation::Numerics;
        if (a.index() != b.index()) return false;
        return std::visit([&b](const auto& av) -> bool
        {
            using T = std::decay_t<decltype(av)>;
            const T* bv = std::get_if<T>(&b);
            if (!bv) return false;
            if constexpr (std::is_same_v<T, D2D1_MATRIX_5X4_F>)
                return std::memcmp(&av, bv, sizeof(T)) == 0;
            else if constexpr (std::is_same_v<T, num::float2>)
                return av.x == bv->x && av.y == bv->y;
            else if constexpr (std::is_same_v<T, num::float3>)
                return av.x == bv->x && av.y == bv->y && av.z == bv->z;
            else if constexpr (std::is_same_v<T, num::float4>)
                return av.x == bv->x && av.y == bv->y &&
                       av.z == bv->z && av.w == bv->w;
            else
                return av == *bv;
        }, a);
    }

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

            // Phase 8: detect GPU-routable bindings before doing CPU
            // readback. A binding is GPU-routable when:
            //   * the feature flag is on,
            //   * the consumer's parameter is flagged gpuBindable in
            //     its CustomEffectDefinition,
            //   * the upstream effect publishes IEngineComputeOutput.
            //
            // Discovery uses m_bridgeImplCache directly: D2D's outer
            // ID2D1Effect's QI does not delegate arbitrary IIDs to the
            // inner impl, so a raw QI on the cached effect would always
            // fail for IEngineComputeOutput. The bridge impl pointer
            // we captured during CreateOrGetEffect (commit 2f69acd)
            // is the canonical "is this upstream a GPU-output producer"
            // signal -- the bridge always implements IEngineComputeOutput
            // by delegation to its internal D3D11ComputeRunner.
            //
            // The actual SRV-to-t-slot routing happens in DispatchViaBridge
            // for D3D11 compute consumers; this branch only bumps a
            // detection telemetry counter so we can see how many
            // bindings *could* be routed regardless of consumer kind.
            if (Performance::IsGpuBindingsEnabled() &&
                node.customEffect.has_value() &&
                !binding.wholeArray)
            {
                bool propGpuBindable = false;
                for (const auto& p : node.customEffect->parameters)
                {
                    if (p.name == propName) { propGpuBindable = p.gpuBindable; break; }
                }
                if (propGpuBindable && !binding.sources.empty() &&
                    binding.sources[0].has_value())
                {
                    uint32_t srcId = binding.sources[0]->sourceNodeId;
                    auto bridgeIt = m_bridgeImplCache.find(srcId);
                    if (bridgeIt != m_bridgeImplCache.end() && bridgeIt->second)
                    {
                        Performance::IncrementGpuBindingDetection();
                    }
                }
            }

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
                        // Only report a change when the resolved value
                        // actually differs — "resolved every frame" used
                        // to mean "changed every frame", which re-applied
                        // properties and defeated output caching on every
                        // binding consumer even for static values.
                        PropertyValue resolvedArr = fv.arrayData;
                        if (!PropertyValuesEqual(propIt->second, resolvedArr))
                        {
                            effectiveProps[propName] = std::move(resolvedArr);
                            anyChanged = true;
                        }
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
                // Value-compare before reporting change (see whole-array
                // note above): the resolved value equals the stored one on
                // every frame where the source didn't move, and reporting
                // "changed" then would dirty the consumer needlessly.
                if (!PropertyValuesEqual(propIt->second, newVal))
                {
                    effectiveProps[propName] = newVal;
                    anyChanged = true;
                }
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
        const std::map<std::wstring, PropertyValue>& effectiveProps,
        const EffectGraph& graph,
        ID2D1DeviceContext5* dc)
    {
        if (!node.customEffect.has_value()) return;
        auto& def = node.customEffect.value();

        auto implIt = m_customImplCache.find(node.id);
        if (implIt == m_customImplCache.end())
            return;

        // ---- lane 3: analysis textures into a pixel shader ----------------
        // Build the plan first: it decides which shader VARIANT to load, so it
        // has to run before the bytecode is chosen.
        //
        // A parameter only goes texture-mode if the producer has actually
        // published a bitmap. Before the first compute dispatch there is none,
        // so the parameter stays cbuffer-bound and reads the CPU value -- the
        // path degrades to correct-but-slower rather than to wrong. Once the
        // producer has run, the bitmap persists and the mode is stable.
        struct AnalysisBind { uint32_t pin; uint32_t fieldIndex; std::wstring name; ID2D1Bitmap1* bmp; };
        std::vector<AnalysisBind> analysisBinds;
        uint32_t gpuModeBits = 0;
        if (node.type == NodeType::PixelShader &&
            Performance::IsGpuBindingsEnabled() &&
            GpuBindablePinCount(def) > 0)
        {
            const uint32_t declared = DeclaredInputCount(def);
            uint32_t gi = 0;
            for (const auto& prm : def.parameters)
            {
                if (!prm.gpuBindable) continue;
                const uint32_t idx = gi++;

                auto bIt = node.propertyBindings.find(prm.name);
                if (bIt == node.propertyBindings.end()) continue;
                const auto& binding = bIt->second;
                if (binding.wholeArray || binding.sources.size() != 1 ||
                    !binding.sources[0].has_value())
                    continue;

                const uint32_t srcId = binding.sources[0]->sourceNodeId;
                ID2D1Bitmap1* bmp = AnalysisBitmap(srcId);
                if (!bmp) continue;

                const EffectNode* srcNode = graph.FindNode(srcId);
                if (!srcNode || !srcNode->customEffect.has_value()) continue;

                // Same prefix-sum the structured-buffer lane uses, so both
                // lanes address a field identically.
                uint32_t fieldIndex = 0;
                bool found = false;
                for (const auto& fd : srcNode->customEffect->analysisFields)
                {
                    if (fd.name == binding.sources[0]->sourceFieldName)
                    {
                        fieldIndex += binding.sources[0]->sourceIndex;
                        found = true;
                        break;
                    }
                    fieldIndex += fd.pixelCount();
                }
                if (!found) continue;

                analysisBinds.push_back({ declared + idx, fieldIndex, prm.name, bmp });
                gpuModeBits |= (2u << (2u * idx));   // mode 2 for this parameter
            }
        }

        // Pick the bytecode the plan implies. Falling back to baseline on a
        // cache miss keeps the node rendering (cbuffer mode, CPU values)
        // rather than failing.
        // Specialised options apply to pixel shaders only; a D2D-tiled compute
        // node always runs its generic build.
        const std::vector<uint8_t>* activeBytecode = &def.compiledBytecode;
        std::shared_ptr<const std::vector<uint8_t>> variantBytes;
        uint64_t optionKey = 0;
        if (node.type == NodeType::PixelShader)
        {
            QueueOptionVariants(node, "ps_5_0", IntendedGpuBits(node, /*pixel*/ true));
            optionKey = Effects::OptionKeyFor(def, effectiveProps);
        }
        if (gpuModeBits != 0 || optionKey != 0)
        {
            const std::wstring variantEffectId = def.shaderLabEffectId.empty() ? node.name : def.shaderLabEffectId;
            const uint32_t variantEffectVersion = def.shaderLabEffectVersion ? def.shaderLabEffectVersion : 1u;
            const auto choice = SelectVariant(node, variantEffectId, variantEffectVersion, "ps_5_0",
                                              gpuModeBits, optionKey);
            if (gpuModeBits != 0 && choice.bits == 0)
                analysisBinds.clear();
            gpuModeBits  = choice.bits;
            optionKey    = choice.options;
            variantBytes = choice.bytes;
            if (variantBytes && !variantBytes->empty())
                activeBytecode = variantBytes.get();
        }
        m_appliedGpuBits[node.id] = gpuModeBits;

        // Fill EVERY reserved pin -- bound ones with their analysis bitmap,
        // the rest with a 1x1 dummy. Direct2D will not render a custom effect
        // that has an unconnected input: the output rect comes back empty and
        // the node draws nothing, with no error raised anywhere. Leaving the
        // unbound pins null therefore breaks the effect even when no binding
        // is in play, which is exactly how this surfaced -- a tone mapper with
        // no bindings at all rendered nothing the moment the pins existed.
        // WireInputs (which runs next) fills unwired lookup pins with the
        // placeholder, and has no device context to create it with.
        if (effect && node.type == NodeType::PixelShader && (def.lookupInputCount > 0 || def.variadicInputs))
            EnsureAnalysisDummy(dc);

        if (effect && node.type == NodeType::PixelShader && node.customEffect.has_value())
        {
            const uint32_t declared = DeclaredInputCount(def);
            const uint32_t total    = TotalInputCount(node, def);
            if (total > declared)
            {
                EnsureAnalysisDummy(dc);
                for (uint32_t pin = declared; pin < total; ++pin)
                {
                    ID2D1Image* desired = static_cast<ID2D1Image*>(m_analysisDummyBitmap.get());
                    for (const auto& ab : analysisBinds)
                        if (ab.pin == pin) { desired = static_cast<ID2D1Image*>(ab.bmp); break; }
                    if (!desired) continue;
                    winrt::com_ptr<ID2D1Image> current;
                    effect->GetInput(pin, current.put());
                    // Only on change: re-setting an input D2D already holds
                    // counts as a topology change and drops the cached
                    // intermediate.
                    if (current.get() != desired)
                        effect->SetInput(pin, desired);
                }
            }
        }

        // Load bytecode only if not already loaded (bytecode doesn't change
        // on property updates, only on recompile/update-in-graph).
        bool needsShaderLoad = false;
        if (node.type == NodeType::PixelShader && implIt->second.pixelImpl)
        {
            const BytecodeId wanted = IdOfBytecode(*activeBytecode);
            auto loaded = m_pixelLoadedVariant.find(node.id);
            if (implIt->second.pixelImpl->NeedsShaderLoad() || loaded == m_pixelLoadedVariant.end() ||
                loaded->second != wanted)
            {
                implIt->second.pixelImpl->SetShaderGuid(ShaderGuidFor(def.shaderGuid, *activeBytecode));
                implIt->second.pixelImpl->LoadShaderBytecode(
                    activeBytecode->data(),
                    static_cast<UINT32>(activeBytecode->size()));
                m_pixelLoadedVariant[node.id] = wanted;
                needsShaderLoad = true;
            }
        }
        else if (node.type == NodeType::ComputeShader && implIt->second.computeImpl)
        {
            implIt->second.computeImpl->SetShaderGuid(ShaderGuidFor(def.shaderGuid, def.compiledBytecode));
            implIt->second.computeImpl->LoadShaderBytecode(
                def.compiledBytecode.data(),
                static_cast<UINT32>(def.compiledBytecode.size()));
            implIt->second.computeImpl->SetThreadGroupSize(
                def.threadGroupX, def.threadGroupY, def.threadGroupZ);
        }

        // Reflect the bytecode to discover cbuffer layout.
        // Reflect what is actually loaded. A texture-mode parameter swaps its
        // cbuffer value slot for a uint index slot, so every offset after it
        // moves -- reflecting the baseline would pack every later parameter
        // at the wrong offset.
        std::shared_ptr<const Effects::ShaderReflectionResult> reflectionPtr;
        {
            const BytecodeId rid = IdOfBytecode(*activeBytecode);
            auto rit = m_pixelReflectMemo.find(rid);
            if (rit != m_pixelReflectMemo.end())
                reflectionPtr = rit->second;
            else
            {
                if (m_pixelReflectMemo.size() > 256) m_pixelReflectMemo.clear();
                reflectionPtr = std::make_shared<const Effects::ShaderReflectionResult>(
                    Effects::ShaderCompiler::Reflect(*activeBytecode));
                m_pixelReflectMemo[rid] = reflectionPtr;
            }
        }
        const auto& reflection = *reflectionPtr;
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

                // Typed pack: converts float-stored enum properties to the
                // declared uint/int/bool HLSL slot type. The previous raw
                // memcpy here wrote float bit patterns into uint slots, so
                // `uint Mode` read 3.0f as 1077936128 and every uint-enum
                // switch on the D2D pixel/compute-shader path silently fell
                // through to its default branch for any non-zero value.
                if (var.offset < cbData.size())
                {
                    Effects::PackPropertyToCBuffer(
                        cbData.data() + var.offset,
                        static_cast<uint32_t>(cbData.size()) - var.offset,
                        var.type, var.columns, propIt->second);
                }
            }

            // Lane 3: the variant replaced each bound parameter's value slot
            // with `uint _SLIdx_<name>`, which no property matches by name --
            // the host supplies the field index that the shader Loads with.
            for (const auto& ab : analysisBinds)
            {
                const std::wstring slot = L"_SLIdx_" + ab.name;
                for (const auto& var : cb.variables)
                {
                    if (var.name != slot) continue;
                    if (var.offset + sizeof(uint32_t) > cbData.size()) break;
                    const uint32_t v = ab.fieldIndex;
                    std::memcpy(cbData.data() + var.offset, &v, sizeof(v));
                    break;
                }
            }

            // Derived constants: parameter-only tables a built-in computes on
            // the CPU instead of per pixel. Written only into variables this
            // bytecode declares, so older saved HLSL is untouched.
            if (!def.shaderLabEffectId.empty())
            {
                const auto* slDesc =
                    Effects::ShaderLabEffects::Instance().FindById(def.shaderLabEffectId);
                if (slDesc && slDesc->deriveConstants)
                {
                    slDesc->deriveConstants(effectiveProps,
                        [&](std::wstring_view name, const float* data, size_t floatCount)
                        {
                            for (const auto& var : cb.variables)
                            {
                                if (var.name != name) continue;
                                if (var.offset >= cbData.size()) return;
                                size_t bytes = (std::min)(floatCount * sizeof(float),
                                    static_cast<size_t>(var.size));
                                bytes = (std::min)(bytes, cbData.size() - var.offset);
                                std::memcpy(cbData.data() + var.offset, data, bytes);
                                return;
                            }
                        });
                }
            }

            // Set the packed cbuffer on the concrete impl.
            if (node.type == NodeType::PixelShader && implIt->second.pixelImpl)
            {
                implIt->second.pixelImpl->SetConstantBufferData(cbData.data(), static_cast<UINT32>(cbData.size()));

                // Fixed-output sizing.
                //   - Source effects (no inputs): always size to a Size/PatchSize
                //     param so D2D doesn't render a 0x0 or full-source-bounds
                //     output.
                //   - Effects with inputs: only fix the output size if the
                //     descriptor *explicitly* declares a `DiagramSize`/`OutputSize`/
                //     `PatchSize` parameter. Without this, an analysis-style
                //     viewer like Gamut Coverage / Waveform Monitor (whose pixel
                //     shader does an O(N) inner loop per output pixel) would
                //     render at the full source resolution -- billions of
                //     samples per frame at 4K, hanging the GPU. The shader
                //     already uses `DiagramSize` to map output coords to its
                //     visualization domain, so honoring it as the actual
                //     output size keeps D2D in sync with shader intent.
                bool hasFixedSizeParam = false;
                UINT32 outW = 512, outH = 512;
                {
                    auto pickFloat = [&](const std::wstring& key, UINT32* w, UINT32* h) -> bool {
                        auto it = effectiveProps.find(key);
                        if (it == effectiveProps.end()) return false;
                        if (auto* f = std::get_if<float>(&it->second))
                        {
                            *w = static_cast<UINT32>(*f);
                            *h = static_cast<UINT32>(*f);
                            return true;
                        }
                        return false;
                    };
                    if (pickFloat(L"DiagramSize", &outW, &outH))      hasFixedSizeParam = true;
                    else if (pickFloat(L"OutputSize", &outW, &outH))  hasFixedSizeParam = true;
                    else
                    {
                        auto patchIt = effectiveProps.find(L"PatchSize");
                        if (patchIt != effectiveProps.end())
                        {
                            if (auto* f = std::get_if<float>(&patchIt->second))
                            {
                                outW = static_cast<UINT32>(*f * 6);
                                outH = static_cast<UINT32>(*f * 4);
                                hasFixedSizeParam = true;
                            }
                        }
                    }
                    if (!hasFixedSizeParam)
                    {
                        // Generic fallback for source effects: any param whose
                        // name contains "Size" / "size".
                        if (node.customEffect->inputNames.empty())
                        {
                            for (const auto& [key, val] : effectiveProps)
                            {
                                if (key.find(L"Size") != std::wstring::npos ||
                                    key.find(L"size") != std::wstring::npos)
                                {
                                    if (auto* f = std::get_if<float>(&val))
                                    {
                                        outW = outH = static_cast<UINT32>(*f);
                                        hasFixedSizeParam = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
                const bool applyFixedSize =
                    hasFixedSizeParam ||
                    node.customEffect->inputNames.empty();
                if (applyFixedSize)
                    implIt->second.pixelImpl->SetFixedOutputSize(outW, outH);
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
        D2D1_RECT_L px{};
        if (FAILED(Effects::SnapComputeInputRect(bounds, px)))
        {
            node.runtimeError = L"histogram input is unbounded, empty or larger than 16384 px; crop it first";
            return;
        }
        uint32_t w = (std::min)(static_cast<uint32_t>(px.right - px.left), 4096u);
        uint32_t h = (std::min)(static_cast<uint32_t>(px.bottom - px.top), 4096u);

        // Recreate temp target if size changed.
        if (!m_histogramTarget || m_histogramTargetW != w || m_histogramTargetH != h)
        {
            m_histogramTarget = nullptr;
            D2D1_BITMAP_PROPERTIES1 props = {};
            props.pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED };
            props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
            HRESULT hr = dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, m_histogramTarget.put());
            if (FAILED(hr)) { dc->SetTarget(prevTarget.get()); return; }
            m_histogramTargetW = w;
            m_histogramTargetH = h;
        }

        dc->SetTarget(m_histogramTarget.get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->DrawImage(node.cachedOutput, D2D1::Point2F(
            -static_cast<float>(px.left), -static_cast<float>(px.top)));
        HRESULT histHr = dc->EndDraw();
        dc->SetTarget(prevTarget.get());
        if (FAILED(histHr))
        {
            node.runtimeError = L"histogram draw failed (nested draw session?)";
            std::fwprintf(stderr,
                L"[ShaderLab] ReadHistogramOutput node %u EndDraw failed "
                L"hr=0x%08X -- histogram is STALE\n",
                node.id, static_cast<unsigned>(histHr));
            return;
        }

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

        if (!m_analysisTarget || m_analysisTargetW != w || m_analysisTargetH != h)
        {
            m_analysisTarget = nullptr;
            D2D1_BITMAP_PROPERTIES1 props = {};
            props.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
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
        HRESULT drawHr = dc->EndDraw();
        dc->SetTarget(prevTarget.get());
        if (FAILED(drawHr))
        {
            // Silent before: nothing reached runtimeError, analysisOutput or
            // LastError, so a broken nested session produced stale-but-
            // plausible analysis fields with no diagnostic anywhere.
            node.runtimeError = L"analysis draw failed (nested draw session?)";
            std::fwprintf(stderr,
                L"[ShaderLab] ReadCustomAnalysisOutput node %u EndDraw failed "
                L"hr=0x%08X -- analysis fields are STALE\n",
                node.id, static_cast<unsigned>(drawHr));
            return;
        }

        // CPU-readable bitmap for the analysis row, kept across calls.
        uint32_t readW = (std::max)(totalPixels, 1u);
        if (!m_analysisCpuBitmap || m_analysisCpuBitmapW != readW)
        {
            m_analysisCpuBitmap = nullptr;
            D2D1_BITMAP_PROPERTIES1 cpuProps = {};
            cpuProps.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
            cpuProps.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
            HRESULT chr = dc->CreateBitmap(D2D1::SizeU(readW, 1), nullptr, 0, cpuProps,
                                           m_analysisCpuBitmap.put());
            if (FAILED(chr)) return;
            m_analysisCpuBitmapW = readW;
        }
        ID2D1Bitmap1* cpuBitmap = m_analysisCpuBitmap.get();
        HRESULT hr = S_OK;

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



}

