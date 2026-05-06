#pragma once

#include "pch_engine.h"
#include "../EngineExport.h"
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

namespace ShaderLab::Effects
{
	class SHADERLAB_API WindowsGraphicsCaptureSourceProvider
	{
	public:
		WindowsGraphicsCaptureSourceProvider() = default;
		WindowsGraphicsCaptureSourceProvider(const WindowsGraphicsCaptureSourceProvider&) = delete;
		WindowsGraphicsCaptureSourceProvider& operator=(const WindowsGraphicsCaptureSourceProvider&) = delete;
		~WindowsGraphicsCaptureSourceProvider();

		bool Open(
			ID2D1DeviceContext5* dc,
			ID3D11Device* d3dDevice,
			winrt::Windows::Graphics::Capture::GraphicsCaptureItem const& item,
			bool rawFP16 = false);
		void Close();

		bool IsOpen() const { return m_session != nullptr; }
		bool CaptureNextFrame(ID2D1DeviceContext5* dc);
		ID2D1Image* CurrentBitmap() const { return m_bitmap.get(); }

		uint32_t Width() const { return m_width; }
		uint32_t Height() const { return m_height; }
		uint64_t FrameCount() const { return m_frameCount; }
		bool RawFP16() const { return m_rawFP16; }
		DXGI_FORMAT OutputFormat() const { return m_outputFormat; }
		const std::wstring& ItemName() const { return m_itemName; }
		const std::wstring& LastError() const { return m_lastError; }

	private:
		bool CreateDevice(ID3D11Device* d3dDevice);
		bool CreateOutputTextureAndBitmap(ID2D1DeviceContext5* dc, ID3D11Device* d3dDevice, uint32_t width, uint32_t height, DXGI_FORMAT format);
		bool RecreateForSize(ID2D1DeviceContext5* dc, winrt::Windows::Graphics::SizeInt32 const& size);
		winrt::Windows::Graphics::DirectX::DirectXPixelFormat CapturePixelFormat() const;

		winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_device{ nullptr };
		winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item{ nullptr };
		winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{ nullptr };
		winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{ nullptr };

		winrt::com_ptr<ID3D11Device> m_d3dDevice;
		winrt::com_ptr<ID3D11Texture2D> m_outputTexture;
		winrt::com_ptr<ID2D1Bitmap1> m_bitmap;

		uint32_t m_width{ 0 };
		uint32_t m_height{ 0 };
		bool m_rawFP16{ false };
		DXGI_FORMAT m_outputFormat{ DXGI_FORMAT_B8G8R8A8_UNORM };
		uint64_t m_frameCount{ 0 };
		std::wstring m_itemName;
		std::wstring m_lastError;
	};
}
