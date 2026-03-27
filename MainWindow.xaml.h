#pragma once

#include "MainWindow.g.h"
#include "Rendering/RenderEngine.h"
#include "Rendering/DisplayMonitor.h"
#include "Rendering/GraphEvaluator.h"
#include "Rendering/ToneMapper.h"
#include "Graph/EffectGraph.h"
#include "Effects/EffectRegistry.h"
#include "Effects/SourceNodeFactory.h"
#include "Effects/CustomPixelShaderEffect.h"
#include "Effects/CustomComputeShaderEffect.h"
#include "Controls/ShaderEditorController.h"
#include "Controls/NodeGraphController.h"
#include "Controls/PixelInspectorController.h"

namespace winrt::ShaderLab::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

    private:
        HWND GetWindowHandle();
        void InitializeRendering();
        void RegisterCustomEffects();
        void UpdateStatusBar();

        // Event handlers.
        void OnPreviewSizeChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const& args);
        void OnShaderEditorKeyDown(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args);

        // Render loop.
        void OnRenderTick(
            winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer const& sender,
            winrt::Windows::Foundation::IInspectable const& args);
        void RenderFrame();

        // Device stack.
        ::ShaderLab::Rendering::RenderEngine    m_renderEngine;
        ::ShaderLab::Rendering::DisplayMonitor  m_displayMonitor;
        ::ShaderLab::Rendering::GraphEvaluator  m_graphEvaluator;
        ::ShaderLab::Rendering::ToneMapper      m_toneMapper;

        // Effect graph.
        ::ShaderLab::Graph::EffectGraph         m_graph;
        ::ShaderLab::Effects::SourceNodeFactory m_sourceFactory;

        // Controllers.
        ::ShaderLab::Controls::ShaderEditorController    m_shaderEditor;
        ::ShaderLab::Controls::NodeGraphController       m_nodeGraphController;
        ::ShaderLab::Controls::PixelInspectorController  m_pixelInspector;

        // Render loop timer.
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_renderTimer{ nullptr };
        uint32_t m_frameCount{ 0 };
        std::chrono::steady_clock::time_point m_fpsTimePoint;

        HWND m_hwnd{ nullptr };
        bool m_customEffectsRegistered{ false };
    };
}

namespace winrt::ShaderLab::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
