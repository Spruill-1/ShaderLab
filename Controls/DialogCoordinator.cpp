#include "pch.h"
#include "DialogCoordinator.h"

namespace ShaderLab::Controls
{
    namespace XC = winrt::Microsoft::UI::Xaml::Controls;

    winrt::Windows::Foundation::IAsyncOperation<XC::ContentDialogResult>
        DialogCoordinator::ShowAsync(XC::ContentDialog dialog)
    {
        // Keep the background modal down until this dialog has closed.
        struct OtherDialogScope
        {
            uint32_t& count;
            explicit OtherDialogScope(uint32_t& counter) : count(counter) { ++count; }
            ~OtherDialogScope() { --count; }
        } scope(m_otherDialogs);

        HideBackground();

        auto result = XC::ContentDialogResult::None;
        try
        {
            // The background modal closes asynchronously; wait for it before opening another.
            if (auto closing = std::exchange(m_backgroundOperation, nullptr))
            {
                if (closing.Status() == winrt::Windows::Foundation::AsyncStatus::Started)
                {
                    m_waiting.push_back(dialog);
                    co_await closing;
                    // Hide while waiting removes the entry: do not show at all.
                    if (!RemoveWaiting(dialog))
                        co_return XC::ContentDialogResult::None;
                }
            }
            result = co_await dialog.ShowAsync();
        }
        catch (winrt::hresult_error const& error)
        {
            // A dialog opened outside the coordinator is up. Callers treat None as cancel.
            RemoveWaiting(dialog);
            OutputDebugStringW((L"[ShaderLab] DialogCoordinator::ShowAsync: " + std::wstring(error.message()) + L"\n").c_str());
        }
        co_return result;
    }

    void DialogCoordinator::Hide(XC::ContentDialog const& dialog)
    {
        if (!RemoveWaiting(dialog))
            dialog.Hide();
    }

    bool DialogCoordinator::TryShowBackground(XC::ContentDialog const& modal,
                                              winrt::Microsoft::UI::Xaml::XamlRoot const& root)
    {
        if (m_backgroundShowing || !root || m_otherDialogs != 0)
            return false;
        modal.XamlRoot(root);
        try
        {
            m_backgroundOperation = modal.ShowAsync();
        }
        catch (winrt::hresult_error const&)
        {
            // A dialog opened outside the coordinator is up; the caller tries
            // again on its next update.
            return false;
        }
        m_background = modal;
        m_backgroundShowing = true;
        return true;
    }

    void DialogCoordinator::HideBackground()
    {
        if (!m_backgroundShowing)
            return;
        m_backgroundShowing = false;
        m_background.Hide();
    }

    bool DialogCoordinator::RemoveWaiting(XC::ContentDialog const& dialog)
    {
        auto it = std::find(m_waiting.begin(), m_waiting.end(), dialog);
        if (it == m_waiting.end()) return false;
        m_waiting.erase(it);
        return true;
    }
}
