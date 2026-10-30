#pragma once

// The launcher, "<GAME_PREFIX> Launcher.exe" (ANALYSIS.md, Phase 5, Packaging, The launcher): what a player starts. It
// runs from the release folder tools\package.ps1 makes, which holds the game's executable and the game's code,
// recompiled (<executable>_ppc.dll), but none of the game's data. It asks for the player's disc image and checks it as
// the game does, remembers it in the game's settings, and starts the game, on this PC or in a netplay session.
//
// Paths are wide strings throughout, so the release folder can be anywhere. The game gets its data directory relative
// to the release folder and the disc image in the narrow code page its paths use (NarrowPath).
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace launcher
{
    // --- Text and files ---
    std::string Utf8(const std::wstring& text);
    std::wstring FromUtf8(const std::string& text);
    std::wstring FromNarrow(const std::string& text); // the ANSI code page's, as the runtime's paths are
    // `path` as the runtime's narrow paths take it (the ANSI code page): itself, or else its short (8.3) form, if one
    // converts there and back unchanged; empty if neither does.
    std::string NarrowPath(const std::wstring& path);
    // Windows' text for an error code, without the line break; `insert` in place of its %1, if it has one.
    std::wstring SystemMessage(unsigned long error, const std::wstring& insert = L"");
    std::wstring Join(const std::wstring& directory, const std::wstring& name);
    std::wstring FileNameOf(const std::wstring& path);
    std::wstring ParentOf(const std::wstring& path);
    std::wstring FullPath(const std::wstring& path);
    bool IsFile(const std::wstring& path);
    bool IsDirectory(const std::wstring& path);
    // Makes the folder and the ones above it. True if it's there.
    bool MakeDirectories(const std::wstring& path);
    bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& bytes);
    // Writes the file through a temporary one beside it, so a reader never sees half of it. 0, or the error.
    unsigned long WriteFileAtomically(const std::wstring& path, const void* data, size_t size);
    // One argument for a command line, quoted as CommandLineToArgvW and the C runtime read it.
    std::wstring Quote(const std::wstring& argument);
    // This process's environment, as name=value strings, and an environment block for CreateProcessW made of some
    // (sorted by name, as Windows keeps them).
    std::vector<std::wstring> EnvironmentEntries();
    std::wstring EnvironmentBlock(std::vector<std::wstring> entries);

    // --- The release folder ---
    struct Folder
    {
        std::wstring root; // the folder the launcher is in
        std::wstring data; // the game's data directory: data\ there, unless --data names another

        std::wstring Executable() const { return Join(root, L"" GAME_EXECUTABLE ".exe"); }
        std::wstring Module() const { return Join(root, L"" GAME_EXECUTABLE "_ppc.dll"); }
    };
    // The launcher's folder, with `data` (empty: data\ there) as its data directory.
    Folder FindFolder(const std::wstring& data);
    // What's missing from the release folder (a file a release ships, relative to it), or empty.
    std::wstring MissingFile(const Folder& folder);

    // --- The disc image ---
    struct Disc
    {
        std::wstring path;        // as chosen, full
        std::string narrow;       // as the game opens it (NarrowPath)
        std::vector<uint8_t> xex; // its default.xex, as it is on the disc
    };
    // Checks a disc image as the game does (disc::ReadExecutable: the supported version's default.xex), reading its
    // default.xex into `disc`. Empty if it's the one, else what's wrong, for the player.
    std::wstring CheckDisc(const std::wstring& path, Disc& disc);
    // The disc image settings.ini names (disc_image, the game's own setting), or empty.
    std::wstring RememberedDisc(const Folder& folder);
    // The .iso files next to the launcher and in the data directory, where the game also looks for one.
    std::vector<std::wstring> DiscsNearby(const Folder& folder);
    // Writes disc_image into settings.ini, keeping everything else there. Empty, or what's wrong.
    std::wstring RememberDisc(const Folder& folder, const Disc& disc);

    // --- How to play: the player's choices, kept in <data>\launcher.ini ---
    struct Choices
    {
        bool netplay = false;
        bool host = true;
        uint16_t hostPort = 7000;
        std::wstring address;
        uint16_t joinPort = 7000;
        bool rollback = true; // unless the player chooses lockstep
    };
    Choices LoadChoices(const Folder& folder);
    void SaveChoices(const Folder& folder, const Choices& choices);
    // The game's NETPLAY setting for the choices (host:<port> or join:<IPv4 address>:<port>; a name is looked up), or
    // empty for this PC. Sets `problem` to what's wrong with them, if anything.
    std::string NetplaySetting(const Choices& choices, std::wstring& problem);
    // Starts the game from the release folder with its data directory, as the choices say (NETPLAY, NETPLAY_ROLLBACK).
    // With `wait`, waits for it to exit (ending it if the launcher ends first) and gives its exit code. Empty, or what's
    // wrong.
    std::wstring StartGame(const Folder& folder, const Choices& choices, bool wait, unsigned long* exitCode);

    // --- Getting ready to play ---
    // What the checks are doing, for the player.
    using Reporter = std::function<void(const std::wstring& text)>;

    enum class Readiness
    {
        kReady,    // the release is whole and the disc image is right
        kNeedDisc, // no disc image, or not the right one: ask for one
        kFailed,   // something else is wrong: the message says what
    };
    // Checks the release and the disc image (`chosen`, else the remembered one, else an .iso nearby), and remembers the
    // disc image. `message` says what's wrong, if anything; `disc` is the disc image found. Runs on any thread.
    Readiness Prepare(const Folder& folder, const std::wstring& chosen, const Reporter& report, std::wstring& message, Disc& disc);
}
