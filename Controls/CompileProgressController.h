#pragma once

#include "pch.h"
#include "../Graph/GraphUiSnapshot.h"

namespace ShaderLab::Controls
{
    // A modal "Recompiling shaders" dialog, shown while any node's shader is
    // compiling in the background (EffectNode::compilePending).
    //
    // Driven from the UI tick with the per-frame snapshot, never the live graph.
    // Compiles that finish within ShowDelay never raise it, so a cache hit does
    // not flash a dialog.
    //
    // WinUI allows one ContentDialog at a time per window, so every other dialog
    // in the main window is shown through ShowDialogAsync.
    class CompileProgressController
    {
    public:
        void Update(const std::shared_ptr<const Graph::GraphUiSnapshot>& snapshot,
                    winrt::Microsoft::UI::Xaml::XamlRoot const& root);

        // Where to report option variants compiling in the background
        // (EffectNode::variantsCompiling). Those nodes keep rendering with their
        // generic shader, so this is a status line rather than the modal.
        void SetVariantStatusText(winrt::Microsoft::UI::Xaml::Controls::TextBlock const& text) { m_variantText = text; }

        // Show another dialog. The compile dialog is hidden first and is not
        // shown again until this one closes. Returns None if the dialog cannot
        // be opened or HideDialog was called before it opened.
        winrt::Windows::Foundation::IAsyncOperation<winrt::Microsoft::UI::Xaml::Controls::ContentDialogResult>
            ShowDialogAsync(winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog);

        // Close a dialog opened with ShowDialogAsync, including one still
        // waiting for the compile dialog to close.
        void HideDialog(winrt::Microsoft::UI::Xaml::Controls::ContentDialog const& dialog);

    private:
        static constexpr auto ShowDelay = std::chrono::milliseconds(400);

        void EnsureDialog();
        bool RemoveWaitingDialog(winrt::Microsoft::UI::Xaml::Controls::ContentDialog const& dialog);

        winrt::Microsoft::UI::Xaml::Controls::ContentDialog m_dialog{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock    m_text{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::ProgressBar  m_bar{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock    m_variantText{ nullptr };
        uint32_t m_variantsShown{ 0 };   // last count written, to skip no-op updates

        // Completes when the compile dialog has closed.
        winrt::Windows::Foundation::IAsyncOperation<winrt::Microsoft::UI::Xaml::Controls::ContentDialogResult>
            m_showOperation{ nullptr };
        uint32_t m_otherDialogs{ 0 };   // dialogs open or waiting in ShowDialogAsync
        std::vector<winrt::Microsoft::UI::Xaml::Controls::ContentDialog> m_waitingDialogs; // waiting for the compile dialog to close

        bool     m_busy{ false };      // compiles pending in the current burst
        bool     m_showing{ false };
        bool     m_dismissed{ false }; // closed by the user; stays down for this burst
        uint32_t m_peak{ 0 };          // most compiles pending at once this burst
        std::chrono::steady_clock::time_point m_burstStart{};
    };
}
