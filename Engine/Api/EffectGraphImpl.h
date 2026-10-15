#pragma once

#include "pch_engine.h"
#include "EffectGraphInternal.h"
#include "../../Graph/GraphModel.h"
#include "../../Rendering/DeviceStack.h"
#include "../../Rendering/DisplayProfile.h"
#include "../../Rendering/FrameExecutor.h"
#include "../../Rendering/GraphEvaluator.h"
#include "../../Effects/SourceNodeFactory.h"

// The classes behind the EffectGraph interfaces. Not exported: callers only
// ever hold the interfaces. C++/WinRT's implements answers QueryInterface only
// for the interfaces it lists, so each class lists every level of its chain.
namespace ShaderLab::Api
{
    class Context;
    class NodeBase;

    // Lets the context find the object behind a node interface it is handed,
    // and so reject a node from another context.
    MIDL_INTERFACE("d0020199-8f9d-4d16-a15d-499bdc6bb782")
    INodeIdentity : public IUnknown
    {
        virtual NodeBase* STDMETHODCALLTYPE Object() = 0;
    };

    // What an input node reads, and the image the graph sees for it.
    struct InputSource
    {
        winrt::com_ptr<ID3D11Texture2D> texture;   // null for an image input
        winrt::com_ptr<ID2D1Bitmap1> bitmap;       // the texture, wrapped
        winrt::com_ptr<ID2D1Effect> decode;        // to linear scRGB, when the encoding needs it
        winrt::com_ptr<ID2D1Image> output;         // the node's output
    };

    class Context : public winrt::implements<Context, IEffectGraphContextInternal, IEffectGraphContext>
    {
    public:
        ~Context();

        HRESULT Initialize(ID3D11Device* device, const EFFECTGRAPH_CONTEXT_DESC* desc);

        // IEffectGraphContext
        STDMETHOD(CreateEffect)(PCWSTR name, IEffectGraphNode** node) override;
        STDMETHOD(CreateD2DEffect)(REFCLSID effectId, IEffectGraphNode** node) override;
        STDMETHOD(CreateTextureInput)(ID3D11Texture2D* texture, EFFECTGRAPH_INPUT_ENCODING encoding,
                                      IEffectGraphNode** node) override;
        STDMETHOD(CreateImageInput)(ID2D1Image* image, IEffectGraphNode** node) override;
        STDMETHOD(RemoveNode)(IEffectGraphNode* node) override;
        STDMETHOD(LoadGraph)(PCWSTR path) override;
        STDMETHOD(FindNode)(PCWSTR name, IEffectGraphNode** node) override;
        STDMETHOD(Realize)(IEffectGraphNode* node, ID2D1Image** image) override;
        STDMETHOD(RealizeTo)(IEffectGraphNode* node, ID3D11Texture2D* target) override;
        STDMETHOD(Draw)(ID2D1Image* target, EFFECTGRAPH_DRAW_CALLBACK callback, void* callbackContext) override;
        STDMETHOD_(void, GetD3DDevice)(ID3D11Device** device) override;
        STDMETHOD_(void, GetD2DDeviceContext)(ID2D1DeviceContext5** deviceContext) override;

        // IEffectGraphContextInternal
        STDMETHOD_(Graph::GraphModel&, Model)() override { return m_graph; }
        STDMETHOD_(Rendering::GraphEvaluator&, Evaluator)() override { return m_evaluator; }
        STDMETHOD_(Effects::SourceNodeFactory&, Sources)() override { return m_sources; }
        STDMETHOD_(const Rendering::DeviceStack&, Devices)() override { return m_devices; }
        STDMETHOD_(uint32_t, NodeId)(IEffectGraphNode* node) override;
        STDMETHOD(NodeFor)(uint32_t nodeId, IEffectGraphNode** node) override;
        STDMETHOD_(Rendering::FrameResult, RunFrame)(const Rendering::FrameRequest& request) override;
        STDMETHOD_(void, SetDisplayProfile)(const Rendering::DisplayProfile& profile) override { m_displayProfile = profile; }
        STDMETHOD(MoveToDevice)(ID3D11Device* device) override;

        // For the node objects.
        uint64_t Epoch() const { return m_epoch; }
        bool IsInput(uint32_t nodeId) const { return m_inputs.contains(nodeId); }
        void ForgetNodeObject(uint32_t nodeId, const NodeBase* object);
        // The id of a live node of this context, or an EFFECTGRAPH_E_* error.
        HRESULT IdOf(IEffectGraphNode* node, uint32_t& nodeId) const;
        // A D2D effect of the class, created once, that answers property
        // metadata (names, types, defaults) for built-in nodes. Null when the
        // class cannot be created.
        ID2D1Effect* MetadataEffect(const GUID& clsid);
        HRESULT SetInputTexture(uint32_t nodeId, ID3D11Texture2D* texture, EFFECTGRAPH_INPUT_ENCODING encoding);
        HRESULT SetInputImage(uint32_t nodeId, ID2D1Image* image);

    private:
        HRESULT AddNodeAndWrap(Graph::EffectNode node, IEffectGraphNode** out);
        HRESULT WrapTexture(ID3D11Texture2D* texture, EFFECTGRAPH_INPUT_ENCODING encoding, InputSource& source);
        // Points every input node at its image and prepares changed sources:
        // what a frame needs before FrameExecutor::Run.
        void SyncFrameInputs();
        // One frame for `nodeId`, then the node's state as an HRESULT.
        HRESULT EvaluateFor(uint32_t nodeId, ID2D1Image* target,
                            std::function<void(ID2D1DeviceContext5*)> draw);

        Rendering::DeviceStack m_devices;
        winrt::com_ptr<ID2D1DeviceContext5> m_dc;
        Graph::GraphModel m_graph;
        Rendering::GraphEvaluator m_evaluator;
        Effects::SourceNodeFactory m_sources;
        std::unordered_map<uint32_t, InputSource> m_inputs;
        Rendering::DisplayProfile m_displayProfile;
        // Weak: a node object removes itself when it is released.
        std::unordered_map<uint32_t, NodeBase*> m_nodeObjects;
        std::vector<std::pair<GUID, winrt::com_ptr<ID2D1Effect>>> m_metadataEffects;
        // Bumped when the graph is replaced, so node objects from before see
        // their node as removed even though a new node may reuse the id.
        uint64_t m_epoch{ 1 };
        bool m_inDraw{ false };
        std::wstring m_extractDir;
    };

    // What every node object shares: which node it stands for, and the
    // behaviour behind IEffectGraphNode and ID2D1Properties.
    class NodeBase
    {
    public:
        NodeBase(Context* context, uint32_t nodeId);
        virtual ~NodeBase();
        NodeBase(const NodeBase&) = delete;
        NodeBase& operator=(const NodeBase&) = delete;

        uint32_t Id() const { return m_nodeId; }
        const Context* Owner() const { return m_context.get(); }
        // The object's IEffectGraphNode, with a reference added.
        virtual void GetInterface(IEffectGraphNode** node) = 0;

        // IEffectGraphNode
        void GetContextImpl(IEffectGraphContext** context);
        HRESULT SetInputImpl(UINT32 index, IEffectGraphNode* input);
        void GetInputImpl(UINT32 index, IEffectGraphNode** input);
        UINT32 GetInputCountImpl();
        HRESULT BindImpl(PCWSTR property, IEffectGraphNode* source, PCWSTR field);
        HRESULT UnbindImpl(PCWSTR property);
        HRESULT GetValuesImpl(IEffectGraphValues** values);
        UINT32 GetErrorLengthImpl();
        HRESULT GetErrorImpl(PWSTR buffer, UINT32 bufferCount);
        UINT32 GetNameLengthImpl();
        HRESULT GetNameImpl(PWSTR buffer, UINT32 bufferCount);
        HRESULT SetNameImpl(PCWSTR name);

        // ID2D1Properties
        UINT32 GetPropertyCountImpl() const;
        HRESULT GetPropertyNameImpl(UINT32 index, PWSTR name, UINT32 nameCount) const;
        UINT32 GetPropertyNameLengthImpl(UINT32 index) const;
        D2D1_PROPERTY_TYPE GetTypeImpl(UINT32 index) const;
        UINT32 GetPropertyIndexImpl(PCWSTR name) const;
        HRESULT SetValueByNameImpl(PCWSTR name, D2D1_PROPERTY_TYPE type, const BYTE* data, UINT32 dataSize);
        HRESULT SetValueImpl(UINT32 index, D2D1_PROPERTY_TYPE type, const BYTE* data, UINT32 dataSize);
        HRESULT GetValueByNameImpl(PCWSTR name, D2D1_PROPERTY_TYPE type, BYTE* data, UINT32 dataSize) const;
        HRESULT GetValueImpl(UINT32 index, D2D1_PROPERTY_TYPE type, BYTE* data, UINT32 dataSize) const;
        UINT32 GetValueSizeImpl(UINT32 index) const;
        HRESULT GetSubPropertiesImpl(UINT32 index, ID2D1Properties** subProperties) const;

        // IEffectGraphInput, for InputNode
        void InvalidateImpl();
        HRESULT SetTextureImpl(ID3D11Texture2D* texture, EFFECTGRAPH_INPUT_ENCODING encoding);
        HRESULT SetImageImpl(ID2D1Image* image);

    protected:
        struct Property
        {
            std::wstring name;
            D2D1_PROPERTY_TYPE type{ D2D1_PROPERTY_TYPE_UNKNOWN };
        };
        // The node while it is still in the graph, else null.
        Graph::EffectNode* Live() const;
        // The node's properties in index order: a D2D effect's own order for
        // a built-in, else the declared parameters, then any other property.
        std::vector<Property> Properties(const Graph::EffectNode& node) const;
        std::optional<Property> PropertyAt(UINT32 index) const;
        std::optional<Property> PropertyNamed(PCWSTR name) const;
        // The value a property has now: the node's, or the D2D default.
        std::optional<Graph::PropertyValue> CurrentValue(const Graph::EffectNode& node, const Property& property) const;
        HRESULT Write(const Property& property, D2D1_PROPERTY_TYPE type, const BYTE* data, UINT32 dataSize);
        HRESULT Read(const Property& property, D2D1_PROPERTY_TYPE type, BYTE* data, UINT32 dataSize) const;

        winrt::com_ptr<Context> m_context;
        uint32_t m_nodeId;
        uint64_t m_epoch;
    };

    // The COM object for a node: the shared behaviour plus the interfaces in
    // `Extra` (IEffectGraphInput for an input node).
    template <typename D, typename... Extra>
    class NodeT : public winrt::implements<D, IEffectGraphNode, ID2D1Properties, INodeIdentity, Extra...>,
                  public NodeBase
    {
    public:
        NodeT(Context* context, uint32_t nodeId) : NodeBase(context, nodeId) {}

        void GetInterface(IEffectGraphNode** node) override
        {
            this->QueryInterface(__uuidof(IEffectGraphNode), reinterpret_cast<void**>(node));
        }

        // INodeIdentity
        NodeBase* STDMETHODCALLTYPE Object() override { return this; }

        // ID2D1Properties
        STDMETHOD_(UINT32, GetPropertyCount)() const override { return GetPropertyCountImpl(); }
        STDMETHOD(GetPropertyName)(UINT32 index, PWSTR name, UINT32 nameCount) const override { return GetPropertyNameImpl(index, name, nameCount); }
        STDMETHOD_(UINT32, GetPropertyNameLength)(UINT32 index) const override { return GetPropertyNameLengthImpl(index); }
        STDMETHOD_(D2D1_PROPERTY_TYPE, GetType)(UINT32 index) const override { return GetTypeImpl(index); }
        STDMETHOD_(UINT32, GetPropertyIndex)(PCWSTR name) const override { return GetPropertyIndexImpl(name); }
        STDMETHOD(SetValueByName)(PCWSTR name, D2D1_PROPERTY_TYPE type, const BYTE* data, UINT32 dataSize) override { return SetValueByNameImpl(name, type, data, dataSize); }
        STDMETHOD(SetValue)(UINT32 index, D2D1_PROPERTY_TYPE type, const BYTE* data, UINT32 dataSize) override { return SetValueImpl(index, type, data, dataSize); }
        STDMETHOD(GetValueByName)(PCWSTR name, D2D1_PROPERTY_TYPE type, BYTE* data, UINT32 dataSize) const override { return GetValueByNameImpl(name, type, data, dataSize); }
        STDMETHOD(GetValue)(UINT32 index, D2D1_PROPERTY_TYPE type, BYTE* data, UINT32 dataSize) const override { return GetValueImpl(index, type, data, dataSize); }
        STDMETHOD_(UINT32, GetValueSize)(UINT32 index) const override { return GetValueSizeImpl(index); }
        STDMETHOD(GetSubProperties)(UINT32 index, ID2D1Properties** subProperties) const override { return GetSubPropertiesImpl(index, subProperties); }

        // IEffectGraphNode
        STDMETHOD_(void, GetContext)(IEffectGraphContext** context) override { GetContextImpl(context); }
        STDMETHOD(SetInput)(UINT32 index, IEffectGraphNode* input) override { return SetInputImpl(index, input); }
        STDMETHOD_(void, GetInput)(UINT32 index, IEffectGraphNode** input) override { GetInputImpl(index, input); }
        STDMETHOD_(UINT32, GetInputCount)() override { return GetInputCountImpl(); }
        STDMETHOD(Bind)(PCWSTR property, IEffectGraphNode* source, PCWSTR field) override { return BindImpl(property, source, field); }
        STDMETHOD(Unbind)(PCWSTR property) override { return UnbindImpl(property); }
        STDMETHOD(GetValues)(IEffectGraphValues** values) override { return GetValuesImpl(values); }
        STDMETHOD_(UINT32, GetErrorLength)() override { return GetErrorLengthImpl(); }
        STDMETHOD(GetError)(PWSTR buffer, UINT32 bufferCount) override { return GetErrorImpl(buffer, bufferCount); }
        STDMETHOD_(UINT32, GetNameLength)() override { return GetNameLengthImpl(); }
        STDMETHOD(GetName)(PWSTR buffer, UINT32 bufferCount) override { return GetNameImpl(buffer, bufferCount); }
        STDMETHOD(SetName)(PCWSTR name) override { return SetNameImpl(name); }
    };

    class Node : public NodeT<Node>
    {
    public:
        Node(Context* context, uint32_t nodeId) : NodeT(context, nodeId) {}
    };

    class InputNode : public NodeT<InputNode, IEffectGraphInput>
    {
    public:
        InputNode(Context* context, uint32_t nodeId) : NodeT(context, nodeId) {}

        // IEffectGraphInput
        STDMETHOD_(void, Invalidate)() override { InvalidateImpl(); }
        STDMETHOD(SetTexture)(ID3D11Texture2D* texture, EFFECTGRAPH_INPUT_ENCODING encoding) override { return SetTextureImpl(texture, encoding); }
        STDMETHOD(SetImage)(ID2D1Image* image) override { return SetImageImpl(image); }
    };

    // A snapshot of a node's analysis fields.
    class Values : public winrt::implements<Values, IEffectGraphValues>
    {
    public:
        explicit Values(std::vector<Graph::AnalysisFieldValue> fields) : m_fields(std::move(fields)) {}

        STDMETHOD_(UINT32, GetFieldCount)() override { return static_cast<UINT32>(m_fields.size()); }
        STDMETHOD_(UINT32, GetFieldIndex)(PCWSTR name) override;
        STDMETHOD(GetFieldName)(UINT32 index, PWSTR buffer, UINT32 bufferCount) override;
        STDMETHOD(GetFieldInfo)(UINT32 index, EFFECTGRAPH_FIELD_INFO* info) override;
        STDMETHOD(GetFloats)(UINT32 index, float* values, UINT32 valueCount) override;
        STDMETHOD(GetFloat)(PCWSTR name, float* value) override;

    private:
        std::vector<Graph::AnalysisFieldValue> m_fields;
    };

    // Copies `text` with its terminator into a caller's buffer, or fails with
    // HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER).
    HRESULT CopyString(std::wstring_view text, PWSTR buffer, UINT32 bufferCount);
}
