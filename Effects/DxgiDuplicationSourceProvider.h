#pragma once

#include "pch_engine.h"
#include "../EngineExport.h"

namespace ShaderLab::Effects
{
	struct SHADERLAB_API DxgiOutputInfo
	{
		uint32_t adapterIndex{ 0 };
		uint32_t outputIndex{ 0 };
		std::wstring deviceName;
		RECT desktopCoordinates{};
		bool attachedToDesktop{ false };
	};

	class SHADERLAB_API DxgiDuplicationSourceProvider
	{
	public:
		DxgiDuplicationSourceProvider() = default;
		DxgiDuplicationSourceProvider(const DxgiDuplicationSourceProvider&) = delete;
		DxgiDuplicationSourceProvider& operator=(const DxgiDuplicationSourceProvider&) = delete;
		~DxgiDuplicationSourceProvider();

		static std::vector<DxgiOutputInfo> EnumerateOutputs(ID3D11Device* d3dDevice = nullptr);

		bool Open(
			ID2D1DeviceContext5* dc,
			ID3D11Device* d3dDevice,
			uint32_t adapterIndex,
			uint32_t outputIndex,
			bool rawFP16 = false);
		void Close();

		bool IsOpen() const { return m_duplication != nullptr; }
		bool CaptureNextFrame(ID2D1DeviceContext5* dc);
		ID2D1Image* CurrentBitmap() const { return m_bitmap.get(); }

		uint32_t Width() const { return m_width; }
		uint32_t Height() const { return m_height; }
		uint64_t FrameCount() const { return m_frameCount; }
		uint32_t AdapterIndex() const { return m_adapterIndex; }
		uint32_t OutputIndex() const { return m_outputIndex; }
		bool RawFP16() const { return m_rawFP16; }
		DXGI_FORMAT OutputFormat() const { return m_outputFormat; }
		const std::wstring& OutputName() const { return m_outputName; }
		const std::wstring& LastError() const { return m_lastError; }

	private:
		bool CreateOutputTextureAndBitmap(ID2D1DeviceContext5* dc, ID3D11Device* d3dDevice, uint32_t width, uint32_t height, DXGI_FORMAT format);
		bool Reopen(ID2D1DeviceContext5* dc);

		winrt::com_ptr<IDXGIOutputDuplication> m_duplication;
		winrt::com_ptr<ID3D11Device> m_d3dDevice;
		winrt::com_ptr<ID3D11Texture2D> m_outputTexture;
		winrt::com_ptr<ID2D1Bitmap1> m_bitmap;

		uint32_t m_adapterIndex{ 0 };
		uint32_t m_outputIndex{ 0 };
		uint32_t m_width{ 0 };
		uint32_t m_height{ 0 };
		bool m_rawFP16{ false };
		DXGI_FORMAT m_outputFormat{ DXGI_FORMAT_B8G8R8A8_UNORM };
		uint64_t m_frameCount{ 0 };
		std::wstring m_outputName;
		std::wstring m_lastError;
	};
}
