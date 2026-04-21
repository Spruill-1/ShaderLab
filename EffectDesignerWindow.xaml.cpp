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
        AddToGraphButton().Click({ this, &EffectDesignerWindow::OnAddToGraph });
        UpdateInGraphButton().Click({ this, &EffectDesignerWindow::OnUpdateInGraph });
        ShaderTypeSelector().SelectionChanged({ this, &EffectDesignerWindow::OnShaderTypeChanged });
        AddInputButton().Click({ this, &EffectDesignerWindow::OnAddInput });
        AddParamButton().Click({ this, &EffectDesignerWindow::OnAddParam });

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

        auto row = Controls::StackPanel();
        row.Orientation(Controls::Orientation::Horizontal);
        row.Spacing(4);

        auto nameBox = Controls::TextBox();
        nameBox.PlaceholderText(L"Name");
        nameBox.MinWidth(120);
        row.Children().Append(nameBox);

        auto typeCombo = Controls::ComboBox();
        typeCombo.Items().Append(box_value(L"float"));
        typeCombo.Items().Append(box_value(L"float2"));
        typeCombo.Items().Append(box_value(L"float3"));
        typeCombo.Items().Append(box_value(L"float4"));
        typeCombo.Items().Append(box_value(L"int"));
        typeCombo.Items().Append(box_value(L"uint"));
        typeCombo.Items().Append(box_value(L"bool"));
        typeCombo.SelectedIndex(0);
        typeCombo.MinWidth(90);
        row.Children().Append(typeCombo);

        // Default value container -- rebuilds when type changes.
        auto defContainer = Controls::StackPanel();
        defContainer.Orientation(Controls::Orientation::Horizontal);
        defContainer.Spacing(2);
        row.Children().Append(defContainer);

        // Helper to rebuild default fields based on type.
        auto rebuildDefaults = [defContainer](int typeIdx)
        {
            defContainer.Children().Clear();
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
                defContainer.Children().Append(nb);
            }
        };
        rebuildDefaults(0);  // Initial: float = 1 box.

        typeCombo.SelectionChanged([rebuildDefaults](auto&& sender, auto&&)
        {
            auto combo = sender.template as<Controls::ComboBox>();
            rebuildDefaults(combo.SelectedIndex());
        });

        auto minBox = Controls::NumberBox();
        minBox.Header(box_value(L"Min"));
        minBox.Value(0.0);
        minBox.Width(80);
        minBox.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Compact);
        row.Children().Append(minBox);

        auto maxBox = Controls::NumberBox();
        maxBox.Header(box_value(L"Max"));
        maxBox.Value(1.0);
        maxBox.Width(80);
        maxBox.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Compact);
        row.Children().Append(maxBox);

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
            auto row = paramPanel.Children().GetAt(i).as<Controls::StackPanel>();
            if (row.Children().Size() < 6) continue;

            auto nameBox = row.Children().GetAt(0).as<Controls::TextBox>();
            auto typeCombo = row.Children().GetAt(1).as<Controls::ComboBox>();
            auto defContainer = row.Children().GetAt(2).as<Controls::StackPanel>();
            auto minBox = row.Children().GetAt(3).as<Controls::NumberBox>();
            auto maxBox = row.Children().GetAt(4).as<Controls::NumberBox>();

            ::ShaderLab::Graph::ParameterDefinition param;
            param.name = std::wstring(nameBox.Text());
            param.typeName = std::wstring(unbox_value<hstring>(typeCombo.SelectedItem()));

            param.minValue = static_cast<float>(minBox.Value());
            param.maxValue = static_cast<float>(maxBox.Value());

            // Read default values from the NumberBox(es) in the container.
            uint32_t numBoxes = defContainer.Children().Size();
            if (param.typeName == L"float" && numBoxes >= 1)
            {
                param.defaultValue = static_cast<float>(
                    defContainer.Children().GetAt(0).as<Controls::NumberBox>().Value());
            }
            else if (param.typeName == L"float2" && numBoxes >= 2)
            {
                namespace Num = winrt::Windows::Foundation::Numerics;
                param.defaultValue = Num::float2{
                    static_cast<float>(defContainer.Children().GetAt(0).as<Controls::NumberBox>().Value()),
                    static_cast<float>(defContainer.Children().GetAt(1).as<Controls::NumberBox>().Value()) };
            }
            else if (param.typeName == L"float3" && numBoxes >= 3)
            {
                namespace Num = winrt::Windows::Foundation::Numerics;
                param.defaultValue = Num::float3{
                    static_cast<float>(defContainer.Children().GetAt(0).as<Controls::NumberBox>().Value()),
                    static_cast<float>(defContainer.Children().GetAt(1).as<Controls::NumberBox>().Value()),
                    static_cast<float>(defContainer.Children().GetAt(2).as<Controls::NumberBox>().Value()) };
            }
            else if (param.typeName == L"float4" && numBoxes >= 4)
            {
                namespace Num = winrt::Windows::Foundation::Numerics;
                param.defaultValue = Num::float4{
                    static_cast<float>(defContainer.Children().GetAt(0).as<Controls::NumberBox>().Value()),
                    static_cast<float>(defContainer.Children().GetAt(1).as<Controls::NumberBox>().Value()),
                    static_cast<float>(defContainer.Children().GetAt(2).as<Controls::NumberBox>().Value()),
                    static_cast<float>(defContainer.Children().GetAt(3).as<Controls::NumberBox>().Value()) };
            }
            else if (param.typeName == L"int" && numBoxes >= 1)
            {
                param.defaultValue = static_cast<int32_t>(
                    defContainer.Children().GetAt(0).as<Controls::NumberBox>().Value());
            }
            else if (param.typeName == L"uint" && numBoxes >= 1)
            {
                param.defaultValue = static_cast<uint32_t>(
                    defContainer.Children().GetAt(0).as<Controls::NumberBox>().Value());
            }
            else if (param.typeName == L"bool" && numBoxes >= 1)
            {
                param.defaultValue = defContainer.Children().GetAt(0).as<Controls::NumberBox>().Value() != 0.0;
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

            if (def.analysisOutputType == ::ShaderLab::Graph::AnalysisOutputType::KeyValue)
            {
                // Analysis compute shader: reads entire input, writes summary stats
                // to the first N pixels of the output row.
                hlsl += L"// Analysis compute shader pattern:\n";
                hlsl += L"// The output texture is used as a data buffer.\n";
                hlsl += L"// Write float4 results to Output[int2(fieldIndex, 0)].\n";
                hlsl += L"// The host reads these pixels back after evaluation.\n\n";

                if (!def.parameters.empty())
                {
                    hlsl += L"cbuffer Constants : register(b0)\n{\n";
                    for (const auto& p : def.parameters)
                        hlsl += L"    " + p.typeName + L" " + p.name + L";\n";
                    hlsl += L"};\n\n";
                }

                // Generate field index comments.
                for (size_t i = 0; i < def.analysisFieldNames.size(); ++i)
                    hlsl += std::format(L"// Output[int2({}, 0)] = {}\n", i, def.analysisFieldNames[i]);
                hlsl += L"\n";

                hlsl += std::format(L"[numthreads({}, {}, {})]\n",
                    def.threadGroupX, def.threadGroupY, def.threadGroupZ);
                hlsl += L"void main(uint3 DTid : SV_DispatchThreadID)\n";
                hlsl += L"{\n";

                if (!def.inputNames.empty())
                {
                    hlsl += L"    // Get input dimensions.\n";
                    hlsl += L"    uint inW, inH;\n";
                    hlsl += std::format(L"    {}.GetDimensions(inW, inH);\n\n", def.inputNames[0]);
                    hlsl += L"    // Each thread processes one pixel of the input.\n";
                    hlsl += L"    if (DTid.x >= inW || DTid.y >= inH) return;\n\n";
                    hlsl += std::format(L"    float4 pixel = {}.Load(int3(DTid.xy, 0));\n\n", def.inputNames[0]);
                }

                hlsl += L"    // TODO: Accumulate statistics across pixels.\n";
                hlsl += L"    // Note: Compute shaders cannot easily reduce across threads without\n";
                hlsl += L"    // shared memory atomics. For now, this scaffold processes per-pixel.\n";
                hlsl += L"    // For whole-image statistics, use groupshared memory and atomic ops,\n";
                hlsl += L"    // or output per-pixel data and reduce on the CPU.\n\n";

                hlsl += L"    // Write results to known pixel locations.\n";
                for (size_t i = 0; i < def.analysisFieldNames.size(); ++i)
                    hlsl += std::format(L"    // Output[int2({}, 0)] = ...; // {}\n", i, def.analysisFieldNames[i]);
                if (def.analysisFieldNames.empty())
                    hlsl += L"    // Output[int2(0, 0)] = float4(result, 0, 0, 0);\n";

                hlsl += L"}\n";
            }
            else
            {
                // Image-processing compute shader (passthrough scaffold).
                hlsl += L"// Image-processing compute shader:\n";
                hlsl += L"// Each thread processes one output pixel.\n\n";

                if (!def.parameters.empty())
                {
                    hlsl += L"cbuffer Constants : register(b0)\n{\n";
                    for (const auto& p : def.parameters)
                        hlsl += L"    " + p.typeName + L" " + p.name + L";\n";
                    hlsl += L"};\n\n";
                }

                hlsl += std::format(L"[numthreads({}, {}, {})]\n",
                    def.threadGroupX, def.threadGroupY, def.threadGroupZ);
                hlsl += L"void main(uint3 DTid : SV_DispatchThreadID)\n";
                hlsl += L"{\n";
                hlsl += L"    uint width, height;\n";
                hlsl += L"    Output.GetDimensions(width, height);\n";
                hlsl += L"    if (DTid.x >= width || DTid.y >= height) return;\n\n";
                if (!def.inputNames.empty())
                {
                    hlsl += std::format(L"    float4 color = {}.Load(int3(DTid.xy, 0));\n", def.inputNames[0]);
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

    void EffectDesignerWindow::OnGenerateScaffold(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto def = BuildDefinition();
        auto hlsl = GenerateHlsl(def);
        HlslEditorBox().Text(winrt::hstring(hlsl));
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
            auto lastRow = ParamsPanel().Children().GetAt(ParamsPanel().Children().Size() - 1)
                .as<Controls::StackPanel>();
            lastRow.Children().GetAt(0).as<Controls::TextBox>().Text(winrt::hstring(p.name));
            // Set type combo.
            auto combo = lastRow.Children().GetAt(1).as<Controls::ComboBox>();
            for (int32_t i = 0; i < static_cast<int32_t>(combo.Items().Size()); ++i)
            {
                if (unbox_value<hstring>(combo.Items().GetAt(i)) == p.typeName)
                {
                    combo.SelectedIndex(i);
                    break;
                }
            }

            // Restore default/min/max values.
            // Row layout: [NameBox, TypeCombo, DefContainer(StackPanel), MinBox, MaxBox, DeleteBtn]
            // Default NumberBoxes are INSIDE DefContainer, not direct row children.
            
            // Find DefContainer (the StackPanel child of the row).
            Controls::StackPanel defContainer{ nullptr };
            for (uint32_t ci = 0; ci < lastRow.Children().Size(); ++ci)
            {
                auto sp = lastRow.Children().GetAt(ci).try_as<Controls::StackPanel>();
                if (sp) { defContainer = sp; break; }
            }

            // Set default value in DefContainer's NumberBoxes.
            if (defContainer)
            {
                std::vector<Controls::NumberBox> defBoxes;
                for (uint32_t ci = 0; ci < defContainer.Children().Size(); ++ci)
                {
                    auto nb = defContainer.Children().GetAt(ci).try_as<Controls::NumberBox>();
                    if (nb) defBoxes.push_back(nb);
                }

                std::visit([&defBoxes](const auto& v)
                {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, float>)
                    {
                        if (defBoxes.size() >= 1) defBoxes[0].Value(v);
                    }
                    else if constexpr (std::is_same_v<T, int32_t>)
                    {
                        if (defBoxes.size() >= 1) defBoxes[0].Value(static_cast<double>(v));
                    }
                    else if constexpr (std::is_same_v<T, uint32_t>)
                    {
                        if (defBoxes.size() >= 1) defBoxes[0].Value(static_cast<double>(v));
                    }
                    else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>)
                    {
                        if (defBoxes.size() >= 1) defBoxes[0].Value(v.x);
                        if (defBoxes.size() >= 2) defBoxes[1].Value(v.y);
                    }
                    else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>)
                    {
                        if (defBoxes.size() >= 1) defBoxes[0].Value(v.x);
                        if (defBoxes.size() >= 2) defBoxes[1].Value(v.y);
                        if (defBoxes.size() >= 3) defBoxes[2].Value(v.z);
                    }
                    else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>)
                    {
                        if (defBoxes.size() >= 1) defBoxes[0].Value(v.x);
                        if (defBoxes.size() >= 2) defBoxes[1].Value(v.y);
                        if (defBoxes.size() >= 3) defBoxes[2].Value(v.z);
                        if (defBoxes.size() >= 4) defBoxes[3].Value(v.w);
                    }
                }, p.defaultValue);
            }

            // Set Min and Max (direct NumberBox children of row, after DefContainer).
            std::vector<Controls::NumberBox> rowBoxes;
            for (uint32_t ci = 0; ci < lastRow.Children().Size(); ++ci)
            {
                auto nb = lastRow.Children().GetAt(ci).try_as<Controls::NumberBox>();
                if (nb) rowBoxes.push_back(nb);
            }
            // rowBoxes = [MinBox, MaxBox]
            if (rowBoxes.size() >= 2)
            {
                rowBoxes[0].Value(p.minValue);
                rowBoxes[1].Value(p.maxValue);
            }
        }

        // Compute settings.
        if (def.shaderType == ::ShaderLab::Graph::CustomShaderType::ComputeShader)
        {
            ThreadGroupX().Value(def.threadGroupX);
            ThreadGroupY().Value(def.threadGroupY);
            ThreadGroupZ().Value(def.threadGroupZ);
        }

        // HLSL source.
        HlslEditorBox().Text(winrt::hstring(def.hlslSource));

        // Update button states.
        UpdateInGraphButton().Visibility(Visibility::Visible);
        UpdateInGraphButton().IsEnabled(!def.compiledBytecode.empty());
        AddToGraphButton().IsEnabled(!def.compiledBytecode.empty());

        CompileStatusText().Text(def.compiledBytecode.empty()
            ? L"Loaded -- not yet compiled"
            : L"V Loaded -- compiled and ready");
    }
}
