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
            // Texture inputs + sampler.
            for (uint32_t i = 0; i < def.inputNames.size(); ++i)
            {
                hlsl += std::format(L"Texture2D {} : register(t{});\n", def.inputNames[i], i);
            }
            if (!def.inputNames.empty())
            {
                hlsl += L"SamplerState Sampler0 : register(s0);\n";
                hlsl += L"\n// D2D provides TEXCOORD in pixel/scene space.\n";
                hlsl += L"// Normalize with GetDimensions() before sampling.\n";
                hlsl += L"// D2D's sampler uses CLAMP addressing (edge color for out-of-bounds).\n\n";
            }

            // Entry point with per-input TEXCOORD semantics.
            hlsl += L"float4 main(\n";
            hlsl += L"    float4 pos : SV_POSITION";
            for (uint32_t i = 0; i < def.inputNames.size(); ++i)
            {
                hlsl += std::format(L",\n    float4 uv{} : TEXCOORD{}", i, i);
            }
            hlsl += L") : SV_TARGET\n";
            hlsl += L"{\n";

            if (def.inputNames.size() >= 1)
            {
                for (uint32_t i = 0; i < def.inputNames.size(); ++i)
                {
                    hlsl += std::format(L"    float {0}w, {0}h;\n", def.inputNames[i]);
                    hlsl += std::format(L"    {0}.GetDimensions({0}w, {0}h);\n", def.inputNames[i]);
                    hlsl += std::format(L"    float4 color{0} = {1}.SampleLevel(Sampler0, uv{0}.xy / float2({1}w, {1}h), 0);\n\n",
                        i, def.inputNames[i]);
                }

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
            hlsl += L"RWTexture2D<float4> OutputTexture : register(u0);\n";
            if (!def.inputNames.empty())
                hlsl += L"SamplerState Sampler0 : register(s0);\n";
            hlsl += L"\n";

            hlsl += std::format(L"[numthreads({}, {}, {})]\n",
                def.threadGroupX, def.threadGroupY, def.threadGroupZ);
            hlsl += L"void main(uint3 DTid : SV_DispatchThreadID)\n";
            hlsl += L"{\n";
            hlsl += L"    uint width, height;\n";
            hlsl += L"    OutputTexture.GetDimensions(width, height);\n";
            hlsl += L"    if (DTid.x >= width || DTid.y >= height) return;\n\n";
            if (!def.inputNames.empty())
            {
                hlsl += L"    float2 uv = float2(DTid.xy) / float2(width, height);\n";
                hlsl += L"    float4 color = " + def.inputNames[0] + L".SampleLevel(Sampler0, uv, 0);\n";
                hlsl += L"\n    // Your code here\n\n";
                hlsl += L"    OutputTexture[DTid.xy] = color;\n";
            }
            else
            {
                hlsl += L"    // Your code here\n";
                hlsl += L"    OutputTexture[DTid.xy] = float4(0, 0, 0, 1);\n";
            }
            hlsl += L"}\n";
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
            // Parameter row layout: [NameBox, TypeCombo, DefaultBox, MinBox, MaxBox, DeleteBtn]
            // or with vector types there are multiple default boxes.
            // Find NumberBoxes by iterating children.
            std::vector<Controls::NumberBox> numberBoxes;
            for (uint32_t ci = 0; ci < lastRow.Children().Size(); ++ci)
            {
                auto nb = lastRow.Children().GetAt(ci).try_as<Controls::NumberBox>();
                if (nb) numberBoxes.push_back(nb);
            }

            // Set default value from the parameter definition.
            if (!numberBoxes.empty())
            {
                std::visit([&numberBoxes](const auto& v)
                {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, float>)
                    {
                        if (numberBoxes.size() >= 1) numberBoxes[0].Value(v);
                    }
                    else if constexpr (std::is_same_v<T, int32_t>)
                    {
                        if (numberBoxes.size() >= 1) numberBoxes[0].Value(static_cast<double>(v));
                    }
                    else if constexpr (std::is_same_v<T, uint32_t>)
                    {
                        if (numberBoxes.size() >= 1) numberBoxes[0].Value(static_cast<double>(v));
                    }
                    else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>)
                    {
                        if (numberBoxes.size() >= 1) numberBoxes[0].Value(v.x);
                        if (numberBoxes.size() >= 2) numberBoxes[1].Value(v.y);
                    }
                    else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>)
                    {
                        if (numberBoxes.size() >= 1) numberBoxes[0].Value(v.x);
                        if (numberBoxes.size() >= 2) numberBoxes[1].Value(v.y);
                        if (numberBoxes.size() >= 3) numberBoxes[2].Value(v.z);
                    }
                    else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>)
                    {
                        if (numberBoxes.size() >= 1) numberBoxes[0].Value(v.x);
                        if (numberBoxes.size() >= 2) numberBoxes[1].Value(v.y);
                        if (numberBoxes.size() >= 3) numberBoxes[2].Value(v.z);
                        if (numberBoxes.size() >= 4) numberBoxes[3].Value(v.w);
                    }
                }, p.defaultValue);

                // Min and max are the last two NumberBoxes in the row.
                size_t nb_count = numberBoxes.size();
                if (nb_count >= 3)
                {
                    numberBoxes[nb_count - 2].Value(p.minValue);
                    numberBoxes[nb_count - 1].Value(p.maxValue);
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
