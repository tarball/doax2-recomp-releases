// The release folder's files, the disc image and the settings the launcher keeps (launcher.h).
#include "launcher/launcher.h"

#include "disc_image.h"

#include <algorithm>
#include <cwchar>

#include <windows.h>

namespace launcher
{
    namespace
    {
        std::wstring Widen(const std::string& text, UINT codePage)
        {
            if (text.empty())
                return {};
            const int size = MultiByteToWideChar(codePage, 0, text.data(), int(text.size()), nullptr, 0);
            std::wstring wide(size_t(std::max(size, 0)), L'\0');
            if (size > 0)
                MultiByteToWideChar(codePage, 0, text.data(), int(text.size()), wide.data(), size);
            return wide;
        }

        std::string Trim(const std::string& text)
        {
            const size_t first = text.find_first_not_of(" \t\r\n"), last = text.find_last_not_of(" \t\r\n");
            return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
        }

        // The file's lines, without their line breaks.
        std::vector<std::string> ReadLines(const std::wstring& path)
        {
            std::vector<std::string> lines;
            std::vector<uint8_t> bytes;
            if (!ReadFileBytes(path, bytes))
                return lines;
            std::string text(bytes.begin(), bytes.end());
            if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0)
                text.erase(0, 3); // a UTF-8 byte order mark (PowerShell's)
            size_t start = 0;
            while (start < text.size())
            {
                size_t end = text.find('\n', start);
                if (end == std::string::npos)
                    end = text.size();
                std::string line = text.substr(start, end - start);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                lines.push_back(std::move(line));
                start = end + 1;
            }
            return lines;
        }

        // A settings file's "name = value" lines, as the game's settings.cpp reads them: [sections] and ; comments are
        // skipped, the last of a name counts.
        std::string Setting(const std::vector<std::string>& lines, const char* wanted)
        {
            std::string value;
            for (const std::string& line : lines)
            {
                const size_t equals = line.find('=');
                if (line.empty() || line[0] == ';' || line[0] == '[' || equals == std::string::npos)
                    continue;
                if (Trim(line.substr(0, equals)) == wanted)
                    value = Trim(line.substr(equals + 1));
            }
            return value;
        }

        std::wstring CannotWrite(const std::wstring& path, DWORD error)
        {
            return L"Can't write " + path + L": " + SystemMessage(error) +
                L"\n\nMove the game to a folder you can change, such as one in Documents.";
        }
    }

    std::string Utf8(const std::wstring& text)
    {
        if (text.empty())
            return {};
        const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
        std::string narrow(size_t(std::max(size, 0)), '\0');
        if (size > 0)
            WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), narrow.data(), size, nullptr, nullptr);
        return narrow;
    }

    std::wstring FromUtf8(const std::string& text)
    {
        return Widen(text, CP_UTF8);
    }

    std::wstring FromNarrow(const std::string& text)
    {
        return Widen(text, CP_ACP);
    }

    std::string NarrowPath(const std::wstring& path)
    {
        // Without loss only if it converts back to the same text: a character the code page lacks becomes another
        // (best fit) or '?'.
        const auto convert = [](const std::wstring& text) -> std::string {
            if (text.empty())
                return {};
            const int size = WideCharToMultiByte(CP_ACP, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
            if (size <= 0)
                return {};
            std::string narrow(size_t(size), '\0');
            WideCharToMultiByte(CP_ACP, 0, text.data(), int(text.size()), narrow.data(), size, nullptr, nullptr);
            return Widen(narrow, CP_ACP) == text ? narrow : std::string();
        };
        if (std::string narrow = convert(path); !narrow.empty())
            return narrow;
        // The short form has only ASCII, where the volume keeps 8.3 names.
        const DWORD length = GetShortPathNameW(path.c_str(), nullptr, 0);
        if (!length)
            return {};
        std::wstring shortPath(length, L'\0');
        const DWORD written = GetShortPathNameW(path.c_str(), shortPath.data(), length);
        if (!written || written >= length)
            return {};
        shortPath.resize(written);
        return convert(shortPath);
    }

    std::wstring SystemMessage(unsigned long error, const std::wstring& insert)
    {
        wchar_t* text = nullptr;
        const DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_ALLOCATE_BUFFER,
            nullptr, error, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
        std::wstring message = length && text ? std::wstring(text, length) : L"error " + std::to_wstring(error) + L".";
        if (text)
            LocalFree(text);
        while (!message.empty() && (message.back() == L'\n' || message.back() == L'\r' || message.back() == L' '))
            message.pop_back();
        if (const size_t at = message.find(L"%1"); at != std::wstring::npos)
            message.replace(at, 2, insert.empty() ? L"the program" : insert);
        return message;
    }

    std::wstring Join(const std::wstring& directory, const std::wstring& name)
    {
        if (directory.empty())
            return name;
        if (directory.back() == L'\\' || directory.back() == L'/')
            return directory + name;
        return directory + L"\\" + name;
    }

    std::wstring FileNameOf(const std::wstring& path)
    {
        const size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? path : path.substr(slash + 1);
    }

    std::wstring ParentOf(const std::wstring& path)
    {
        const size_t slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return {};
        // A drive's root keeps its backslash (C:\).
        if (slash == 2 && path.size() > 1 && path[1] == L':')
            return path.substr(0, 3);
        return path.substr(0, slash);
    }

    std::wstring FullPath(const std::wstring& path)
    {
        if (path.empty())
            return {};
        const DWORD length = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
        if (!length)
            return path;
        std::wstring full(length, L'\0');
        const DWORD written = GetFullPathNameW(path.c_str(), length, full.data(), nullptr);
        if (!written || written >= length)
            return path;
        full.resize(written);
        // Without a trailing backslash, but for a drive's root.
        while (full.size() > 3 && (full.back() == L'\\' || full.back() == L'/'))
            full.pop_back();
        return full;
    }

    bool IsFile(const std::wstring& path)
    {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    bool IsDirectory(const std::wstring& path)
    {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    bool MakeDirectories(const std::wstring& path)
    {
        if (path.empty() || IsDirectory(path))
            return true;
        const std::wstring parent = ParentOf(path);
        if (!parent.empty() && parent != path && !MakeDirectories(parent))
            return false;
        return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
    }

    bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& bytes)
    {
        bytes.clear();
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        LARGE_INTEGER size{};
        bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && size.QuadPart < (1ll << 31);
        if (ok)
        {
            bytes.resize(size_t(size.QuadPart));
            DWORD read = 0;
            ok = bytes.empty() || (ReadFile(file, bytes.data(), DWORD(bytes.size()), &read, nullptr) && read == bytes.size());
        }
        CloseHandle(file);
        return ok;
    }

    unsigned long WriteFileAtomically(const std::wstring& path, const void* data, size_t size)
    {
        const std::wstring temporary = path + L".new";
        const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return GetLastError();
        DWORD error = 0;
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t done = 0; done < size && !error;)
        {
            const DWORD chunk = DWORD(std::min<size_t>(size - done, 1u << 24));
            DWORD written = 0;
            if (!WriteFile(file, bytes + done, chunk, &written, nullptr) || written != chunk)
                error = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
            done += written;
        }
        CloseHandle(file);
        if (!error && !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            error = GetLastError();
        if (error)
            DeleteFileW(temporary.c_str());
        return error;
    }

    std::wstring Quote(const std::wstring& argument)
    {
        if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
            return argument;
        // Backslashes are literal but before a quote, where each is doubled and the quote escaped; those before the
        // closing quote are doubled too.
        std::wstring quoted = L"\"";
        for (size_t i = 0;; i++)
        {
            size_t backslashes = 0;
            while (i < argument.size() && argument[i] == L'\\')
                i++, backslashes++;
            if (i == argument.size())
            {
                quoted.append(backslashes * 2, L'\\');
                break;
            }
            if (argument[i] == L'"')
                quoted.append(backslashes * 2 + 1, L'\\');
            else
                quoted.append(backslashes, L'\\');
            quoted.push_back(argument[i]);
        }
        quoted.push_back(L'"');
        return quoted;
    }

    std::vector<std::wstring> EnvironmentEntries()
    {
        std::vector<std::wstring> entries;
        wchar_t* block = GetEnvironmentStringsW();
        if (!block)
            return entries;
        for (const wchar_t* entry = block; *entry; entry += wcslen(entry) + 1)
            entries.push_back(entry);
        FreeEnvironmentStringsW(block);
        return entries;
    }

    std::wstring EnvironmentBlock(std::vector<std::wstring> entries)
    {
        // By name, ignoring case, as Windows sorts them (the hidden =C: entries, which begin with '=', first).
        const auto name = [](const std::wstring& entry) {
            const size_t equals = entry.find(L'=', 1);
            return entry.substr(0, equals);
        };
        std::stable_sort(entries.begin(), entries.end(), [&](const std::wstring& a, const std::wstring& b) {
            const std::wstring x = name(a), y = name(b);
            return CompareStringOrdinal(x.c_str(), int(x.size()), y.c_str(), int(y.size()), TRUE) == CSTR_LESS_THAN;
        });
        std::wstring block;
        for (const std::wstring& entry : entries)
        {
            block += entry;
            block.push_back(L'\0');
        }
        block.push_back(L'\0');
        return block;
    }

    Folder FindFolder(const std::wstring& data)
    {
        Folder folder;
        std::wstring path(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
            if (length && length < path.size())
            {
                path.resize(length);
                break;
            }
            if (!length || path.size() > 32768)
            {
                path.clear();
                break;
            }
            path.resize(path.size() * 2);
        }
        folder.root = ParentOf(path);
        folder.data = data.empty() ? Join(folder.root, L"data") : FullPath(data);
        return folder;
    }

    std::wstring MissingFile(const Folder& folder)
    {
        for (const wchar_t* file : { L"" GAME_EXECUTABLE ".exe", L"" GAME_EXECUTABLE "_ppc.dll" })
        {
            if (!IsFile(Join(folder.root, file)))
                return file;
        }
        return {};
    }

    std::wstring CheckDisc(const std::wstring& chosen, Disc& disc)
    {
        disc = {};
        const std::wstring path = FullPath(chosen);
        const std::wstring name = FileNameOf(path);
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
            return L"The disc image " + path + L" isn't there.";
        if (attributes & FILE_ATTRIBUTE_DIRECTORY)
            return path + L" is a folder, not a disc image.";
        // Opened as the game opens it: for reading, others may read it too.
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return L"Can't open " + name + L": " + SystemMessage(GetLastError());
        CloseHandle(file);
        // The game's paths are narrow strings, in this PC's code page for programs that aren't Unicode.
        const std::string narrow = NarrowPath(path);
        if (narrow.empty())
            return L"The game can't open " + path + L": its path has characters it can't use. Move the disc image to a folder "
                L"such as C:\\Games, or rename it.";
        std::vector<uint8_t> xex;
        switch (disc::ReadExecutable(narrow, xex))
        {
        case disc::Check::kOk:
            break;
        case disc::Check::kUnreadable:
        case disc::Check::kOtherGame:
            return name + L" doesn't appear to be a valid " GAME_DISC " disc image.";
        }
        disc.path = path;
        disc.narrow = narrow;
        disc.xex = std::move(xex);
        return {};
    }

    std::wstring RememberedDisc(const Folder& folder)
    {
        return FromNarrow(Setting(ReadLines(Join(folder.data, L"settings.ini")), "disc_image"));
    }

    std::vector<std::wstring> DiscsNearby(const Folder& folder)
    {
        std::vector<std::wstring> found;
        for (const std::wstring& directory : { folder.root, folder.data })
        {
            WIN32_FIND_DATAW entry;
            const HANDLE search = FindFirstFileW(Join(directory, L"*.iso").c_str(), &entry);
            if (search == INVALID_HANDLE_VALUE)
                continue;
            std::vector<std::wstring> names;
            do
            {
                if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    names.push_back(entry.cFileName);
            } while (FindNextFileW(search, &entry));
            FindClose(search);
            std::sort(names.begin(), names.end());
            for (const std::wstring& name : names)
                found.push_back(Join(directory, name));
        }
        return found;
    }

    std::wstring RememberDisc(const Folder& folder, const Disc& disc)
    {
        if (!MakeDirectories(folder.data))
            return CannotWrite(folder.data, GetLastError());
        // As setup.ps1 writes it, and settings.cpp (which keeps disc_image when it saves the video settings): the header
        // and [game] first, then everything else that was there. In the narrow code page, as the game reads it.
        const std::wstring path = Join(folder.data, L"settings.ini");
        const std::string header = "; " GAME_TITLE " recomp settings", comment = "; The disc image the game runs from.";
        std::string text = header + "\r\n[game]\r\n" + comment + "\r\ndisc_image = " + disc.narrow + "\r\n";
        std::vector<std::string> kept;
        for (const std::string& line : ReadLines(path))
        {
            const size_t equals = line.find('=');
            const bool discImage = equals != std::string::npos && line[0] != ';' && Trim(line.substr(0, equals)) == "disc_image";
            if (!discImage && line != "[game]" && line != comment && line.rfind(header, 0) != 0)
                kept.push_back(line);
        }
        while (!kept.empty() && Trim(kept.back()).empty())
            kept.pop_back();
        for (const std::string& line : kept)
            text += line + "\r\n";
        if (const DWORD error = WriteFileAtomically(path, text.data(), text.size()))
            return CannotWrite(path, error);
        return {};
    }

    Choices LoadChoices(const Folder& folder)
    {
        Choices choices;
        const std::vector<std::string> lines = ReadLines(Join(folder.data, L"launcher.ini"));
        const auto port = [&](const char* name, uint16_t fallback) {
            const std::string value = Setting(lines, name);
            const long number = value.empty() ? 0 : strtol(value.c_str(), nullptr, 10);
            return number >= 1 && number <= 65535 ? uint16_t(number) : fallback;
        };
        choices.netplay = Setting(lines, "netplay") == "1";
        choices.host = Setting(lines, "role") != "join";
        choices.hostPort = port("host_port", choices.hostPort);
        choices.address = FromUtf8(Setting(lines, "address"));
        choices.joinPort = port("join_port", choices.joinPort);
        if (const std::string fights = Setting(lines, "fights"); !fights.empty())
            choices.rollback = fights == "rollback";
        return choices;
    }

    void SaveChoices(const Folder& folder, const Choices& choices)
    {
        if (!MakeDirectories(folder.data))
            return;
        std::string text = "; " GAME_PREFIX " Launcher's choices of how to play (the game's own settings are in settings.ini)\r\n"
            "[play]\r\n"
            "; 0: on this PC, 1: netplay with another PC\r\n";
        text += "netplay = " + std::string(choices.netplay ? "1" : "0") + "\r\n";
        text += "[netplay]\r\n; host or join\r\nrole = " + std::string(choices.host ? "host" : "join") + "\r\n";
        text += "host_port = " + std::to_string(choices.hostPort) + "\r\n";
        text += "address = " + Utf8(choices.address) + "\r\n";
        text += "join_port = " + std::to_string(choices.joinPort) + "\r\n";
        text += "; lockstep or rollback: both players choose the same\r\nfights = " + std::string(choices.rollback ? "rollback" : "lockstep") + "\r\n";
        WriteFileAtomically(Join(folder.data, L"launcher.ini"), text.data(), text.size());
    }
}
