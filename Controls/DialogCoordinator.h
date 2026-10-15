#pragma once

#include "pch.h"

namespace ShaderLab::Controls
{
    // WinUI shows one ContentDialog at a time per window. Every dialog in the
    // main window goes through here: ShowAsync for the ordinary ones, and the
    // background-modal calls for the one modal that steps aside for them (the
    // compile progress dialog). The background modal is hidden while another
    // dialog is open and is not shown again until that one closes.
    class DialogCoordinator
    {
    public:
        // Shows `dialog`, hiding the background modal first. Returns None if
        // the dialog cannot be opened or Hide was called before it opened.
        winrt::Windows::Foundation::IAsyncOperation<winrt::Microsoft::UI::Xaml::Controls::ContentDialogResult>
            ShowAsync(winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog);

        // Closes a dialog opened with ShowAsync, including one still waiting
        // for the background modal to close.
        void Hide(winrt::Microsoft::UI::Xaml::Controls::ContentDialog const& dialog);

        // Shows the background modal unless another dialog is open or it is
        // already up. False when it is not shown.
        bool TryShowBackground(winrt::Microsoft::UI::Xaml::Controls::ContentDialog const& modal,
                               winrt::Microsoft::UI::Xaml::XamlRoot const& root);

        // Hides the background modal if it is up.
        void HideBackground();

        // True from TryShowBackground until it is hidden or closed. Read in the
        // modal's Closed handler, it tells a close by the user (still true)
        // from HideBackground (already false).
        bool BackgroundShowing() const { return m_backgroundShowing; }

        // Call from the background modal's Closed handler.
        void BackgroundClosed() { m_backgroundShowing = false; }

    private:
        bool RemoveWaiting(winrt::Microsoft::UI::Xaml::Controls::ContentDialog const& dialog);

        winrt::Microsoft::UI::Xaml::Controls::ContentDialog m_background{ nullptr };
        bool m_backgroundShowing{ false };
        // Completes when the background modal has closed.
        winrt::Windows::Foundation::IAsyncOperation<winrt::Microsoft::UI::Xaml::Controls::ContentDialogResult>
            m_backgroundOperation{ nullptr };
        uint32_t m_otherDialogs{ 0 };   // open or waiting in ShowAsync
        std::vector<winrt::Microsoft::UI::Xaml::Controls::ContentDialog> m_waiting;   // waiting for the background modal to close
    };
}
