#include "pch_engine.h"
#include "EffectGraphImpl.h"
#include "../EngineBootstrap.h"
#include "../../Graph/GraphEdits.h"
#include "../../Effects/EffectBlob.h"
#include "../../Effects/EffectRegistry.h"
#include "../../Effects/NodeCatalog.h"
#include "../../Effects/ShaderLabEffects.h"
#include "../../Effects/SourceTextureUtil.h"
#include "../../Rendering/D2DHelpers.h"
#include "../../Rendering/EffectGraphFile.h"
#include "../../Rendering/SourceRuntime.h"
#include "../../Rendering/DisplayProfile.h"
#include "../../Rendering/WorkingSpaceSync.h"

namespace ShaderLab::Api
{
    namespace
    {
        bool IsFloatFormat(DXGI_FORMAT format)
        {
            return format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R32G32B32A32_FLOAT;
        }

        // The formats each encoding is read from.
        bool FormatMatchesEncoding(DXGI_FORMAT format, EFFECTGRAPH_INPUT_ENCODING encoding)
        {
            switch (encoding)
            {
            case EFFECTGRAPH_INPUT_ENCODING_SCRGB_LINEAR: return IsFloatFormat(format);
            case EFFECTGRAPH_INPUT_ENCODING_SRGB:
                return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM;
            case EFFECTGRAPH_INPUT_ENCODING_HDR10: return format == DXGI_FORMAT_R10G10B10A2_UNORM;
            default: return false;
            }
        }

        // The shader cache directory is process-wide; the first context that
        // names one sets it.
        std::once_flag sCacheConfigured;
    }

    HRESULT CopyString(std::wstring_view text, PWSTR buffer, UINT32 bufferCount)
    {
        if (!buffer && bufferCount) return E_INVALIDARG;
        if (text.size() + 1 > bufferCount) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        std::copy(text.begin(), text.end(), buffer);
        buffer[text.size()] = L'\0';
        return S_OK;
    }

    Context::~Context()
    {
        // Node objects hold a reference to the context, so none is left here.
        m_graph.ClearCachedOutputs();
        m_evaluator.ReleaseCache();
        if (!m_extractDir.empty())
        {
            std::error_code ec;
            std::filesystem::remove_all(m_extractDir, ec);   // best effort: a temp folder
        }
    }

    HRESULT Context::Initialize(ID3D11Device* device, const EFFECTGRAPH_CONTEXT_DESC* desc)
    {
        if (!device) return E_INVALIDARG;
        if (!(device->GetCreationFlags() & D3D11_CREATE_DEVICE_BGRA_SUPPORT))
            return EFFECTGRAPH_E_UNSUPPORTED_DEVICE;
        if (HRESULT hr = Rendering::CreateDeviceStackOn(device, m_devices); FAILED(hr)) return hr;
        if (HRESULT hr = m_devices.CreateDeviceContext(m_dc); FAILED(hr)) return hr;

        winrt::com_ptr<ID2D1Factory1> factory;
        m_devices.d2dFactory->QueryInterface(IID_PPV_ARGS(factory.put()));
        if (HRESULT hr = Engine::RegisterEngineD2DEffects(factory.get()); FAILED(hr)) return hr;

        if (desc)
        {
            if (desc->shaderCacheDirectory && *desc->shaderCacheDirectory)
            {
                std::wstring directory(desc->shaderCacheDirectory);
                std::call_once(sCacheConfigured, [&] { Engine::ConfigureBytecodeCache(std::move(directory)); });
            }
            if (desc->flags & EFFECTGRAPH_CONTEXT_FLAGS_ASYNC_COMPILE)
                m_evaluator.SetAsyncCompile(true);
            if (desc->flags & EFFECTGRAPH_CONTEXT_FLAGS_ALLOW_RUNTIME_COMPILE)
            {
                auto options = m_evaluator.Options();
                options.compilePrebuiltEffects = true;
                m_evaluator.SetOptions(options);
            }
        }
        return S_OK;
    }

    // ---- Node bookkeeping

    void Context::ForgetNodeObject(uint32_t nodeId, const NodeBase* object)
    {
        // After LoadGraph the id may belong to another node's object.
        if (auto it = m_nodeObjects.find(nodeId); it != m_nodeObjects.end() && it->second == object)
            m_nodeObjects.erase(it);
    }

    HRESULT Context::IdOf(IEffectGraphNode* node, uint32_t& nodeId) const
    {
        nodeId = 0;
        if (!node) return E_INVALIDARG;
        winrt::com_ptr<INodeIdentity> identity;
        if (FAILED(node->QueryInterface(__uuidof(INodeIdentity), identity.put_void())) || !identity)
            return EFFECTGRAPH_E_WRONG_CONTEXT;
        const NodeBase* object = identity->Object();
        if (object->Owner() != this) return EFFECTGRAPH_E_WRONG_CONTEXT;
        // Gone from the graph, or from a graph LoadGraph replaced.
        const auto it = m_nodeObjects.find(object->Id());
        if (!m_graph.FindNode(object->Id()) || it == m_nodeObjects.end() || it->second != object)
            return EFFECTGRAPH_E_NODE_REMOVED;
        nodeId = object->Id();
        return S_OK;
    }

    STDMETHODIMP_(uint32_t) Context::NodeId(IEffectGraphNode* node)
    {
        uint32_t nodeId = 0;
        return SUCCEEDED(IdOf(node, nodeId)) ? nodeId : 0;
    }

    STDMETHODIMP Context::NodeFor(uint32_t nodeId, IEffectGraphNode** node)
    {
        if (!node) return E_POINTER;
        *node = nullptr;
        if (!m_graph.FindNode(nodeId)) return S_FALSE;
        if (auto it = m_nodeObjects.find(nodeId); it != m_nodeObjects.end())
        {
            it->second->GetInterface(node);
            return S_OK;
        }
        NodeBase* object = nullptr;
        if (IsInput(nodeId))
        {
            auto created = winrt::make_self<InputNode>(this, nodeId);
            object = created.get();
            object->GetInterface(node);
        }
        else
        {
            auto created = winrt::make_self<Node>(this, nodeId);
            object = created.get();
            object->GetInterface(node);
        }
        m_nodeObjects[nodeId] = object;
        return S_OK;
    }

    HRESULT Context::AddNodeAndWrap(Graph::EffectNode node, IEffectGraphNode** out)
    {
        const uint32_t id = m_graph.AddNode(std::move(node));
        return NodeFor(id, out);
    }

    ID2D1Effect* Context::MetadataEffect(const GUID& clsid)
    {
        for (auto& [id, effect] : m_metadataEffects)
            if (id == clsid) return effect.get();
        winrt::com_ptr<ID2D1Effect> effect;
        if (FAILED(m_dc->CreateEffect(clsid, effect.put())) || !effect) return nullptr;
        m_metadataEffects.emplace_back(clsid, effect);
        return effect.get();
    }

    // ---- Creating nodes

    STDMETHODIMP Context::CreateEffect(PCWSTR name, IEffectGraphNode** node)
    {
        if (!name || !node) return E_INVALIDARG;
        *node = nullptr;
        auto created = Effects::CreateNodeByName(name, {}, m_graph);
        if (!created.node) return EFFECTGRAPH_E_UNKNOWN_EFFECT;
        const bool prepare = created.needsPrepare;
        HRESULT hr = AddNodeAndWrap(std::move(*created.node), node);
        if (SUCCEEDED(hr) && prepare)
        {
            if (auto* added = m_graph.FindNode(NodeId(*node)))
                m_sources.PrepareSourceNode(*added, m_dc.get(), m_devices.d3dDevice.get(), m_devices.d3dContext.get());
        }
        return hr;
    }

    STDMETHODIMP Context::CreateD2DEffect(REFCLSID effectId, IEffectGraphNode** node)
    {
        if (!node) return E_INVALIDARG;
        *node = nullptr;
        if (const auto* desc = Effects::EffectRegistry::Instance().FindByClsid(effectId))
            return AddNodeAndWrap(Effects::EffectRegistry::CreateNode(*desc), node);

        // A class ShaderLab does not list: its pins and name come from an instance.
        ID2D1Effect* effect = MetadataEffect(effectId);
        if (!effect) return EFFECTGRAPH_E_UNKNOWN_EFFECT;
        Graph::EffectNode created;
        created.type = Graph::NodeType::BuiltInEffect;
        created.effectClsid = effectId;
        wchar_t displayName[256]{};
        if (FAILED(effect->GetValue(D2D1_PROPERTY_DISPLAYNAME, reinterpret_cast<BYTE*>(displayName), sizeof(displayName))))
            wcscpy_s(displayName, L"D2D Effect");
        Graph::RenameNode(created, displayName);
        for (UINT32 i = 0; i < effect->GetInputCount(); ++i)
            created.inputPins.push_back({ std::format(L"Input {}", i), i });
        created.outputPins.push_back({ L"Output", 0 });
        return AddNodeAndWrap(std::move(created), node);
    }

    HRESULT Context::WrapTexture(ID3D11Texture2D* texture, EFFECTGRAPH_INPUT_ENCODING encoding, InputSource& source)
    {
        if (!texture) return E_INVALIDARG;
        winrt::com_ptr<ID3D11Device> owner;
        texture->GetDevice(owner.put());
        if (owner.get() != static_cast<ID3D11Device*>(m_devices.d3dDevice.get())) return E_INVALIDARG;
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (!(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) || desc.MipLevels != 1 || desc.ArraySize != 1 ||
            desc.SampleDesc.Count != 1 || !FormatMatchesEncoding(desc.Format, encoding))
            return EFFECTGRAPH_E_UNSUPPORTED_FORMAT;

        winrt::com_ptr<IDXGISurface> surface;
        if (HRESULT hr = texture->QueryInterface(IID_PPV_ARGS(surface.put())); FAILED(hr)) return hr;
        D2D1_BITMAP_PROPERTIES1 props{};
        props.pixelFormat = { desc.Format, D2D1_ALPHA_MODE_PREMULTIPLIED };
        props.dpiX = props.dpiY = 96.0f;
        winrt::com_ptr<ID2D1Bitmap1> bitmap;
        if (FAILED(m_dc->CreateBitmapFromDxgiSurface(surface.get(), &props, bitmap.put())))
            return EFFECTGRAPH_E_UNSUPPORTED_FORMAT;

        InputSource wrapped;
        wrapped.texture.copy_from(texture);
        wrapped.bitmap = bitmap;
        if (encoding == EFFECTGRAPH_INPUT_ENCODING_HDR10)
        {
            // PQ / BT.2020 to scRGB at float precision, as ImageLoader decodes HDR10 files.
            winrt::com_ptr<ID2D1ColorContext1> pq;
            winrt::com_ptr<ID2D1ColorContext> scrgb;
            HRESULT hr = m_dc->CreateColorContextFromDxgiColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, pq.put());
            if (SUCCEEDED(hr)) hr = m_dc->CreateColorContext(D2D1_COLOR_SPACE_SCRGB, nullptr, 0, scrgb.put());
            if (SUCCEEDED(hr)) hr = m_dc->CreateEffect(CLSID_D2D1ColorManagement, wrapped.decode.put());
            if (FAILED(hr)) return hr;
            wrapped.decode->SetInput(0, bitmap.get());
            wrapped.decode->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_COLOR_CONTEXT, pq.get());
            wrapped.decode->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_COLOR_CONTEXT, scrgb.get());
            wrapped.decode->SetValue(D2D1_COLORMANAGEMENT_PROP_QUALITY, D2D1_COLORMANAGEMENT_QUALITY_BEST);
            wrapped.decode->GetOutput(wrapped.output.put());
        }
        else
        {
            std::wstring error;
            wrapped.output = Effects::AsLinearScRgb(m_dc.get(), bitmap.get(), desc.Format, wrapped.decode, error);
            if (!wrapped.output) return E_FAIL;
        }
        source = std::move(wrapped);
        return S_OK;
    }

    STDMETHODIMP Context::CreateTextureInput(ID3D11Texture2D* texture, EFFECTGRAPH_INPUT_ENCODING encoding,
                                             IEffectGraphNode** node)
    {
        if (!node) return E_INVALIDARG;
        *node = nullptr;
        InputSource source;
        if (HRESULT hr = WrapTexture(texture, encoding, source); FAILED(hr)) return hr;
        const uint32_t id = m_graph.AddNode(Effects::SourceNodeFactory::CreateImageSourceNode(L"", L"Texture Input"));
        m_inputs[id] = std::move(source);
        return NodeFor(id, node);
    }

    STDMETHODIMP Context::CreateImageInput(ID2D1Image* image, IEffectGraphNode** node)
    {
        if (!image || !node) return E_INVALIDARG;
        *node = nullptr;
        const uint32_t id = m_graph.AddNode(Effects::SourceNodeFactory::CreateImageSourceNode(L"", L"Image Input"));
        m_inputs[id].output.copy_from(image);
        return NodeFor(id, node);
    }

    HRESULT Context::SetInputTexture(uint32_t nodeId, ID3D11Texture2D* texture, EFFECTGRAPH_INPUT_ENCODING encoding)
    {
        auto it = m_inputs.find(nodeId);
        if (it == m_inputs.end() || !it->second.texture) return E_NOT_VALID_STATE;
        InputSource source;
        if (HRESULT hr = WrapTexture(texture, encoding, source); FAILED(hr)) return hr;
        it->second = std::move(source);
        Graph::MarkNodeChanged(m_graph, nodeId);
        return S_OK;
    }

    HRESULT Context::SetInputImage(uint32_t nodeId, ID2D1Image* image)
    {
        auto it = m_inputs.find(nodeId);
        if (it == m_inputs.end() || it->second.texture) return E_NOT_VALID_STATE;
        if (!image) return E_INVALIDARG;
        it->second.output.copy_from(image);
        Graph::MarkNodeChanged(m_graph, nodeId);
        return S_OK;
    }

    STDMETHODIMP Context::RemoveNode(IEffectGraphNode* node)
    {
        uint32_t id = 0;
        if (HRESULT hr = IdOf(node, id); FAILED(hr)) return hr;
        m_graph.RemoveNode(id);
        m_inputs.erase(id);
        m_evaluator.InvalidateNode(m_graph, id);
        m_sources.PruneOrphans(m_graph.Nodes());
        return S_OK;
    }

    STDMETHODIMP Context::LoadGraph(PCWSTR path)
    {
        if (!path) return E_INVALIDARG;
        std::wstring error;
        auto loaded = Rendering::EffectGraphFile::LoadAny(path, Rendering::TempDirectory(), error);
        if (!loaded) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        Graph::GraphModel graph;
        try
        {
            graph = Graph::GraphModel::FromJson(winrt::hstring(loaded->graphJson));
        }
        catch (...)
        {
            std::error_code ec;
            std::filesystem::remove_all(loaded->extractDir, ec);   // best effort: a temp folder
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }
        Rendering::EffectGraphFile::ResolveMediaTokens(graph, loaded->mediaMap);

        // Everything from before goes: nodes, inputs, cached effects, media.
        m_graph.ClearCachedOutputs();
        m_evaluator.ReleaseCache();
        m_inputs.clear();
        m_nodeObjects.clear();
        ++m_epoch;
        m_graph = std::move(graph);
        m_sources.PruneOrphans(m_graph.Nodes());
        if (!m_extractDir.empty())
        {
            std::error_code ec;
            std::filesystem::remove_all(m_extractDir, ec);   // best effort: a temp folder
        }
        m_extractDir = loaded->extractDir;
        Rendering::PrepareSources(m_graph, m_sources,
            { m_dc.get(), m_devices.d3dDevice.get(), m_devices.d3dContext.get() }, Rendering::SourceScope::All);
        return S_OK;
    }

    STDMETHODIMP Context::FindNode(PCWSTR name, IEffectGraphNode** node)
    {
        if (!name || !node) return E_INVALIDARG;
        *node = nullptr;
        for (const auto& candidate : m_graph.Nodes())
            if (candidate.name == name) return NodeFor(candidate.id, node);
        return S_FALSE;
    }

    // ---- Evaluating

    void Context::SyncFrameInputs()
    {
        for (auto& [id, source] : m_inputs)
            if (auto* node = m_graph.FindNode(id)) node->cachedOutput = source.output.get();

        // Sources from a loaded graph: the dirty ones and any video, as
        // PrepareSources(Changed) would, but never an input node.
        for (auto& node : m_graph.MutableNodes())
        {
            if (node.type != Graph::NodeType::Source || m_inputs.contains(node.id)) continue;
            if (!node.dirty && !m_sources.HasVideoProvider(node.id)) continue;
            // Preparing an unchanged file marks the source clean, which the
            // GUI wants on a redraw; here a source marked changed must still
            // re-render what reads it. Evaluating it clears the flag.
            const bool changed = node.dirty;
            m_sources.PrepareSourceNode(node, m_dc.get(), m_devices.d3dDevice.get(), m_devices.d3dContext.get());
            if (changed) Graph::MarkNodeChanged(node);
        }
        Rendering::UpdateWorkingSpaceNodes(m_graph, m_displayProfile);
        if (std::ranges::any_of(m_graph.Nodes(), [&](const Graph::EffectNode& n) { return m_sources.HasVideoProvider(n.id); }))
            Rendering::SettleVideoSources(m_graph, m_sources, m_evaluator, m_dc.get());
    }

    HRESULT Context::EvaluateFor(uint32_t nodeId, ID2D1Image* target, std::function<void(ID2D1DeviceContext5*)> draw)
    {
        if (m_inDraw) return D2DERR_WRONG_STATE;
        SyncFrameInputs();
        Rendering::FrameRequest request;
        request.demand = Rendering::FrameDemand::Roots;
        request.roots = { nodeId };
        request.target = target;
        request.draw = std::move(draw);
        const auto result = Rendering::FrameExecutor::Run(m_graph, m_evaluator, m_dc.get(), request);
        if (FAILED(result.endDraw)) return result.endDraw;

        const auto* node = m_graph.FindNode(nodeId);
        if (!node) return EFFECTGRAPH_E_NODE_REMOVED;
        if (node->compilePending) return EFFECTGRAPH_E_COMPILE_PENDING;
        if (!node->runtimeError.empty()) return EFFECTGRAPH_E_NODE_FAILED;
        if (!node->outputPins.empty() && !node->cachedOutput) return EFFECTGRAPH_E_NODE_FAILED;
        return S_OK;
    }

    STDMETHODIMP Context::Realize(IEffectGraphNode* node, ID2D1Image** image)
    {
        if (image) *image = nullptr;
        uint32_t id = 0;
        if (HRESULT hr = IdOf(node, id); FAILED(hr)) return hr;
        if (HRESULT hr = EvaluateFor(id, nullptr, {}); FAILED(hr)) return hr;
        if (!image) return S_OK;
        auto* output = m_graph.FindNode(id)->cachedOutput;
        if (!output) return S_FALSE;
        output->AddRef();
        *image = output;
        return S_OK;
    }

    STDMETHODIMP Context::RealizeTo(IEffectGraphNode* node, ID3D11Texture2D* target)
    {
        uint32_t id = 0;
        if (HRESULT hr = IdOf(node, id); FAILED(hr)) return hr;
        if (!target) return E_INVALIDARG;
        if (m_graph.FindNode(id)->outputPins.empty()) return E_INVALIDARG;

        winrt::com_ptr<ID3D11Device> owner;
        target->GetDevice(owner.put());
        if (owner.get() != static_cast<ID3D11Device*>(m_devices.d3dDevice.get())) return E_INVALIDARG;
        D3D11_TEXTURE2D_DESC desc{};
        target->GetDesc(&desc);
        if (!(desc.BindFlags & D3D11_BIND_RENDER_TARGET) || desc.ArraySize != 1 || desc.SampleDesc.Count != 1)
            return EFFECTGRAPH_E_UNSUPPORTED_FORMAT;
        winrt::com_ptr<IDXGISurface> surface;
        if (HRESULT hr = target->QueryInterface(IID_PPV_ARGS(surface.put())); FAILED(hr)) return hr;
        D2D1_BITMAP_PROPERTIES1 props{};
        props.pixelFormat = { desc.Format, D2D1_ALPHA_MODE_PREMULTIPLIED };
        props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        props.dpiX = props.dpiY = 96.0f;
        winrt::com_ptr<ID2D1Bitmap1> bitmap;
        if (FAILED(m_dc->CreateBitmapFromDxgiSurface(surface.get(), &props, bitmap.put())))
            return EFFECTGRAPH_E_UNSUPPORTED_FORMAT;

        // Drawn inside the frame's own session, after its compute work.
        auto& graph = m_graph;
        HRESULT hr = EvaluateFor(id, bitmap.get(), [&graph, id](ID2D1DeviceContext5* dc)
        {
            dc->Clear(D2D1::ColorF(0, 0, 0, 0));
            if (const auto* rendered = graph.FindNode(id); rendered && rendered->cachedOutput)
                dc->DrawImage(rendered->cachedOutput, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                              D2D1_COMPOSITE_MODE_SOURCE_COPY);
        });
        // EndDraw submitted the draw to the device's immediate context, so
        // D3D11 work the caller issues next runs after it.
        return hr;
    }

    STDMETHODIMP Context::Draw(ID2D1Image* target, EFFECTGRAPH_DRAW_CALLBACK callback, void* callbackContext)
    {
        if (!callback) return E_INVALIDARG;
        if (m_inDraw) return D2DERR_WRONG_STATE;
        winrt::com_ptr<ID2D1Image> previousTarget;
        m_dc->GetTarget(previousTarget.put());
        D2D1_MATRIX_3X2_F previousTransform{};
        m_dc->GetTransform(&previousTransform);
        float dpiX = 96.0f, dpiY = 96.0f;
        m_dc->GetDpi(&dpiX, &dpiY);

        if (target) m_dc->SetTarget(target);
        m_inDraw = true;
        m_dc->BeginDraw();
        callback(m_dc.get(), callbackContext);
        const HRESULT hr = m_dc->EndDraw();
        m_inDraw = false;

        m_dc->SetTarget(previousTarget.get());
        m_dc->SetTransform(previousTransform);
        m_dc->SetDpi(dpiX, dpiY);
        return hr;
    }

    STDMETHODIMP_(void) Context::GetD3DDevice(ID3D11Device** device)
    {
        if (!device) return;
        *device = m_devices.d3dDevice.get();
        (*device)->AddRef();
    }

    STDMETHODIMP_(void) Context::GetD2DDeviceContext(ID2D1DeviceContext5** deviceContext)
    {
        if (!deviceContext) return;
        m_dc.copy_to(deviceContext);
    }

    // ---- IEffectGraphContextInternal

    STDMETHODIMP_(Rendering::FrameResult) Context::RunFrame(const Rendering::FrameRequest& request)
    {
        SyncFrameInputs();
        return Rendering::FrameExecutor::Run(m_graph, m_evaluator, m_dc.get(), request);
    }

    STDMETHODIMP Context::MoveToDevice(ID3D11Device*)
    {
        return E_NOTIMPL;   // Phase 6: the GUI's adapter switch moves here
    }
}

// ---- Exports

EFFECTGRAPH_API HRESULT WINAPI EffectGraphCreateContext(ID3D11Device* device, const EFFECTGRAPH_CONTEXT_DESC* desc,
                                                        REFIID riid, void** context)
{
    if (!context) return E_POINTER;
    *context = nullptr;
    try
    {
        auto created = winrt::make_self<ShaderLab::Api::Context>();
        if (HRESULT hr = created->Initialize(device, desc); FAILED(hr)) return hr;
        return created->QueryInterface(riid, context);
    }
    catch (...)
    {
        return winrt::to_hresult();
    }
}

namespace
{
    HRESULT ResultOf(const ShaderLab::Effects::EffectBlobRegistration& registration)
    {
        using ShaderLab::Effects::EffectBlobStatus;
        switch (registration.status)
        {
        case EffectBlobStatus::Registered:    return registration.error.empty() ? S_OK : S_FALSE;
        case EffectBlobStatus::NoneIncluded:  return S_FALSE;
        case EffectBlobStatus::Incompatible:  return EFFECTGRAPH_E_INCOMPATIBLE_EFFECT;
        case EffectBlobStatus::NotRegistered: return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        default:                              return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }
    }
}

EFFECTGRAPH_API HRESULT WINAPI EffectGraphRegisterEffects(const EFFECTGRAPH_EFFECT_SOURCE* source)
{
    if (!source || !source->description || (source->buildCount && !source->builds)) return E_INVALIDARG;
    ShaderLab::Effects::EffectBlobParts parts;
    parts.format = source->format;
    parts.contractHash = source->contractHash;
    parts.description = { static_cast<const uint8_t*>(source->description), source->descriptionSize };
    for (UINT32 i = 0; i < source->buildCount; ++i)
        if (source->builds[i].data)
            parts.records.emplace_back(static_cast<const uint8_t*>(source->builds[i].data), source->builds[i].size);
    return ResultOf(ShaderLab::Effects::RegisterEffectBlobParts(parts));
}

EFFECTGRAPH_API HRESULT WINAPI EffectGraphRegisterEffect(const void* data, UINT32 size)
{
    if (!data || size == 0) return E_INVALIDARG;
    return ResultOf(ShaderLab::Effects::RegisterEffectBlob(
        std::span<const uint8_t>(static_cast<const uint8_t*>(data), size)));
}

EFFECTGRAPH_API HRESULT WINAPI EffectGraphLoadEffects(PCWSTR directory)
{
    if (!directory) return E_INVALIDARG;
    const auto& report = ShaderLab::Effects::ShaderLabEffects::Instance().LoadUserEffects(directory);
    return report.errors.empty() ? S_OK : S_FALSE;
}

EFFECTGRAPH_API void WINAPI EffectGraphShutdown()
{
    ShaderLab::Engine::ShutdownEngineServices();
}
