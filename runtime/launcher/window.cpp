// The launcher's window (window.h). Plain Win32 controls in a window laid out at its monitor's scale (per-monitor DPI
// aware, from the manifest): a white area with what's going on and the choices, and a band with the buttons. The
// checks run on a thread of their own (Prepare), which reports to the window by messages.
#include "launcher/window.h"

#include <algorithm>
#include <mutex>
#include <thread>

#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <shobjidl.h>

namespace launcher
{
    namespace
    {
        constexpr wchar_t kClassName[] = L"" GAME_PREFIX "Launcher";
        constexpr wchar_t kTitle[] = L"" GAME_TITLE " Launcher";
        constexpr UINT kReportMessage = WM_APP + 1, kDoneMessage = WM_APP + 2;
        constexpr UINT_PTR kShowTimer = 1;
        constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN, kExStyle = WS_EX_CONTROLPARENT;
        // The heading's color: Windows' for a dialog's main instruction.
        constexpr COLORREF kHeadingColor = RGB(0x00, 0x33, 0x99), kNoteColor = RGB(0x5F, 0x5F, 0x5F);

        const wchar_t kNote[] = L"Both players must choose the same. Rollback plays better over the internet; lockstep waits for both players' "
                                L"moves every frame.\n\nOnly play with people you trust, on a network you trust: netplay hasn't had a "
                                L"security review. Over the internet, the host's UDP port must be forwarded to the host's PC.";

        enum Id : int
        {
            kHeading = 100,
            kBody,
            kDiscLabel,
            kDiscPath,
            kDiscChange,
            kLocal,
            kNetplay,
            kHost,
            kHostPortLabel,
            kHostPort,
            kJoin,
            kAddressLabel,
            kAddress,
            kJoinPortLabel,
            kJoinPort,
            kFightsLabel,
            kLockstep,
            kRollback,
            kNoteText,
            kPrimary = IDOK,
            kSecondary = IDCANCEL,
        };

        enum class Screen
        {
            kChecking,
            kNeedDisc,
            kReady,
            kFailed,
        };

        struct State
        {
            HWND window = nullptr;
            UINT dpi = 96;
            HFONT font = nullptr, headingFont = nullptr;
            HBRUSH background = nullptr, separator = nullptr;
            int bandTop = 0; // where the button band begins

            Folder folder;
            Choices choices;
            std::wstring discPath; // the disc image in use, once it checks out
            Screen screen = Screen::kChecking;
            std::wstring message; // what's wrong, for the need-disc and failed screens
            std::wstring status;  // the checks' latest
            bool closing = false; // closed while the checks ran: the window goes when they're done

            std::thread worker;
            std::mutex lock; // the worker's results, below
            std::wstring report;
            bool reportPosted = false;
            Readiness result = Readiness::kFailed;
            std::wstring resultMessage, resultDisc;
        };

        State* g_state = nullptr;

        HWND Control(int id)
        {
            return GetDlgItem(g_state->window, id);
        }

        int Scale(int value)
        {
            return MulDiv(value, int(g_state->dpi), 96);
        }

        bool Visible(int id)
        {
            return (GetWindowLongW(Control(id), GWL_STYLE) & WS_VISIBLE) != 0;
        }

        void Show(int id, bool visible)
        {
            ShowWindow(Control(id), visible ? SW_SHOWNA : SW_HIDE);
        }

        void SetText(int id, const std::wstring& text)
        {
            SetWindowTextW(Control(id), text.c_str());
        }

        std::wstring Text(int id)
        {
            const HWND control = Control(id);
            std::wstring text(size_t(GetWindowTextLengthW(control)) + 1, L'\0');
            text.resize(size_t(GetWindowTextW(control, text.data(), int(text.size()))));
            return text;
        }

        bool Checked(int id)
        {
            return SendMessageW(Control(id), BM_GETCHECK, 0, 0) == BST_CHECKED;
        }

        void Check(int id, bool checked)
        {
            SendMessageW(Control(id), BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
        }

        // The height of `text` wrapped at `width` in `font`, or a line's.
        int TextHeight(HFONT font, const std::wstring& text, int width)
        {
            const HDC dc = GetDC(g_state->window);
            const HGDIOBJ old = SelectObject(dc, font);
            RECT rect{ 0, 0, width, 0 };
            DrawTextW(dc, text.empty() ? L"Ag" : text.c_str(), -1, &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
            SelectObject(dc, old);
            ReleaseDC(g_state->window, dc);
            return rect.bottom - rect.top;
        }

        int TextWidth(const std::wstring& text)
        {
            const HDC dc = GetDC(g_state->window);
            const HGDIOBJ old = SelectObject(dc, g_state->font);
            RECT rect{ 0, 0, 0, 0 };
            DrawTextW(dc, text.c_str(), -1, &rect, DT_CALCRECT | DT_SINGLELINE);
            SelectObject(dc, old);
            ReleaseDC(g_state->window, dc);
            return rect.right - rect.left;
        }

        int ButtonWidth(int id)
        {
            return std::max(Scale(88), TextWidth(Text(id)) + Scale(24));
        }

        void CreateFonts()
        {
            State& s = *g_state;
            NONCLIENTMETRICSW metrics{};
            metrics.cbSize = sizeof(metrics);
            SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, s.dpi);
            LOGFONTW font = metrics.lfMessageFont;
            const HFONT body = CreateFontIndirectW(&font);
            font.lfHeight = MulDiv(font.lfHeight, 4, 3); // 12 points to the message font's 9
            const HFONT heading = CreateFontIndirectW(&font);
            for (HWND child = GetWindow(s.window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
                SendMessageW(child, WM_SETFONT, WPARAM(GetDlgCtrlID(child) == kHeading ? heading : body), TRUE);
            if (s.font)
                DeleteObject(s.font);
            if (s.headingFont)
                DeleteObject(s.headingFont);
            s.font = body;
            s.headingFont = heading;
        }

        void CreateControls(HINSTANCE instance)
        {
            const auto add = [&](int id, const wchar_t* type, const wchar_t* text, DWORD style, DWORD exStyle = 0) {
                CreateWindowExW(exStyle, type, text, WS_CHILD | style, 0, 0, 10, 10, g_state->window, HMENU(INT_PTR(id)), instance, nullptr);
            };
            const DWORD label = SS_LEFT | SS_NOPREFIX, radio = BS_AUTORADIOBUTTON | WS_TABSTOP, edit = ES_AUTOHSCROLL | WS_TABSTOP;
            add(kHeading, WC_STATICW, L"", WS_VISIBLE | label);
            add(kBody, WC_STATICW, L"", WS_VISIBLE | label);
            add(kDiscLabel, WC_STATICW, L"Disc image", label);
            add(kDiscPath, WC_STATICW, L"", SS_LEFTNOWORDWRAP | SS_NOPREFIX); // its text compacted to fit (Layout)
            add(kDiscChange, WC_BUTTONW, L"&Change...", BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP);
            add(kLocal, WC_BUTTONW, L"Pl&ay", radio | WS_GROUP);
            add(kNetplay, WC_BUTTONW, L"&Netplay: Versus with another PC", radio);
            // A radio button group runs from a WS_GROUP control to the next (in this order, which is also the tab order).
            add(kHost, WC_BUTTONW, L"&Host", radio | WS_GROUP);
            add(kJoin, WC_BUTTONW, L"&Join", radio);
            add(kHostPortLabel, WC_STATICW, L"Port", label | WS_GROUP);
            add(kHostPort, WC_EDITW, L"", edit | ES_NUMBER, WS_EX_CLIENTEDGE);
            add(kAddressLabel, WC_STATICW, L"Address", label);
            add(kAddress, WC_EDITW, L"", edit, WS_EX_CLIENTEDGE);
            add(kJoinPortLabel, WC_STATICW, L"Port", label);
            add(kJoinPort, WC_EDITW, L"", edit | ES_NUMBER, WS_EX_CLIENTEDGE);
            add(kFightsLabel, WC_STATICW, L"Fights", label);
            add(kLockstep, WC_BUTTONW, L"Lock&step", radio | WS_GROUP); // (&Launch has L)
            add(kRollback, WC_BUTTONW, L"&Rollback", radio);
            add(kNoteText, WC_STATICW, kNote, label | WS_GROUP);
            add(kPrimary, WC_BUTTONW, L"&Launch", BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP);
            add(kSecondary, WC_BUTTONW, L"Close", WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP);
            SendMessageW(Control(kHostPort), EM_SETLIMITTEXT, 5, 0);
            SendMessageW(Control(kJoinPort), EM_SETLIMITTEXT, 5, 0);
            SendMessageW(Control(kAddress), EM_SETLIMITTEXT, 253, 0);
            SendMessageW(Control(kAddress), EM_SETCUEBANNER, TRUE, LPARAM(L"203.0.113.7"));
        }

        void ShowChoices()
        {
            const Choices& c = g_state->choices;
            Check(kLocal, !c.netplay);
            Check(kNetplay, c.netplay);
            Check(kHost, c.host);
            Check(kJoin, !c.host);
            SetText(kHostPort, std::to_wstring(c.hostPort));
            SetText(kAddress, c.address);
            SetText(kJoinPort, std::to_wstring(c.joinPort));
            Check(kLockstep, !c.rollback);
            Check(kRollback, c.rollback);
        }

        void ReadChoices()
        {
            Choices& c = g_state->choices;
            const auto port = [](int id) {
                const long number = wcstol(Text(id).c_str(), nullptr, 10);
                return number >= 1 && number <= 65535 ? uint16_t(number) : uint16_t(0);
            };
            c.netplay = Checked(kNetplay);
            c.host = !Checked(kJoin);
            c.hostPort = port(kHostPort);
            c.address = Text(kAddress);
            c.joinPort = port(kJoinPort);
            c.rollback = Checked(kRollback);
        }

        void SaveTheChoices()
        {
            ReadChoices();
            Choices saved = g_state->choices;
            // A port left empty or wrong keeps its default.
            if (!saved.hostPort)
                saved.hostPort = Choices().hostPort;
            if (!saved.joinPort)
                saved.joinPort = Choices().joinPort;
            SaveChoices(g_state->folder, saved);
        }

        // Places every visible control at the window's scale, top to bottom, and sizes the window to them.
        void Layout()
        {
            State& s = *g_state;
            const int width = Scale(520), margin = Scale(20), inner = width - 2 * margin, line = TextHeight(s.font, L"", inner);
            // A control moves without its old pixels, which can hold what was around it before: all are drawn again below.
            const auto place = [](int id, int x, int y, int cx, int cy) {
                SetWindowPos(Control(id), nullptr, x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
            };
            int y = Scale(18);
            const int headingHeight = TextHeight(s.headingFont, Text(kHeading), inner);
            place(kHeading, margin, y, inner, headingHeight);
            y += headingHeight + Scale(8);
            if (const std::wstring body = Text(kBody); !body.empty())
            {
                const int height = TextHeight(s.font, body, inner);
                place(kBody, margin, y, inner, height);
                y += height + Scale(14);
            }
            else
            {
                // Without text it takes no room: where an earlier screen's text had it, it would paint its background over
                // the disc image's row (siblings don't clip each other), as it did when the ready screen followed another.
                place(kBody, margin, y, inner, 0);
                y += Scale(6);
            }
            if (Visible(kDiscPath))
            {
                const int row = Scale(25), labelWidth = TextWidth(Text(kDiscLabel)) + Scale(12), buttonWidth = ButtonWidth(kDiscChange);
                const int pathWidth = inner - labelWidth - buttonWidth - Scale(10);
                // The path with its middle left out (...) where it's too long: the file's name stays.
                std::wstring shown = s.discPath;
                if (shown.size() < MAX_PATH)
                {
                    shown.resize(MAX_PATH);
                    const HDC dc = GetDC(s.window);
                    const HGDIOBJ old = SelectObject(dc, s.font);
                    PathCompactPathW(dc, shown.data(), UINT(pathWidth));
                    SelectObject(dc, old);
                    ReleaseDC(s.window, dc);
                    shown.resize(wcslen(shown.c_str()));
                }
                SetText(kDiscPath, shown);
                place(kDiscLabel, margin, y + (row - line) / 2, labelWidth, line);
                place(kDiscPath, margin + labelWidth, y + (row - line) / 2, pathWidth, line);
                place(kDiscChange, margin + inner - buttonWidth, y, buttonWidth, row);
                y += row + Scale(16);
            }
            if (Visible(kLocal))
            {
                const int radio = Scale(22);
                place(kLocal, margin, y, inner, radio);
                y += radio + Scale(2);
                place(kNetplay, margin, y, inner, radio);
                y += radio + Scale(6);
                if (Visible(kHost))
                {
                    const int indent = margin + Scale(22), edit = Scale(23), right = margin + inner;
                    const int roleWidth = std::max({ TextWidth(Text(kHost)), TextWidth(Text(kJoin)), TextWidth(Text(kFightsLabel)) }) + Scale(30);
                    const int portLabel = TextWidth(Text(kHostPortLabel)) + Scale(8), addressLabel = TextWidth(Text(kAddressLabel)) + Scale(8);
                    const int portWidth = Scale(64), labelY = (edit - line) / 2;
                    int x = indent + roleWidth;
                    place(kHost, indent, y, roleWidth, edit);
                    place(kHostPortLabel, x, y + labelY, portLabel, line);
                    place(kHostPort, x + portLabel, y, portWidth, edit);
                    y += edit + Scale(6);
                    place(kJoin, indent, y, roleWidth, edit);
                    place(kAddressLabel, x, y + labelY, addressLabel, line);
                    x += addressLabel;
                    const int addressWidth = right - x - Scale(14) - portLabel - portWidth;
                    place(kAddress, x, y, addressWidth, edit);
                    x += addressWidth + Scale(14);
                    place(kJoinPortLabel, x, y + labelY, portLabel, line);
                    place(kJoinPort, x + portLabel, y, portWidth, edit);
                    y += edit + Scale(8);
                    x = indent + roleWidth;
                    const int lockstepWidth = TextWidth(Text(kLockstep)) + Scale(30);
                    place(kFightsLabel, indent, y + (radio - line) / 2, roleWidth, line);
                    place(kLockstep, x, y, lockstepWidth, radio);
                    place(kRollback, x + lockstepWidth + Scale(10), y, TextWidth(Text(kRollback)) + Scale(30), radio);
                    y += radio + Scale(8);
                    const int noteHeight = TextHeight(s.font, kNote, right - indent);
                    place(kNoteText, indent, y, right - indent, noteHeight);
                    y += noteHeight + Scale(6);
                }
                y += Scale(10);
            }
            s.bandTop = y;
            const int button = Scale(25), padding = Scale(12);
            int x = margin + inner;
            for (const int id : { kSecondary, kPrimary })
            {
                if (!Visible(id))
                    continue;
                const int buttonWidth = ButtonWidth(id);
                x -= buttonWidth;
                place(id, x, y + padding, buttonWidth, button);
                x -= Scale(8);
            }
            const int height = y + 2 * padding + button;

            // The window around that, kept on its monitor.
            RECT frame{ 0, 0, width, height };
            AdjustWindowRectExForDpi(&frame, kStyle, FALSE, kExStyle, s.dpi);
            RECT current{};
            GetWindowRect(s.window, &current);
            const int frameWidth = frame.right - frame.left, frameHeight = frame.bottom - frame.top;
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            GetMonitorInfoW(MonitorFromWindow(s.window, MONITOR_DEFAULTTONEAREST), &monitor);
            const int top = std::max(int(monitor.rcWork.top), std::min(int(current.top), int(monitor.rcWork.bottom) - frameHeight));
            SetWindowPos(s.window, nullptr, current.left, top, frameWidth, frameHeight, SWP_NOZORDER | SWP_NOACTIVATE);
            // The controls too: the window clips them, so invalidating it alone leaves them as they were.
            RedrawWindow(s.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        }

        // Sets the controls for the screen and the choices, then lays them out.
        void ShowScreen()
        {
            State& s = *g_state;
            std::wstring heading, body, primary = L"&Launch";
            bool showPrimary = true, showDisc = !s.discPath.empty(), showPlay = false;
            switch (s.screen)
            {
            case Screen::kChecking:
                heading = L"Getting ready";
                body = s.status;
                showPrimary = false;
                break;
            case Screen::kNeedDisc:
                heading = L"Select your " GAME_DISC " disc image";
                body = s.message; // what's wrong with the one tried, if one was
                primary = L"&Select disc image...";
                showDisc = false;
                break;
            case Screen::kReady:
                heading = L"Ready to play";
                showPlay = true;
                break;
            case Screen::kFailed:
                heading = L"Can't start the game";
                body = s.message;
                primary = L"&Try again";
                break;
            }
            const bool busy = s.worker.joinable();
            SetText(kHeading, heading);
            SetText(kBody, body);
            for (const int id : { kDiscLabel, kDiscPath, kDiscChange })
                Show(id, showDisc);
            EnableWindow(Control(kDiscChange), !busy);
            // A game without netplay (GAME_NETPLAY 0) just plays: no choices.
            Show(kLocal, showPlay && GAME_NETPLAY);
            Show(kNetplay, showPlay && GAME_NETPLAY);
            const bool netplay = GAME_NETPLAY && showPlay && Checked(kNetplay);
            for (const int id : { kHost, kHostPortLabel, kHostPort, kJoin, kAddressLabel, kAddress, kJoinPortLabel, kJoinPort, kFightsLabel, kLockstep,
                     kRollback, kNoteText })
                Show(id, netplay);
            const bool host = !Checked(kJoin);
            EnableWindow(Control(kHostPort), host);
            EnableWindow(Control(kHostPortLabel), host);
            for (const int id : { kAddress, kAddressLabel, kJoinPort, kJoinPortLabel })
                EnableWindow(Control(id), !host);
            SetText(kPrimary, primary);
            Show(kPrimary, showPrimary);
            EnableWindow(Control(kPrimary), showPrimary);
            Layout();
            // The keyboard's focus on something that's still there.
            const HWND focus = GetFocus();
            if (!focus || !IsWindowVisible(focus) || !IsWindowEnabled(focus))
                SetFocus(IsWindowEnabled(Control(kPrimary)) && Visible(kPrimary) ? Control(kPrimary) : Control(kSecondary));
        }

        // The checks, on the worker thread.
        void StartPreparing(const std::wstring& chosen)
        {
            State& s = *g_state;
            if (s.worker.joinable())
                return;
            s.message.clear();
            s.status = L"Checking...";
            s.screen = Screen::kChecking;
            s.worker = std::thread([chosen] {
                State& s = *g_state;
                std::wstring message;
                Disc disc;
                const Readiness result = Prepare(
                    s.folder, chosen,
                    [](const std::wstring& text) {
                        State& s = *g_state;
                        std::lock_guard lock(s.lock);
                        s.report = text;
                        if (!s.reportPosted)
                        {
                            s.reportPosted = true;
                            PostMessageW(s.window, kReportMessage, 0, 0);
                        }
                    },
                    message, disc);
                {
                    std::lock_guard lock(s.lock);
                    s.result = result;
                    s.resultMessage = message;
                    s.resultDisc = disc.path;
                }
                PostMessageW(s.window, kDoneMessage, 0, 0);
            });
            ShowScreen();
        }

        std::wstring AskForDisc()
        {
            State& s = *g_state;
            std::wstring chosen;
            IFileOpenDialog* dialog = nullptr;
            if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
                return chosen;
            const COMDLG_FILTERSPEC types[] = { { L"Disc images (*.iso)", L"*.iso" }, { L"All files", L"*.*" } };
            dialog->SetFileTypes(2, types);
            dialog->SetTitle(L"Select your " GAME_DISC " disc image");
            dialog->SetOkButtonLabel(L"Select");
            FILEOPENDIALOGOPTIONS options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
            const std::wstring folder = s.discPath.empty() ? s.folder.root : ParentOf(s.discPath);
            IShellItem* item = nullptr;
            if (!folder.empty() && SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item))))
            {
                dialog->SetFolder(item);
                item->Release();
            }
            if (SUCCEEDED(dialog->Show(s.window)) && SUCCEEDED(dialog->GetResult(&item)))
            {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
                {
                    chosen = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
            dialog->Release();
            return chosen;
        }

        void ChooseDisc()
        {
            if (g_state->worker.joinable())
                return;
            const std::wstring chosen = AskForDisc();
            if (!chosen.empty())
                StartPreparing(chosen);
        }

        void Play()
        {
            State& s = *g_state;
            SaveTheChoices();
            const HCURSOR cursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
            const std::wstring problem = StartGame(s.folder, s.choices, false, nullptr);
            SetCursor(cursor);
            if (!problem.empty())
            {
                MessageBoxW(s.window, problem.c_str(), kTitle, MB_ICONWARNING | MB_OK);
                return;
            }
            DestroyWindow(s.window);
        }

        void OnReport()
        {
            State& s = *g_state;
            std::wstring text;
            {
                std::lock_guard lock(s.lock);
                text = s.report;
                s.reportPosted = false;
            }
            if (s.closing)
                return;
            if (s.screen == Screen::kChecking && s.status != text)
            {
                s.status = text;
                SetText(kBody, s.status);
            }
        }

        void OnDone()
        {
            State& s = *g_state;
            if (s.worker.joinable())
                s.worker.join();
            if (s.closing)
            {
                DestroyWindow(s.window);
                return;
            }
            {
                std::lock_guard lock(s.lock);
                if (!s.resultDisc.empty())
                    s.discPath = s.resultDisc;
                s.message = s.resultMessage;
                switch (s.result)
                {
                case Readiness::kReady:
                    s.screen = Screen::kReady;
                    break;
                case Readiness::kNeedDisc:
                    s.screen = Screen::kNeedDisc;
                    s.discPath.clear();
                    break;
                case Readiness::kFailed:
                    s.screen = Screen::kFailed;
                    break;
                }
            }
            ShowScreen();
            if (!IsWindowVisible(s.window))
                ShowWindow(s.window, SW_SHOW);
        }

        void Paint(HDC dc)
        {
            State& s = *g_state;
            RECT client{};
            GetClientRect(s.window, &client);
            RECT content = client, band = client, line = client;
            content.bottom = s.bandTop;
            band.top = s.bandTop;
            line.top = s.bandTop;
            line.bottom = s.bandTop + std::max(1, Scale(1));
            FillRect(dc, &content, s.background);
            FillRect(dc, &band, GetSysColorBrush(COLOR_BTNFACE));
            FillRect(dc, &line, s.separator);
        }

        LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
        {
            State& s = *g_state;
            switch (message)
            {
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
            {
                PAINTSTRUCT paint;
                const HDC dc = BeginPaint(window, &paint);
                Paint(dc);
                EndPaint(window, &paint);
                return 0;
            }
            case WM_PRINTCLIENT: // the controls' themed backgrounds
                Paint(HDC(wParam));
                return 0;
            case WM_CTLCOLORSTATIC:
            {
                const HDC dc = HDC(wParam);
                const HWND control = HWND(lParam);
                wchar_t type[16] = L"";
                GetClassNameW(control, type, 16);
                if (_wcsicmp(type, WC_EDITW) == 0)
                    break; // a disabled edit box: Windows' gray
                const int id = GetDlgCtrlID(control);
                SetTextColor(dc, id == kHeading ? kHeadingColor : id == kNoteText ? kNoteColor : GetSysColor(COLOR_WINDOWTEXT));
                SetBkColor(dc, GetSysColor(COLOR_WINDOW));
                return LRESULT(s.background);
            }
            case DM_GETDEFID:
                return MAKELRESULT(kPrimary, DC_HASDEFID);
            case WM_COMMAND:
            {
                const int id = LOWORD(wParam);
                const int code = HIWORD(wParam);
                switch (id)
                {
                case kPrimary:
                    if (!IsWindowEnabled(Control(kPrimary)) || !Visible(kPrimary))
                        return 0;
                    if (s.screen == Screen::kNeedDisc)
                        ChooseDisc();
                    else if (s.screen == Screen::kReady)
                        Play();
                    else if (s.screen == Screen::kFailed)
                        StartPreparing(s.discPath);
                    return 0;
                case kSecondary:
                    SendMessageW(window, WM_CLOSE, 0, 0);
                    return 0;
                case kDiscChange:
                    ChooseDisc();
                    return 0;
                case kLocal:
                case kNetplay:
                case kHost:
                case kJoin:
                    if (code == BN_CLICKED)
                        ShowScreen();
                    return 0;
                default:
                    break;
                }
                break;
            }
            case kReportMessage:
                OnReport();
                return 0;
            case kDoneMessage:
                OnDone();
                return 0;
            case WM_TIMER:
                if (wParam == kShowTimer)
                {
                    KillTimer(window, kShowTimer);
                    if (!IsWindowVisible(window))
                        ShowWindow(window, SW_SHOW);
                }
                return 0;
            case WM_DPICHANGED:
            {
                s.dpi = HIWORD(wParam);
                CreateFonts();
                Layout();
                const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(window, nullptr, suggested->left, suggested->top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            case WM_SETTINGCHANGE:
                if (wParam == SPI_SETNONCLIENTMETRICS)
                {
                    CreateFonts();
                    Layout();
                }
                break;
            case WM_CLOSE:
                if (s.worker.joinable())
                {
                    // The checks take a moment: the window closes when they're done (OnDone).
                    s.closing = true;
                    ShowWindow(window, SW_HIDE);
                    return 0;
                }
                SaveTheChoices();
                DestroyWindow(window);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            default:
                break;
            }
            return DefWindowProcW(window, message, wParam, lParam);
        }
    }

    int RunWindow(const Folder& folder, const std::wstring& disc, const Choices& choices)
    {
        State state;
        g_state = &state;
        state.folder = folder;
        state.choices = choices;
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&controls);

        WNDCLASSEXW type{};
        type.cbSize = sizeof(type);
        type.lpfnWndProc = WindowProc;
        type.hInstance = instance;
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        type.lpszClassName = kClassName;
        if (!RegisterClassExW(&type))
            return 1;
        state.background = GetSysColorBrush(COLOR_WINDOW);
        state.separator = GetSysColorBrush(COLOR_3DLIGHT);
        state.window = CreateWindowExW(kExStyle, kClassName, kTitle, kStyle, CW_USEDEFAULT, CW_USEDEFAULT, 400, 300, nullptr, nullptr, instance, nullptr);
        if (!state.window)
            return 1;
        state.dpi = GetDpiForWindow(state.window);
        CreateControls(instance);
        CreateFonts();
        ShowChoices();
        StartPreparing(disc);

        // In the middle of its monitor. Shown when the checks are done, or after a moment if they take longer, so a start
        // that's ready shows no checking screen.
        RECT frame{};
        GetWindowRect(state.window, &frame);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromWindow(state.window, MONITOR_DEFAULTTOPRIMARY), &monitor);
        const RECT& work = monitor.rcWork;
        SetWindowPos(state.window, nullptr, work.left + (work.right - work.left - (frame.right - frame.left)) / 2,
            work.top + (work.bottom - work.top - (frame.bottom - frame.top)) / 3, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        SetTimer(state.window, kShowTimer, 400, nullptr);

        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            if (!IsDialogMessageW(state.window, &message))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (state.worker.joinable())
            state.worker.join();
        if (state.font)
            DeleteObject(state.font);
        if (state.headingFont)
            DeleteObject(state.headingFont);
        g_state = nullptr;
        return 0;
    }
}
