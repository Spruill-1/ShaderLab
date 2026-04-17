#pragma once

#include "EffectDesignerWindow.g.h"
#include "Graph/EffectNode.h"
#include "Effects/ShaderCompiler.h"
#include "Effects/CustomPixelShaderEffect.h"
#include "Effects/CustomComputeShaderEffect.h"

namespace winrt::ShaderLab::implementation
{
    struct EffectDesignerWindow : EffectDesignerWindowT<EffectDesignerWindow>
    {
        EffectDesignerWindow();

        // Callback to add/update a custom effect node in the main window's graph.
        using AddToGraphCallback = std::function<uint32_t(::ShaderLab::Graph::EffectNode)>;
        using UpdateInGraphCallback = std::function<void(uint32_t, ::ShaderLab::Graph::CustomEffectDefinition)>;

        void SetAddToGraphCallback(AddToGraphCallback cb) { m_addToGraph = std::move(cb); }
        void SetUpdateInGraphCallback(UpdateInGraphCallback cb) { m_updateInGraph = std::move(cb); }

        // Load an existing custom effect definition for editing.
        void LoadDefinition(uint32_t nodeId, const ::ShaderLab::Graph::CustomEffectDefinition& def);

    private:
        void OnGenerateScaffold(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnCompile(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnAddToGraph(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnUpdateInGraph(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnShaderTypeChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
        void OnAddInput(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnAddParam(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);

        // Build a CustomEffectDefinition from the current UI state.
        ::ShaderLab::Graph::CustomEffectDefinition BuildDefinition();

        // Generate HLSL scaffold from the current definition.
        std::wstring GenerateHlsl(const ::ShaderLab::Graph::CustomEffectDefinition& def);

        // Compile the current HLSL and update status.
        bool CompileHlsl(::ShaderLab::Graph::CustomEffectDefinition& def);

        ::ShaderLab::Effects::ShaderCompiler m_compiler;
        AddToGraphCallback m_addToGraph;
        UpdateInGraphCallback m_updateInGraph;
        uint32_t m_editingNodeId{ 0 };  // Non-zero when editing an existing node.
        std::vector<uint8_t> m_lastBytecode;
    };
}

namespace winrt::ShaderLab::factory_implementation
{
    struct EffectDesignerWindow : EffectDesignerWindowT<EffectDesignerWindow, implementation::EffectDesignerWindow>
    {
    };
}
