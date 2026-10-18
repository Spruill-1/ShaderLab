#include "pch_engine.h"
#include "CaptureSources.h"
#include "DxgiDuplicationSourceProvider.h"
#include "WindowsGraphicsCaptureSourceProvider.h"
#include "Effects/LiveSource.h"
#include "Effects/NodeCatalog.h"
#include "Effects/SourceNodeFactory.h"
#include "Graph/SourceKind.h"

namespace ShaderLab::Sources
{
    namespace
    {
        using Effects::ILiveSource;

        template <typename T>
        T ReadProperty(const Graph::EffectNode& node, const wchar_t* name, T fallback)
        {
            auto it = node.properties.find(name);
            if (it == node.properties.end()) return fallback;
            const T* value = std::get_if<T>(&it->second);
            return value ? *value : fallback;
        }

        class DesktopDuplicationSource final : public ILiveSource
        {
        public:
            bool Open(const Graph::EffectNode& node, ID2D1DeviceContext5* dc, ID3D11Device* d3dDevice) override
            {
                m_adapterIndex = ReadProperty(node, L"AdapterIndex", uint32_t{ 0 });
                m_outputIndex = ReadProperty(node, L"OutputIndex", uint32_t{ 0 });
                m_rawFP16 = ReadProperty(node, L"RawFP16", true);
                return m_provider.Open(dc, d3dDevice, m_adapterIndex, m_outputIndex, m_rawFP16);
            }

            bool Reopen(ID2D1DeviceContext5* dc) override { return m_provider.Reopen(dc); }

            bool Matches(const Graph::EffectNode& node) const override
            {
                return ReadProperty(node, L"AdapterIndex", uint32_t{ 0 }) == m_adapterIndex &&
                       ReadProperty(node, L"OutputIndex", uint32_t{ 0 }) == m_outputIndex &&
                       ReadProperty(node, L"RawFP16", true) == m_rawFP16;
            }

            bool IsOpen() const override { return m_provider.IsOpen(); }
            Effects::CaptureResult NextFrame(ID2D1DeviceContext5* dc) override { return m_provider.CaptureNextFrame(dc); }
            ID2D1Image* CurrentImage() const override { return m_provider.CurrentBitmap(); }
            uint32_t Width() const override { return m_provider.Width(); }
            uint32_t Height() const override { return m_provider.Height(); }
            uint64_t FrameCount() const override { return m_provider.FrameCount(); }
            bool DeliversFP16() const override { return Effects::IsLinearScRgbFormat(m_provider.OutputFormat()); }
            const std::wstring& LastError() const override { return m_provider.LastError(); }

            void Describe(Graph::EffectNode& node) const override
            {
                node.properties[L"OutputName"] = m_provider.OutputName();
            }

        private:
            DxgiDuplicationSourceProvider m_provider;
            uint32_t m_adapterIndex{ 0 };
            uint32_t m_outputIndex{ 0 };
            bool m_rawFP16{ true };
        };

        class GraphicsCaptureSource final : public ILiveSource
        {
        public:
            explicit GraphicsCaptureSource(winrt::Windows::Graphics::Capture::GraphicsCaptureItem item)
                : m_item(std::move(item)) {}

            bool Open(const Graph::EffectNode& node, ID2D1DeviceContext5* dc, ID3D11Device* d3dDevice) override
            {
                return m_provider.Open(dc, d3dDevice, m_item, ReadProperty(node, L"RawFP16", true));
            }

            bool Reopen(ID2D1DeviceContext5* dc) override { return m_provider.Reopen(dc); }
            bool IsOpen() const override { return m_provider.IsOpen(); }
            Effects::CaptureResult NextFrame(ID2D1DeviceContext5* dc) override { return m_provider.CaptureNextFrame(dc); }
            ID2D1Image* CurrentImage() const override { return m_provider.CurrentBitmap(); }
            uint32_t Width() const override { return m_provider.Width(); }
            uint32_t Height() const override { return m_provider.Height(); }
            uint64_t FrameCount() const override { return m_provider.FrameCount(); }
            bool DeliversFP16() const override { return Effects::IsLinearScRgbFormat(m_provider.OutputFormat()); }
            const std::wstring& LastError() const override { return m_provider.LastError(); }

            void Describe(Graph::EffectNode& node) const override
            {
                if (m_provider.ItemName().empty()) return;
                node.name = m_provider.ItemName();
                node.properties[L"CaptureItemName"] = m_provider.ItemName();
            }

        private:
            winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item{ nullptr };
            WindowsGraphicsCaptureSourceProvider m_provider;
        };

        // Desktop duplication is pure data (adapter and output index), so
        // unlike Windows Graphics Capture, which needs the OS picker, it can be
        // created by name over MCP and headless.
        Effects::NodeCreateResult MakeDesktopDuplicationNode(const Effects::NodeCreateArgs& args, const Graph::GraphModel&)
        {
            auto argument = [&](const wchar_t* name)
            {
                auto it = args.uints.find(name);
                return it == args.uints.end() ? 0u : it->second;
            };
            const uint32_t adapterIndex = argument(L"adapterIndex");
            const uint32_t outputIndex = argument(L"outputIndex");

            Effects::NodeCreateResult result;
            const auto outputs = DxgiDuplicationSourceProvider::EnumerateOutputs();
            auto match = std::find_if(outputs.begin(), outputs.end(), [&](const auto& output)
            {
                return output.adapterIndex == adapterIndex && output.outputIndex == outputIndex;
            });
            if (match == outputs.end())
            {
                // A capture of a missing output would render black, which is
                // hard to tell from a correctly captured black screen.
                std::wstring available;
                for (const auto& output : outputs)
                {
                    if (!available.empty()) available += L", ";
                    available += std::format(L"{}:{}", output.adapterIndex, output.outputIndex);
                }
                result.error = std::format(L"No DXGI output {}:{}. Available adapter:output pairs: {}",
                    adapterIndex, outputIndex, available.empty() ? L"(none)" : available);
                return result;
            }
            const std::wstring label = match->deviceName.empty()
                ? std::format(L"DXGI {}:{}", adapterIndex, outputIndex)
                : std::format(L"DXGI {} ({},{})", match->deviceName, adapterIndex, outputIndex);
            result.node = CreateDesktopDuplicationNode(adapterIndex, outputIndex, label);
            result.needsPrepare = true;
            return result;
        }
    }

    void RegisterCaptureSources()
    {
        Effects::LiveSourceKind duplication;
        duplication.marker = cDesktopDuplicationMarker;
        duplication.displayName = L"DXGI output capture";
        duplication.create = [](winrt::Windows::Foundation::IInspectable const&, std::wstring&) -> std::unique_ptr<ILiveSource>
        {
            return std::make_unique<DesktopDuplicationSource>();
        };
        Effects::RegisterLiveSourceKind(std::move(duplication));

        Effects::LiveSourceKind graphicsCapture;
        graphicsCapture.marker = cGraphicsCaptureMarker;
        graphicsCapture.displayName = L"Windows Graphics Capture source";
        graphicsCapture.create = [](winrt::Windows::Foundation::IInspectable const& target, std::wstring& error)
            -> std::unique_ptr<ILiveSource>
        {
            auto item = target ? target.try_as<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>() : nullptr;
            if (!item)
            {
                error = L"Choose a window or monitor for this Windows Graphics Capture source.";
                return nullptr;
            }
            return std::make_unique<GraphicsCaptureSource>(std::move(item));
        };
        Effects::RegisterLiveSourceKind(std::move(graphicsCapture));

        Effects::RegisterNodeMaker({ L"Desktop Duplication", L"DXGI Duplicate Output", L"Display Capture" },
                                   &MakeDesktopDuplicationNode);
    }

    Graph::EffectNode CreateDesktopDuplicationNode(
        uint32_t adapterIndex,
        uint32_t outputIndex,
        const std::wstring& displayName)
    {
        auto node = Effects::SourceNodeFactory::CreateLiveSourceNode(cDesktopDuplicationMarker,
            displayName.empty() ? std::format(L"DXGI Output {}:{}", adapterIndex, outputIndex) : displayName);
        node.properties[L"AdapterIndex"] = adapterIndex;
        node.properties[L"OutputIndex"] = outputIndex;
        node.properties[L"RawFP16"] = true;
        return node;
    }

    Graph::EffectNode CreateGraphicsCaptureNode(const std::wstring& displayName)
    {
        auto node = Effects::SourceNodeFactory::CreateLiveSourceNode(cGraphicsCaptureMarker,
            displayName.empty() ? std::wstring(L"Windows Graphics Capture") : displayName);
        node.properties[L"RawFP16"] = true;
        return node;
    }

    bool IsDesktopDuplication(const Graph::EffectNode& node)
    {
        return Graph::LiveSourceMarker(node) == cDesktopDuplicationMarker;
    }
}
