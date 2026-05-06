// MainWindow partial (Phase 4 split): display-profile selection and the
// per-frame Working Space node sync. All methods are members of
// `winrt::ShaderLab::implementation::MainWindow` and share state via
// MainWindow.xaml.h. Extracted from MainWindow.xaml.cpp lines 1102-1370
// at commit c177770 (Phase 3 + earlier health phases).

#include "pch.h"
#include "MainWindow.xaml.h"

#include "Rendering/IccProfileParser.h"

namespace winrt::ShaderLab::implementation
{
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
        currentItem.Tag(winrt::box_value(L"current"));
        selector.Items().Append(currentItem);

        // Preset items.
        m_displayPresets = ::ShaderLab::Rendering::AllPresets();
        for (size_t i = 0; i < m_displayPresets.size(); ++i)
        {
            auto item = winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem();
            item.Content(winrt::box_value(winrt::hstring(m_displayPresets[i].profileName)));
            item.Tag(winrt::box_value(L"preset:" + std::to_wstring(i)));
            selector.Items().Append(item);
        }

        // "Load ICC Profile..." action item.
        auto loadItem = winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem();
        loadItem.Content(winrt::box_value(L"Load ICC Profile\u2026"));
        loadItem.Tag(winrt::box_value(L"load"));
        selector.Items().Append(loadItem);

        selector.SelectedIndex(0);
        m_committedProfileIndex = 0;
        m_suppressProfileEvent = false;
    }

    void MainWindow::ApplyDisplayProfile(const ::ShaderLab::Rendering::DisplayProfile& profile)
    {
        m_displayMonitor.SetSimulatedProfile(profile);
        m_graph.MarkAllDirty();
        m_forceRender = true;
        UpdateWorkingSpaceNodes();
        UpdateStatusBar();
    }

    void MainWindow::RevertToLiveDisplay()
    {
        m_displayMonitor.ClearSimulatedProfile();
        m_graph.MarkAllDirty();
        m_forceRender = true;
        UpdateWorkingSpaceNodes();
        UpdateStatusBar();
    }

    void MainWindow::UpdateWorkingSpaceNodes()
    {
        // Walk every node in the graph and, for each "Working Space"
        // parameter node, write the 14 fields of the active profile into
        // the node's properties keyed by analysis-field name. Marks the
        // node dirty only when at least one field actually changed so the
        // render loop doesn't re-evaluate every tick when the profile is
        // stable.
        using namespace ::ShaderLab::Graph;
        using winrt::Windows::Foundation::Numerics::float2;

        auto profile = m_displayMonitor.ActiveProfile();
        const auto& caps = profile.caps;
        const bool isSim = m_displayMonitor.IsSimulated();

        struct ScalarField { const wchar_t* name; float value; };
        const ScalarField scalars[] = {
            { L"ActiveColorMode",  static_cast<float>(caps.activeColorMode) },
            { L"HdrSupported",     caps.hdrSupported    ? 1.0f : 0.0f },
            { L"HdrUserEnabled",   caps.hdrUserEnabled  ? 1.0f : 0.0f },
            { L"WcgSupported",     caps.wcgSupported    ? 1.0f : 0.0f },
            { L"WcgUserEnabled",   caps.wcgUserEnabled  ? 1.0f : 0.0f },
            { L"IsSimulated",      isSim ? 1.0f : 0.0f },
            { L"SdrWhiteNits",     caps.sdrWhiteLevelNits },
            { L"PeakNits",         caps.maxLuminanceNits },
            { L"MinNits",          caps.minLuminanceNits },
            { L"MaxFullFrameNits", caps.maxFullFrameLuminanceNits },
        };

        struct VectorField { const wchar_t* name; float2 value; };
        const VectorField vectors[] = {
            { L"RedPrimary",   float2{ profile.primaryRed.x,   profile.primaryRed.y   } },
            { L"GreenPrimary", float2{ profile.primaryGreen.x, profile.primaryGreen.y } },
            { L"BluePrimary",  float2{ profile.primaryBlue.x,  profile.primaryBlue.y  } },
            { L"WhitePoint",   float2{ profile.whitePoint.x,   profile.whitePoint.y   } },
        };

        bool anyChanged = false;
        for (auto& node : const_cast<std::vector<EffectNode>&>(m_graph.Nodes()))
        {
            if (!node.customEffect.has_value()) continue;
            if (node.customEffect->shaderLabEffectId != L"Working Space") continue;

            bool nodeChanged = false;

            for (const auto& f : scalars)
            {
                auto it = node.properties.find(f.name);
                if (it == node.properties.end())
                {
                    node.properties[f.name] = PropertyValue{ f.value };
                    nodeChanged = true;
                    continue;
                }
                if (auto* cur = std::get_if<float>(&it->second))
                {
                    if (*cur != f.value)
                    {
                        *cur = f.value;
                        nodeChanged = true;
                    }
                }
                else
                {
                    it->second = PropertyValue{ f.value };
                    nodeChanged = true;
                }
            }

            for (const auto& f : vectors)
            {
                auto it = node.properties.find(f.name);
                if (it == node.properties.end())
                {
                    node.properties[f.name] = PropertyValue{ f.value };
                    nodeChanged = true;
                    continue;
                }
                if (auto* cur = std::get_if<float2>(&it->second))
                {
                    if (cur->x != f.value.x || cur->y != f.value.y)
                    {
                        *cur = f.value;
                        nodeChanged = true;
                    }
                }
                else
                {
                    it->second = PropertyValue{ f.value };
                    nodeChanged = true;
                }
            }

            if (nodeChanged)
            {
                node.dirty = true;
                anyChanged = true;
            }
        }

        if (anyChanged)
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

        auto item = selected.as<winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem>();
        auto tag = winrt::unbox_value<winrt::hstring>(item.Tag());

        if (tag == L"current")
        {
            RevertToLiveDisplay();
            m_committedProfileIndex = selector.SelectedIndex();
        }
        else if (tag.size() > 7 && tag.c_str()[0] == L'p') // "preset:N"
        {
            auto indexStr = std::wstring(tag.c_str() + 7);
            size_t presetIdx = static_cast<size_t>(std::stoul(indexStr));
            if (presetIdx < m_displayPresets.size())
            {
                ApplyDisplayProfile(m_displayPresets[presetIdx]);
                m_committedProfileIndex = selector.SelectedIndex();
            }
        }
        else if (tag == L"icc")
        {
            if (m_loadedIccProfile.has_value())
            {
                ApplyDisplayProfile(m_loadedIccProfile.value());
                m_committedProfileIndex = selector.SelectedIndex();
            }
        }
        else if (tag == L"load")
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
            auto prevTag = winrt::unbox_value<winrt::hstring>(prevItem.Tag());
            if (prevTag == L"icc")
            {
                selector.Items().RemoveAt(loadIdx - 1);
                loadIdx--;
            }
        }

        // Insert the new ICC item before the "Load..." sentinel.
        auto iccItem = winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem();
        iccItem.Content(winrt::box_value(winrt::hstring(profile.profileName)));
        iccItem.Tag(winrt::box_value(L"icc"));
        selector.Items().InsertAt(loadIdx, iccItem);

        // Select the newly inserted item.
        selector.SelectedIndex(loadIdx);
        m_committedProfileIndex = static_cast<int32_t>(loadIdx);
        m_suppressProfileEvent = false;

        ApplyDisplayProfile(profile);
    }
}
