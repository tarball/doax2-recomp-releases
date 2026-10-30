#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>

namespace ui
{
    // The game window. Created and pumped on the host main thread; other threads only read its size
    // and post title updates and display mode changes.
    class Window
    {
    public:
        // `width` x `height` is the client size when windowed (in pixels); `fullscreen` starts in borderless fullscreen.
        bool Create(const wchar_t* title, uint32_t width, uint32_t height, bool fullscreen = false);
        HWND Handle() const { return m_hwnd; }
        // The window's icon, in its title bar and the taskbar, from an image file in memory (the game's own image, a PNG,
        // read from the player's game at run time): made at the sizes Windows uses at the window's scale, and again as
        // the window moves to a monitor with another. Before Create, or on the window's thread after it.
        void SetIcon(std::vector<uint8_t> image);

        // Pumps messages until the window is closed (returns true) or `waitHandle` is signaled (returns false).
        bool RunMessageLoop(HANDLE waitHandle);

        // Thread-safe.
        void SetTitle(const std::string& title);
        // Borderless fullscreen (the window covers its monitor, without a frame) or windowed (the frame, position and
        // size it had before). Thread-safe: applied on the window's thread, whose WM_SIZE the presenter follows.
        void SetFullscreen(bool fullscreen);
        uint32_t ClientWidth() const { return m_clientWidth; }
        uint32_t ClientHeight() const { return m_clientHeight; }

    private:
        static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
        // On the window's thread.
        void ApplyFullscreen(bool fullscreen);
        // The icons for a DPI, from m_iconImage.
        void ApplyIcon(uint32_t dpi);

        HWND m_hwnd = nullptr;
        std::vector<uint8_t> m_iconImage;
        HICON m_icons[2] = {}; // big and small, as last set
        std::atomic<uint32_t> m_clientWidth{ 0 };
        std::atomic<uint32_t> m_clientHeight{ 0 };
        bool m_fullscreen = false;                           // (window thread)
        WINDOWPLACEMENT m_windowed{ sizeof(WINDOWPLACEMENT) }; // where fullscreen goes back to
    };

    // Whether the game window exists and has keyboard focus (keyboard input is only read then).
    bool GameWindowFocused();
    // A status shown after the game window's title (empty: none), such as a netplay session's. Thread-safe.
    void SetTitleStatus(const std::string& status);
    // Closes the game window as its player would (the game then ends: main.cpp). False if there's none (HEADLESS).
    // Thread-safe.
    bool CloseGameWindow();
}
