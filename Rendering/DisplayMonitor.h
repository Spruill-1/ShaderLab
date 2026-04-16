#pragma once

#include "pch.h"
#include "DisplayInfo.h"
#include "DisplayProfile.h"

namespace ShaderLab::Rendering
{
    // Monitors display capability changes (HDR toggle, luminance, adapter hot-plug)
    // and notifies subscribers so the rendering pipeline can adapt.
    //
    // Two detection paths:
    //   1. WM_DISPLAYCHANGE — resolution/depth changes, HDR toggle
    //   2. IDXGIFactory7::RegisterAdaptersChangedEvent — GPU hot-plug / driver update
    //
    // Both paths re-query IDXGIOutput6::GetDesc1 and fire the callback
    // only when capabilities actually differ from the cached snapshot.
    class DisplayMonitor
    {
    public:
        DisplayMonitor() = default;
        ~DisplayMonitor();

        DisplayMonitor(const DisplayMonitor&) = delete;
        DisplayMonitor& operator=(const DisplayMonitor&) = delete;

        // Initialize monitoring for the window identified by hWnd.
        // dxgiFactory is used for adapter-change registration (may be nullptr
        // if the D3D device isn't created yet — adapter monitoring will be skipped).
        void Initialize(HWND appHwnd, IDXGIFactory7* dxgiFactory = nullptr);

        // Tear down the message window and unregister the adapter event.
        void Shutdown();

        // Force a re-query of display capabilities right now.
        DisplayCapabilities QueryCurrentCapabilities() const;

        // Returns the most recently cached capabilities (no DXGI call).
        // When a simulated profile is active, returns the simulated caps.
        DisplayCapabilities CachedCapabilities() const
        {
            std::lock_guard lock(m_capsMutex);
            if (m_simulatedProfile.has_value())
                return m_simulatedProfile->caps;
            return m_caps;
        }

        // Register / clear the change callback.
        void SetCallback(DisplayChangeCallback callback);

        // Simulated profile support — override live display with a preset or ICC profile.
        void SetSimulatedProfile(const DisplayProfile& profile);
        void ClearSimulatedProfile();

        bool IsSimulated() const
        {
            std::lock_guard lock(m_capsMutex);
            return m_simulatedProfile.has_value();
        }

        // Returns the simulated profile if active, otherwise constructs one from live caps.
        DisplayProfile ActiveProfile() const;

    private:
        // Hidden message-only window for WM_DISPLAYCHANGE.
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        void CreateMessageWindow();
        void DestroyMessageWindow();

        // Adapter-changed event via IDXGIFactory7.
        void RegisterAdapterChangeEvent(IDXGIFactory7* factory);
        void UnregisterAdapterChangeEvent();

        // Re-query and fire callback if changed.
        void OnDisplayChanged();

        // Find the IDXGIOutput6 that covers the app window.
        winrt::com_ptr<IDXGIOutput6> GetOutputForWindow() const;

        HWND                            m_appHwnd{ nullptr };
        HWND                            m_msgHwnd{ nullptr };
        ATOM                            m_wndClass{ 0 };

        winrt::com_ptr<IDXGIFactory7>   m_dxgiFactory;
        DWORD                           m_adapterCookie{ 0 };
        HANDLE                          m_adapterEvent{ nullptr };
        std::jthread                    m_adapterThread;

        DisplayCapabilities             m_caps{};
        std::optional<DisplayProfile>   m_simulatedProfile;
        mutable std::mutex              m_capsMutex;
        DisplayChangeCallback           m_callback;
        std::mutex                      m_callbackMutex;

        // Periodic monitor-change polling (WM_DISPLAYCHANGE doesn't fire on window move).
        HMONITOR                        m_lastMonitor{ nullptr };
        std::jthread                    m_monitorPollThread;
    };
}
