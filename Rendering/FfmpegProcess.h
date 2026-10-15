#pragma once

#include "pch_engine.h"

#include <mutex>
#include <string>
#include <thread>

namespace ShaderLab::Rendering
{
    // `s` quoted for a Windows command line when it contains a space, tab or
    // quote; unchanged otherwise.
    std::wstring QuoteCommandArgument(const std::wstring& s);

    // An ffmpeg child process. stdin carries raw frames; stdout and stderr
    // share one pipe that a reader thread drains CONTINUOUSLY. Leaving it
    // undrained deadlocks an export: ffmpeg blocks writing its log, stops
    // reading stdin, and the frame write blocks forever.
    class FfmpegProcess
    {
    public:
        FfmpegProcess() = default;
        ~FfmpegProcess() { Abort(); }
        FfmpegProcess(const FfmpegProcess&) = delete;
        FfmpegProcess& operator=(const FfmpegProcess&) = delete;

        // Starts `exe args`; `withStdin` opens a pipe for Write.
        bool Start(const std::wstring& exe, const std::wstring& args, bool withStdin, std::wstring& error);

        // False on a broken pipe: ffmpeg exited, and Log says why.
        bool Write(const void* data, size_t size);

        // Closes stdin (end of stream) and waits for exit. Returns the exit
        // code, or -1 on timeout (the process is then terminated).
        int Finish(DWORD timeoutMs);

        void Abort();

        // Everything ffmpeg wrote so far (the newest 128-256 KB).
        std::string Log();

    private:
        HANDLE m_process{ nullptr };
        HANDLE m_stdinWrite{ nullptr };
        HANDLE m_outRead{ nullptr };
        std::thread m_reader;
        std::mutex m_logMutex;
        std::string m_log;
    };
}
