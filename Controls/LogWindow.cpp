#include "pch.h"
#include "LogWindow.h"

namespace ShaderLab::Controls
{
    LogWindow::~LogWindow()
    {
        Close();
    }

    void LogWindow::Create(uint32_t nodeId, const std::wstring& nodeName)
    {
        if (m_isOpen) return;

        m_nodeId = nodeId;
        m_lastSeq = 0;
        m_isOpen = true;

        m_window = winrt::Microsoft::UI::Xaml::Window();
        m_window.Title(winrt::hstring(std::format(L"Logs — {} (Node {})", nodeName, nodeId)));

        // Build layout: toolbar + scrollable log view.
        auto root = winrt::Microsoft::UI::Xaml::Controls::Grid();
        auto rows = root.RowDefinitions();
        auto topRow = winrt::Microsoft::UI::Xaml::Controls::RowDefinition();
        topRow.Height(winrt::Microsoft::UI::Xaml::GridLengthHelper::Auto());
        rows.Append(topRow);
        auto contentRow = winrt::Microsoft::UI::Xaml::Controls::RowDefinition();
        contentRow.Height(winrt::Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
            1.0, winrt::Microsoft::UI::Xaml::GridUnitType::Star));
        rows.Append(contentRow);

        // Toolbar with Clear and Copy buttons.
        auto toolbar = winrt::Microsoft::UI::Xaml::Controls::StackPanel();
        toolbar.Orientation(winrt::Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
        toolbar.Padding({ 8, 4, 8, 4 });
        toolbar.Spacing(8);
        toolbar.Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(
            winrt::Windows::UI::Color{ 255, 32, 32, 32 }));

        auto clearBtn = winrt::Microsoft::UI::Xaml::Controls::Button();
        clearBtn.Content(winrt::box_value(L"Clear"));
        clearBtn.Click([this](auto&&, auto&&) {
            if (m_logView)
                m_logView.Blocks().Clear();
            m_lastSeq = UINT64_MAX; // skip all existing entries on next update
        });
        toolbar.Children().Append(clearBtn);

        winrt::Microsoft::UI::Xaml::Controls::Grid::SetRow(toolbar, 0);
        root.Children().Append(toolbar);

        // Scrollable log view.
        m_scrollViewer = winrt::Microsoft::UI::Xaml::Controls::ScrollViewer();
        m_scrollViewer.VerticalScrollBarVisibility(
            winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::Auto);
        m_scrollViewer.Padding({ 8, 4, 8, 4 });

        m_logView = winrt::Microsoft::UI::Xaml::Controls::RichTextBlock();
        m_logView.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Cascadia Mono, Consolas, Courier New"));
        m_logView.FontSize(12);
        m_logView.IsTextSelectionEnabled(true);
        m_logView.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap);

        m_scrollViewer.Content(m_logView);
        winrt::Microsoft::UI::Xaml::Controls::Grid::SetRow(m_scrollViewer, 1);
        root.Children().Append(m_scrollViewer);

        root.RequestedTheme(winrt::Microsoft::UI::Xaml::ElementTheme::Dark);
        m_window.Content(root);

        // Track window close.
        m_closedToken = m_window.Closed([this](auto&&, auto&&) {
            m_isOpen = false;
            m_window = nullptr;
            m_logView = nullptr;
            m_scrollViewer = nullptr;
        });

        // Resize and activate.
        if (auto appWin = m_window.AppWindow())
        {
            appWin.Resize({ 700, 400 });
        }
        m_window.Activate();
    }

    void LogWindow::Update(const NodeLog& log)
    {
        if (!m_isOpen || !m_logView) return;

        bool added = false;
        for (const auto& entry : log.Entries())
        {
            if (entry.sequence <= m_lastSeq)
                continue;
            m_lastSeq = entry.sequence;
            AppendEntry(entry);
            added = true;
        }

        // Auto-scroll to bottom.
        if (added && m_scrollViewer)
        {
            m_scrollViewer.UpdateLayout();
            m_scrollViewer.ChangeView(nullptr,
                m_scrollViewer.ScrollableHeight(),
                nullptr);
        }
    }

    void LogWindow::AppendEntry(const NodeLogEntry& entry)
    {
        if (!m_logView) return;

        auto para = winrt::Microsoft::UI::Xaml::Documents::Paragraph();
        para.Margin({ 0, 0, 0, 2 });

        // Timestamp run.
        auto timeRun = winrt::Microsoft::UI::Xaml::Documents::Run();
        auto tt = std::chrono::system_clock::to_time_t(entry.timestamp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            entry.timestamp.time_since_epoch()).count() % 1000;
        struct tm tm_buf{};
        localtime_s(&tm_buf, &tt);
        wchar_t timeBuf[32]{};
        swprintf_s(timeBuf, L"%02d:%02d:%02d.%03d",
            tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, static_cast<int>(ms));
        timeRun.Text(timeBuf);
        timeRun.Foreground(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(
            winrt::Windows::UI::Color{ 255, 128, 128, 128 }));
        para.Inlines().Append(timeRun);

        // Level badge.
        auto levelRun = winrt::Microsoft::UI::Xaml::Documents::Run();
        winrt::Windows::UI::Color levelColor{};
        switch (entry.level)
        {
        case LogLevel::Info:
            levelRun.Text(L" INFO ");
            levelColor = { 255, 100, 180, 255 };
            break;
        case LogLevel::Warning:
            levelRun.Text(L" WARN ");
            levelColor = { 255, 255, 200, 60 };
            break;
        case LogLevel::Error:
            levelRun.Text(L" ERR  ");
            levelColor = { 255, 255, 80, 80 };
            break;
        }
        levelRun.Foreground(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(levelColor));
        para.Inlines().Append(levelRun);

        // Message.
        auto msgRun = winrt::Microsoft::UI::Xaml::Documents::Run();
        msgRun.Text(winrt::hstring(entry.message));
        para.Inlines().Append(msgRun);

        m_logView.Blocks().Append(para);
    }

    void LogWindow::Close()
    {
        if (m_isOpen && m_window)
        {
            try { m_window.Close(); } catch (...) {}
        }
        m_isOpen = false;
        m_window = nullptr;
        m_logView = nullptr;
        m_scrollViewer = nullptr;
    }
}
