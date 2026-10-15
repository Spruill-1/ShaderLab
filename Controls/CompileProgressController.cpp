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
            if (m_dialogs.BackgroundShowing() && m_busy) m_dismissed = true;
            m_dialogs.BackgroundClosed();
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
            m_dialogs.HideBackground();
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

        m_dialogs.TryShowBackground(m_dialog, root);
    }
}
