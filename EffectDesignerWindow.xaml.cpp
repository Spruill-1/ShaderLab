#include "pch.h"
#include "EffectDesignerWindow.xaml.h"
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
            bool showFields = (OutputTypeSelector().SelectedIndex() == 3); // Typed Analysis
            AnalysisFieldsSection().Visibility(showFields
                ? Visibility::Visible : Visibility::Collapsed);
        });
        AddAnalysisFieldButton().Click([this](auto&&, auto&&)
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
        ComputeSettingsPanel().Visibility(isCompute ? Visibility::Visible : Visibility::Collapsed);
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

        def.shaderType = ShaderTypeSelector().SelectedIndex() == 1
            ? ::ShaderLab::Graph::CustomShaderType::ComputeShader
            : ::ShaderLab::Graph::CustomShaderType::PixelShader;

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
            else if (outIdx == 2) def.analysisOutputType = ::ShaderLab::Graph::AnalysisOutputType::FloatBuffer;
            else if (outIdx == 3)
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

    std::wstring EffectDesignerWindow::GenerateHlsl(const ::ShaderLab::Graph::CustomEffectDefinition& def)
    {
        std::wstring hlsl;

        // Constant buffer.
        if (!def.parameters.empty())
        {
            hlsl += L"cbuffer Constants : register(b0)\n{\n";
            for (const auto& p : def.parameters)
            {
                hlsl += L"    " + p.typeName + L" " + p.name + L";\n";
            }
            hlsl += L"};\n\n";
        }

        if (def.shaderType == ::ShaderLab::Graph::CustomShaderType::PixelShader)
        {
            // Texture inputs.
            for (uint32_t i = 0; i < def.inputNames.size(); ++i)
            {
                hlsl += std::format(L"Texture2D {} : register(t{});\n", def.inputNames[i], i);
            }
            if (!def.inputNames.empty())
            {
                hlsl += L"\n// D2D provides TEXCOORD in pixel/scene space.\n";
                hlsl += L"// All inputs share TEXCOORD0 (same coordinate space).\n";
                hlsl += L"// Use Load(int3(uv0.xy, 0)) for direct texel access.\n\n";
            }

            // Entry point — all inputs use TEXCOORD0 since MapOutputRectToInputRects
            // returns the same rect for all inputs (1:1 mapping).
            hlsl += L"float4 main(\n";
            hlsl += L"    float4 pos : SV_POSITION,\n";
            hlsl += L"    float4 uv0 : TEXCOORD0) : SV_TARGET\n";
            hlsl += L"{\n";

            if (def.inputNames.size() >= 1)
            {
                for (uint32_t i = 0; i < def.inputNames.size(); ++i)
                {
                    hlsl += std::format(L"    float4 color{0} = {1}.Load(int3(uv0.xy, 0));\n",
                        i, def.inputNames[i]);
                }

                hlsl += L"\n";
                if (def.inputNames.size() == 1)
                {
                    hlsl += L"    // Your code here\n\n";
                    hlsl += L"    return color0;\n";
                }
                else
                {
                    hlsl += L"    // Your code here -- blend, combine, or process the inputs\n\n";
                    hlsl += L"    return color0;\n";
                }
            }
            else
            {
                hlsl += L"    // Your code here (no inputs)\n";
                hlsl += L"    return float4(0, 0, 0, 1);\n";
            }
            hlsl += L"}\n";
        }
        else
        {
            // Compute shader.
            for (uint32_t i = 0; i < def.inputNames.size(); ++i)
            {
                hlsl += std::format(L"Texture2D {} : register(t{});\n", def.inputNames[i], i);
            }
            hlsl += L"RWTexture2D<float4> Output : register(u0);\n";
            if (!def.inputNames.empty())
                hlsl += L"SamplerState Sampler0 : register(s0);\n";
            hlsl += L"\n";

            if (def.analysisOutputType == ::ShaderLab::Graph::AnalysisOutputType::Typed)
            {
                // Analysis compute shader: reads entire input, writes summary stats
                // to the output row. Each field occupies a known pixel offset.
                hlsl += L"// Analysis compute shader pattern:\n";
                hlsl += L"// The output texture is used as a data buffer.\n";
                hlsl += L"// Write results to Output[int2(pixelOffset, 0)].\n";
                hlsl += L"// The host reads typed fields back after evaluation.\n\n";

                hlsl += L"cbuffer Constants : register(b0)\n{\n";
                hlsl += L"    int2  _TileOffset;  // Auto-injected: tile origin in full image\n";
                for (const auto& p : def.parameters)
                    hlsl += L"    " + p.typeName + L" " + p.name + L";\n";
                hlsl += L"};\n\n";

                // Generate field pixel offset comments.
                uint32_t pixOff = 0;
                for (const auto& fd : def.analysisFields)
                {
                    std::wstring typeTag;
                    switch (fd.type)
                    {
                    case ::ShaderLab::Graph::AnalysisFieldType::Float:       typeTag = L"float"; break;
                    case ::ShaderLab::Graph::AnalysisFieldType::Float2:      typeTag = L"float2"; break;
                    case ::ShaderLab::Graph::AnalysisFieldType::Float3:      typeTag = L"float3"; break;
                    case ::ShaderLab::Graph::AnalysisFieldType::Float4:      typeTag = L"float4"; break;
                    case ::ShaderLab::Graph::AnalysisFieldType::FloatArray:   typeTag = L"float[]"; break;
                    case ::ShaderLab::Graph::AnalysisFieldType::Float2Array:  typeTag = L"float2[]"; break;
                    case ::ShaderLab::Graph::AnalysisFieldType::Float3Array:  typeTag = L"float3[]"; break;
                    case ::ShaderLab::Graph::AnalysisFieldType::Float4Array:  typeTag = L"float4[]"; break;
                    }
                    hlsl += std::format(L"// pixel {}: {} {} ({})\n",
                        pixOff, typeTag, fd.name,
                        ::ShaderLab::Graph::AnalysisFieldIsArray(fd.type)
                            ? std::format(L"{} pixels", fd.pixelCount())
                            : L"1 pixel");
                    pixOff += fd.pixelCount();
                }
                hlsl += L"\n";

                hlsl += std::format(L"[numthreads({}, {}, {})]\n",
                    def.threadGroupX, def.threadGroupY, def.threadGroupZ);
                hlsl += L"void main(uint3 DTid : SV_DispatchThreadID)\n";
                hlsl += L"{\n";

                if (!def.inputNames.empty())
                {
                    hlsl += L"    // Get full source dimensions and compute global position.\n";
                    hlsl += L"    uint srcW, srcH;\n";
                    hlsl += std::format(L"    {}.GetDimensions(srcW, srcH);\n", def.inputNames[0]);
                    hlsl += L"    uint2 globalPos = DTid.xy + uint2(_TileOffset);\n";
                    hlsl += L"    if (globalPos.x >= srcW || globalPos.y >= srcH) return;\n\n";
                    hlsl += L"    // Sample input using normalized UVs (Load() not available in D2D compute).\n";
                    hlsl += L"    float2 uv = (float2(globalPos) + 0.5) / float2(srcW, srcH);\n";
                    hlsl += std::format(L"    float4 pixel = {}.SampleLevel(Sampler0, uv, 0);\n\n", def.inputNames[0]);
                }

                hlsl += L"    // TODO: Accumulate statistics across pixels.\n";
                hlsl += L"    // For whole-image statistics, use groupshared memory and atomic ops,\n";
                hlsl += L"    // or output per-pixel data and reduce on the CPU.\n\n";

                hlsl += L"    // Write results to known pixel locations.\n";
                pixOff = 0;
                for (const auto& fd : def.analysisFields)
                {
                    hlsl += std::format(L"    // Output[int2({}, 0)] = ...; // {}\n", pixOff, fd.name);
                    pixOff += fd.pixelCount();
                }
                if (def.analysisFields.empty())
                    hlsl += L"    // Output[int2(0, 0)] = float4(result, 0, 0, 0);\n";

                hlsl += L"}\n";
            }
            else
            {
                // Image-processing compute shader (passthrough scaffold).
                hlsl += L"// Image-processing compute shader:\n";
                hlsl += L"// D2D evaluates compute effects in tiles. _TileOffset is auto-injected\n";
                hlsl += L"// per tile so the shader sees correct global coordinates.\n";
                hlsl += L"// Use Source.GetDimensions() for the full image size.\n";
                hlsl += L"// Use SampleLevel() with normalized UVs (Load() not available in D2D compute).\n\n";

                hlsl += L"cbuffer Constants : register(b0)\n{\n";
                hlsl += L"    int2  _TileOffset;  // Auto-injected: tile origin in full image\n";
                for (const auto& p : def.parameters)
                    hlsl += L"    " + p.typeName + L" " + p.name + L";\n";
                hlsl += L"};\n\n";

                hlsl += std::format(L"[numthreads({}, {}, {})]\n",
                    def.threadGroupX, def.threadGroupY, def.threadGroupZ);
                hlsl += L"void main(uint3 DTid : SV_DispatchThreadID)\n";
                hlsl += L"{\n";
                if (!def.inputNames.empty())
                {
                    hlsl += L"    // Get full source dimensions and compute global position.\n";
                    hlsl += L"    uint srcW, srcH;\n";
                    hlsl += std::format(L"    {}.GetDimensions(srcW, srcH);\n", def.inputNames[0]);
                    hlsl += L"    uint2 globalPos = DTid.xy + uint2(_TileOffset);\n";
                    hlsl += L"    if (globalPos.x >= srcW || globalPos.y >= srcH) return;\n\n";
                    hlsl += L"    // Sample input using normalized UVs.\n";
                    hlsl += L"    float2 uv = (float2(globalPos) + 0.5) / float2(srcW, srcH);\n";
                    hlsl += std::format(L"    float4 color = {}.SampleLevel(Sampler0, uv, 0);\n", def.inputNames[0]);
                    hlsl += L"\n    // Your code here\n\n";
                    hlsl += L"    Output[DTid.xy] = color;\n";
                }
                else
                {
                    hlsl += L"    // Your code here\n";
                    hlsl += L"    Output[DTid.xy] = float4(0, 0, 0, 1);\n";
                }
                hlsl += L"}\n";
            }
        }

        return hlsl;
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

        auto result = ::ShaderLab::Effects::ShaderCompiler::CompileFromString(
            hlslUtf8, "EffectDesigner", winrt::to_string(entryPoint), winrt::to_string(target));
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
        auto hlsl = GenerateHlsl(def);
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

        // Build the node.
        ::ShaderLab::Graph::EffectNode node;
        node.type = (def.shaderType == ::ShaderLab::Graph::CustomShaderType::PixelShader)
            ? ::ShaderLab::Graph::NodeType::PixelShader
            : ::ShaderLab::Graph::NodeType::ComputeShader;
        node.name = std::wstring(EffectNameBox().Text());
        if (node.name.empty()) node.name = L"Custom Effect";

        // Configure pins from inputs.
        for (uint32_t i = 0; i < def.inputNames.size(); ++i)
            node.inputPins.push_back({ def.inputNames[i], i });
        node.outputPins.push_back({ L"Output", 0 });

        // Set custom effect CLSID based on shader type.
        node.effectClsid = (def.shaderType == ::ShaderLab::Graph::CustomShaderType::PixelShader)
            ? ::ShaderLab::Effects::CustomPixelShaderEffect::CLSID_CustomPixelShader
            : ::ShaderLab::Effects::CustomComputeShaderEffect::CLSID_CustomComputeShader;

        // Copy parameters as node properties with defaults.
        for (const auto& p : def.parameters)
            node.properties[p.name] = p.defaultValue;

        node.customEffect = std::move(def);

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
        // Preserve the existing shader GUID.
        if (def.shaderGuid == GUID{})
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
            def.shaderType == ::ShaderLab::Graph::CustomShaderType::ComputeShader ? 1 : 0);

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
        if (def.shaderType == ::ShaderLab::Graph::CustomShaderType::ComputeShader)
        {
            ThreadGroupX().Value(def.threadGroupX);
            ThreadGroupY().Value(def.threadGroupY);
            ThreadGroupZ().Value(def.threadGroupZ);
        }

        // HLSL source — format for readability.
        HlslEditorBox().Text(winrt::hstring(FormatHlsl(def.hlslSource)));

        // Update button states.
        UpdateInGraphButton().Visibility(Visibility::Visible);
        UpdateInGraphButton().IsEnabled(!def.compiledBytecode.empty());
        AddToGraphButton().IsEnabled(!def.compiledBytecode.empty());

        CompileStatusText().Text(def.compiledBytecode.empty()
            ? L"Loaded -- not yet compiled"
            : L"V Loaded -- compiled and ready");
    }
}
