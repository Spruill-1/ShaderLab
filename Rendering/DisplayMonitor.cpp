#include "pch.h"
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

            // SDR white level: when HDR is on, this value matters for
            // tone-mapping the SDR-reference white to the correct nit level.
            // DXGI doesn't surface it directly in OUTPUT_DESC1; the
            // canonical way is DwmGetWindowAttribute(DWMWA_SDR_WHITE_LEVEL)
            // but that requires a top-level HWND at runtime. Fall back to
            // the D3D11 SDR reference-white. For now, use 80 nits default.
            caps.sdrWhiteLevelNits = 80.0f;
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
