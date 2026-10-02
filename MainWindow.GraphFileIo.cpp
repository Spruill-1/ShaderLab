// MainWindow partial (Phase 4 split): graph save/load + embedded-media
// archive + heartbeat / stale-temp-dir reaper. All methods are members
// of `winrt::ShaderLab::implementation::MainWindow`. Extracted from
// MainWindow.xaml.cpp at commit c177770 (Phase 3 + earlier health phases).

#include "pch.h"
#include "MainWindow.xaml.h"

#include "Rendering/EffectGraphFile.h"
#include "Effects/ShaderLabEffects.h"
#include "Effects/BytecodeCache.h"
#include "Version.h"

namespace winrt::ShaderLab::implementation
{
    // -----------------------------------------------------------------------
    // Graph save/load
    // -----------------------------------------------------------------------

    void MainWindow::OnSaveGraphClicked(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
    {
        SaveGraphAsync();
    }

    void MainWindow::OnLoadGraphClicked(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
    {
        LoadGraphAsync();
    }

    void MainWindow::OnSaveAccelerator(
        winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args)
    {
        args.Handled(true);
        SaveGraphAsync();
    }

    void MainWindow::OnSaveAsAccelerator(
        winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args)
    {
        args.Handled(true);
        SaveGraphAsAsync();
    }

    void MainWindow::MarkUnsaved()
    {
        if (m_unsavedChanges) return;
        m_unsavedChanges = true;
        RefreshTitleBar();
    }

    void MainWindow::RefreshTitleBar()
    {
        // Title format: "<filename>[*] - ShaderLab <version> (effects lib vN)".
        // The unsaved star is the standard editor convention; the filename
        // is derived from m_currentFilePath if known, else "Untitled". The
        // effect-library version moved here from the status bar to free up
        // room for the FPS readout on the right.
        std::wstring base = m_currentFilePath.empty() ? std::wstring(L"Untitled") :
            std::filesystem::path(m_currentFilePath).filename().wstring();
        auto& lib = ::ShaderLab::Effects::ShaderLabEffects::Instance();
        std::wstring title = base + (m_unsavedChanges ? L"*" : L"")
            + L" - ShaderLab " + ::ShaderLab::VersionString
            + L" (effects lib v" + std::to_wstring(lib.LibraryVersion()) + L")";
        try { Title(winrt::hstring(title)); } catch (...) {}
    }

    bool MainWindow::SaveGraphToCurrentPath()
    {
        if (m_currentFilePath.empty()) return false;

        // Synchronous flavor: used by the close-confirmation dialog
        // which is itself async, so we don't add a second progress
        // dialog. Also used as the bottom-half of the async save.
        try
        {
            // Copy the graph on the render thread, its single writer.
            std::vector<::ShaderLab::Rendering::EffectGraphFile::MediaEntry> media;
            const std::wstring jsonText = ::ShaderLab::Rendering::EffectGraphFile::SerializeForSave(
                m_renderDispatcher.DispatchSync([this] { return m_graph; }), m_embedMedia, media);

            const bool ok = ::ShaderLab::Rendering::EffectGraphFile::Save(
                m_currentFilePath, jsonText, media);
            if (ok)
            {
                m_unsavedChanges = false;
                RefreshTitleBar();
                PipelineFormatText().Text(
                    L"Graph saved: " +
                    winrt::hstring(std::filesystem::path(m_currentFilePath).filename().wstring()));
            }
            else
            {
                PipelineFormatText().Text(L"Error: Failed to save graph");
            }
            return ok;
        }
        catch (...)
        {
            PipelineFormatText().Text(L"Error: Failed to save graph");
            return false;
        }
    }

    winrt::Windows::Foundation::IAsyncAction MainWindow::SaveGraphToCurrentPathAsync()
    {
        // Trivial wrapper kept for symmetry with SaveGraphAsAsync.
        // The synchronous save runs on the calling thread; for the
        // close-confirmation flow that's the UI thread, which is
        // acceptable because the user is staring at a modal dialog.
        auto strong = get_strong();
        SaveGraphToCurrentPath();
        co_return;
    }

    winrt::Windows::Foundation::IAsyncAction MainWindow::RunSaveWithProgressAsync()
    {
        auto strong = get_strong();
        if (m_currentFilePath.empty()) co_return;

        // Build a lightweight progress dialog. The save runs on the UI
        // thread (it's normally a few hundred ms even with embedded
        // media), and the progress callback updates the dialog text
        // synchronously between zip entries. Doing the save on a
        // background thread tripped RPC_E_WRONG_THREAD because the
        // EffectGraphFile::Save path touches non-agile XAML/Storage
        // objects indirectly; keeping it on the UI thread side-steps
        // that entire class of marshalling bug.
        namespace XC = winrt::Microsoft::UI::Xaml::Controls;
        XC::ContentDialog dialog;
        dialog.XamlRoot(this->Content().XamlRoot());
        dialog.Title(winrt::box_value(L"Saving graph"));

        XC::TextBlock statusLine;
        statusLine.Text(L"Preparing\u2026");
        statusLine.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::NoWrap);
        XC::ProgressBar bar;
        bar.IsIndeterminate(false);
        bar.Minimum(0); bar.Maximum(1); bar.Value(0); bar.Width(360);
        XC::StackPanel sp;
        sp.Spacing(8);
        sp.Children().Append(statusLine);
        sp.Children().Append(bar);
        dialog.Content(sp);

        // Show the dialog asynchronously, then yield once so XAML
        // gets a chance to lay it out before kicking off the save on
        // a background thread. Progress callbacks marshal back to the
        // UI thread via DispatcherQueue so the bar actually animates
        // while miniz is compressing media.
        winrt::apartment_context ui_thread;
        auto showOp = m_compileProgress.ShowDialogAsync(dialog);
        co_await winrt::resume_after(std::chrono::milliseconds(16));
        co_await ui_thread;

        // Build media entries + rewritten JSON from a copy of the graph
        // taken on the render thread, then drive a single synchronous save
        // with a progress callback that updates the dialog in-place.
        std::vector<::ShaderLab::Rendering::EffectGraphFile::MediaEntry> media;
        std::wstring jsonText;
        bool serialized = false;
        try
        {
            jsonText = ::ShaderLab::Rendering::EffectGraphFile::SerializeForSave(
                m_renderDispatcher.DispatchSync([this] { return m_graph; }), m_embedMedia, media);
            serialized = true;
        }
        catch (...)
        {
            // Reported below through the status bar, like a failed write.
        }
        if (!serialized)
        {
            m_compileProgress.HideDialog(dialog);
            co_await showOp;
            PipelineFormatText().Text(L"Error: Failed to save graph");
            co_return;
        }

        // Synchronous progress callback: marshal each update back to
        // the UI thread via DispatcherQueue so the dialog actually
        // animates while the background save runs. The callback
        // itself returns immediately -- we don't wait for the marshal
        // to complete (best-effort visual feedback, latest value
        // wins).
        auto dispatcher = this->DispatcherQueue();
        auto progressCb = [dispatcher, statusLine, bar]
            (uint32_t cur, uint32_t total, const std::wstring& msg) -> bool
        {
            dispatcher.TryEnqueue([statusLine, bar, cur, total, msg]() {
                bar.Maximum(static_cast<double>(total));
                bar.Value(static_cast<double>(cur));
                statusLine.Text(winrt::hstring(msg));
            });
            return true;
        };

        // Run the actual save on a threadpool thread. EffectGraphFile::Save
        // is pure native code (file IO + miniz) -- no XAML or WinRT
        // marshalling -- so this is safe. Without this, miniz blocks the
        // UI thread for tens of seconds on large media payloads and the
        // ProgressBar never repaints.
        std::wstring path = m_currentFilePath;
        bool ok = false;
        ::ShaderLab::Rendering::EffectGraphFile::SaveStats stats;
        co_await winrt::resume_background();
        try
        {
            ok = ::ShaderLab::Rendering::EffectGraphFile::Save(
                path, jsonText, media, progressCb, &stats);
        }
        catch (...)
        {
            ok = false;
        }
        co_await ui_thread;

        m_compileProgress.HideDialog(dialog);
        co_await showOp;

        if (ok)
        {
            m_unsavedChanges = false;
            RefreshTitleBar();
            std::wstring detail;
            if (stats.mediaUnchanged > 0)
                detail = L" (" + std::to_wstring(stats.mediaUnchanged) +
                         (stats.mediaUnchanged == 1 ? L" media file unchanged" : L" media files unchanged") +
                         (stats.inPlace ? L", updated in place)" : L", copied)");
            PipelineFormatText().Text(
                L"Graph saved: " +
                winrt::hstring(std::filesystem::path(m_currentFilePath).filename().wstring() + detail));
        }
        else
        {
            PipelineFormatText().Text(L"Error: Failed to save graph");
        }
    }

    winrt::fire_and_forget MainWindow::SaveGraphAsync()
    {
        auto strong = get_strong();
        try
        {
            // If we already have a destination from a previous save / load,
            // overwrite silently -- this is the standard "Ctrl+S" path.
            if (!m_currentFilePath.empty())
            {
                co_await RunSaveWithProgressAsync();
                co_return;
            }
            co_await SaveGraphAsAsync();
        }
        catch (winrt::hresult_error const& e)
        {
            try
            {
                PipelineFormatText().Text(
                    winrt::hstring(L"Save failed: ") + e.message());
            }
            catch (...) {}
        }
        catch (...)
        {
            try { PipelineFormatText().Text(L"Save failed: unknown error"); }
            catch (...) {}
        }
    }

    winrt::Windows::Foundation::IAsyncAction MainWindow::SaveGraphAsAsync()
    {
        auto strong = get_strong();

        winrt::Windows::Storage::Pickers::FileSavePicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(m_hwnd);
        picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::DocumentsLibrary);
        picker.SuggestedFileName(
            m_currentFilePath.empty() ? winrt::hstring(L"graph") :
            winrt::hstring(std::filesystem::path(m_currentFilePath).stem().wstring()));
        picker.FileTypeChoices().Insert(L"ShaderLab Graph",
            winrt::single_threaded_vector<winrt::hstring>({ L".effectgraph" }));

        auto file = co_await picker.PickSaveFileAsync();
        if (!file) co_return;

        m_currentFilePath = std::wstring(file.Path());

        // Count source nodes that reference an external file. If
        // there is at least one, ask the user whether to embed the
        // media. The system FileSavePicker doesn't have a hook for
        // extra options, so we ask via a follow-up ContentDialog.
        // Collect the paths on the render thread; check the disk here.
        const auto sourcePaths = m_renderDispatcher.DispatchSync([this]
        {
            std::vector<std::wstring> paths;
            for (const auto& node : m_graph.Nodes())
            {
                if (node.type == ::ShaderLab::Graph::NodeType::Source &&
                    node.shaderPath.has_value() && !node.shaderPath->empty() &&
                    !node.shaderPath->starts_with(L"media://"))
                    paths.push_back(*node.shaderPath);
            }
            return paths;
        });
        bool hasExternalMedia = false;
        for (const auto& path : sourcePaths)
        {
            if (std::filesystem::exists(path))
            {
                hasExternalMedia = true;
                break;
            }
        }

        if (hasExternalMedia)
        {
            namespace XC = winrt::Microsoft::UI::Xaml::Controls;
            XC::ContentDialog dialog;
            dialog.XamlRoot(this->Content().XamlRoot());
            dialog.Title(winrt::box_value(L"Embed media?"));
            XC::CheckBox cb;
            cb.Content(winrt::box_value(winrt::hstring(
                L"Embed referenced images / videos / ICC files inside the .effectgraph")));
            // Default to last-used preference. We deliberately don't pre-set
            // IsChecked via IReference<bool> -- that path has been fragile in
            // this WinRT version. Users can toggle and we read it back below.
            XC::TextBlock blurb;
            blurb.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap);
            blurb.Opacity(0.7);
            blurb.Margin({ 0, 8, 0, 0 });
            blurb.Text(L"Embedding makes the graph portable -- the recipient won't need the "
                       L"original file paths. Without embedding, the saved graph stores "
                       L"absolute paths that may break on another machine.");
            XC::StackPanel sp;
            sp.Orientation(winrt::Microsoft::UI::Xaml::Controls::Orientation::Vertical);
            sp.Children().Append(cb);
            sp.Children().Append(blurb);
            dialog.Content(sp);
            dialog.PrimaryButtonText(L"Save");
            dialog.CloseButtonText(L"Cancel");
            dialog.DefaultButton(XC::ContentDialogButton::Primary);
            auto result = co_await m_compileProgress.ShowDialogAsync(dialog);
            if (result != XC::ContentDialogResult::Primary)
            {
                m_currentFilePath.clear();
                co_return;
            }
            auto checked = cb.IsChecked();
            m_embedMedia = checked && checked.Value();
        }

        co_await RunSaveWithProgressAsync();
    }

    winrt::fire_and_forget MainWindow::LoadGraphAsync()
    {
        auto strong = get_strong();

        winrt::Windows::Storage::Pickers::FileOpenPicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(m_hwnd);
        picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::DocumentsLibrary);
        picker.FileTypeFilter().Append(L".effectgraph");

        auto file = co_await picker.PickSingleFileAsync();
        if (!file) co_return;

        co_await LoadGraphFromPathAsync(file.Path());
    }

    winrt::Windows::Foundation::IAsyncAction MainWindow::LoadGraphFromPathAsync(winrt::hstring path)
    {
        auto strong = get_strong();
        std::wstring pathStr(path);
        std::wstring versionError;
        std::wstring fileName = std::filesystem::path(pathStr).filename().wstring();

        // Build the progress dialog and run the actual load on a
        // background thread so big media archives don't freeze the UI.
        namespace XC = winrt::Microsoft::UI::Xaml::Controls;
        XC::ContentDialog dialog;
        dialog.XamlRoot(this->Content().XamlRoot());
        dialog.Title(winrt::box_value(L"Loading graph"));
        XC::TextBlock statusLine;
        statusLine.Text(L"Reading\u2026");
        XC::ProgressBar bar;
        bar.Minimum(0); bar.Maximum(1); bar.Value(0); bar.Width(360);
        XC::StackPanel sp;
        sp.Spacing(8);
        sp.Children().Append(statusLine);
        sp.Children().Append(bar);
        dialog.Content(sp);

        auto dispatcher = this->DispatcherQueue();
        std::optional<::ShaderLab::Rendering::EffectGraphFile::LoadResult> loadResult;
        std::wstring loadError;

        // Show the progress dialog, yield once for layout, then run
        // the load on a background thread so miniz inflate doesn't
        // freeze the UI on big media archives.
        winrt::apartment_context ui_thread;
        auto showOp = m_compileProgress.ShowDialogAsync(dialog);
        co_await winrt::resume_after(std::chrono::milliseconds(16));
        co_await ui_thread;

        wchar_t tempBuf[MAX_PATH + 1]{};
        DWORD len = ::GetTempPathW(MAX_PATH, tempBuf);
        std::wstring tempRoot = (len > 0) ? std::wstring(tempBuf, len) : std::wstring(L".\\");

        auto progressCb = [dispatcher, statusLine, bar]
            (uint32_t cur, uint32_t total, const std::wstring& msg) -> bool
        {
            dispatcher.TryEnqueue([statusLine, bar, cur, total, msg]() {
                bar.Maximum(static_cast<double>(total));
                bar.Value(static_cast<double>(cur));
                statusLine.Text(winrt::hstring(msg));
            });
            return true;
        };

        co_await winrt::resume_background();
        try
        {
            auto r = ::ShaderLab::Rendering::EffectGraphFile::Load(pathStr, tempRoot, progressCb);
            if (r.has_value()) loadResult = std::move(r);
            else loadError = L"Could not read graph from .effectgraph";
        }
        catch (const std::exception& ex) { loadError = winrt::to_hstring(ex.what()); }
        catch (...)                       { loadError = L"Unknown load failure"; }
        co_await ui_thread;

        m_compileProgress.HideDialog(dialog);
        co_await showOp;

        if (!loadResult.has_value())
        {
            // Defer the error path through the existing dialog so
            // versioning errors and IO errors look the same to users.
            versionError = loadError.empty() ? L"Failed to load graph" : loadError;
        }
        else
        {
            try
            {
                auto loaded = ::ShaderLab::Graph::EffectGraph::FromJson(
                    winrt::hstring(loadResult->graphJson));

                // Rewrite media:// tokens on source nodes to the
                // extracted temp paths so the live graph can render
                // them through the existing image / video pipeline.
                ::ShaderLab::Rendering::EffectGraphFile::ResolveMediaTokens(loaded, loadResult->mediaMap);

                // On the render thread: we're on the UI thread here, and the
                // worker is the graph's single writer -- releasing its caches
                // or replacing the graph under a live Evaluate is a data race
                // (the same one that crashed the GPU-bindings toggle).
                m_renderDispatcher.DispatchSync([this, &loaded] {
                    m_graphEvaluator.ReleaseCache(m_graph);
                    m_graph = std::move(loaded);
                });
                m_currentFilePath = pathStr;
                m_unsavedChanges = false;
                if (!loadResult->extractDir.empty()
                    && std::filesystem::exists(loadResult->extractDir))
                {
                    m_extractedMediaDirs.push_back(loadResult->extractDir);
                    // Touch heartbeat immediately so a concurrent
                    // instance starting up doesn't reap us.
                    TouchHeartbeats();
                    StartHeartbeatTimer();
                }

                ResetAfterGraphLoad();
                RefreshTitleBar();
                PipelineFormatText().Text(L"Graph loaded: " + winrt::hstring(fileName));
            }
            catch (const std::runtime_error& ex)
            {
                versionError = winrt::to_hstring(ex.what());
            }
            catch (const std::exception& ex)
            {
                PipelineFormatText().Text(L"Load error: " + winrt::to_hstring(ex.what()));
            }
            catch (...)
            {
                PipelineFormatText().Text(L"Error: Failed to load graph");
            }
        }

        if (!versionError.empty())
        {
            auto edialog = winrt::Microsoft::UI::Xaml::Controls::ContentDialog();
            edialog.XamlRoot(this->Content().XamlRoot());
            edialog.Title(winrt::box_value(L"Cannot Open Graph"));
            edialog.Content(winrt::box_value(winrt::hstring(versionError)));
            edialog.CloseButtonText(L"OK");
            co_await m_compileProgress.ShowDialogAsync(edialog);
        }
    }

    winrt::Windows::Foundation::IAsyncOperation<int32_t> MainWindow::PromptUnsavedChangesAsync()
    {
        auto strong = get_strong();
        auto dialog = winrt::Microsoft::UI::Xaml::Controls::ContentDialog();
        dialog.XamlRoot(this->Content().XamlRoot());
        dialog.Title(winrt::box_value(L"Unsaved changes"));
        std::wstring fname = m_currentFilePath.empty() ? std::wstring(L"this graph") :
            std::filesystem::path(m_currentFilePath).filename().wstring();
        dialog.Content(winrt::box_value(winrt::hstring(
            L"You have unsaved changes to " + fname + L". Save before closing?")));
        dialog.PrimaryButtonText(L"Save");
        dialog.SecondaryButtonText(L"Discard");
        dialog.CloseButtonText(L"Cancel");
        dialog.DefaultButton(winrt::Microsoft::UI::Xaml::Controls::ContentDialogButton::Primary);
        auto result = co_await m_compileProgress.ShowDialogAsync(dialog);
        switch (result)
        {
            case winrt::Microsoft::UI::Xaml::Controls::ContentDialogResult::Primary:   co_return 0; // Save
            case winrt::Microsoft::UI::Xaml::Controls::ContentDialogResult::Secondary: co_return 1; // Discard
            default:                                                                   co_return 2; // Cancel
        }
    }

    // ---- Heartbeat / temp-dir reaper ----------------------------------------
    //
    // Every extracted .effectgraph media dir gets a .heartbeat file
    // that we touch every HeartbeatIntervalSec. On startup we scan
    // %TEMP% for ShaderLab-* directories whose .heartbeat (or, if
    // missing, mtime of the dir itself) is older than HeartbeatStaleSec
    // -- those are crash leftovers and we offer to delete them.

    void MainWindow::StartHeartbeatTimer()
    {
        if (m_heartbeatTimer) return;
        m_heartbeatTimer = DispatcherQueue().CreateTimer();
        m_heartbeatTimer.Interval(std::chrono::seconds(HeartbeatIntervalSec));
        m_heartbeatTimer.Tick([this](auto&&, auto&&) { TouchHeartbeats(); });
        m_heartbeatTimer.Start();
    }

    void MainWindow::TouchHeartbeats()
    {
        // Write the current FILETIME into <dir>\.heartbeat. Cheap
        // (one tiny file write per loaded graph, every minute) and
        // resilient to clock skew because we only compare against
        // FILETIMEs from the same machine.
        FILETIME now{};
        ::GetSystemTimeAsFileTime(&now);
        for (const auto& d : m_extractedMediaDirs)
        {
            std::error_code ec;
            if (!std::filesystem::exists(d, ec)) continue;
            std::wstring path = d + L"\\.heartbeat";
            HANDLE h = ::CreateFileW(path.c_str(),
                GENERIC_WRITE, 0, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
            if (h == INVALID_HANDLE_VALUE) continue;
            DWORD written = 0;
            ::WriteFile(h, &now, sizeof(now), &written, nullptr);
            ::CloseHandle(h);
        }
    }

    winrt::fire_and_forget MainWindow::ReapStaleMediaDirsAsync()
    {
        auto strong = get_strong();

        // Snapshot %TEMP% and look for ShaderLab-* directories that
        // either have no .heartbeat or whose heartbeat is older than
        // HeartbeatStaleSec. Anything matching is from a crashed
        // instance (or a previous version that didn't write
        // heartbeats); offer to delete the lot.
        wchar_t tempBuf[MAX_PATH + 1]{};
        DWORD len = ::GetTempPathW(MAX_PATH, tempBuf);
        if (len == 0) co_return;
        std::wstring tempRoot(tempBuf, len);

        std::vector<std::wstring> stale;
        FILETIME nowFt{};
        ::GetSystemTimeAsFileTime(&nowFt);
        const uint64_t now = (static_cast<uint64_t>(nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime;
        const uint64_t staleTicks = static_cast<uint64_t>(HeartbeatStaleSec) * 10'000'000ULL;

        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(tempRoot, ec))
        {
            if (ec) break;
            if (!entry.is_directory(ec)) continue;
            const auto name = entry.path().filename().wstring();
            if (!name.starts_with(L"ShaderLab-")) continue;

            // Determine the dir's "last touched" time. Prefer the
            // .heartbeat file's mtime; fall back to the directory's
            // own mtime so directories from older builds (which
            // didn't write heartbeats) still get reaped.
            FILETIME ft{};
            std::wstring beat = entry.path().wstring() + L"\\.heartbeat";
            HANDLE h = ::CreateFileW(beat.c_str(),
                GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_HIDDEN, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                ::GetFileTime(h, nullptr, nullptr, &ft);
                ::CloseHandle(h);
            }
            else
            {
                WIN32_FILE_ATTRIBUTE_DATA fad{};
                if (::GetFileAttributesExW(entry.path().c_str(), GetFileExInfoStandard, &fad))
                    ft = fad.ftLastWriteTime;
            }
            const uint64_t touched = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
            if (touched == 0 || (now > touched && (now - touched) > staleTicks))
                stale.push_back(entry.path().wstring());
        }

        if (stale.empty()) co_return;

        namespace XC = winrt::Microsoft::UI::Xaml::Controls;
        XC::ContentDialog dialog;
        dialog.XamlRoot(this->Content().XamlRoot());
        dialog.Title(winrt::box_value(L"Clean up old graph media?"));

        std::wstring msg = std::format(
            L"Found {} ShaderLab media folder{} in your %TEMP% from a "
            L"previous session that didn't shut down cleanly. They are "
            L"only useful while the graph that produced them is open.\n\nDelete them now?",
            stale.size(), stale.size() == 1 ? L"" : L"s");
        XC::TextBlock blurb;
        blurb.Text(winrt::hstring(msg));
        blurb.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap);
        dialog.Content(blurb);
        dialog.PrimaryButtonText(L"Delete");
        dialog.CloseButtonText(L"Keep");
        dialog.DefaultButton(XC::ContentDialogButton::Primary);

        auto result = co_await m_compileProgress.ShowDialogAsync(dialog);
        if (result != XC::ContentDialogResult::Primary) co_return;

        co_await winrt::resume_background();
        std::error_code rmEc;
        for (const auto& d : stale)
            std::filesystem::remove_all(d, rmEc);
    }

    // ---- Status-bar broom (Phase 8 p8-status-bar-button) -------------------
    //
    // Single-click runs both reapers (graph-temp media dirs + shader
    // bytecode cache) and reports freed bytes in ReaperStatusText.
    // Runs on a background thread so the UI doesn't stall on a slow
    // filesystem walk.

    winrt::fire_and_forget MainWindow::OnReaperBroomClicked(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
    {
        auto strong = get_strong();
        auto dispatcher = DispatcherQueue();

        // Disable the button + show "Sweeping…" while we work.
        ReaperBroomButton().IsEnabled(false);
        ReaperStatusText().Text(L"Sweeping…");

        co_await winrt::resume_background();

        // ---- Reap graph-temp media dirs --------------------------------
        size_t graphDirsDeleted = 0;
        size_t graphBytesFreed  = 0;
        {
            wchar_t tempBuf[MAX_PATH + 1]{};
            DWORD len = ::GetTempPathW(MAX_PATH, tempBuf);
            if (len > 0)
            {
                std::wstring tempRoot(tempBuf, len);
                FILETIME nowFt{};
                ::GetSystemTimeAsFileTime(&nowFt);
                const uint64_t now = (static_cast<uint64_t>(nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime;
                const uint64_t staleTicks = static_cast<uint64_t>(HeartbeatStaleSec) * 10'000'000ULL;

                std::error_code ec;
                std::vector<std::pair<std::wstring, uint64_t>> victims;
                for (const auto& entry : std::filesystem::directory_iterator(tempRoot, ec))
                {
                    if (ec) break;
                    if (!entry.is_directory(ec)) continue;
                    const auto name = entry.path().filename().wstring();
                    if (!name.starts_with(L"ShaderLab-")) continue;

                    FILETIME ft{};
                    std::wstring beat = entry.path().wstring() + L"\\.heartbeat";
                    HANDLE h = ::CreateFileW(beat.c_str(),
                        GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_HIDDEN, nullptr);
                    if (h != INVALID_HANDLE_VALUE)
                    {
                        ::GetFileTime(h, nullptr, nullptr, &ft);
                        ::CloseHandle(h);
                    }
                    else
                    {
                        WIN32_FILE_ATTRIBUTE_DATA fad{};
                        if (::GetFileAttributesExW(entry.path().c_str(), GetFileExInfoStandard, &fad))
                            ft = fad.ftLastWriteTime;
                    }
                    const uint64_t touched = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
                    if (touched == 0 || (now > touched && (now - touched) > staleTicks))
                    {
                        // Sum the dir's contents for the freed-bytes display.
                        uint64_t bytes = 0;
                        for (auto it = std::filesystem::recursive_directory_iterator(entry.path(), ec);
                             it != std::filesystem::recursive_directory_iterator(); ++it)
                        {
                            if (ec) { ec.clear(); continue; }
                            if (it->is_regular_file(ec))
                                bytes += static_cast<uint64_t>(it->file_size(ec));
                        }
                        victims.emplace_back(entry.path().wstring(), bytes);
                    }
                }

                std::error_code rmEc;
                for (const auto& [path, bytes] : victims)
                {
                    std::filesystem::remove_all(path, rmEc);
                    if (!rmEc)
                    {
                        ++graphDirsDeleted;
                        graphBytesFreed += bytes;
                    }
                    rmEc.clear();
                }
            }
        }

        // ---- Reap shader bytecode cache --------------------------------
        // Use a 7-day threshold for the broom (less aggressive than
        // ClearDisk; preserves recently-used compiles). Engine
        // already configured the disk root via ConfigureBytecodeCache
        // at startup.
        constexpr uint64_t kBroomShaderStaleSec = 7ull * 24 * 60 * 60;
        auto shaderStats = ::ShaderLab::Effects::BytecodeCache::Instance()
            .ReapDisk(kBroomShaderStaleSec);

        // ---- Format status string + return to UI thread ---------------
        const uint64_t totalBytes = graphBytesFreed + shaderStats.bytesFreed;
        std::wstring summary;
        if (totalBytes == 0 && graphDirsDeleted == 0 && shaderStats.filesDeleted == 0)
        {
            summary = L"Cache clean";
        }
        else
        {
            // Format bytes as "1.2 MB" or "534 KB".
            wchar_t bytesBuf[64]{};
            if (totalBytes >= 1024ull * 1024)
                swprintf_s(bytesBuf, L"%.1f MB", totalBytes / (1024.0 * 1024.0));
            else if (totalBytes >= 1024)
                swprintf_s(bytesBuf, L"%.1f KB", totalBytes / 1024.0);
            else
                swprintf_s(bytesBuf, L"%llu B", static_cast<unsigned long long>(totalBytes));
            summary = std::format(
                L"Freed {} \u00B7 {} graph dir{}, {} shader variant{}",
                bytesBuf,
                graphDirsDeleted, graphDirsDeleted == 1 ? L"" : L"s",
                shaderStats.filesDeleted, shaderStats.filesDeleted == 1 ? L"" : L"s");
        }

        // UI update via TryEnqueue (WinUI 3's DispatcherQueue isn't
        // compatible with winrt::resume_foreground, which targets
        // Windows.System.DispatcherQueue from the Windows SDK).
        winrt::hstring summaryH(summary);
        winrt::hstring tooltipH = winrt::hstring(L"Last sweep: " + summary +
            L"\n(Click to sweep again. Default thresholds: 150s heartbeat for graph temps, 7 days for shader cache.)");
        dispatcher.TryEnqueue([strong, summaryH, tooltipH]()
        {
            strong->ReaperStatusText().Text(summaryH);
            strong->ReaperBroomButton().IsEnabled(true);
            winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
                strong->ReaperBroomButton(), winrt::box_value(tooltipH));
        });

        // Auto-clear the inline label after 5 seconds (the tooltip
        // keeps the result for as long as the user wants it).
        co_await winrt::resume_after(std::chrono::seconds(5));
        dispatcher.TryEnqueue([strong]()
        {
            // Only clear if the user hasn't kicked off another sweep in
            // the meantime (button enabled = no in-flight work).
            if (strong->ReaperBroomButton().IsEnabled())
                strong->ReaperStatusText().Text(L"");
        });
    }

    void MainWindow::ResetAfterGraphLoad(bool reopenOutputWindows)
    {
        m_previewNodeId = 0;
        m_previewViews.clear();
        m_traceActive = false;
        m_lastTraceTopologyHash = 0;
        m_traceRowCache.clear();

        // Close all existing output windows.
        m_outputWindows.clear();

        // Restore isClock flag from ShaderLab effect descriptors BEFORE
        // SetGraph triggers a layout pass. The flag isn't serialized in
        // JSON (it's derived from the effect definition), so a freshly-
        // deserialized Clock node has isClock=false. RebuildLayout reads
        // node.isClock to decide whether to allocate space for the
        // play/pause button + progress bar; if it runs first the visual
        // ends up sized as a regular parameter node and the controls
        // never appear until something else triggers another layout.
        auto resetGraph = [this]
        {
            ::ShaderLab::Effects::ShaderLabEffects::RestoreRuntimeFlags(m_graph);
            m_nodeGraphController.SetGraph(&m_graph);
            m_graph.MarkAllDirty();
        };
        // The graph is written on the render thread. The adapter switch calls
        // this with the worker stopped, when there is no render thread to use.
        if (m_renderWorker.joinable())
        {
            m_renderDispatcher.DispatchSync(resetGraph);
        }
        else
        {
            resetGraph();
            PublishGraphSnapshot();
        }
        PopulatePreviewNodeSelector();

        // Reset trace UI.
        PixelTracePanel().Children().Clear();
        TracePositionText().Text(L"Click preview to trace a pixel");

        // Defer FitPreviewToView until after the first evaluation
        // produces valid cachedOutput (image bounds aren't available yet).
        m_needsFitPreview = true;
        UpdateStatusBar();

        // Reopen output windows for all Output nodes in the loaded graph.
        if (reopenOutputWindows)
        {
            if (const auto snapshot = CurrentGraphSnapshot())
            {
                for (const auto& node : snapshot->nodes)
                {
                    if (node.type != ::ShaderLab::Graph::NodeType::Output) continue;
                    try { OpenOutputWindow(node.id); } catch (...) {}
                }
            }
        }
    }

}

