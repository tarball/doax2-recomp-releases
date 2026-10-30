#include "ui/window.h"

#include "log.h"
#include "settings.h"
#include "util/image.h"

#include <mutex>

namespace ui
{
    namespace
    {
        constexpr UINT kSetTitleMessage = WM_APP + 1;      // lParam: heap-allocated std::wstring
        constexpr UINT kSetFullscreenMessage = WM_APP + 2; // wParam: fullscreen
        std::atomic<HWND> g_gameWindow{ nullptr };
        // The title as last set and the status shown after it.
        std::mutex g_titleLock;
        std::string g_title, g_titleStatus;

        void PostTitle(HWND window, const std::string& title, const std::string& status)
        {
            const std::string text = status.empty() ? title : title + " - " + status;
            PostMessageW(window, kSetTitleMessage, 0, reinterpret_cast<LPARAM>(new std::wstring(text.begin(), text.end())));
        }

        // An icon of `size` x `size` pixels from an image file in memory (its alpha as the icon's), or nullptr.
        HICON MakeIcon(const std::vector<uint8_t>& image, uint32_t size)
        {
            uint32_t width = size, height = size;
            std::vector<uint8_t> bgra;
            if (!util::DecodeImage(image.data(), image.size(), width, height, false, bgra))
                return nullptr;
            BITMAPV5HEADER header{};
            header.bV5Size = sizeof(header);
            header.bV5Width = LONG(size);
            header.bV5Height = -LONG(size); // top-down
            header.bV5Planes = 1;
            header.bV5BitCount = 32;
            header.bV5Compression = BI_BITFIELDS;
            header.bV5RedMask = 0x00FF0000;
            header.bV5GreenMask = 0x0000FF00;
            header.bV5BlueMask = 0x000000FF;
            header.bV5AlphaMask = 0xFF000000;
            void* bits = nullptr;
            const HDC screen = GetDC(nullptr);
            const HBITMAP color = CreateDIBSection(screen, reinterpret_cast<const BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
            ReleaseDC(nullptr, screen);
            if (!color)
                return nullptr;
            memcpy(bits, bgra.data(), bgra.size());
            // The mask is all clear: the color bitmap's alpha decides (rows of 1-bit pixels, padded to 16 bits).
            const std::vector<uint8_t> clear(size_t((size + 15) / 16 * 2) * size, 0);
            const HBITMAP mask = CreateBitmap(int(size), int(size), 1, 1, clear.data());
            ICONINFO info{ TRUE, 0, 0, mask, color };
            const HICON icon = mask ? CreateIconIndirect(&info) : nullptr;
            DeleteObject(color);
            if (mask)
                DeleteObject(mask);
            return icon;
        }
    }

    void Window::SetIcon(std::vector<uint8_t> image)
    {
        m_iconImage = std::move(image);
        if (m_hwnd)
            ApplyIcon(GetDpiForWindow(m_hwnd));
    }

    void Window::ApplyIcon(uint32_t dpi)
    {
        if (m_iconImage.empty() || !m_hwnd)
            return;
        const uint32_t sizes[2] = { uint32_t(GetSystemMetricsForDpi(SM_CXICON, dpi)), uint32_t(GetSystemMetricsForDpi(SM_CXSMICON, dpi)) };
        HICON icons[2] = { MakeIcon(m_iconImage, sizes[0]), MakeIcon(m_iconImage, sizes[1]) };
        if (!icons[0] || !icons[1])
        {
            LOG_WARN("Window: the game's image can't be made into icons");
            for (const HICON icon : icons)
            {
                if (icon)
                    DestroyIcon(icon);
            }
            return;
        }
        SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icons[0]));
        SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icons[1]));
        for (int i = 0; i < 2; i++)
        {
            if (m_icons[i])
                DestroyIcon(m_icons[i]);
            m_icons[i] = icons[i];
        }
        LOG_INFO("Window: the game's image as its icons, %ux%u and %ux%u pixels (%u DPI)", sizes[0], sizes[0], sizes[1], sizes[1], dpi);
    }

    void SetTitleStatus(const std::string& status)
    {
        std::lock_guard lock(g_titleLock);
        if (status == g_titleStatus)
            return;
        g_titleStatus = status;
        if (const HWND window = g_gameWindow.load(); window && !g_title.empty())
            PostTitle(window, g_title, g_titleStatus);
    }

    bool GameWindowFocused()
    {
        const HWND window = g_gameWindow.load();
        return window && GetForegroundWindow() == window;
    }

    bool CloseGameWindow()
    {
        const HWND window = g_gameWindow.load();
        return window && PostMessageW(window, WM_CLOSE, 0, 0);
    }

    bool Window::Create(const wchar_t* title, uint32_t width, uint32_t height, bool fullscreen)
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        windowClass.lpszClassName = L"DOA4Recomp";
        RegisterClassExW(&windowClass);

        // Size the window so the client area is width x height (pixels: the process is DPI-aware).
        RECT rect{ 0, 0, LONG(width), LONG(height) };
        AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForSystem());
        m_hwnd = CreateWindowExW(0, windowClass.lpszClassName, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, windowClass.hInstance, this);
        if (!m_hwnd)
            return false;

        m_clientWidth = width;
        m_clientHeight = height;
        {
            std::lock_guard lock(g_titleLock);
            for (const wchar_t* c = title; *c; c++)
                g_title += char(*c < 0x80 ? *c : '?');
            if (!g_titleStatus.empty())
                PostTitle(m_hwnd, g_title, g_titleStatus);
        }
        g_gameWindow = m_hwnd;
        ApplyIcon(GetDpiForWindow(m_hwnd));
        if (fullscreen)
            ApplyFullscreen(true);
        ShowWindow(m_hwnd, SW_SHOW);
        return true;
    }

    void Window::SetFullscreen(bool fullscreen)
    {
        if (m_hwnd)
            PostMessageW(m_hwnd, kSetFullscreenMessage, fullscreen, 0);
    }

    void Window::ApplyFullscreen(bool fullscreen)
    {
        if (fullscreen == m_fullscreen)
            return;
        const LONG style = GetWindowLongW(m_hwnd, GWL_STYLE);
        if (fullscreen)
        {
            if (IsIconic(m_hwnd))
                ShowWindow(m_hwnd, SW_RESTORE);
            MONITORINFO monitor{ sizeof(monitor) };
            if (!GetWindowPlacement(m_hwnd, &m_windowed) || !GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTOPRIMARY), &monitor))
                return;
            if (m_windowed.showCmd != SW_SHOWMAXIMIZED)
                m_windowed.showCmd = SW_SHOWNORMAL; // (a window not shown yet has SW_HIDE)
            // No frame, covering the whole monitor (the taskbar too).
            SetWindowLongW(m_hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            const RECT& area = monitor.rcMonitor;
            SetWindowPos(m_hwnd, HWND_TOP, area.left, area.top, area.right - area.left, area.bottom - area.top,
                SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
        else
        {
            SetWindowLongW(m_hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
            SetWindowPlacement(m_hwnd, &m_windowed);
            SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
        m_fullscreen = fullscreen;
    }

    bool Window::RunMessageLoop(HANDLE waitHandle)
    {
        for (;;)
        {
            const DWORD result = MsgWaitForMultipleObjects(waitHandle ? 1 : 0, &waitHandle, FALSE, INFINITE, QS_ALLINPUT);
            if (waitHandle && result == WAIT_OBJECT_0)
                return false;

            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT)
                    return true;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }

    void Window::SetTitle(const std::string& title)
    {
        std::lock_guard lock(g_titleLock);
        g_title = title;
        if (m_hwnd)
            PostTitle(m_hwnd, g_title, g_titleStatus);
    }

    LRESULT CALLBACK Window::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE)
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        auto* window = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (message)
        {
        case WM_SIZE:
            if (window && wParam != SIZE_MINIMIZED)
            {
                window->m_clientWidth = LOWORD(lParam);
                window->m_clientHeight = HIWORD(lParam);
            }
            return 0;
        case kSetTitleMessage:
        {
            auto* title = reinterpret_cast<std::wstring*>(lParam);
            SetWindowTextW(hwnd, title->c_str());
            delete title;
            return 0;
        }
        case kSetFullscreenMessage:
            if (window)
                window->ApplyFullscreen(wParam != 0);
            return 0;
        case WM_SYSKEYDOWN:
            // Alt+Enter switches between windowed and fullscreen: the setting, which SETTINGS > VIDEO shows.
            if (window && wParam == VK_RETURN && (HIWORD(lParam) & KF_ALTDOWN) && !(HIWORD(lParam) & KF_REPEAT))
            {
                settings::Graphics graphics = settings::GetGraphics();
                graphics.fullscreen = !window->m_fullscreen;
                settings::SetGraphics(graphics);
                settings::Save();
                window->ApplyFullscreen(graphics.fullscreen);
                return 0;
            }
            break;
        case WM_SYSCHAR:
            if (wParam == VK_RETURN)
                return 0; // (Alt+Enter would beep)
            break;
        case WM_SETCURSOR:
            // No mouse pointer over the game in fullscreen.
            if (window && window->m_fullscreen && LOWORD(lParam) == HTCLIENT)
            {
                SetCursor(nullptr);
                return TRUE;
            }
            break;
        case WM_DPICHANGED:
            // Moved to a monitor with another scale: keep the window's size there, as Windows suggests, and its icons
            // at the sizes it uses there.
            if (window && !window->m_fullscreen)
            {
                const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                    suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            if (window)
                window->ApplyIcon(HIWORD(wParam));
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}
