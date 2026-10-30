// The launcher's entry point (launcher.h): its window, or with --check or --play the same work without it.
//
// Usage: "<GAME_PREFIX> Launcher.exe" [--disc <disc image>] [--data <folder>] [--check | --play [--wait]]
//          [--netplay off|host:<port>|join:<address>:<port>] [--lockstep | --rollback]
//   --disc     the disc image, checked and remembered as if chosen in the window
//   --data     the game's data directory (default: data, next to the launcher), e.g. for two instances on one PC
//   --check    checks the release and the disc image without the window: exit code 0 once they're right, 1 if
//              something's wrong, 3 without a disc image
//   --play     the same, then starts the game; with --wait, waits for it and exits with its exit code, and ending the
//              launcher ends the game (for tests)
//   --netplay, --lockstep, --rollback: how to play, remembered as the window's choices
// Without --check or --play the window opens, with those options applied. Messages go to the standard output when there
// is one, or else to the console the launcher was started from.
#include "launcher/launcher.h"
#include "launcher/window.h"

#include <cstdlib>
#include <cwchar>

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

namespace
{
    using namespace launcher;

    // A game without netplay (GAME_NETPLAY 0, CMakeLists.txt) takes only --netplay off.
    const wchar_t kUsage[] = L"Usage: \"" GAME_PREFIX " Launcher.exe\" [--disc <disc image>] [--data <folder>] [--check | --play [--wait]]\n"
#if GAME_NETPLAY
                             L"    [--netplay off|host:<port>|join:<address>:<port>] [--lockstep | --rollback]\n"
#endif
                             L"\n"
                             L"--disc: the disc image to use, checked and remembered.\n"
                             L"--data: the game's data folder (default: data, next to the launcher).\n"
                             L"--check: checks the game's files and the disc image, without the window.\n"
#if GAME_NETPLAY
                             L"--play: the same, then starts the game; --wait waits for it.\n"
                             L"--netplay, --lockstep, --rollback: how to play, remembered.";
#else
                             L"--play: the same, then starts the game; --wait waits for it.";
#endif

    struct Options
    {
        std::wstring disc, data;
        bool check = false, play = false, wait = false, help = false;
        std::wstring netplay; // off, host:<port> or join:<address>:<port>
        int rollback = -1;    // -1: as remembered
        std::wstring error;
    };

    Options Parse(int count, wchar_t** arguments)
    {
        Options options;
        for (int i = 1; i < count; i++)
        {
            const std::wstring argument = arguments[i];
            const auto value = [&](std::wstring& into) {
                if (i + 1 < count)
                    into = arguments[++i];
                else
                    options.error = argument + L" needs a value.";
            };
            if (argument == L"--disc")
                value(options.disc);
            else if (argument == L"--data")
                value(options.data);
            else if (argument == L"--check")
                options.check = true;
            else if (argument == L"--play")
                options.play = true;
            else if (argument == L"--wait")
                options.wait = true;
            else if (argument == L"--netplay")
                value(options.netplay);
            else if (argument == L"--lockstep")
                options.rollback = 0;
            else if (argument == L"--rollback")
                options.rollback = 1;
            else if (argument == L"--help" || argument == L"-h" || argument == L"/?")
                options.help = true;
            else
                options.error = L"Unknown option: " + argument;
        }
        return options;
    }

    // The choices the options change. False if --netplay isn't right.
    bool Apply(const Options& options, Choices& choices)
    {
        if (options.rollback >= 0)
            choices.rollback = options.rollback != 0;
        const std::wstring& netplay = options.netplay;
        if (netplay.empty())
            return true;
        if (!GAME_NETPLAY && netplay != L"off")
            return false;
        const auto port = [](const std::wstring& text) {
            const long number = wcstol(text.c_str(), nullptr, 10);
            return number >= 1 && number <= 65535 ? uint16_t(number) : uint16_t(0);
        };
        if (netplay == L"off")
            choices.netplay = false;
        else if (netplay.rfind(L"host:", 0) == 0 && port(netplay.substr(5)))
        {
            choices.netplay = true;
            choices.host = true;
            choices.hostPort = port(netplay.substr(5));
        }
        else if (netplay.rfind(L"join:", 0) == 0 && netplay.rfind(L':') > 5 && port(netplay.substr(netplay.rfind(L':') + 1)))
        {
            const size_t colon = netplay.rfind(L':');
            choices.netplay = true;
            choices.host = false;
            choices.address = netplay.substr(5, colon - 5);
            choices.joinPort = port(netplay.substr(colon + 1));
        }
        else
            return false;
        return true;
    }

    // The standard output, or the console the launcher was started from (it has none of its own).
    class Output
    {
    public:
        Output()
        {
            m_handle = GetStdHandle(STD_OUTPUT_HANDLE);
            if (!Usable(m_handle) && AttachConsole(ATTACH_PARENT_PROCESS))
                m_handle = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            DWORD mode = 0;
            m_console = Usable(m_handle) && GetConsoleMode(m_handle, &mode);
        }

        void Line(const std::wstring& text)
        {
            if (!Usable(m_handle))
                return;
            const std::wstring line = text + L"\r\n";
            DWORD written = 0;
            if (m_console)
                WriteConsoleW(m_handle, line.c_str(), DWORD(line.size()), &written, nullptr);
            else
            {
                const std::string bytes = Utf8(line);
                WriteFile(m_handle, bytes.data(), DWORD(bytes.size()), &written, nullptr);
            }
        }

    private:
        static bool Usable(HANDLE handle)
        {
            return handle && handle != INVALID_HANDLE_VALUE && GetFileType(handle) != FILE_TYPE_UNKNOWN;
        }

        HANDLE m_handle = nullptr;
        bool m_console = false;
    };

    // --check and --play: the window's work, reported as lines.
    int RunCommand(const Folder& folder, const Options& options, const Choices& choices)
    {
        Output output;
        std::wstring lastText;
        const Reporter report = [&](const std::wstring& text) {
            if (text != lastText)
                output.Line(text);
            lastText = text;
        };
        std::wstring message;
        Disc disc;
        switch (Prepare(folder, options.disc, report, message, disc))
        {
        case Readiness::kReady:
            output.Line(L"Ready: the disc image is " + disc.path + L".");
            break;
        case Readiness::kNeedDisc:
            output.Line(message.empty() ? L"No disc image: give one with --disc <path>." : message);
            return 3;
        case Readiness::kFailed:
            output.Line(message);
            return 1;
        }
        if (!options.play)
            return 0;
        std::wstring problem;
        const std::string netplay = NetplaySetting(choices, problem);
        if (!problem.empty())
        {
            output.Line(problem);
            return 1;
        }
        output.Line(L"Starting the game" + (netplay.empty() ? std::wstring() : L" (" GAME_PREFIX "_NETPLAY=" + FromUtf8(netplay) +
            L", " GAME_PREFIX "_NETPLAY_ROLLBACK=" + (choices.rollback ? L"1" : L"0") + L")") + L", its data in " + folder.data);
        unsigned long exitCode = 0;
        if (const std::wstring failure = StartGame(folder, choices, options.wait, &exitCode); !failure.empty())
        {
            output.Line(failure);
            return 1;
        }
        if (options.wait)
        {
            output.Line(L"The game exited with " + std::to_wstring(exitCode));
            return int(exitCode);
        }
        return 0;
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    // The manifest's, in case it's missing: sizes in pixels at each monitor's scale.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // No dialogs for critical errors (a disc image on a drive that isn't ready is reported as the checks find it); the
    // game gets Windows' default (StartGame).
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    const Options options = arguments ? Parse(count, arguments) : Options{};
    if (arguments)
        LocalFree(arguments);
    if (options.help || !options.error.empty())
    {
        const std::wstring text = options.error.empty() ? kUsage : options.error + L"\n\n" + kUsage;
        Output().Line(text);
        if (options.check || options.play)
            return 4;
        MessageBoxW(nullptr, text.c_str(), L"" GAME_TITLE " Launcher", options.error.empty() ? MB_ICONINFORMATION : MB_ICONWARNING);
        return 4;
    }

    const Folder folder = FindFolder(options.data);
    Choices choices = LoadChoices(folder);
    if (!Apply(options, choices))
    {
        const std::wstring text = GAME_NETPLAY ? L"--netplay is off, host:<port> or join:<address>:<port>, not " + options.netplay
                                               : L"" GAME_TITLE " has no netplay: --netplay is only off, not " + options.netplay;
        Output().Line(text);
        if (!options.check && !options.play)
            MessageBoxW(nullptr, text.c_str(), L"" GAME_TITLE " Launcher", MB_ICONWARNING);
        return 4;
    }
    if (!options.netplay.empty() || options.rollback >= 0)
        SaveChoices(folder, choices);
    if (options.check || options.play)
        return RunCommand(folder, options, choices);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const int code = RunWindow(folder, options.disc, choices);
    CoUninitialize();
    return code;
}
