#include "pch_engine.h"
#include "DisplayMonitor.h"

namespace ShaderLab::Rendering
{
    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    DisplayMonitor::~DisplayMonitor()
    {
        Shutdown();
    }

    void DisplayMonitor::Initialize(HWND appHwnd, IDXGIFactory7* dxgiFactory)
    {
        m_appHwnd = appHwnd;

        // Take a fresh snapshot before anything else.
        m_caps = QueryCurrentCapabilities();
        m_lastMonitor = MonitorFromWindow(m_appHwnd, MONITOR_DEFAULTTOPRIMARY);

        // Create a hidden message-only window to receive WM_DISPLAYCHANGE.
        CreateMessageWindow();

        // If a DXGI factory is available, register for adapter hot-plug.
        if (dxgiFactory)
        {
            RegisterAdapterChangeEvent(dxgiFactory);
        }

        // Poll for monitor changes (moving the window between displays).
        m_monitorPollThread = std::jthread([this](std::stop_token stop)
        {
            while (!stop.stop_requested())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (stop.stop_requested()) break;

                HMONITOR current = MonitorFromWindow(m_appHwnd, MONITOR_DEFAULTTOPRIMARY);
                if (current != m_lastMonitor)
                {
                    m_lastMonitor = current;
                    OnDisplayChanged();
                }
            }
        });
    }

    void DisplayMonitor::Shutdown()
    {
        if (m_monitorPollThread.joinable())
        {
            m_monitorPollThread.request_stop();
            m_monitorPollThread.join();
        }
        UnregisterAdapterChangeEvent();
        DestroyMessageWindow();
        m_appHwnd = nullptr;
    }

    // -----------------------------------------------------------------------
    // Capability query
    // -----------------------------------------------------------------------

    DisplayCapabilities DisplayMonitor::QueryCurrentCapabilities() const
    {
        DisplayCapabilities caps{};

        auto output = GetOutputForWindow();
        if (!output)
            return caps;

        DXGI_OUTPUT_DESC1 desc{};
        if (SUCCEEDED(output->GetDesc1(&desc)))
        {
            // AdvancedColorSupported flag alone isn't enough — the user
            // must also have toggled "Use HDR" in Windows Settings, which
            // sets AdvancedColor*Active* (aka the color-space check).
            caps.hdrEnabled = (desc.ColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
            caps.bitsPerColor = desc.BitsPerColor;
            caps.colorSpace = desc.ColorSpace;
            caps.maxLuminanceNits = desc.MaxLuminance;
            caps.minLuminanceNits = desc.MinLuminance;
            caps.maxFullFrameLuminanceNits = desc.MaxFullFrameLuminance;

            // Monitor color primaries from DXGI EDID data.
            caps.redPrimaryX   = desc.RedPrimary[0];
            caps.redPrimaryY   = desc.RedPrimary[1];
            caps.greenPrimaryX = desc.GreenPrimary[0];
            caps.greenPrimaryY = desc.GreenPrimary[1];
            caps.bluePrimaryX  = desc.BluePrimary[0];
            caps.bluePrimaryY  = desc.BluePrimary[1];
            caps.whitePointX   = desc.WhitePoint[0];
            caps.whitePointY   = desc.WhitePoint[1];

            // SDR white level: when HDR is on, this controls the nit value
            // that scRGB 1.0 (a.k.a. SDR reference white) maps to on the
            // display. Read it from the OS via DisplayConfigGetDeviceInfo
            // so it tracks the user's Windows Settings -> Display -> HDR ->
            // "SDR content brightness" slider. The returned SDRWhiteLevel
            // is in 1/1000ths of 80 nits, per Microsoft's documentation.
            // Fallback to 80 nits when the call isn't available (older
            // Windows builds, non-DXGI outputs, virtual displays).
            caps.sdrWhiteLevelNits = QuerySdrWhiteLevelForOutput(output.get());
        }

        return caps;
    }

    // -----------------------------------------------------------------------
    // DXGI output for the app window
    // -----------------------------------------------------------------------

    winrt::com_ptr<IDXGIOutput6> DisplayMonitor::GetOutputForWindow() const
    {
        if (!m_appHwnd)
            return nullptr;

        // Find the monitor that contains the majority of the app window.
        HMONITOR hmon = MonitorFromWindow(m_appHwnd, MONITOR_DEFAULTTOPRIMARY);

        // Enumerate adapters → outputs to find the matching HMONITOR.
        winrt::com_ptr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put()))))
            return nullptr;

        winrt::com_ptr<IDXGIAdapter1> adapter;
        for (UINT ai = 0; factory->EnumAdapters1(ai, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++ai)
        {
            winrt::com_ptr<IDXGIOutput> output;
            for (UINT oi = 0; adapter->EnumOutputs(oi, output.put()) != DXGI_ERROR_NOT_FOUND; ++oi)
            {
                DXGI_OUTPUT_DESC desc{};
                if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == hmon)
                {
                    winrt::com_ptr<IDXGIOutput6> output6;
                    if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(output6.put()))))
                        return output6;
                }
                output = nullptr;
            }
            adapter = nullptr;
        }

        return nullptr;
    }

    // -----------------------------------------------------------------------
    // SDR white level query (DisplayConfig)
    // -----------------------------------------------------------------------

    float DisplayMonitor::QuerySdrWhiteLevelForOutput(IDXGIOutput6* output)
    {
        // Default: 80 nits == scRGB 1.0 reference. Returned on any failure
        // path (older Windows, virtual outputs, no DXGI desc).
        constexpr float kDefaultNits = 80.0f;
        if (!output) return kDefaultNits;

        DXGI_OUTPUT_DESC desc{};
        if (FAILED(output->GetDesc(&desc)) || desc.Monitor == nullptr)
            return kDefaultNits;

        // Resolve HMONITOR -> GDI device name -> source mode -> target.
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!::GetMonitorInfoW(desc.Monitor, &mi))
            return kDefaultNits;

        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        if (::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
            return kDefaultNits;

        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        if (::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,
                                 &pathCount, paths.data(),
                                 &modeCount, modes.data(),
                                 nullptr) != ERROR_SUCCESS)
            return kDefaultNits;
        paths.resize(pathCount);
        modes.resize(modeCount);

        for (const auto& path : paths)
        {
            // Match by GDI device name on the source.
            DISPLAYCONFIG_SOURCE_DEVICE_NAME srcName{};
            srcName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            srcName.header.size = sizeof(srcName);
            srcName.header.adapterId = path.sourceInfo.adapterId;
            srcName.header.id = path.sourceInfo.id;
            if (::DisplayConfigGetDeviceInfo(&srcName.header) != ERROR_SUCCESS)
                continue;
            if (wcscmp(srcName.viewGdiDeviceName, mi.szDevice) != 0)
                continue;

            // SDRWhiteLevel is reported in 1/1000ths of 80 nits, i.e.
            // nits = SDRWhiteLevel / 1000.0 * 80.0. Confirmed by the
            // documented sample on Microsoft Learn.
            DISPLAYCONFIG_SDR_WHITE_LEVEL whiteLevel{};
            whiteLevel.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
            whiteLevel.header.size = sizeof(whiteLevel);
            whiteLevel.header.adapterId = path.targetInfo.adapterId;
            whiteLevel.header.id = path.targetInfo.id;
            if (::DisplayConfigGetDeviceInfo(&whiteLevel.header) != ERROR_SUCCESS)
                return kDefaultNits;

            const float nits = static_cast<float>(whiteLevel.SDRWhiteLevel) / 1000.0f * 80.0f;
            if (nits >= 40.0f && nits <= 480.0f)  // sanity-clamp to the slider's UI range
                return nits;
            return kDefaultNits;
        }

        return kDefaultNits;
    }

    // -----------------------------------------------------------------------
    // WM_DISPLAYCHANGE via hidden message-only window
    // -----------------------------------------------------------------------

    void DisplayMonitor::CreateMessageWindow()
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &DisplayMonitor::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ShaderLab_DisplayMonitor";

        m_wndClass = RegisterClassExW(&wc);
        if (!m_wndClass)
            return;

        // HWND_MESSAGE makes this a message-only window (invisible, no taskbar).
        m_msgHwnd = CreateWindowExW(
            0, MAKEINTATOM(m_wndClass), L"",
            0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, wc.hInstance, this);
    }

    void DisplayMonitor::DestroyMessageWindow()
    {
        if (m_msgHwnd)
        {
            DestroyWindow(m_msgHwnd);
            m_msgHwnd = nullptr;
        }
        if (m_wndClass)
        {
            UnregisterClassW(MAKEINTATOM(m_wndClass), GetModuleHandleW(nullptr));
            m_wndClass = 0;
        }
    }

    LRESULT CALLBACK DisplayMonitor::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_CREATE)
        {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return 0;
        }

        auto* self = reinterpret_cast<DisplayMonitor*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        if (msg == WM_DISPLAYCHANGE && self)
        {
            self->OnDisplayChanged();
            return 0;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    // -----------------------------------------------------------------------
    // IDXGIFactory7 adapter-changed event
    // -----------------------------------------------------------------------

    void DisplayMonitor::RegisterAdapterChangeEvent(IDXGIFactory7* factory)
    {
        if (!factory)
            return;

        m_dxgiFactory.copy_from(factory);

        m_adapterEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_adapterEvent)
            return;

        if (FAILED(m_dxgiFactory->RegisterAdaptersChangedEvent(m_adapterEvent, &m_adapterCookie)))
        {
            CloseHandle(m_adapterEvent);
            m_adapterEvent = nullptr;
            return;
        }

        // Background thread waits on the event and calls OnDisplayChanged.
        m_adapterThread = std::jthread([this](std::stop_token stop)
        {
            while (!stop.stop_requested())
            {
                DWORD result = WaitForSingleObject(m_adapterEvent, 500 /*ms poll for stop*/);
                if (result == WAIT_OBJECT_0)
                {
                    OnDisplayChanged();
                }
            }
        });
    }

    void DisplayMonitor::UnregisterAdapterChangeEvent()
    {
        // Stop the wait thread first.
        if (m_adapterThread.joinable())
        {
            m_adapterThread.request_stop();
            m_adapterThread.join();
        }

        if (m_dxgiFactory && m_adapterCookie)
        {
            m_dxgiFactory->UnregisterAdaptersChangedEvent(m_adapterCookie);
            m_adapterCookie = 0;
        }

        if (m_adapterEvent)
        {
            CloseHandle(m_adapterEvent);
            m_adapterEvent = nullptr;
        }

        m_dxgiFactory = nullptr;
    }

    // -----------------------------------------------------------------------
    // Change detection & callback dispatch
    // -----------------------------------------------------------------------

    void DisplayMonitor::OnDisplayChanged()
    {
        auto newCaps = QueryCurrentCapabilities();

        // Only fire the callback if something meaningful changed.
        bool changed = false;
        {
            std::lock_guard lock(m_capsMutex);
            changed = (newCaps.hdrEnabled != m_caps.hdrEnabled)
                || (newCaps.colorSpace != m_caps.colorSpace)
                || (newCaps.bitsPerColor != m_caps.bitsPerColor)
                || (std::abs(newCaps.maxLuminanceNits - m_caps.maxLuminanceNits) > 0.5f)
                || (std::abs(newCaps.redPrimaryX - m_caps.redPrimaryX) > 0.001f)
                || (std::abs(newCaps.greenPrimaryX - m_caps.greenPrimaryX) > 0.001f)
                || (std::abs(newCaps.bluePrimaryX - m_caps.bluePrimaryX) > 0.001f);
            m_caps = newCaps;
        }

        if (changed)
        {
            std::lock_guard lock(m_callbackMutex);
            if (m_callback)
                m_callback(newCaps);
        }
    }

    void DisplayMonitor::SetCallback(DisplayChangeCallback callback)
    {
        std::lock_guard lock(m_callbackMutex);
        m_callback = std::move(callback);
    }

    // -----------------------------------------------------------------------
    // Simulated profile support
    // -----------------------------------------------------------------------

    void DisplayMonitor::SetSimulatedProfile(const DisplayProfile& profile)
    {
        {
            std::lock_guard lock(m_capsMutex);
            m_simulatedProfile = profile;
            m_simulatedProfile->isSimulated = true;
        }

        // Notify subscribers with the simulated capabilities.
        std::lock_guard lock(m_callbackMutex);
        if (m_callback)
            m_callback(profile.caps);
    }

    void DisplayMonitor::ClearSimulatedProfile()
    {
        DisplayCapabilities liveCaps{};
        {
            std::lock_guard lock(m_capsMutex);
            m_simulatedProfile.reset();
            // Re-query live display to get current state.
            m_caps = QueryCurrentCapabilities();
            liveCaps = m_caps;
        }

        std::lock_guard lock(m_callbackMutex);
        if (m_callback)
            m_callback(liveCaps);
    }

    DisplayProfile DisplayMonitor::ActiveProfile() const
    {
        std::lock_guard lock(m_capsMutex);
        if (m_simulatedProfile.has_value())
            return *m_simulatedProfile;

        DisplayProfile p{};
        p.caps = m_caps;
        p.isSimulated = false;
        p.profileName = L"Live Display";
        p.primaryRed   = { m_caps.redPrimaryX,   m_caps.redPrimaryY };
        p.primaryGreen = { m_caps.greenPrimaryX, m_caps.greenPrimaryY };
        p.primaryBlue  = { m_caps.bluePrimaryX,  m_caps.bluePrimaryY };
        p.whitePoint   = { m_caps.whitePointX,   m_caps.whitePointY };
        return p;
    }

    DisplayProfile DisplayMonitor::LiveProfile() const
    {
        std::lock_guard lock(m_capsMutex);
        DisplayProfile p{};
        p.caps = m_caps;
        p.isSimulated = false;
        p.profileName = L"Live Display";
        p.primaryRed   = { m_caps.redPrimaryX,   m_caps.redPrimaryY };
        p.primaryGreen = { m_caps.greenPrimaryX, m_caps.greenPrimaryY };
        p.primaryBlue  = { m_caps.bluePrimaryX,  m_caps.bluePrimaryY };
        p.whitePoint   = { m_caps.whitePointX,   m_caps.whitePointY };
        return p;
    }
}
