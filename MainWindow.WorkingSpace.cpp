// MainWindow partial: display-profile selection and the per-frame Working
// Space node sync.

#include "pch.h"
#include "MainWindow.xaml.h"

#include "Rendering/IccProfileParser.h"
#include "Rendering/WorkingSpaceSync.h"

namespace winrt::ShaderLab::implementation
{
    namespace
    {
        // Display-profile selector item tags: a preset's index into
        // m_displayPresets, or one of these.
        constexpr int32_t cProfileTagCurrent = -1;
        constexpr int32_t cProfileTagLoad    = -2;
        constexpr int32_t cProfileTagIcc     = -3;
        constexpr int32_t cProfileTagNone    = -4;   // an item without a tag

        int32_t ProfileTag(winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem const& item)
        {
            return winrt::unbox_value_or<int32_t>(item.Tag(), cProfileTagNone);
        }
    }

    // -----------------------------------------------------------------------
    // Display profile selection
    // -----------------------------------------------------------------------

    void MainWindow::PopulateDisplayProfileSelector()
    {
        m_suppressProfileEvent = true;
        auto selector = DisplayProfileSelector();
        selector.Items().Clear();

        // "Current Monitor" item.
        auto currentItem = winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem();
        currentItem.Content(winrt::box_value(L"Current Monitor"));
        currentItem.Tag(winrt::box_value(cProfileTagCurrent));
        selector.Items().Append(currentItem);

        // Preset items.
        m_displayPresets = ::ShaderLab::Rendering::AllPresets();
        for (size_t i = 0; i < m_displayPresets.size(); ++i)
        {
            auto item = winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem();
            item.Content(winrt::box_value(winrt::hstring(m_displayPresets[i].profileName)));
            item.Tag(winrt::box_value(static_cast<int32_t>(i)));
            selector.Items().Append(item);
        }

        // "Load ICC Profile..." action item.
        auto loadItem = winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem();
        loadItem.Content(winrt::box_value(L"Load ICC Profile\u2026"));
        loadItem.Tag(winrt::box_value(cProfileTagLoad));
        selector.Items().Append(loadItem);

        selector.SelectedIndex(0);
        m_committedProfileIndex = 0;
        m_suppressProfileEvent = false;
    }

    void MainWindow::ApplyDisplayProfile(const ::ShaderLab::Rendering::DisplayProfile& profile)
    {
        // The render worker (the only graph writer) re-syncs Working Space
        // nodes from the active profile on its next tick; only the status
        // bar needs a direct poke.
        m_displayMonitor.SetSimulatedProfile(profile);
        UpdateStatusBar();
    }

    void MainWindow::RevertToLiveDisplay()
    {
        m_displayMonitor.ClearSimulatedProfile();
        UpdateStatusBar();
    }

    void MainWindow::UpdateWorkingSpaceNodes()
    {
        // A changed node forces a frame so its consumers re-render.
        if (::ShaderLab::Rendering::UpdateWorkingSpaceNodes(m_graph, m_displayMonitor.ActiveProfile()))
            m_forceRender = true;
    }

    void MainWindow::OnDisplayProfileSelectionChanged(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& /*args*/)
    {
        if (m_suppressProfileEvent) return;

        auto selector = DisplayProfileSelector();
        auto selected = selector.SelectedItem();
        if (!selected) return;

        const int32_t tag = ProfileTag(selected.as<winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem>());
        if (tag == cProfileTagCurrent)
        {
            RevertToLiveDisplay();
            m_committedProfileIndex = selector.SelectedIndex();
        }
        else if (tag >= 0)
        {
            if (static_cast<size_t>(tag) < m_displayPresets.size())
            {
                ApplyDisplayProfile(m_displayPresets[static_cast<size_t>(tag)]);
                m_committedProfileIndex = selector.SelectedIndex();
            }
        }
        else if (tag == cProfileTagIcc)
        {
            if (m_loadedIccProfile.has_value())
            {
                ApplyDisplayProfile(m_loadedIccProfile.value());
                m_committedProfileIndex = selector.SelectedIndex();
            }
        }
        else if (tag == cProfileTagLoad)
        {
            LoadIccProfileAsync();
        }
    }

    winrt::fire_and_forget MainWindow::LoadIccProfileAsync()
    {
        auto strong = get_strong();

        winrt::Windows::Storage::Pickers::FileOpenPicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(m_hwnd);
        picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::DocumentsLibrary);
        picker.FileTypeFilter().Append(L".icc");
        picker.FileTypeFilter().Append(L".icm");

        auto file = co_await picker.PickSingleFileAsync();

        if (!file)
        {
            // User cancelled — revert to the previously committed selection.
            m_suppressProfileEvent = true;
            DisplayProfileSelector().SelectedIndex(m_committedProfileIndex);
            m_suppressProfileEvent = false;
            co_return;
        }

        auto path = std::wstring(file.Path().c_str());
        auto parsed = ::ShaderLab::Rendering::IccProfileParser::LoadFromFile(path);

        if (!parsed.has_value() || !parsed->valid)
        {
            // Parse failed — revert selection and show error in status bar.
            m_suppressProfileEvent = true;
            DisplayProfileSelector().SelectedIndex(m_committedProfileIndex);
            m_suppressProfileEvent = false;
            PipelineFormatText().Text(L"ICC Error: Failed to parse profile");
            co_return;
        }

        auto profile = ::ShaderLab::Rendering::DisplayProfileFromIcc(parsed.value());
        m_loadedIccProfile = profile;

        // Insert or update the ICC item (just before the "Load ICC..." sentinel).
        m_suppressProfileEvent = true;
        auto selector = DisplayProfileSelector();
        uint32_t loadIdx = selector.Items().Size() - 1;

        // Remove any previous ICC item.
        if (loadIdx > 0)
        {
            auto prevItem = selector.Items().GetAt(loadIdx - 1).as<winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem>();
            if (ProfileTag(prevItem) == cProfileTagIcc)
            {
                selector.Items().RemoveAt(loadIdx - 1);
                loadIdx--;
            }
        }

        // Insert the new ICC item before the "Load..." sentinel.
        auto iccItem = winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem();
        iccItem.Content(winrt::box_value(winrt::hstring(profile.profileName)));
        iccItem.Tag(winrt::box_value(cProfileTagIcc));
        selector.Items().InsertAt(loadIdx, iccItem);

        // Select the newly inserted item.
        selector.SelectedIndex(loadIdx);
        m_committedProfileIndex = static_cast<int32_t>(loadIdx);
        m_suppressProfileEvent = false;

        ApplyDisplayProfile(profile);
    }
}
