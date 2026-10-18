#include "pch.h"
#include "PropertiesPanelController.h"

#include "CurveEditorDialog.h"
#include "DialogCoordinator.h"
#include "PropertyEditors.h"
#include "Effects/EffectRegistry.h"
#include "Effects/EffectUpgrade.h"
#include "Effects/ImageLoader.h"
#include "Effects/ShaderLabEffects.h"
#include "Effects/SourceNodeFactory.h"
#include "Graph/GraphModel.h"
#include "Graph/GraphEdits.h"
#include "../Graph/ParameterVisibility.h"
#include "Graph/SourceKind.h"
#include "Graph/ValueFormat.h"

namespace ShaderLab::Controls
{
    namespace
    {
        namespace Xaml = winrt::Microsoft::UI::Xaml;
        namespace XC = winrt::Microsoft::UI::Xaml::Controls;
        namespace Media = winrt::Microsoft::UI::Xaml::Media;
        using winrt::Microsoft::UI::Colors;
        using winrt::Windows::Foundation::Numerics::float2;
        using winrt::Windows::Foundation::Numerics::float3;
        using winrt::Windows::Foundation::Numerics::float4;

        Media::SolidColorBrush Brush(winrt::Windows::UI::Color color) { return Media::SolidColorBrush(color); }

        XC::TextBlock SectionHeader(const wchar_t* text, Xaml::Thickness margin)
        {
            auto header = XC::TextBlock();
            header.Text(text);
            header.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            header.Margin(margin);
            return header;
        }

        XC::Button WideButton(const wchar_t* content, Xaml::Thickness margin)
        {
            auto button = XC::Button();
            button.Content(winrt::box_value(content));
            button.HorizontalAlignment(Xaml::HorizontalAlignment::Stretch);
            button.Margin(margin);
            return button;
        }

        // A rounded coloured box holding `child`.
        XC::Border Banner(winrt::Windows::UI::Color background, Xaml::Thickness margin, Xaml::UIElement const& child)
        {
            auto border = XC::Border();
            border.Background(Brush(background));
            border.CornerRadius({ 4, 4, 4, 4 });
            border.Padding({ 8, 6, 8, 6 });
            border.Margin(margin);
            border.Child(child);
            return border;
        }

        bool IsExpressionNode(const Graph::EffectNode& node)
        {
            return node.customEffect.has_value() && node.customEffect->nodeKind == Graph::NodeKind::Expression;
        }

        bool IsBuiltIn(const Graph::EffectNode& node, const GUID& clsid)
        {
            return node.effectClsid.has_value() && IsEqualGUID(node.effectClsid.value(), clsid);
        }

        std::optional<float> FloatProperty(const Graph::EffectNode& node, const std::wstring& key)
        {
            auto it = node.properties.find(key);
            if (it == node.properties.end()) return std::nullopt;
            if (auto* f = std::get_if<float>(&it->second)) return *f;
            return std::nullopt;
        }

        // Properties the video controls own, not shown as editors.
        bool IsVideoControlProperty(const std::wstring& key)
        {
            return key == Graph::cVideoMarker || key == L"IsPlaying" || key == L"PlaybackSpeed" || key == L"Loop";
        }

        uint32_t ComponentCount(const Graph::PropertyValue& value)
        {
            if (std::holds_alternative<float2>(value)) return 2;
            if (std::holds_alternative<float3>(value)) return 3;
            if (std::holds_alternative<float4>(value)) return 4;
            return 1;
        }

        constexpr const wchar_t* scComponentSuffixes[] = { L".x", L".y", L".z", L".w" };
    }

    void PropertiesPanelController::Attach(XC::StackPanel panel, PropertiesPanelHost host)
    {
        m_panel = std::move(panel);
        m_host = std::move(host);
    }

    bool PropertiesPanelController::EditGraph(const std::function<bool(PanelGraphEdit&)>& edit)
    {
        return m_host.editGraph && m_host.editGraph(edit);
    }

    bool PropertiesPanelController::IsInteracting() const
    {
        // Walks up from the focused element looking for the panel.
        try
        {
            auto root = m_panel ? m_panel.XamlRoot() : nullptr;
            if (!root) return false;
            auto focused = Xaml::Input::FocusManager::GetFocusedElement(root);
            auto current = focused.try_as<Xaml::DependencyObject>();
            auto target = m_panel.try_as<Xaml::DependencyObject>();
            if (!current || !target) return false;
            for (int hops = 0; hops < 64 && current; ++hops)
            {
                if (current == target) return true;
                current = Media::VisualTreeHelper::GetParent(current);
            }
        }
        catch (...)
        {
            // A failed focus probe only delays a live-value refresh: report not editing.
        }
        return false;
    }

    void PropertiesPanelController::RefreshBoundValues()
    {
        const auto snapshot = m_host.snapshot();
        const auto* node = snapshot ? snapshot->FindNode(m_host.selectedNodeId()) : nullptr;
        if (node && !node->propertyBindings.empty() && !IsInteracting())
            Rebuild();
    }

    void PropertiesPanelController::Rebuild()
    {
        if (!m_panel) return;
        auto children = m_panel.Children();
        children.Clear();

        const uint32_t nodeId = m_host.selectedNodeId();
        if (nodeId == 0)
        {
            auto placeholder = XC::TextBlock();
            placeholder.Text(L"Select a node to view its properties.");
            placeholder.Foreground(Brush(Colors::Gray()));
            children.Append(placeholder);
            return;
        }

        // Built from the published snapshot, held for the whole rebuild.
        const auto snapshot = m_host.snapshot();
        const auto* node = snapshot ? snapshot->FindNode(nodeId) : nullptr;
        if (!node) return;

        AppendHeader(children, *node);
        AppendErrorBanner(children, *node);
        AppendImageSource(children, *node);
        AppendVideoSource(children, *snapshot, *node);
        AppendProperties(children, *snapshot, *node);
        AppendGammaCurvePreview(children, *node);
        AppendHistogram(children, *node);
        AppendAnalysisResults(children, *node);
        AppendInputCount(children, *node);
        AppendUpgradeBanner(children, *node);
        AppendFooter(children, *node);
    }

    void PropertiesPanelController::AppendHeader(Children const& children, const Graph::EffectNode& node)
    {
        const uint32_t nodeId = node.id;

        auto nameLabel = XC::TextBlock();
        nameLabel.Text(L"Name");
        nameLabel.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        children.Append(nameLabel);

        // The name is written as it is typed; the canvas and the preview list
        // pick it up when the box loses focus.
        auto nameBox = XC::TextBox();
        nameBox.Text(winrt::hstring(node.name));
        nameBox.Margin({ 0, 2, 0, 8 });
        nameBox.LostFocus([this, nodeId](auto&&, auto&&)
        {
            const bool exists = EditGraph([nodeId](PanelGraphEdit& e)
            {
                if (!e.graph.FindNode(nodeId)) return false;
                e.rebuildLayout();
                return true;
            });
            if (exists && m_host.refreshNodeList)
                m_host.refreshNodeList();
        });
        nameBox.TextChanged([this, nodeId](auto&& sender, auto&&)
        {
            std::wstring newName(sender.template as<XC::TextBox>().Text().c_str());
            EditGraph([nodeId, &newName](PanelGraphEdit& e)
            {
                return ::ShaderLab::Graph::RenameNode(e.graph, nodeId, std::move(newName));
            });
        });
        children.Append(nameBox);

        const wchar_t* typeName = L"";
        switch (node.type)
        {
        case Graph::NodeType::Source:        typeName = L"Source"; break;
        case Graph::NodeType::BuiltInEffect: typeName = L"Built-in Effect"; break;
        case Graph::NodeType::PixelShader:   typeName = L"Custom Pixel Shader"; break;
        case Graph::NodeType::ComputeShader: typeName = L"Custom Compute Shader"; break;
        case Graph::NodeType::Output:        typeName = L"Output"; break;
        }
        auto typeText = XC::TextBlock();
        typeText.Text(winrt::hstring(L"Type: ") + typeName);
        typeText.Foreground(Brush(Colors::Gray()));
        typeText.Margin({ 0, 0, 0, 8 });
        children.Append(typeText);

        // Shader nodes with HLSL open in the Effect Designer; parameter nodes have none.
        const bool isShader = node.type == Graph::NodeType::PixelShader || node.type == Graph::NodeType::ComputeShader;
        if (isShader && node.customEffect.has_value() && !node.customEffect->hlslSource.empty())
        {
            auto editButton = WideButton(L"\xE70F  Edit in Effect Designer", { 0, 0, 0, 8 });
            editButton.Click([this, nodeId](auto&&, auto&&)
            {
                const auto snapshot = m_host.snapshot();
                const auto* n = snapshot ? snapshot->FindNode(nodeId) : nullptr;
                if (!n || !n->customEffect.has_value()) return;
                m_host.openInDesigner(nodeId, n->customEffect.value(), n->name);
            });
            children.Append(editButton);
        }
    }

    void PropertiesPanelController::AppendErrorBanner(Children const& children, const Graph::EffectNode& node)
    {
        if (node.runtimeError.empty()) return;

        auto errorText = XC::TextBlock();
        errorText.Text(winrt::hstring(node.runtimeError));
        errorText.Foreground(Brush(Colors::White()));
        errorText.TextWrapping(Xaml::TextWrapping::WrapWholeWords);
        errorText.FontSize(12);
        children.Append(Banner(winrt::Windows::UI::Color{ 255, 180, 30, 30 }, { 0, 0, 0, 8 }, errorText));

        // A video that failed can be retried.
        if (Graph::ClassifySource(node) != Graph::SourceKind::Video || !node.mediaPath.has_value())
            return;
        const uint32_t nodeId = node.id;
        auto reloadButton = WideButton(L"\xE72C  Reload Video", { 0, 0, 0, 8 });
        reloadButton.Click([this, nodeId](auto&&, auto&&)
        {
            const bool exists = EditGraph([nodeId](PanelGraphEdit& e)
            {
                return ::ShaderLab::Graph::RetryNode(e.graph, nodeId);
            });
            if (!exists) return;
            m_host.requestRender();
            Rebuild();
        });
        children.Append(reloadButton);
    }

    void PropertiesPanelController::AppendImageSource(Children const& children, const Graph::EffectNode& node)
    {
        // Only file images: a live capture owns its bitmap and has no path.
        const auto kind = Graph::ClassifySource(node);
        if (node.type != Graph::NodeType::Source || node.effectClsid.has_value() ||
            kind == Graph::SourceKind::Video || Graph::IsLiveSource(kind))
            return;

        children.Append(SectionHeader(L"Image Path", { 0, 4, 0, 2 }));

        const bool hasPath = node.mediaPath.has_value() && !node.mediaPath.value().empty();
        auto pathText = XC::TextBlock();
        pathText.Text(hasPath ? winrt::hstring(node.mediaPath.value()) : L"(no file selected)");
        pathText.FontSize(12);
        pathText.IsTextSelectionEnabled(true);
        pathText.TextWrapping(Xaml::TextWrapping::WrapWholeWords);
        pathText.Foreground(Brush(hasPath ? Colors::White() : Colors::Gray()));
        pathText.Margin({ 0, 0, 0, 4 });
        children.Append(pathText);

        const uint32_t nodeId = node.id;
        auto browseButton = WideButton(L"Browse...", { 0, 0, 0, 8 });
        browseButton.Click([this, nodeId](auto&&, auto&&) { BrowseImage(nodeId); });
        children.Append(browseButton);
    }

    void PropertiesPanelController::AppendVideoSource(Children const& children,
        const Graph::GraphUiSnapshot& snapshot, const Graph::EffectNode& node)
    {
        if (Graph::ClassifySource(node) != Graph::SourceKind::Video) return;
        const uint32_t nodeId = node.id;

        children.Append(SectionHeader(L"Video File", { 0, 4, 0, 2 }));

        auto pathText = XC::TextBlock();
        pathText.Text(node.mediaPath.has_value() && !node.mediaPath.value().empty()
            ? winrt::hstring(std::filesystem::path(node.mediaPath.value()).filename().wstring())
            : L"(no file)");
        pathText.FontSize(12);
        pathText.TextWrapping(Xaml::TextWrapping::WrapWholeWords);
        pathText.Margin({ 0, 0, 0, 2 });
        children.Append(pathText);

        auto browseButton = WideButton(L"Browse...", { 0, 0, 0, 6 });
        browseButton.Click([this, nodeId](auto&&, auto&&) { BrowseVideo(nodeId); });
        children.Append(browseButton);

        const auto* video = snapshot.FindVideoSource(nodeId);
        if (!video || !video->open) return;

        auto infoText = XC::TextBlock();
        infoText.Text(std::format(L"{}x{} - {:.1f} fps - {:.1f}s",
            video->width, video->height, video->frameRate, video->duration));
        infoText.FontSize(11);
        infoText.Foreground(Brush(Colors::Gray()));
        infoText.Margin({ 0, 0, 0, 6 });
        children.Append(infoText);

        auto loopToggle = XC::ToggleSwitch();
        loopToggle.Header(winrt::box_value(L"Loop"));
        loopToggle.IsOn(video->loop);
        loopToggle.Margin({ 0, 4, 0, 8 });
        loopToggle.Toggled([this, nodeId](auto&& sender, auto&&)
        {
            const bool loop = sender.template as<XC::ToggleSwitch>().IsOn();
            EditGraph([nodeId, loop](PanelGraphEdit& e)
            {
                return ::ShaderLab::Graph::SetNodeProperty(e.graph, nodeId, L"Loop", loop);
            });
        });
        children.Append(loopToggle);
    }

    PropertiesPanelController::MetadataMap PropertiesPanelController::CustomParameterMetadata(
        const Graph::EffectNode& node)
    {
        // Custom-effect parameters carry their range and labels in the definition.
        MetadataMap metadata;
        if (!node.customEffect.has_value()) return metadata;
        for (const auto& p : node.customEffect->parameters)
        {
            Effects::PropertyMetadata meta;
            if (!p.enumLabels.empty())
            {
                meta.uiHint = Effects::PropertyUIHint::ComboBox;
                meta.enumLabels = p.enumLabels;
            }
            else
            {
                meta.uiHint = Effects::PropertyUIHint::Slider;
            }
            meta.minValue = p.minValue;
            meta.maxValue = p.maxValue;
            meta.step = p.step;
            metadata[p.name] = std::move(meta);
        }
        return metadata;
    }

    void PropertiesPanelController::AppendProperties(Children const& children,
        const Graph::GraphUiSnapshot& snapshot, const Graph::EffectNode& node)
    {
        if (node.properties.empty())
        {
            auto noProperties = XC::TextBlock();
            noProperties.Text(L"No editable properties.");
            noProperties.Foreground(Brush(Colors::Gray()));
            noProperties.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
            noProperties.Margin({ 0, 4, 0, 0 });
            children.Append(noProperties);
            return;
        }

        children.Append(SectionHeader(L"Properties", { 0, 4, 0, 4 }));

        const bool isExpression = IsExpressionNode(node);
        if (isExpression)
            AppendExpressionEditor(children, node);

        // Built-in D2D effects describe their properties in the registry.
        const Effects::EffectDescriptor* descriptor = nullptr;
        if (node.type == Graph::NodeType::BuiltInEffect && node.effectClsid.has_value())
            descriptor = Effects::EffectRegistry::Instance().FindByClsid(node.effectClsid.value());
        const auto customMetadata = CustomParameterMetadata(node);

        for (const auto& [key, value] : node.properties)
        {
            if (key == L"analysisFields" || key == L"propertyBindings") continue;
            if (isExpression && key == L"Expression") continue;   // has its own editor above
            if (!Graph::IsParameterShown(node, key)) continue;
            if (IsVideoControlProperty(key)) continue;

            const Effects::PropertyMetadata* meta = nullptr;
            if (descriptor)
                if (auto it = descriptor->propertyMetadata.find(key); it != descriptor->propertyMetadata.end())
                    meta = &it->second;
            if (!meta)
                if (auto it = customMetadata.find(key); it != customMetadata.end())
                    meta = &it->second;

            AppendProperty(children, snapshot, node, key, value, meta);
        }
    }

    void PropertiesPanelController::AppendExpressionEditor(Children const& children, const Graph::EffectNode& node)
    {
        auto label = XC::TextBlock();
        label.Text(L"Expression");
        label.FontSize(12);
        label.Margin({ 0, 6, 0, 2 });
        children.Append(label);

        std::wstring expression;
        if (auto it = node.properties.find(L"Expression"); it != node.properties.end())
            if (auto* s = std::get_if<std::wstring>(&it->second)) expression = *s;

        auto box = XC::TextBox();
        box.Text(winrt::hstring(expression));
        box.AcceptsReturn(false);
        box.FontFamily(Media::FontFamily(L"Consolas"));
        box.Margin({ 0, 0, 0, 8 });
        const uint32_t nodeId = node.id;
        box.TextChanged([this, nodeId](auto&& sender, auto&&)
        {
            std::wstring text(sender.template as<XC::TextBox>().Text().c_str());
            const bool exists = EditGraph([nodeId, &text](PanelGraphEdit& e)
            {
                return ::ShaderLab::Graph::SetNodeProperty(e.graph, nodeId, L"Expression", std::move(text));
            });
            if (!exists) return;
            m_host.requestRender();
            m_host.requestCanvasRedraw();
        });
        children.Append(box);
    }

    void PropertiesPanelController::AppendProperty(Children const& children,
        const Graph::GraphUiSnapshot& snapshot, const Graph::EffectNode& node,
        const std::wstring& key, const Graph::PropertyValue& value, const Effects::PropertyMetadata* meta)
    {
        const uint32_t nodeId = node.id;

        // The label row: name, then the remove, binding and bind controls.
        auto labelRow = XC::StackPanel();
        labelRow.Orientation(XC::Orientation::Horizontal);
        labelRow.Spacing(6);
        auto label = XC::TextBlock();
        label.Text(winrt::hstring(key));
        label.FontSize(12);
        label.Margin({ 0, 6, 0, 2 });
        labelRow.Children().Append(label);

        // A Numeric Expression input can be removed while another remains.
        if (IsExpressionNode(node) && std::holds_alternative<float>(value) && Graph::CanRemoveExpressionInput(node))
        {
            auto removeButton = XC::Button();
            removeButton.Content(winrt::box_value(L"✕"));
            removeButton.FontSize(10);
            removeButton.MinWidth(24);
            removeButton.Padding({ 4, 0, 4, 0 });
            removeButton.Click([this, nodeId, key](auto&&, auto&&) { RemoveExpressionInput(nodeId, key); });
            labelRow.Children().Append(removeButton);
        }

        const auto bindIt = node.propertyBindings.find(key);
        const bool isBound = bindIt != node.propertyBindings.end();
        if (isBound)
            AppendBindingInfo(labelRow.Children(), snapshot, node, key, value, bindIt->second);
        else
            AppendBindMenu(labelRow.Children(), snapshot, nodeId, key);
        children.Append(labelRow);

        // A bound property shows its live value instead of an editor.
        if (isBound)
        {
            auto boundValue = XC::TextBlock();
            boundValue.Text(winrt::hstring(Graph::FormatPropertyValue(value, 4)));
            boundValue.FontSize(12);
            boundValue.FontFamily(Media::FontFamily(L"Consolas"));
            boundValue.Foreground(Brush(Colors::CornflowerBlue()));
            boundValue.Margin({ 0, 0, 0, 4 });
            children.Append(boundValue);
            return;
        }

        PropertyEditorSpec spec;
        spec.key = key;
        spec.meta = meta;
        // A parameter node's Value slider spans its current Min and Max.
        if (key == L"Value" && node.customEffect.has_value() && node.customEffect->hlslSource.empty())
        {
            spec.minOverride = FloatProperty(node, L"Min");
            spec.maxOverride = FloatProperty(node, L"Max");
        }
        spec.openCurveEditor = [this, nodeId, key] { OpenCurveEditor(nodeId, key); };

        PropertyEditTarget target;
        target.apply = [this, nodeId, key](const std::function<bool(Graph::PropertyValue&)>& edit)
        {
            return EditGraph([&](PanelGraphEdit& e) { return Graph::EditNodeProperty(e.graph, nodeId, key, edit); });
        };
        target.changed = [this, nodeId, key] { OnPropertyChanged(nodeId, key); };

        AppendPropertyEditor(children, value, spec, target);
    }

    void PropertiesPanelController::AppendBindingInfo(Children const& row,
        const Graph::GraphUiSnapshot& snapshot, const Graph::EffectNode& node,
        const std::wstring& key, const Graph::PropertyValue& value, const Graph::PropertyBinding& binding)
    {
        const uint32_t nodeId = node.id;

        // The binding's first source names it: "<- Node.Field".
        uint32_t sourceId = 0;
        std::wstring sourceField;
        uint32_t sourceComponent = 0;
        if (binding.wholeArray)
        {
            sourceId = binding.wholeArraySourceNodeId;
            sourceField = binding.wholeArraySourceFieldName;
        }
        else
        {
            for (const auto& source : binding.sources)
            {
                if (!source.has_value()) continue;
                sourceId = source->sourceNodeId;
                sourceField = source->sourceFieldName;
                sourceComponent = source->sourceComponent;
                break;
            }
        }

        const auto* sourceNode = snapshot.FindNode(sourceId);
        uint32_t sourceComponents = 1;
        if (sourceNode && sourceNode->customEffect.has_value())
        {
            for (const auto& field : sourceNode->customEffect->analysisFields)
            {
                if (field.name != sourceField) continue;
                sourceComponents = Graph::AnalysisFieldComponentCount(field.type);
                break;
            }
        }
        const uint32_t destComponents = ComponentCount(value);

        std::wstring bindLabel = L"← ";
        if (sourceNode) bindLabel += sourceNode->name + L".";
        bindLabel += sourceField;
        auto bindInfo = XC::TextBlock();
        bindInfo.Text(winrt::hstring(bindLabel));
        bindInfo.FontSize(11);
        bindInfo.Foreground(Brush(Colors::CornflowerBlue()));
        row.Append(bindInfo);

        if (sourceComponents > 1 && destComponents == 1)
        {
            // A scalar fed from a vector field picks its component.
            auto picker = XC::ComboBox();
            picker.FontSize(11);
            picker.MinWidth(55);
            picker.Padding({ 4, 0, 4, 0 });
            for (uint32_t c = 0; c < sourceComponents && c < 4; ++c)
                picker.Items().Append(winrt::box_value(winrt::hstring(scComponentSuffixes[c])));
            picker.SelectedIndex(static_cast<int32_t>((std::min)(sourceComponent, sourceComponents - 1)));
            picker.SelectionChanged([this, nodeId, key](winrt::Windows::Foundation::IInspectable const& sender, auto&&)
            {
                const auto component = static_cast<uint32_t>(sender.as<XC::ComboBox>().SelectedIndex());
                EditGraph([nodeId, &key, component](PanelGraphEdit& e)
                {
                    return ::ShaderLab::Graph::SetBindingComponent(e.graph, nodeId, key, component);
                });
            });
            row.Append(picker);
        }
        else if (destComponents == 1 && sourceComponents == 1)
        {
            auto componentText = XC::TextBlock();
            componentText.Text(L".x");
            componentText.FontSize(11);
            componentText.Foreground(Brush(Colors::CornflowerBlue()));
            row.Append(componentText);
        }

        auto unbindButton = XC::HyperlinkButton();
        unbindButton.Content(winrt::box_value(L"Unbind"));
        unbindButton.FontSize(11);
        unbindButton.Padding({ 2, 0, 2, 0 });
        unbindButton.Click([this, nodeId, key](auto&&, auto&&)
        {
            const bool exists = EditGraph([nodeId, &key](PanelGraphEdit& e)
            {
                if (!e.graph.FindNode(nodeId)) return false;
                e.graph.UnbindProperty(nodeId, key);
                e.rebuildLayout();
                return true;
            });
            if (exists) Rebuild();
        });
        row.Append(unbindButton);
    }

    void PropertiesPanelController::AppendBindMenu(Children const& row,
        const Graph::GraphUiSnapshot& snapshot, uint32_t nodeId, const std::wstring& key)
    {
        // Offered only when another node publishes typed analysis fields.
        auto isSource = [nodeId](const Graph::EffectNode& n)
        {
            return n.id != nodeId && n.analysisOutput.type == Graph::AnalysisOutputType::Typed &&
                   !n.analysisOutput.fields.empty();
        };
        if (std::none_of(snapshot.nodes.begin(), snapshot.nodes.end(), isSource)) return;

        auto bindButton = XC::DropDownButton();
        bindButton.Content(winrt::box_value(L"\U0001F517"));
        bindButton.FontSize(10);
        bindButton.Padding({ 4, 0, 4, 0 });
        bindButton.MinWidth(0);
        bindButton.MinHeight(0);

        auto menuItem = [this, nodeId, key](const std::wstring& text, uint32_t sourceId,
            const std::wstring& field, uint32_t component)
        {
            auto item = XC::MenuFlyoutItem();
            item.Text(winrt::hstring(text));
            item.Click([this, nodeId, key, sourceId, field, component](auto&&, auto&&)
            {
                Bind(nodeId, key, sourceId, field, component);
            });
            return item;
        };

        // One submenu per source node; a vector field offers the whole
        // vector and each component.
        auto flyout = XC::MenuFlyout();
        for (const auto& source : snapshot.nodes)
        {
            if (!isSource(source)) continue;
            auto sourceMenu = XC::MenuFlyoutSubItem();
            sourceMenu.Text(winrt::hstring(source.name));
            for (const auto& field : source.analysisOutput.fields)
            {
                const uint32_t components = Graph::AnalysisFieldComponentCount(field.type);
                if (Graph::AnalysisFieldIsArray(field.type) || components == 1)
                {
                    sourceMenu.Items().Append(menuItem(field.name, source.id, field.name, 0));
                    continue;
                }
                auto fieldMenu = XC::MenuFlyoutSubItem();
                fieldMenu.Text(winrt::hstring(field.name));
                fieldMenu.Items().Append(menuItem(L"(all)", source.id, field.name, 0));
                for (uint32_t c = 0; c < components; ++c)
                    fieldMenu.Items().Append(menuItem(field.name + scComponentSuffixes[c], source.id, field.name, c));
                sourceMenu.Items().Append(fieldMenu);
            }
            flyout.Items().Append(sourceMenu);
        }
        bindButton.Flyout(flyout);
        row.Append(bindButton);
    }

    void PropertiesPanelController::Bind(uint32_t nodeId, const std::wstring& key, uint32_t sourceId,
        const std::wstring& field, uint32_t component)
    {
        EditGraph([&](PanelGraphEdit& e)
        {
            e.graph.BindProperty(nodeId, key, sourceId, field, component);
            e.rebuildLayout();
            return true;
        });
        Rebuild();
    }

    void PropertiesPanelController::OnPropertyChanged(uint32_t nodeId, const std::wstring& key)
    {
        const auto snapshot = m_host.snapshot();
        const auto* node = snapshot ? snapshot->FindNode(nodeId) : nullptr;
        if (!node) return;

        if (auto it = node->properties.find(key); it != node->properties.end())
            m_host.logInfo(nodeId, std::format(L"Property '{}' = {}", key, Graph::FormatPropertyValue(it->second, 3)));

        // Changes that can show or hide other parameters, move a parameter
        // node's slider range, or reshape the histogram rebuild the panel and
        // the node's pins. Queued at normal priority so the UI tick cannot
        // starve it.
        const bool hasVisibleWhen = node->customEffect.has_value() &&
            std::any_of(node->customEffect->parameters.begin(), node->customEffect->parameters.end(),
                [](const Graph::ParameterDefinition& p) { return !p.visibleWhen.empty(); });
        const bool isParameterNode = node->customEffect.has_value() &&
            Graph::IsParameterKind(node->customEffect->nodeKind);
        if (!hasVisibleWhen && !isParameterNode && !IsBuiltIn(*node, CLSID_D2D1Histogram))
            return;

        m_panel.DispatcherQueue().TryEnqueue(
            winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Normal,
            [this]
            {
                if (m_host.isShuttingDown()) return;
                Rebuild();
                EditGraph([](PanelGraphEdit& e) { e.rebuildLayout(); return true; });
                m_host.requestCanvasRedraw();
                m_host.requestRender();
            });
    }

    void PropertiesPanelController::OpenCurveEditor(uint32_t nodeId, const std::wstring& key)
    {
        const auto snapshot = m_host.snapshot();
        const auto* node = snapshot ? snapshot->FindNode(nodeId) : nullptr;
        if (!node) return;
        auto it = node->properties.find(key);
        if (it == node->properties.end()) return;
        const auto* lut = std::get_if<std::vector<float>>(&it->second);
        if (!lut) return;

        ShowCurveEditorDialog(m_dialogs, m_panel.XamlRoot(), key, *lut,
            [this, nodeId, key](const CurveShape& curve)
            {
                const bool applied = EditGraph([&](PanelGraphEdit& e)
                {
                    return Graph::EditNodeProperty(e.graph, nodeId, key, [&curve](Graph::PropertyValue& property)
                    {
                        auto* values = std::get_if<std::vector<float>>(&property);
                        if (!values) return false;
                        curve.WriteTo(*values);
                        return true;
                    });
                });
                if (!applied) return;
                OnPropertyChanged(nodeId, key);
                Rebuild();
            });
    }

    void PropertiesPanelController::AppendGammaCurvePreview(Children const& children, const Graph::EffectNode& node)
    {
        if (node.type != Graph::NodeType::BuiltInEffect || !IsBuiltIn(node, CLSID_D2D1GammaTransfer)) return;

        children.Append(SectionHeader(L"Curve Preview", { 0, 8, 0, 4 }));

        constexpr float cWidth = 200.0f;
        constexpr float cHeight = 80.0f;
        auto canvas = XC::Canvas();
        canvas.Width(cWidth);
        canvas.Height(cHeight);
        canvas.Background(Brush(winrt::Microsoft::UI::ColorHelper::FromArgb(255, 30, 30, 30)));

        // Each enabled channel: output = amplitude * input^exponent + offset.
        struct Channel { const wchar_t* prefix; winrt::Windows::UI::Color color; };
        static constexpr Channel scChannels[] = {
            { L"Red",   { 200, 255, 80, 80 } },
            { L"Green", { 200, 80, 255, 80 } },
            { L"Blue",  { 200, 80, 80, 255 } },
        };
        for (const auto& channel : scChannels)
        {
            const std::wstring prefix = channel.prefix;
            const auto disable = node.properties.find(prefix + L"Disable");
            if (disable != node.properties.end())
                if (auto* off = std::get_if<bool>(&disable->second); off && *off) continue;
            const float amplitude = FloatProperty(node, prefix + L"Amplitude").value_or(1.0f);
            const float exponent = FloatProperty(node, prefix + L"Exponent").value_or(1.0f);
            const float offset = FloatProperty(node, prefix + L"Offset").value_or(0.0f);

            auto line = Xaml::Shapes::Polyline();
            line.Stroke(Brush(channel.color));
            line.StrokeThickness(1.5);
            auto points = Media::PointCollection();
            for (int i = 0; i <= 50; ++i)
            {
                const float input = static_cast<float>(i) / 50.0f;
                const float output = std::clamp(amplitude * std::pow(input, exponent) + offset, 0.0f, 1.0f);
                points.Append({ input * cWidth, cHeight - output * cHeight });
            }
            line.Points(points);
            canvas.Children().Append(line);
        }
        children.Append(canvas);
    }

    void PropertiesPanelController::AppendHistogram(Children const& children, const Graph::EffectNode& node)
    {
        const auto& output = node.analysisOutput;
        if (output.type != Graph::AnalysisOutputType::Histogram || output.data.empty()) return;

        auto header = XC::TextBlock();
        header.Text(winrt::hstring(output.label));
        header.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        header.Margin({ 0, 8, 0, 4 });
        children.Append(header);

        constexpr float cWidth = 300.0f;
        constexpr float cHeight = 120.0f;
        auto canvas = XC::Canvas();
        canvas.Width(cWidth);
        canvas.Height(cHeight);
        canvas.Background(Brush(winrt::Windows::UI::Color{ 255, 30, 30, 30 }));

        const auto& bins = output.data;
        float maxBin = *std::max_element(bins.begin(), bins.end());
        if (maxBin <= 0.0f) maxBin = 1.0f;

        winrt::Windows::UI::Color barColor;
        switch (output.channelIndex)
        {
        case 0:  barColor = { 200, 255, 60, 60 }; break;
        case 1:  barColor = { 200, 60, 255, 60 }; break;
        case 2:  barColor = { 200, 60, 100, 255 }; break;
        default: barColor = { 200, 200, 200, 200 }; break;
        }
        const auto barBrush = Brush(barColor);

        const float barWidth = cWidth / static_cast<float>(bins.size());
        for (uint32_t i = 0; i < bins.size(); ++i)
        {
            const float barHeight = bins[i] / maxBin * cHeight;
            if (barHeight < 0.5f) continue;
            auto bar = Xaml::Shapes::Rectangle();
            bar.Width((std::max)(1.0, static_cast<double>(barWidth)));
            bar.Height(static_cast<double>(barHeight));
            bar.Fill(barBrush);
            XC::Canvas::SetLeft(bar, static_cast<double>(i * barWidth));
            XC::Canvas::SetTop(bar, static_cast<double>(cHeight - barHeight));
            canvas.Children().Append(bar);
        }
        children.Append(canvas);
    }

    void PropertiesPanelController::AppendAnalysisResults(Children const& children, const Graph::EffectNode& node)
    {
        const auto& output = node.analysisOutput;
        if (output.type != Graph::AnalysisOutputType::Typed || output.fields.empty()) return;

        children.Append(SectionHeader(L"Analysis Results", { 0, 8, 0, 4 }));

        // Arrays show their first 32 elements.
        constexpr uint32_t cShownElements = 32;
        for (const auto& field : output.fields)
        {
            auto row = XC::StackPanel();
            row.Orientation(XC::Orientation::Horizontal);
            row.Spacing(8);
            row.Margin({ 0, 2, 0, 2 });

            auto name = XC::TextBlock();
            name.Text(winrt::hstring(field.name));
            name.FontSize(12);
            name.Width(140);
            name.Foreground(Brush(Colors::Gray()));
            row.Children().Append(name);

            auto valueText = XC::TextBlock();
            valueText.Text(winrt::hstring(Graph::FormatAnalysisField(field, cShownElements)));
            valueText.FontSize(12);
            valueText.FontFamily(Media::FontFamily(L"Consolas"));
            valueText.IsTextSelectionEnabled(true);
            valueText.TextWrapping(Xaml::TextWrapping::NoWrap);
            row.Children().Append(valueText);

            children.Append(row);
        }
    }

    void PropertiesPanelController::AppendInputCount(Children const& children, const Graph::EffectNode& node)
    {
        if (node.inputPins.empty()) return;
        auto pins = XC::TextBlock();
        pins.Text(L"Inputs: " + std::to_wstring(node.inputPins.size()));
        pins.Foreground(Brush(Colors::Gray()));
        pins.Margin({ 0, 8, 0, 0 });
        children.Append(pins);
    }

    void PropertiesPanelController::AppendUpgradeBanner(Children const& children, const Graph::EffectNode& node)
    {
        const auto* latest = Effects::NewerDescriptorFor(node);
        if (!latest) return;

        auto stack = XC::StackPanel();
        stack.Spacing(6);
        auto text = XC::TextBlock();
        text.Text(std::format(L"Newer version available (v{} -> v{})",
            node.customEffect->shaderLabEffectVersion, latest->effectVersion));
        text.FontSize(12);
        text.Foreground(Brush(Colors::White()));
        stack.Children().Append(text);

        const uint32_t nodeId = node.id;
        auto button = XC::Button();
        button.Content(winrt::box_value(L"\xE777  Update Effect"));
        button.HorizontalAlignment(Xaml::HorizontalAlignment::Stretch);
        button.Click([this, nodeId](auto&&, auto&&) { UpgradeNode(nodeId); });
        stack.Children().Append(button);
        children.Append(Banner(winrt::Windows::UI::Color{ 255, 50, 80, 140 }, { 0, 8, 0, 0 }, stack));
    }

    void PropertiesPanelController::AppendFooter(Children const& children, const Graph::EffectNode& node)
    {
        const uint32_t nodeId = node.id;
        if (IsExpressionNode(node))
        {
            auto addButton = WideButton(L"➕  Add Input", { 0, 12, 0, 0 });
            addButton.Click([this, nodeId](auto&&, auto&&) { AddExpressionInput(nodeId); });
            children.Append(addButton);
        }

        auto logButton = WideButton(L"\xE7BA  Show Logs", { 0, 12, 0, 0 });
        logButton.Click([this, nodeId](auto&&, auto&&) { m_host.openLog(nodeId); });
        children.Append(logButton);
    }

    void PropertiesPanelController::UpgradeNode(uint32_t nodeId)
    {
        const bool upgraded = EditGraph([nodeId](PanelGraphEdit& e)
        {
            auto* node = e.graph.FindNode(nodeId);
            const auto* desc = node ? Effects::NewerDescriptorFor(*node) : nullptr;
            if (!desc || !Effects::UpgradeNode(e.graph, *node, *desc)) return false;
            e.invalidateNode(nodeId);
            e.rebuildLayout();
            return true;
        });
        if (!upgraded) return;
        m_host.requestRender();
        m_host.refreshNodeList();
        Rebuild();
    }

    void PropertiesPanelController::AddExpressionInput(uint32_t nodeId)
    {
        const bool added = EditGraph([nodeId](PanelGraphEdit& e)
        {
            if (Graph::AddExpressionInput(e.graph, nodeId).empty()) return false;
            e.rebuildLayout();
            return true;
        });
        if (!added) return;
        m_host.requestRender();
        Rebuild();
    }

    void PropertiesPanelController::RemoveExpressionInput(uint32_t nodeId, const std::wstring& name)
    {
        const bool removed = EditGraph([nodeId, &name](PanelGraphEdit& e)
        {
            if (!Graph::RemoveExpressionInput(e.graph, nodeId, name)) return false;
            e.rebuildLayout();
            return true;
        });
        if (!removed) return;
        m_host.requestRender();
        Rebuild();
    }

    winrt::fire_and_forget PropertiesPanelController::BrowseImage(uint32_t nodeId)
    {
        auto keepAlive = m_host.keepAlive ? m_host.keepAlive() : nullptr;

        winrt::Windows::Storage::Pickers::FileOpenPicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(m_host.windowHandle());
        picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::PicturesLibrary);
        // The formats WIC can decode on this machine, installed codecs included.
        for (const auto& ext : Effects::ImageLoader::SupportedExtensions())
            picker.FileTypeFilter().Append(winrt::hstring(ext));

        auto file = co_await picker.PickSingleFileAsync();
        if (!file) co_return;

        std::wstring filePath(file.Path().c_str());
        std::wstring fileName(file.Name().c_str());
        const bool exists = EditGraph([nodeId, &filePath, &fileName](PanelGraphEdit& e)
        {
            auto* node = e.graph.FindNode(nodeId);
            if (!node) return false;
            ::ShaderLab::Graph::SetNodeMedia(*node, std::move(filePath));
            ::ShaderLab::Graph::RenameNode(*node, std::move(fileName));
            e.prepareSource(*node);
            e.rebuildLayout();
            return true;
        });
        if (!exists) co_return;

        m_host.refreshNodeList();
        m_host.fitPreview();
        Rebuild();
    }

    winrt::fire_and_forget PropertiesPanelController::BrowseVideo(uint32_t nodeId)
    {
        auto keepAlive = m_host.keepAlive ? m_host.keepAlive() : nullptr;

        winrt::Windows::Storage::Pickers::FileOpenPicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(m_host.windowHandle());
        picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::VideosLibrary);
        for (const auto ext : Effects::SourceNodeFactory::VideoExtensions())
            picker.FileTypeFilter().Append(winrt::hstring(ext));

        auto file = co_await picker.PickSingleFileAsync();
        if (!file) co_return;

        std::wstring filePath(file.Path().c_str());
        std::wstring fileName(file.Name().c_str());
        const bool exists = EditGraph([nodeId, &filePath, &fileName](PanelGraphEdit& e)
        {
            auto* node = e.graph.FindNode(nodeId);
            if (!node) return false;
            ::ShaderLab::Graph::SetNodeMedia(*node, filePath);
            ::ShaderLab::Graph::RenameNode(*node, fileName);
            e.prepareSource(*node);   // opens the new file
            e.rebuildLayout();
            return true;
        });
        if (!exists) co_return;

        m_host.refreshNodeList();
        Rebuild();
    }
}
