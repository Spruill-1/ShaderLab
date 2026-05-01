#pragma once

#include "App.xaml.g.h"

namespace winrt::ShaderLab::implementation
{
    struct App : AppT<App>
    {
        App();
        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

    private:
        void RunCliMode(const std::wstring& cmdLine, int devicePref);

    private:
        winrt::Microsoft::UI::Xaml::Window window{ nullptr };
    };
}
