#include "pch_engine.h"
#include "McpPeerIdentity.h"
#include "../../Version.h"

#include <appmodel.h>
#include <sddl.h>
#include <format>

namespace ShaderLab::Mcp
{
    std::optional<PeerIdentity> ResolveProcessIdentity(uint32_t pid)
    {
        // PROCESS_QUERY_LIMITED_INFORMATION suffices for same-account
        // targets — no SeDebugPrivilege involved.
        HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h)
            return std::nullopt;

        PeerIdentity id;
        id.pid = pid;

        FILETIME exitT{}, kernelT{}, userT{};
        ::GetProcessTimes(h, &id.creationTime, &exitT, &kernelT, &userT);

        wchar_t image[MAX_PATH * 2]{};
        DWORD imageLen = ARRAYSIZE(image);
        if (::QueryFullProcessImageNameW(h, 0, image, &imageLen))
        {
            std::wstring path(image, imageLen);
            auto slash = path.find_last_of(L'\\');
            id.imageDirectory = (slash == std::wstring::npos) ? path : path.substr(0, slash);
        }

        // GetPackageFamilyName(HANDLE) skips the OpenProcessToken step.
        // Trap: for a packaged process the sizing call reports
        // ERROR_INSUFFICIENT_BUFFER, not success — so just supply the
        // max-size buffer up front. Unpackaged peers report
        // APPMODEL_ERROR_NO_PACKAGE.
        wchar_t pfn[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1]{};
        UINT32 pfnLen = ARRAYSIZE(pfn);
        LONG rc = ::GetPackageFamilyName(h, &pfnLen, pfn);
        ::CloseHandle(h);

        if (rc == ERROR_SUCCESS)
        {
            id.kind = PeerIdentityKind::Packaged;
            // pfnLen includes the null terminator.
            id.packageFamilyName.assign(pfn, pfnLen ? pfnLen - 1 : 0);
        }
        else if (rc == APPMODEL_ERROR_NO_PACKAGE)
        {
            id.kind = PeerIdentityKind::Unpackaged;
        }
        else
        {
            return std::nullopt;
        }
        return id;
    }

    std::optional<PeerIdentity> ResolvePipeClientIdentity(HANDLE serverPipeHandle)
    {
        ULONG pid = 0;
        if (!::GetNamedPipeClientProcessId(serverPipeHandle, &pid))
            return std::nullopt;
        return ResolveProcessIdentity(static_cast<uint32_t>(pid));
    }

    std::optional<PeerIdentity> ResolvePipeServerIdentity(HANDLE clientPipeHandle)
    {
        // Documented ambiguously (server-side handles), measured to work
        // from the client handle — the spike result the unit test pins.
        ULONG pid = 0;
        if (!::GetNamedPipeServerProcessId(clientPipeHandle, &pid))
            return std::nullopt;
        return ResolveProcessIdentity(static_cast<uint32_t>(pid));
    }

    std::wstring LocalBuildId()
    {
        return std::format(L"{}#abi{}", ::ShaderLab::VersionString,
            static_cast<unsigned>(SHADERLAB_ENGINE_ABI_VERSION));
    }

    std::wstring DefaultPipeBaseName()
    {
        std::wstring sid = L"nouser";
        HANDLE tok{};
        if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tok))
        {
            BYTE buf[SECURITY_MAX_SID_SIZE + sizeof(TOKEN_USER)]{};
            DWORD len = sizeof(buf);
            if (::GetTokenInformation(tok, TokenUser, buf, len, &len))
            {
                LPWSTR s = nullptr;
                if (::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf)->User.Sid, &s))
                {
                    sid = s;
                    ::LocalFree(s);
                }
            }
            ::CloseHandle(tok);
        }
        return L"ShaderLab.mcp.v1." + sid;
    }

    namespace
    {
        bool UnpackagedFallbackAllowed()
        {
            wchar_t buf[8]{};
            DWORD n = ::GetEnvironmentVariableW(L"SHADERLAB_MCP_ALLOW_UNPACKAGED", buf, ARRAYSIZE(buf));
            return n == 1 && buf[0] == L'1';
        }

        bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b)
        {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (towlower(a[i]) != towlower(b[i])) return false;
            return true;
        }

        // Directory one level up, trailing separator stripped. Used to
        // compare the shared config root of two dev binaries that live in
        // sibling per-project out dirs.
        std::wstring_view ParentDir(std::wstring_view dir)
        {
            while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
                dir.remove_suffix(1);
            auto slash = dir.find_last_of(L"\\/");
            return slash == std::wstring_view::npos ? dir : dir.substr(0, slash);
        }
    }

    PairingVerdict EvaluatePairing(
        const PeerIdentity& self,
        const PeerIdentity& peer,
        std::wstring_view selfBuildId,
        std::wstring_view peerBuildId)
    {
        const bool selfPackaged = self.kind == PeerIdentityKind::Packaged;
        const bool peerPackaged = peer.kind == PeerIdentityKind::Packaged;

        // Mixed packaged/unpackaged is always refused — no env override.
        if (selfPackaged != peerPackaged)
            return PairingVerdict::RefusedMixed;

        if (selfPackaged)
        {
            return self.packageFamilyName == peer.packageFamilyName
                ? PairingVerdict::Accept
                : PairingVerdict::RefusedMismatch;
        }

        // Both unpackaged: dev/CI fallback, explicitly opted into.
        if (!UnpackagedFallbackAllowed())
            return PairingVerdict::RefusedUnpackagedNotAllowed;

        // Build id (version + engine ABI) must match: that is the real
        // "same build tree" signal. Directory match is the belt: accept
        // either the exact same directory (hub + shim are one exe) OR a
        // shared parent directory (sibling per-project out dirs like
        // <cfg>\ShaderLabMcpBroker\ vs <cfg>\ShaderLabHeadless\). The
        // shared-parent relaxation only ever engages behind the env gate,
        // never in an installed (packaged) configuration, which is the
        // whole point of the gate. The strict packaged path above is
        // untouched.
        const bool sameBuild = selfBuildId == peerBuildId && !selfBuildId.empty();
        if (!sameBuild)
            return PairingVerdict::RefusedMismatch;
        const bool sameDir = EqualsIgnoreCase(self.imageDirectory, peer.imageDirectory);
        const bool sameRoot = EqualsIgnoreCase(
            ParentDir(self.imageDirectory), ParentDir(peer.imageDirectory));
        return (sameDir || sameRoot)
            ? PairingVerdict::Accept
            : PairingVerdict::RefusedMismatch;
    }
}
