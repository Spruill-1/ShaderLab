#pragma once

#include "pch.h"
#include "../Graph/GraphUiSnapshot.h"
#include "DialogCoordinator.h"

namespace ShaderLab::Controls
{
    // A modal "Recompiling shaders" dialog, shown while any node's shader is
    // compiling in the background (EffectNode::compilePending).
    //
    // Driven from the UI tick with the per-frame snapshot, never the live graph.
    // Compiles that finish within ShowDelay never raise it, so a cache hit does
    // not flash a dialog. It is the DialogCoordinator's background modal, so it
    // steps aside while any other dialog is open.
    class CompileProgressController
    {
    public:
        explicit CompileProgressController(DialogCoordinator& dialogs) : m_dialogs(dialogs) {}

        void Update(const std::shared_ptr<const Graph::GraphUiSnapshot>& snapshot,
                    winrt::Microsoft::UI::Xaml::XamlRoot const& root);

        // Where to report option variants compiling in the background
        // (EffectNode::variantsCompiling). Those nodes keep rendering with their
        // generic shader, so this is a status line rather than the modal.
        void SetVariantStatusText(winrt::Microsoft::UI::Xaml::Controls::TextBlock const& text) { m_variantText = text; }

    private:
        static constexpr auto ShowDelay = std::chrono::milliseconds(400);

        void EnsureDialog();

        DialogCoordinator& m_dialogs;

        winrt::Microsoft::UI::Xaml::Controls::ContentDialog m_dialog{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock    m_text{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::ProgressBar  m_bar{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock    m_variantText{ nullptr };
        uint32_t m_variantsShown{ 0 };   // last count written, to skip no-op updates

        bool     m_busy{ false };      // compiles pending in the current burst
        bool     m_dismissed{ false }; // closed by the user; stays down for this burst
        uint32_t m_peak{ 0 };          // most compiles pending at once this burst
        std::chrono::steady_clock::time_point m_burstStart{};
    };
}
