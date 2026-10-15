#include "pch.h"
#include "EffectDesignerWindow.xaml.h"
#include "Effects/ShaderVariants.h"
#include "Effects/HlslScaffold.h"
#include "Effects/NodeCatalog.h"
#if __has_include("EffectDesignerWindow.g.cpp")
#include "EffectDesignerWindow.g.cpp"
#endif

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::ShaderLab::implementation
{
    EffectDesignerWindow::EffectDesignerWindow()
    {
        InitializeComponent();

        Title(L"Effect Designer");

        ShaderTypeSelector().SelectedIndex(0);
        OutputTypeSelector().SelectedIndex(0);

        GenerateScaffoldButton().Click({ this, &EffectDesignerWindow::OnGenerateScaffold });
        CompileButton().Click({ this, &EffectDesignerWindow::OnCompile });
        FormatButton().Click([this](auto&&, auto&&)
        {
            auto text = std::wstring(HlslEditorBox().Text());
            HlslEditorBox().Text(winrt::hstring(FormatHlsl(text)));
        });
        AddToGraphButton().Click({ this, &EffectDesignerWindow::OnAddToGraph });
        UpdateInGraphButton().Click({ this, &EffectDesignerWindow::OnUpdateInGraph });
        ShaderTypeSelector().SelectionChanged({ this, &EffectDesignerWindow::OnShaderTypeChanged });
        AddInputButton().Click({ this, &EffectDesignerWindow::OnAddInput });
        AddParamButton().Click({ this, &EffectDesignerWindow::OnAddParam });

        // Analysis fields: show/hide based on Output type selection.
        OutputTypeSelector().SelectionChanged([this](auto&&, auto&&)
        {
            bool showFields = (OutputTypeSelector().SelectedIndex() == 2); // Typed Analysis
            AnalysisFieldsSection().Visibility(showFields
                ? Visibility::Visible : Visibility::Collapsed);
        });
        AddAnalysisFieldButton().Click([this](auto&&, auto&&)
        {
            AppendAnalysisFieldRow();
        });

        HlslEditorBox().PreviewKeyDown([this](auto&&, Input::KeyRoutedEventArgs const& args)
        {
            if (args.Key() == winrt::Windows::System::VirtualKey::Tab)
            {
                // Insert 4 spaces at the cursor position instead of moving focus.
                auto editor = HlslEditorBox();
                auto selStart = editor.SelectionStart();
                auto text = std::wstring(editor.Text());
                text.insert(selStart, L"    ");
                editor.Text(winrt::hstring(text));
                editor.SelectionStart(selStart + 4);
                editor.SelectionLength(0);
                args.Handled(true);
            }
        });

        HlslEditorBox().KeyDown([this](auto&&, Input::KeyRoutedEventArgs const& args)
        {
            if (args.Key() == winrt::Windows::System::VirtualKey::Enter)
            {
                if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0)
                {
                    OnCompile(nullptr, nullptr);
                    args.Handled(true);
                }
            }
        });

        // Add a default input.
        OnAddInput(nullptr, nullptr);
    }

    void EffectDesignerWindow::OnShaderTypeChanged(
        winrt::Windows::Foundation::IInspectable const&,
        Controls::SelectionChangedEventArgs const&)
    {
        bool isCompute = ShaderTypeSelector().SelectedIndex() == 1;
        bool isD3D11 = ShaderTypeSelector().SelectedIndex() == 2;
        ComputeSettingsPanel().Visibility(
            (isCompute || isD3D11) ? Visibility::Visible : Visibility::Collapsed);
    }

    void EffectDesignerWindow::OnAddInput(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto panel = InputsPanel();
        uint32_t idx = panel.Children().Size();

        auto row = Controls::StackPanel();
        row.Orientation(Controls::Orientation::Horizontal);
        row.Spacing(4);

        auto indexText = Controls::TextBlock();
        indexText.Text(std::to_wstring(idx) + L":");
        indexText.VerticalAlignment(VerticalAlignment::Center);
        indexText.Width(24);
        row.Children().Append(indexText);

        auto nameBox = Controls::TextBox();
        nameBox.PlaceholderText(L"Input name");
        nameBox.Text(idx == 0 ? L"Input" : std::format(L"Input {}", idx));
        nameBox.MinWidth(150);
        row.Children().Append(nameBox);

        auto removeBtn = Controls::Button();
        removeBtn.Content(box_value(L"X"));
        removeBtn.Click([row, panel](auto&&, auto&&) {
            uint32_t i = 0;
            if (panel.Children().IndexOf(row, i))
                panel.Children().RemoveAt(i);
        });
        row.Children().Append(removeBtn);

        panel.Children().Append(row);
    }

    winrt::Microsoft::UI::Xaml::Controls::StackPanel
    EffectDesignerWindow::AppendAnalysisFieldRow()
    {
        auto panel = AnalysisFieldsPanel();
        auto row = Controls::StackPanel();
        row.Orientation(Controls::Orientation::Horizontal);
        row.Spacing(4);

        auto nameBox = Controls::TextBox();
        nameBox.PlaceholderText(L"Field Name");
        nameBox.MinWidth(120);
        row.Children().Append(nameBox);

        auto typeCombo = Controls::ComboBox();
        typeCombo.Items().Append(box_value(L"Float"));
        typeCombo.Items().Append(box_value(L"Float2"));
        typeCombo.Items().Append(box_value(L"Float3"));
        typeCombo.Items().Append(box_value(L"Float4"));
        typeCombo.Items().Append(box_value(L"Float Array"));
        typeCombo.Items().Append(box_value(L"Float2 Array"));
        typeCombo.Items().Append(box_value(L"Float3 Array"));
        typeCombo.Items().Append(box_value(L"Float4 Array"));
        typeCombo.SelectedIndex(0);
        typeCombo.MinWidth(110);
        row.Children().Append(typeCombo);

        auto lenBox = Controls::NumberBox();
        lenBox.Header(box_value(L"Length"));
        lenBox.Value(0);
        lenBox.Minimum(0);
        lenBox.Maximum(4096);
        lenBox.Width(80);
        lenBox.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Compact);
        lenBox.Visibility(Visibility::Collapsed);
        row.Children().Append(lenBox);

        // Show length only for array types.
        typeCombo.SelectionChanged([lenBox](auto&& sender, auto&&) {
            auto c = sender.template as<Controls::ComboBox>();
            lenBox.Visibility(c.SelectedIndex() >= 4
                ? Visibility::Visible : Visibility::Collapsed);
        });

        auto removeBtn = Controls::Button();
        removeBtn.Content(box_value(L"\xE711"));
        removeBtn.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Segoe Fluent Icons"));
        removeBtn.Click([row, panel](auto&&, auto&&) {
            uint32_t i = 0;
            if (panel.Children().IndexOf(row, i))
                panel.Children().RemoveAt(i);
        });
        row.Children().Append(removeBtn);

        panel.Children().Append(row);
        return row;
    }

    void EffectDesignerWindow::OnAddParam(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto panel = ParamsPanel();

        // Each parameter is a compact grid: 
        // Row 0: Name | Type | Remove
        // Row 1: Default/Min/Max values (or Labels for enum)
        auto card = Controls::Grid();
        card.Padding(winrt::Microsoft::UI::Xaml::ThicknessHelper::FromLengths(8, 6, 8, 6));
        card.Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(
            winrt::Windows::UI::Color{ 255, 40, 40, 40 }));
        card.CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadiusHelper::FromRadii(4, 4, 4, 4));
        card.RowSpacing(4);
        card.ColumnSpacing(8);

        auto rowDef0 = Controls::RowDefinition();
        rowDef0.Height(winrt::Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(1, winrt::Microsoft::UI::Xaml::GridUnitType::Auto));
        auto rowDef1 = Controls::RowDefinition();
        rowDef1.Height(winrt::Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(1, winrt::Microsoft::UI::Xaml::GridUnitType::Auto));
        card.RowDefinitions().Append(rowDef0);
        card.RowDefinitions().Append(rowDef1);

        auto colDef0 = Controls::ColumnDefinition();
        colDef0.Width(winrt::Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(1, winrt::Microsoft::UI::Xaml::GridUnitType::Auto));
        auto colDef1 = Controls::ColumnDefinition();
        colDef1.Width(winrt::Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(1, winrt::Microsoft::UI::Xaml::GridUnitType::Auto));
        auto colDef2 = Controls::ColumnDefinition();
        colDef2.Width(winrt::Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(1, winrt::Microsoft::UI::Xaml::GridUnitType::Star));
        auto colDef3 = Controls::ColumnDefinition();
        colDef3.Width(winrt::Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(1, winrt::Microsoft::UI::Xaml::GridUnitType::Auto));
        card.ColumnDefinitions().Append(colDef0);
        card.ColumnDefinitions().Append(colDef1);
        card.ColumnDefinitions().Append(colDef2);
        card.ColumnDefinitions().Append(colDef3);

        // Row 0: Name + Type + Remove
        auto nameBox = Controls::TextBox();
        nameBox.PlaceholderText(L"Name");
        nameBox.MinWidth(120);
        Controls::Grid::SetRow(nameBox, 0);
        Controls::Grid::SetColumn(nameBox, 0);
        card.Children().Append(nameBox);

        auto typeCombo = Controls::ComboBox();
        typeCombo.Items().Append(box_value(L"float"));
        typeCombo.Items().Append(box_value(L"float2"));
        typeCombo.Items().Append(box_value(L"float3"));
        typeCombo.Items().Append(box_value(L"float4"));
        typeCombo.Items().Append(box_value(L"int"));
        typeCombo.Items().Append(box_value(L"uint"));
        typeCombo.Items().Append(box_value(L"bool"));
        typeCombo.Items().Append(box_value(L"enum"));
        typeCombo.SelectedIndex(0);
        typeCombo.MinWidth(90);
        Controls::Grid::SetRow(typeCombo, 0);
        Controls::Grid::SetColumn(typeCombo, 1);
        card.Children().Append(typeCombo);

        auto removeBtn = Controls::Button();
        removeBtn.Content(box_value(L"X"));
        removeBtn.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Top);
        Controls::Grid::SetRow(removeBtn, 0);
        Controls::Grid::SetColumn(removeBtn, 3);
        removeBtn.Click([card, panel](auto&&, auto&&) {
            uint32_t i = 0;
            if (panel.Children().IndexOf(card, i))
                panel.Children().RemoveAt(i);
        });
        card.Children().Append(removeBtn);

        // Row 1: Value fields container (spans columns 0-2)
        auto valRow = Controls::StackPanel();
        valRow.Orientation(Controls::Orientation::Horizontal);
        valRow.Spacing(8);
        Controls::Grid::SetRow(valRow, 1);
        Controls::Grid::SetColumn(valRow, 0);
        Controls::Grid::SetColumnSpan(valRow, 3);
        card.Children().Append(valRow);

        // Helper to rebuild value fields based on type.
        auto rebuildDefaults = [valRow](int typeIdx)
        {
            valRow.Children().Clear();

            if (typeIdx == 6) // bool
            {
                auto toggle = Controls::ToggleSwitch();
                toggle.Header(box_value(L"Default"));
                toggle.IsOn(false);
                valRow.Children().Append(toggle);
                return;
            }

            if (typeIdx == 7) // enum
            {
                auto labelsBox = Controls::TextBox();
                labelsBox.PlaceholderText(L"Label1, Label2, ...");
                labelsBox.Header(box_value(L"Labels (comma-separated)"));
                labelsBox.MinWidth(200);
                valRow.Children().Append(labelsBox);

                auto minBox = Controls::NumberBox();
                minBox.Header(box_value(L"Min"));
                minBox.Value(0.0);
                minBox.Width(70);
                minBox.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Compact);
                valRow.Children().Append(minBox);

                auto maxBox = Controls::NumberBox();
                maxBox.Header(box_value(L"Max"));
                maxBox.Value(1.0);
                maxBox.Width(70);
                maxBox.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Compact);
                valRow.Children().Append(maxBox);

                // Option specialisation (ShaderVariants.h): one shader per option,
                // with the value compiled in as a constant.
                auto variantsBox = Controls::CheckBox();
                variantsBox.Content(box_value(L"Variant per option"));
                variantsBox.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Bottom);
                Controls::ToolTipService::SetToolTip(variantsBox, box_value(
                    L"Compile a separate shader for each option, with the value fixed, so the "
                    L"compiler can drop the code the others need. Every variant is compiled when "
                    L"the effect is placed, so changing the option later never waits. Parameters "
                    L"used by only one option (a Custom gamut's primaries) stay adjustable."));
                valRow.Children().Append(variantsBox);
                return;
            }

            int count = 1;
            if (typeIdx == 1) count = 2;       // float2
            else if (typeIdx == 2) count = 3;  // float3
            else if (typeIdx == 3) count = 4;  // float4

            for (int i = 0; i < count; ++i)
            {
                auto nb = Controls::NumberBox();
                nb.Value(0.0);
                nb.Width(70);
                nb.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Compact);
                if (count == 1)
                    nb.Header(box_value(L"Default"));
                else
                {
                    static const wchar_t* compLabels[] = { L"X", L"Y", L"Z", L"W" };
                    nb.Header(box_value(winrt::hstring(compLabels[i])));
                }
                valRow.Children().Append(nb);
            }

            auto minBox = Controls::NumberBox();
            minBox.Header(box_value(L"Min"));
            minBox.Value(0.0);
            minBox.Width(70);
            minBox.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Compact);
            valRow.Children().Append(minBox);

            auto maxBox = Controls::NumberBox();
            maxBox.Header(box_value(L"Max"));
            maxBox.Value(1.0);
            maxBox.Width(70);
            maxBox.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Compact);
            valRow.Children().Append(maxBox);
        };
        rebuildDefaults(0);

        typeCombo.SelectionChanged([rebuildDefaults](auto&& sender, auto&&)
        {
            auto combo = sender.template as<Controls::ComboBox>();
            rebuildDefaults(combo.SelectedIndex());
        });

        panel.Children().Append(card);
    }

    ::ShaderLab::Graph::CustomEffectDefinition EffectDesignerWindow::BuildDefinition()
    {
        ::ShaderLab::Graph::CustomEffectDefinition def;

        def.shaderType = [&]() {
            int idx = ShaderTypeSelector().SelectedIndex();
            if (idx == 1) return ::ShaderLab::Graph::CustomShaderType::ComputeShader;
            if (idx == 2) return ::ShaderLab::Graph::CustomShaderType::D3D11ComputeShader;
            return ::ShaderLab::Graph::CustomShaderType::PixelShader;
        }();

        // Inputs.
        auto inputPanel = InputsPanel();
        for (uint32_t i = 0; i < inputPanel.Children().Size(); ++i)
        {
            auto row = inputPanel.Children().GetAt(i).as<Controls::StackPanel>();
            if (row.Children().Size() >= 2)
            {
                auto nameBox = row.Children().GetAt(1).as<Controls::TextBox>();
                def.inputNames.push_back(std::wstring(nameBox.Text()));
            }
        }

        // Parameters.
        auto paramPanel = ParamsPanel();
        for (uint32_t i = 0; i < paramPanel.Children().Size(); ++i)
        {
            auto card = paramPanel.Children().GetAt(i).try_as<Controls::Grid>();
            if (!card) continue;

            // Find children by type: nameBox (TextBox), typeCombo (ComboBox), valRow (StackPanel in row 1)
            Controls::TextBox nameBox{ nullptr };
            Controls::ComboBox typeCombo{ nullptr };
            Controls::StackPanel valRow{ nullptr };

            for (uint32_t c = 0; c < card.Children().Size(); ++c)
            {
                auto child = card.Children().GetAt(c);
                int row = Controls::Grid::GetRow(child.as<winrt::Microsoft::UI::Xaml::FrameworkElement>());
                if (row == 0)
                {
                    if (auto tb = child.try_as<Controls::TextBox>()) nameBox = tb;
                    else if (auto cb = child.try_as<Controls::ComboBox>()) typeCombo = cb;
                }
                else if (row == 1)
                {
                    if (auto sp = child.try_as<Controls::StackPanel>()) valRow = sp;
                }
            }

            if (!nameBox || !typeCombo || !valRow) continue;

            ::ShaderLab::Graph::ParameterDefinition param;
            param.name = std::wstring(nameBox.Text());
            param.typeName = std::wstring(unbox_value<hstring>(typeCombo.SelectedItem()));

            // valRow children: [default value(s)...] [Min NumberBox] [Max NumberBox]
            // Min and Max are the last two NumberBoxes (except for bool which has no min/max).
            uint32_t numChildren = valRow.Children().Size();

            // Find min/max (last two NumberBoxes for non-bool types).
            if (param.typeName != L"bool" && numChildren >= 2)
            {
                auto minNb = valRow.Children().GetAt(numChildren - 2).try_as<Controls::NumberBox>();
                auto maxNb = valRow.Children().GetAt(numChildren - 1).try_as<Controls::NumberBox>();
                if (minNb) param.minValue = static_cast<float>(minNb.Value());
                if (maxNb) param.maxValue = static_cast<float>(maxNb.Value());
            }

            // Read default values.
            if (param.typeName == L"float" && numChildren >= 3)
            {
                param.defaultValue = static_cast<float>(
                    valRow.Children().GetAt(0).as<Controls::NumberBox>().Value());
            }
            else if (param.typeName == L"float2" && numChildren >= 4)
            {
                namespace Num = winrt::Windows::Foundation::Numerics;
                param.defaultValue = Num::float2{
                    static_cast<float>(valRow.Children().GetAt(0).as<Controls::NumberBox>().Value()),
                    static_cast<float>(valRow.Children().GetAt(1).as<Controls::NumberBox>().Value()) };
            }
            else if (param.typeName == L"float3" && numChildren >= 5)
            {
                namespace Num = winrt::Windows::Foundation::Numerics;
                param.defaultValue = Num::float3{
                    static_cast<float>(valRow.Children().GetAt(0).as<Controls::NumberBox>().Value()),
                    static_cast<float>(valRow.Children().GetAt(1).as<Controls::NumberBox>().Value()),
                    static_cast<float>(valRow.Children().GetAt(2).as<Controls::NumberBox>().Value()) };
            }
            else if (param.typeName == L"float4" && numChildren >= 6)
            {
                namespace Num = winrt::Windows::Foundation::Numerics;
                param.defaultValue = Num::float4{
                    static_cast<float>(valRow.Children().GetAt(0).as<Controls::NumberBox>().Value()),
                    static_cast<float>(valRow.Children().GetAt(1).as<Controls::NumberBox>().Value()),
                    static_cast<float>(valRow.Children().GetAt(2).as<Controls::NumberBox>().Value()),
                    static_cast<float>(valRow.Children().GetAt(3).as<Controls::NumberBox>().Value()) };
            }
            else if (param.typeName == L"int" && numChildren >= 3)
            {
                param.defaultValue = static_cast<int32_t>(
                    valRow.Children().GetAt(0).as<Controls::NumberBox>().Value());
            }
            else if (param.typeName == L"uint" && numChildren >= 3)
            {
                param.defaultValue = static_cast<uint32_t>(
                    valRow.Children().GetAt(0).as<Controls::NumberBox>().Value());
            }
            else if (param.typeName == L"bool" && numChildren >= 1)
            {
                // valRow has a ToggleSwitch for bool.
                auto toggle = valRow.Children().GetAt(0).try_as<Controls::ToggleSwitch>();
                param.defaultValue = toggle ? toggle.IsOn() : false;
            }
            else if (param.typeName == L"enum")
            {
                // Enum: stored as float in HLSL cbuffer. Labels in the TextBox.
                param.typeName = L"float";  // HLSL type is float
                auto labelsBox = valRow.Children().GetAt(0).try_as<Controls::TextBox>();
                if (labelsBox)
                {
                    auto text = std::wstring(labelsBox.Text());
                    // Parse comma-separated labels.
                    std::wstring label;
                    for (wchar_t ch : text)
                    {
                        if (ch == L',')
                        {
                            // Trim whitespace.
                            while (!label.empty() && label.front() == L' ') label.erase(label.begin());
                            while (!label.empty() && label.back() == L' ') label.pop_back();
                            if (!label.empty()) param.enumLabels.push_back(label);
                            label.clear();
                        }
                        else label += ch;
                    }
                    while (!label.empty() && label.front() == L' ') label.erase(label.begin());
                    while (!label.empty() && label.back() == L' ') label.pop_back();
                    if (!label.empty()) param.enumLabels.push_back(label);
                }
                param.defaultValue = 0.0f;
                param.maxValue = static_cast<float>(param.enumLabels.size() - 1);
                if (valRow.Children().Size() > 3)
                    if (auto variants = valRow.Children().GetAt(3).try_as<Controls::CheckBox>())
                        param.specialize = variants.IsChecked() && variants.IsChecked().Value();
            }
            else
            {
                param.defaultValue = 0.0f;
            }

            def.parameters.push_back(std::move(param));
        }

        // Compute settings.
        if (def.shaderType == ::ShaderLab::Graph::CustomShaderType::ComputeShader)
        {
            def.threadGroupX = static_cast<uint32_t>(ThreadGroupX().Value());
            def.threadGroupY = static_cast<uint32_t>(ThreadGroupY().Value());
            def.threadGroupZ = static_cast<uint32_t>(ThreadGroupZ().Value());

            int outIdx = OutputTypeSelector().SelectedIndex();
            if (outIdx == 1) def.analysisOutputType = ::ShaderLab::Graph::AnalysisOutputType::Histogram;
            else if (outIdx == 2)
            {
                def.analysisOutputType = ::ShaderLab::Graph::AnalysisOutputType::Typed;

                // Collect analysis output fields from UI.
                auto fieldsPanel = AnalysisFieldsPanel();
                for (uint32_t fi = 0; fi < fieldsPanel.Children().Size(); ++fi)
                {
                    auto fRow = fieldsPanel.Children().GetAt(fi).try_as<Controls::StackPanel>();
                    if (!fRow || fRow.Children().Size() < 3) continue;

                    auto fName = fRow.Children().GetAt(0).try_as<Controls::TextBox>();
                    auto fType = fRow.Children().GetAt(1).try_as<Controls::ComboBox>();
                    auto fLen  = fRow.Children().GetAt(2).try_as<Controls::NumberBox>();
                    if (!fName || !fType) continue;

                    ::ShaderLab::Graph::AnalysisFieldDescriptor fd;
                    fd.name = std::wstring(fName.Text());
                    fd.type = static_cast<::ShaderLab::Graph::AnalysisFieldType>(fType.SelectedIndex());
                    if (fLen && fType.SelectedIndex() >= 4)
                        fd.arrayLength = static_cast<uint32_t>(fLen.Value());
                    def.analysisFields.push_back(std::move(fd));
                }
            }
        }

        def.hlslSource = std::wstring(HlslEditorBox().Text());
        return def;
    }

    bool EffectDesignerWindow::CompileHlsl(::ShaderLab::Graph::CustomEffectDefinition& def)
    {
        auto hlsl = std::wstring(HlslEditorBox().Text());
        def.hlslSource = hlsl;

        // WinUI TextBox uses \r as line separator. D3DCompile needs \n.
        std::string hlslUtf8 = winrt::to_string(hlsl);
        for (auto& ch : hlslUtf8)
        {
            if (ch == '\r') ch = '\n';
        }

        std::wstring target = (def.shaderType == ::ShaderLab::Graph::CustomShaderType::PixelShader)
            ? L"ps_5_0" : L"cs_5_0";
        std::wstring entryPoint = L"main";

        // Compile the generic build only; the evaluator builds the variants once the effect is placed.
        const auto macroDefs = ::ShaderLab::Effects::GenericVariantMacros(def);
        std::vector<::ShaderLab::Effects::ShaderCompiler::MacroDef> macros;
        for (const auto& [name, value] : macroDefs) macros.push_back({ name.c_str(), value.c_str() });
        auto result = ::ShaderLab::Effects::ShaderCompiler::CompileFromString(
            hlslUtf8, "EffectDesigner", winrt::to_string(entryPoint), winrt::to_string(target), macros);
        if (!result.succeeded)
        {
            CompileStatusText().Text(L"X " + winrt::hstring(result.ErrorMessage()));
            def.compiledBytecode.clear();
            return false;
        }

        auto* blob = result.bytecode.get();
        def.compiledBytecode.resize(blob->GetBufferSize());
        memcpy(def.compiledBytecode.data(), blob->GetBufferPointer(), blob->GetBufferSize());

        // Generate a unique GUID for this shader instance if not already set.
        if (def.shaderGuid == GUID{})
            CoCreateGuid(&def.shaderGuid);

        CompileStatusText().Text(L"V Compiled successfully (" +
            winrt::to_hstring(def.compiledBytecode.size()) + L" bytes)");
        return true;
    }

    std::wstring EffectDesignerWindow::FormatHlsl(const std::wstring& source)
    {
        // Split into lines (handle \r, \n, \r\n).
        std::vector<std::wstring> lines;
        std::wstring current;
        for (size_t i = 0; i < source.size(); ++i)
        {
            wchar_t ch = source[i];
            if (ch == L'\r')
            {
                lines.push_back(current);
                current.clear();
                if (i + 1 < source.size() && source[i + 1] == L'\n') ++i;
            }
            else if (ch == L'\n')
            {
                lines.push_back(current);
                current.clear();
            }
            else
            {
                current += ch;
            }
        }
        if (!current.empty()) lines.push_back(current);

        // If input is a single long line (common for embedded HLSL),
        // split on semicolons and braces first.
        if (lines.size() <= 2)
        {
            std::vector<std::wstring> expanded;
            for (const auto& line : lines)
            {
                std::wstring buf;
                bool inString = false;
                bool inLineComment = false;
                for (size_t i = 0; i < line.size(); ++i)
                {
                    wchar_t ch = line[i];
                    wchar_t next = (i + 1 < line.size()) ? line[i + 1] : 0;

                    if (inLineComment) { buf += ch; continue; }
                    if (ch == L'"') inString = !inString;
                    if (inString) { buf += ch; continue; }
                    if (ch == L'/' && next == L'/') { inLineComment = true; buf += ch; continue; }

                    if (ch == L'{')
                    {
                        // Trim trailing whitespace from buf.
                        while (!buf.empty() && buf.back() == L' ') buf.pop_back();
                        if (!buf.empty()) expanded.push_back(buf);
                        expanded.push_back(L"{");
                        buf.clear();
                        // Skip whitespace after brace.
                        while (i + 1 < line.size() && line[i + 1] == L' ') ++i;
                        continue;
                    }
                    if (ch == L'}')
                    {
                        while (!buf.empty() && buf.back() == L' ') buf.pop_back();
                        if (!buf.empty()) expanded.push_back(buf);
                        expanded.push_back(L"}");
                        buf.clear();
                        // Skip whitespace + optional semicolon after }.
                        while (i + 1 < line.size() && line[i + 1] == L' ') ++i;
                        if (i + 1 < line.size() && line[i + 1] == L';') { ++i; expanded.back() += L";"; }
                        continue;
                    }
                    if (ch == L';')
                    {
                        buf += ch;
                        // End this statement as a line.
                        while (!buf.empty() && buf.front() == L' ') buf.erase(buf.begin());
                        expanded.push_back(buf);
                        buf.clear();
                        // Skip whitespace after semicolon.
                        while (i + 1 < line.size() && line[i + 1] == L' ') ++i;
                        continue;
                    }
                    buf += ch;
                }
                while (!buf.empty() && buf.front() == L' ') buf.erase(buf.begin());
                if (!buf.empty()) expanded.push_back(buf);
            }
            lines = std::move(expanded);
        }

        // Now apply indentation based on brace depth.
        std::wstring result;
        int indent = 0;

        for (auto& line : lines)
        {
            // Trim leading/trailing whitespace.
            size_t start = line.find_first_not_of(L" \t");
            size_t end = line.find_last_not_of(L" \t");
            if (start == std::wstring::npos) { result += L"\n"; continue; }
            line = line.substr(start, end - start + 1);

            // Decrease indent for closing braces.
            if (!line.empty() && line[0] == L'}')
                indent = (std::max)(indent - 1, 0);

            // Apply indentation.
            for (int i = 0; i < indent; ++i)
                result += L"    ";
            result += line;
            result += L"\n";

            // Increase indent after opening braces.
            // Count net braces on this line (excluding strings/comments).
            for (wchar_t ch : line)
            {
                if (ch == L'{') ++indent;
                else if (ch == L'}') indent = (std::max)(indent - 1, 0);
            }
            // Re-adjust: we already decremented for leading }, so add back.
            if (!line.empty() && line[0] == L'}')
                indent = indent; // already handled above
        }

        // Remove trailing newline.
        while (!result.empty() && result.back() == L'\n') result.pop_back();
        return result;
    }

    void EffectDesignerWindow::OnGenerateScaffold(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto def = BuildDefinition();
        auto hlsl = ::ShaderLab::Effects::GenerateEffectScaffold(def);
        HlslEditorBox().Text(winrt::hstring(FormatHlsl(hlsl)));
        CompileStatusText().Text(L"Scaffold generated. Edit the HLSL and click Compile.");
    }

    void EffectDesignerWindow::OnCompile(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto def = BuildDefinition();
        bool ok = CompileHlsl(def);
        AddToGraphButton().IsEnabled(ok);

        if (ok && m_editingNodeId != 0)
        {
            UpdateInGraphButton().IsEnabled(true);
        }

        if (ok)
            m_lastBytecode = def.compiledBytecode;
    }

    void EffectDesignerWindow::OnAddToGraph(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto def = BuildDefinition();
        def.compiledBytecode = m_lastBytecode;
        if (def.shaderGuid == GUID{})
            CoCreateGuid(&def.shaderGuid);

        std::wstring name(EffectNameBox().Text());
        if (name.empty()) name = L"Custom Effect";
        auto node = ::ShaderLab::Effects::CreateCustomEffectNode(std::move(def), std::move(name));

        if (m_addToGraph)
        {
            m_editingNodeId = m_addToGraph(std::move(node));
            UpdateInGraphButton().Visibility(Visibility::Visible);
            UpdateInGraphButton().IsEnabled(true);
            CompileStatusText().Text(L"V Added to graph as node " + winrt::to_hstring(m_editingNodeId));
        }
    }

    void EffectDesignerWindow::OnUpdateInGraph(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (m_editingNodeId == 0 || !m_updateInGraph) return;

        auto def = BuildDefinition();
        def.compiledBytecode = m_lastBytecode;
        // D2D ignores LoadPixelShader for a GUID it already holds, so new bytecode needs a new GUID.
        CoCreateGuid(&def.shaderGuid);

        m_updateInGraph(m_editingNodeId, std::move(def));
        CompileStatusText().Text(L"V Updated node " + winrt::to_hstring(m_editingNodeId) + L" in graph");
    }

    void EffectDesignerWindow::LoadDefinition(uint32_t nodeId, const ::ShaderLab::Graph::CustomEffectDefinition& def,
                                              const std::wstring& nodeName)
    {
        m_editingNodeId = nodeId;
        m_lastBytecode = def.compiledBytecode;

        EffectNameBox().Text(nodeName.empty() ? L"Custom Effect" : winrt::hstring(nodeName));
        ShaderTypeSelector().SelectedIndex(
            def.shaderType == ::ShaderLab::Graph::CustomShaderType::ComputeShader ? 1 :
            def.shaderType == ::ShaderLab::Graph::CustomShaderType::D3D11ComputeShader ? 2 : 0);

        // Populate inputs.
        InputsPanel().Children().Clear();
        for (const auto& name : def.inputNames)
        {
            OnAddInput(nullptr, nullptr);
            auto lastRow = InputsPanel().Children().GetAt(InputsPanel().Children().Size() - 1)
                .as<Controls::StackPanel>();
            lastRow.Children().GetAt(1).as<Controls::TextBox>().Text(winrt::hstring(name));
        }

        // Populate parameters.
        ParamsPanel().Children().Clear();
        for (const auto& p : def.parameters)
        {
            OnAddParam(nullptr, nullptr);
            auto lastCard = ParamsPanel().Children().GetAt(ParamsPanel().Children().Size() - 1)
                .as<Controls::Grid>();

            // Find children by type in the card Grid.
            Controls::TextBox nameBox{ nullptr };
            Controls::ComboBox combo{ nullptr };
            Controls::StackPanel valRow{ nullptr };

            for (uint32_t ci = 0; ci < lastCard.Children().Size(); ++ci)
            {
                auto child = lastCard.Children().GetAt(ci);
                int gridRow = Controls::Grid::GetRow(child.as<winrt::Microsoft::UI::Xaml::FrameworkElement>());
                if (gridRow == 0)
                {
                    if (auto tb = child.try_as<Controls::TextBox>()) nameBox = tb;
                    else if (auto cb = child.try_as<Controls::ComboBox>()) combo = cb;
                }
                else if (gridRow == 1)
                {
                    if (auto sp = child.try_as<Controls::StackPanel>()) valRow = sp;
                }
            }

            if (nameBox) nameBox.Text(winrt::hstring(p.name));

            // Set type combo.
            if (combo)
            {
                if (!p.enumLabels.empty())
                    combo.SelectedIndex(7); // enum
                else
                {
                    for (int32_t i = 0; i < static_cast<int32_t>(combo.Items().Size()); ++i)
                    {
                        if (unbox_value<hstring>(combo.Items().GetAt(i)) == p.typeName)
                        { combo.SelectedIndex(i); break; }
                    }
                }
            }

            // Restore values in valRow.
            if (valRow)
            {
                if (!p.enumLabels.empty())
                {
                    auto labelsBox = valRow.Children().GetAt(0).try_as<Controls::TextBox>();
                    if (labelsBox)
                    {
                        std::wstring labels;
                        for (size_t li = 0; li < p.enumLabels.size(); ++li)
                        {
                            if (li > 0) labels += L", ";
                            labels += p.enumLabels[li];
                        }
                        labelsBox.Text(winrt::hstring(labels));
                    }
                    if (valRow.Children().Size() > 3)
                        if (auto variants = valRow.Children().GetAt(3).try_as<Controls::CheckBox>())
                            variants.IsChecked(p.specialize);
                }
                else if (p.typeName == L"bool")
                {
                    auto toggle = valRow.Children().GetAt(0).try_as<Controls::ToggleSwitch>();
                    if (toggle)
                    {
                        std::visit([&toggle](const auto& v) {
                            using T = std::decay_t<decltype(v)>;
                            if constexpr (std::is_same_v<T, bool>) toggle.IsOn(v);
                        }, p.defaultValue);
                    }
                }
                else
                {
                    // Collect all NumberBoxes in valRow.
                    std::vector<Controls::NumberBox> allBoxes;
                    for (uint32_t ci = 0; ci < valRow.Children().Size(); ++ci)
                    {
                        auto nb = valRow.Children().GetAt(ci).try_as<Controls::NumberBox>();
                        if (nb) allBoxes.push_back(nb);
                    }

                    // Last two are Min/Max, rest are default values.
                    if (allBoxes.size() >= 2)
                    {
                        allBoxes[allBoxes.size() - 2].Value(p.minValue);
                        allBoxes[allBoxes.size() - 1].Value(p.maxValue);
                    }

                    // Set default values (all boxes except last 2).
                    size_t defCount = allBoxes.size() >= 2 ? allBoxes.size() - 2 : 0;
                    std::visit([&allBoxes, defCount](const auto& v)
                    {
                        using T = std::decay_t<decltype(v)>;
                        if constexpr (std::is_same_v<T, float>)
                        { if (defCount >= 1) allBoxes[0].Value(v); }
                        else if constexpr (std::is_same_v<T, int32_t>)
                        { if (defCount >= 1) allBoxes[0].Value(static_cast<double>(v)); }
                        else if constexpr (std::is_same_v<T, uint32_t>)
                        { if (defCount >= 1) allBoxes[0].Value(static_cast<double>(v)); }
                        else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>)
                        { if (defCount >= 1) allBoxes[0].Value(v.x); if (defCount >= 2) allBoxes[1].Value(v.y); }
                        else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>)
                        { if (defCount >= 1) allBoxes[0].Value(v.x); if (defCount >= 2) allBoxes[1].Value(v.y); if (defCount >= 3) allBoxes[2].Value(v.z); }
                        else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>)
                        { if (defCount >= 1) allBoxes[0].Value(v.x); if (defCount >= 2) allBoxes[1].Value(v.y); if (defCount >= 3) allBoxes[2].Value(v.z); if (defCount >= 4) allBoxes[3].Value(v.w); }
                    }, p.defaultValue);
                }

                // For enum, set min/max on the last two NumberBoxes in valRow.
                if (!p.enumLabels.empty())
                {
                    std::vector<Controls::NumberBox> enumBoxes;
                    for (uint32_t ci = 0; ci < valRow.Children().Size(); ++ci)
                    {
                        auto nb = valRow.Children().GetAt(ci).try_as<Controls::NumberBox>();
                        if (nb) enumBoxes.push_back(nb);
                    }
                    if (enumBoxes.size() >= 2)
                    {
                        enumBoxes[enumBoxes.size() - 2].Value(p.minValue);
                        enumBoxes[enumBoxes.size() - 1].Value(p.maxValue);
                    }
                }
            }
        }

        // Compute settings.
        if (def.shaderType == ::ShaderLab::Graph::CustomShaderType::ComputeShader ||
            def.shaderType == ::ShaderLab::Graph::CustomShaderType::D3D11ComputeShader)
        {
            ThreadGroupX().Value(def.threadGroupX);
            ThreadGroupY().Value(def.threadGroupY);
            ThreadGroupZ().Value(def.threadGroupZ);
        }

        // Output type + analysis fields. Map AnalysisOutputType -> selector
        // index (None=0, Histogram=1, Float Buffer=2, Typed Analysis=3) so
        // opening a built-in ShaderLab effect (or any saved custom effect)
        // shows the same output configuration the developer originally
        // declared, instead of the empty default.
        {
            int selIdx = 0;
            switch (def.analysisOutputType)
            {
            case ::ShaderLab::Graph::AnalysisOutputType::None:        selIdx = 0; break;
            case ::ShaderLab::Graph::AnalysisOutputType::Histogram:   selIdx = 1; break;
            case ::ShaderLab::Graph::AnalysisOutputType::Typed:       selIdx = 2; break;
            }
            OutputTypeSelector().SelectedIndex(selIdx);
        }

        // Repopulate the Typed Analysis field rows from the definition,
        // matching the manual-add layout (TextBox, ComboBox, NumberBox).
        AnalysisFieldsPanel().Children().Clear();
        if (def.analysisOutputType == ::ShaderLab::Graph::AnalysisOutputType::Typed)
        {
            for (const auto& fd : def.analysisFields)
            {
                auto row = AppendAnalysisFieldRow();

                if (auto nameBox = row.Children().GetAt(0).try_as<Controls::TextBox>())
                    nameBox.Text(winrt::hstring(fd.name));

                int typeIdx = 0;
                switch (fd.type)
                {
                case ::ShaderLab::Graph::AnalysisFieldType::Float:        typeIdx = 0; break;
                case ::ShaderLab::Graph::AnalysisFieldType::Float2:       typeIdx = 1; break;
                case ::ShaderLab::Graph::AnalysisFieldType::Float3:       typeIdx = 2; break;
                case ::ShaderLab::Graph::AnalysisFieldType::Float4:       typeIdx = 3; break;
                case ::ShaderLab::Graph::AnalysisFieldType::FloatArray:   typeIdx = 4; break;
                case ::ShaderLab::Graph::AnalysisFieldType::Float2Array:  typeIdx = 5; break;
                case ::ShaderLab::Graph::AnalysisFieldType::Float3Array:  typeIdx = 6; break;
                case ::ShaderLab::Graph::AnalysisFieldType::Float4Array:  typeIdx = 7; break;
                }
                if (auto typeCombo = row.Children().GetAt(1).try_as<Controls::ComboBox>())
                    typeCombo.SelectedIndex(typeIdx);

                if (auto lenBox = row.Children().GetAt(2).try_as<Controls::NumberBox>())
                    lenBox.Value(static_cast<double>(fd.arrayLength));
            }
        }
        AnalysisFieldsSection().Visibility(
            def.analysisOutputType == ::ShaderLab::Graph::AnalysisOutputType::Typed
            ? Visibility::Visible : Visibility::Collapsed);

        // If no source exists (built-in effects that use hardcoded paths),
        // generate a scaffold so the developer sees representative HLSL.
        std::wstring hlslToShow = def.hlslSource;
        if (hlslToShow.empty())
        {
            auto scaffoldDef = BuildDefinition();
            scaffoldDef.shaderType = def.shaderType;
            scaffoldDef.analysisFields = def.analysisFields;
            scaffoldDef.analysisOutputType = def.analysisOutputType;
            hlslToShow = ::ShaderLab::Effects::GenerateEffectScaffold(scaffoldDef);
        }
        HlslEditorBox().Text(winrt::hstring(FormatHlsl(hlslToShow)));

        // Update button states.
        UpdateInGraphButton().Visibility(Visibility::Visible);
        UpdateInGraphButton().IsEnabled(!def.compiledBytecode.empty());
        AddToGraphButton().IsEnabled(!def.compiledBytecode.empty());

        CompileStatusText().Text(def.compiledBytecode.empty()
            ? L"Loaded -- not yet compiled"
            : L"V Loaded -- compiled and ready");
    }
}
