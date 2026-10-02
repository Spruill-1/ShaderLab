#include "pch.h"
#include "CompileProgressController.h"

namespace ShaderLab::Controls
{
    namespace XC = winrt::Microsoft::UI::Xaml::Controls;

    void CompileProgressController::EnsureDialog()
    {
        if (m_dialog) return;
        m_dialog = XC::ContentDialog();
        m_dialog.Title(winrt::box_value(L"Recompiling shaders"));
        m_text = XC::TextBlock();
        m_text.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::NoWrap);
        m_bar = XC::ProgressBar();
        m_bar.Minimum(0); m_bar.Maximum(1); m_bar.Width(360);
        XC::StackPanel panel;
        panel.Spacing(12);
        panel.Children().Append(m_text);
        panel.Children().Append(m_bar);
        m_dialog.Content(panel);
        // No buttons: it closes itself when compiling finishes. If the user
        // closes it with Escape, it stays down for the rest of this burst.
        m_dialog.Closed([this](auto&&, auto&&)
        {
            if (m_showing && m_busy) m_dismissed = true;
            m_showing = false;
        });
    }

    void CompileProgressController::Update(
        const std::shared_ptr<const Graph::GraphUiSnapshot>& snapshot,
        winrt::Microsoft::UI::Xaml::XamlRoot const& root)
    {
        std::vector<const std::wstring*> compiling;
        uint32_t variants = 0;
        if (snapshot)
            for (const auto& node : snapshot->nodes)
            {
                if (node.compilePending) compiling.push_back(&node.name);
                variants += node.variantsCompiling;
            }

        // Background variants get a status line, not the modal.
        if (m_variantText && variants != m_variantsShown)
        {
            m_variantsShown = variants;
            m_variantText.Text(variants == 0 ? winrt::hstring{}
                : winrt::hstring(L"Precompiling " + std::to_wstring(variants) +
                                 (variants == 1 ? L" shader variant…" : L" shader variants…")));
        }

        const auto now = std::chrono::steady_clock::now();
        if (compiling.empty())
        {
            m_busy = false;
            m_dismissed = false;
            m_peak = 0;
            if (m_showing)
            {
                m_showing = false;
                m_dialog.Hide();
            }
            return;
        }
        if (!m_busy)
        {
            m_busy = true;
            m_burstStart = now;
        }
        m_peak = (std::max)(m_peak, static_cast<uint32_t>(compiling.size()));
        if (m_dismissed || now - m_burstStart < ShowDelay) return;

        EnsureDialog();
        const uint32_t done = m_peak - static_cast<uint32_t>(compiling.size());
        const auto elapsedSeconds = std::chrono::duration_cast<std::chrono::seconds>(now - m_burstStart).count();
        std::wstring text = L"Compiling " + *compiling.front();
        if (m_peak > 1) text += L"  (" + std::to_wstring(done + 1) + L" of " + std::to_wstring(m_peak) + L")";
        text += L"  ·  " + std::to_wstring(elapsedSeconds) + L" s";
        m_text.Text(text);
        // One shader has no progress to report; a batch does.
        m_bar.IsIndeterminate(m_peak <= 1);
        if (m_peak > 1) m_bar.Value(static_cast<double>(done) / m_peak);

        if (!m_showing && root && m_otherDialogs == 0)
        {
            m_dialog.XamlRoot(root);
            try
            {
                m_showOperation = m_dialog.ShowAsync();
                m_showing = true;
            }
            catch (winrt::hresult_error const&)
            {
                // WinUI allows one ContentDialog at a time and another is up.
                // Intentionally swallowed; the next tick tries again.
            }
        }
    }

    winrt::Windows::Foundation::IAsyncOperation<XC::ContentDialogResult>
        CompileProgressController::ShowDialogAsync(XC::ContentDialog dialog)
    {
        // Keep the compile dialog down until this dialog has closed.
        struct OtherDialogScope
        {
            uint32_t& count;
            explicit OtherDialogScope(uint32_t& counter) : count(counter) { ++count; }
            ~OtherDialogScope() { --count; }
        } scope(m_otherDialogs);

        if (m_showing)
        {
            m_showing = false;
            m_dialog.Hide();
        }

        auto result = XC::ContentDialogResult::None;
        try
        {
            // The compile dialog closes asynchronously; wait for it before opening another.
            if (auto closing = std::exchange(m_showOperation, nullptr))
            {
                if (closing.Status() == winrt::Windows::Foundation::AsyncStatus::Started)
                {
                    m_waitingDialogs.push_back(dialog);
                    co_await closing;
                    // HideDialog while waiting removes the entry: do not show at all.
                    if (!RemoveWaitingDialog(dialog))
                        co_return XC::ContentDialogResult::None;
                }
            }
            result = co_await dialog.ShowAsync();
        }
        catch (winrt::hresult_error const& error)
        {
            // Another dialog outside this helper is open. Callers treat None as cancel.
            RemoveWaitingDialog(dialog);
            OutputDebugStringW((L"[ShaderLab] ShowDialogAsync: " + std::wstring(error.message()) + L"\n").c_str());
        }
        co_return result;
    }

    void CompileProgressController::HideDialog(XC::ContentDialog const& dialog)
    {
        if (!RemoveWaitingDialog(dialog))
            dialog.Hide();
    }

    bool CompileProgressController::RemoveWaitingDialog(XC::ContentDialog const& dialog)
    {
        auto it = std::find(m_waitingDialogs.begin(), m_waitingDialogs.end(), dialog);
        if (it == m_waitingDialogs.end()) return false;
        m_waitingDialogs.erase(it);
        return true;
    }
}
