#include "pch_engine.h"
#include "FfmpegProcess.h"

namespace ShaderLab::Rendering
{
    std::wstring QuoteCommandArgument(const std::wstring& s)
    {
        if (s.find_first_of(L" \t\"") == std::wstring::npos) return s;
        std::wstring out = L"\"";
        for (wchar_t c : s) { if (c == L'"') out += L'\\'; out += c; }
        return out + L"\"";
    }

    bool FfmpegProcess::Start(const std::wstring& exe, const std::wstring& args, bool withStdin, std::wstring& error)
    {
        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        HANDLE outRead = nullptr, outWrite = nullptr, inRead = nullptr;
        if (!CreatePipe(&outRead, &outWrite, &sa, 0))
        {
            error = L"CreatePipe(stdout) failed";
            return false;
        }
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
        if (withStdin)
        {
            if (!CreatePipe(&inRead, &m_stdinWrite, &sa, 1 << 20))
            {
                CloseHandle(outRead); CloseHandle(outWrite);
                error = L"CreatePipe(stdin) failed";
                return false;
            }
            SetHandleInformation(m_stdinWrite, HANDLE_FLAG_INHERIT, 0);
        }
        STARTUPINFOW si{ sizeof(si) };
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = withStdin ? inRead : GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = outWrite;
        si.hStdError = outWrite;
        PROCESS_INFORMATION pi{};
        std::wstring cmd = QuoteCommandArgument(exe) + L" " + args;
        const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        // The child owns its ends now; ours must close or EOF never arrives.
        CloseHandle(outWrite);
        if (inRead) CloseHandle(inRead);
        if (!ok)
        {
            const DWORD e = GetLastError();
            CloseHandle(outRead);
            if (m_stdinWrite) { CloseHandle(m_stdinWrite); m_stdinWrite = nullptr; }
            error = std::format(L"could not start ffmpeg ({}): Win32 error {}", exe, e);
            return false;
        }
        CloseHandle(pi.hThread);
        m_process = pi.hProcess;
        m_outRead = outRead;
        m_reader = std::thread([this] {
            char buf[4096];
            DWORD n = 0;
            while (ReadFile(m_outRead, buf, sizeof(buf), &n, nullptr) && n > 0)
            {
                std::scoped_lock lock(m_logMutex);
                m_log.append(buf, n);
                if (m_log.size() > 256 * 1024) m_log.erase(0, m_log.size() - 128 * 1024);
            }
        });
        return true;
    }

    bool FfmpegProcess::Write(const void* data, size_t size)
    {
        const auto* p = static_cast<const uint8_t*>(data);
        while (size > 0)
        {
            DWORD chunk = static_cast<DWORD>((std::min)(size, size_t(1) << 24));
            DWORD written = 0;
            if (!WriteFile(m_stdinWrite, p, chunk, &written, nullptr) || written == 0)
                return false;
            p += written;
            size -= written;
        }
        return true;
    }

    int FfmpegProcess::Finish(DWORD timeoutMs)
    {
        if (m_stdinWrite) { CloseHandle(m_stdinWrite); m_stdinWrite = nullptr; }
        int code = -1;
        if (m_process)
        {
            if (WaitForSingleObject(m_process, timeoutMs) == WAIT_OBJECT_0)
            {
                DWORD c = 0;
                GetExitCodeProcess(m_process, &c);
                code = static_cast<int>(c);
            }
            else
            {
                TerminateProcess(m_process, 1);
                WaitForSingleObject(m_process, 5000);
            }
            CloseHandle(m_process);
            m_process = nullptr;
        }
        if (m_reader.joinable()) m_reader.join();
        if (m_outRead) { CloseHandle(m_outRead); m_outRead = nullptr; }
        return code;
    }

    void FfmpegProcess::Abort()
    {
        if (m_process) TerminateProcess(m_process, 1);
        Finish(5000);
    }

    std::string FfmpegProcess::Log()
    {
        std::scoped_lock lock(m_logMutex);
        return m_log;
    }
}
