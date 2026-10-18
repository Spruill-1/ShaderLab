// MainWindow partial: this window as an MCP session on the broker hub, and
// the shim copy MCP clients launch.

#include "pch.h"
#include "MainWindow.xaml.h"
#include "Mcp/McpPeerIdentity.h"
#include "Version.h"

#include <appmodel.h>
#include <shlobj.h>
#include <shobjidl_core.h>

namespace winrt::ShaderLab::implementation
{
    // The hub's AUMID for a client's shim to activate:
    // "<PackageFamilyName>!Hub". Empty when unpackaged (dev), where the shim
    // works only against a hub that is already running.
    std::wstring MainWindow::HubAumid()
    {
        UINT32 len = 0;
        LONG rc = ::GetCurrentPackageFamilyName(&len, nullptr);
        if (rc != ERROR_INSUFFICIENT_BUFFER)
            return {};   // APPMODEL_ERROR_NO_PACKAGE -> unpackaged
        std::wstring pfn(len, L'\0');
        if (::GetCurrentPackageFamilyName(&len, pfn.data()) != ERROR_SUCCESS)
            return {};
        pfn.resize(len ? len - 1 : 0);   // drop the null terminator
        return pfn + L"!Hub";
    }

    // Copies the shim (ShaderLabMcpBroker.exe, beside ShaderLab.exe in the
    // package) to %LOCALAPPDATA%\ShaderLab\bin\. Unpackaged, that copy
    // survives MSIX updates and uninstalls, so an MCP client keeps working
    // across an upgrade. Returns the copy's path, or empty on failure.
    //
    // A running shim holds the file open, so the old copy is renamed aside
    // (allowed while it runs) and the new one written in its place for the
    // next launch. Old copies whose shim has exited are deleted.
    std::wstring MainWindow::EnsureShimDistributed()
    {
        wchar_t exePath[MAX_PATH * 2]{};
        if (::GetModuleFileNameW(nullptr, exePath, ARRAYSIZE(exePath)) == 0)
            return {};
        std::wstring dir = exePath;
        auto slash = dir.find_last_of(L'\\');
        if (slash == std::wstring::npos) return {};
        std::wstring source = dir.substr(0, slash) + L"\\ShaderLabMcpBroker.exe";
        if (::GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES)
            return {};   // no broker payload (unexpected in a real build)

        PWSTR local = nullptr;
        if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
            return {};
        std::wstring binDir = std::wstring(local) + L"\\ShaderLab\\bin";
        ::CoTaskMemFree(local);
        ::SHCreateDirectoryExW(nullptr, binDir.c_str(), nullptr);
        std::wstring target = binDir + L"\\ShaderLabMcpBroker.exe";

        if (::GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            std::wstring aside = target + L"." + std::to_wstring(::GetTickCount64()) + L".old";
            ::MoveFileExW(target.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        ::CopyFileW(source.c_str(), target.c_str(), FALSE);

        WIN32_FIND_DATAW fd{};
        HANDLE h = ::FindFirstFileW((target + L".*.old").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do { ::DeleteFileW((binDir + L"\\" + fd.cFileName).c_str()); }
            while (::FindNextFileW(h, &fd));
            ::FindClose(h);
        }
        return (::GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES)
            ? target : std::wstring{};
    }

    // Registers this window with the hub as a session. Requests arrive
    // sealed and are routed through m_mcpServer, so tool calls reach the same
    // routes and engine events as any other caller. Idempotent.
    void MainWindow::StartMcpSession()
    {
        if (m_sessionClient || !m_mcpServer)
            return;

        // Refresh the shim on disk; a shim a client already runs keeps going.
        EnsureShimDistributed();

        // Activate the hub before the first connect: the session client only
        // connects, and with no hub running this window would otherwise wait
        // unregistered until some shim happened to activate one. A second
        // activation of the singleton hub is a no-op. Best effort: without it
        // the client keeps retrying until a hub appears.
        if (const std::wstring aumid = HubAumid(); !aumid.empty())
        {
            winrt::com_ptr<IApplicationActivationManager> activation;
            if (SUCCEEDED(::CoCreateInstance(CLSID_ApplicationActivationManager, nullptr,
                    CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(activation.put()))))
            {
                const std::wstring args = std::format(L"--hub --pipe {}", ::ShaderLab::Mcp::ResolvePipeBaseName({}));
                DWORD pid = 0;
                if (FAILED(activation->ActivateApplication(aumid.c_str(), args.c_str(), AO_NONE, &pid)))
                    OutputDebugStringW(L"[ShaderLab] MCP hub activation failed; the session keeps retrying\n");
            }
        }

        if (m_mcpSessionId.empty())
        {
            GUID g{};
            CoCreateGuid(&g);
            wchar_t buf[64]{};
            StringFromGUID2(g, buf, ARRAYSIZE(buf));
            m_mcpSessionId = buf;   // stable for this window's lifetime, not an ordinal
        }

        ::ShaderLab::Mcp::SessionClientOptions opts;
        opts.sessionId = m_mcpSessionId;
        opts.label = std::format(L"ShaderLab {} (pid {})",
            std::wstring(::ShaderLab::VersionString), GetCurrentProcessId());
        m_sessionClient = std::make_unique<::ShaderLab::Mcp::McpSessionClient>(
            *m_mcpServer, std::move(opts));
        m_sessionThread = std::thread([this] { m_sessionClient->Run(); });
    }

    // Runs before the render dispatcher shuts down: a request still in
    // flight could otherwise be waiting on a worker that is gone.
    void MainWindow::StopMcpSession()
    {
        if (m_sessionClient)
            m_sessionClient->Stop();
        if (m_sessionThread.joinable())
            m_sessionThread.join();
        m_sessionClient.reset();
    }

    // Toolbar label: whether this window is exposed, and if so whether the
    // hub has it registered. The tooltip carries the reason when it is not.
    void MainWindow::UpdateMcpStatusLabel()
    {
        auto label = McpServerLabel();
        if (!label)
            return;
        using ::ShaderLab::Mcp::SessionState;
        std::wstring text = L"MCP: off";
        std::wstring detail;
        if (m_sessionClient)
        {
            const auto status = m_sessionClient->Status();
            detail = status.detail;
            switch (status.state)
            {
            case SessionState::Registered: text = L"MCP: on"; break;
            case SessionState::NoHub:      text = L"MCP: no hub"; break;
            case SessionState::Refused:    text = L"MCP: refused"; break;
            default:                       text = L"MCP: connecting"; break;
            }
        }
        if (label.Text() != text)
        {
            label.Text(text);
            winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(label,
                detail.empty() ? nullptr : winrt::box_value(winrt::hstring(detail)));
        }
    }
}
