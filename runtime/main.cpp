// Recompilation runtime entry point.
//
// Usage: <executable> [game data directory] [disc image]
// The game runs from its disc image: an .iso of GAME_DISC (CMakeLists.txt) next to the executable (or in the data
// directory or the working directory, or settings.ini's disc_image), whose default.xex is checked by its SHA-256 (the
// version disc_image.cpp names) and unpacked in memory (see disc_image.h). A development checkout keeps default.xex
// and default.pe (the decrypted, decompressed image produced by tools/xexunpack.ps1) in its data directory instead,
// and they're used when present.
// The data directory holds settings.ini, saves, caches and the hard disk's folders: by default "private" in the working
// directory if it has default.xex (a development checkout), else "data" next to the executable (a release).
// HEADLESS=1 runs without a window or renderer (the GPU command stream is still executed).
#include "audio/audio_system.h"
#include "audio/xma.h"
#include "config.h"
#include "crash_handler.h"
#include "disc_image.h"
#include "gpu/command_processor.h"
#include "gpu/d3d12/d3d12_renderer.h"
#include "gpu/memory_watch.h"
#include "guest_memory.h"
#include "imports.h"
#include "kernel/module.h"
#include "kernel/threads.h"
#include "kernel/variables.h"
#include "log.h"
#include "module/loader.h"
#include "netplay/fingerprint.h"
#include "netplay/session.h"
#include "settings.h"
#include "ui/window.h"
#include "util/profiler.h"
#include "util/write_watch.h"
#include "ppc_recomp_shared.h"
#include "vfs/iso_device.h"
#include "vfs/vfs.h"
#include "xam/spa.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <math.h>
#include <thread>

#include <windows.h>
#include <timeapi.h>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace
{
    // The game's code module (module/loader.h): its function table.
    module::Loaded g_gameCode;

    // The game's image (default.pe) into guest memory.
    bool LoadImage(const std::vector<uint8_t>& image)
    {
        if (image.size() != PPC_IMAGE_SIZE)
        {
            LOG_ERROR("The game's image is 0x%zX bytes, expected 0x%llX", image.size(), PPC_IMAGE_SIZE);
            return false;
        }
        memcpy(guest::ToHost(uint32_t(PPC_IMAGE_BASE)), image.data(), image.size());
        return true;
    }

    bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& data)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            return false;
        data.resize(size_t(file.tellg()));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
        return bool(file);
    }

    // The folder the executable is in (empty if its path doesn't fit the narrow code page the runtime's paths use).
    std::string ExecutableDirectory()
    {
        wchar_t path[MAX_PATH];
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (!length || length >= MAX_PATH)
            return {};
        try
        {
            return std::filesystem::path(path).parent_path().string();
        }
        catch (const std::exception&)
        {
            return {};
        }
    }

    // The data directory when none is given: see the top of this file.
    std::string DefaultDataDirectory()
    {
        std::error_code error;
        if (std::filesystem::exists("private/default.xex", error))
            return "private";
        const std::string executable = ExecutableDirectory();
        return executable.empty() ? "data" : executable + "/data";
    }

    // Whether stderr goes somewhere a person reads as it happens (a console), or nowhere: not a redirected file or pipe.
    bool Interactive()
    {
        const HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
        if (!error || error == INVALID_HANDLE_VALUE)
            return true;
        const DWORD type = GetFileType(error);
        return type != FILE_TYPE_DISK && type != FILE_TYPE_PIPE;
    }

    // Whether startup failures get a message box: unless whoever started the game redirected stderr to read them.
    // Decided before stderr goes to the log, which would look like such a redirection.
    bool g_showFailures = true;

    // A failure at startup: logged, and shown in a message box when someone's there to see it.
    int Fail(const std::string& message)
    {
        LOG_ERROR("%s", message.c_str());
        if (g_showFailures)
            MessageBoxA(nullptr, message.c_str(), GAME_TITLE, MB_ICONERROR | MB_OK);
        return 1;
    }

    // The disc images to try: `first` (from the command line or settings.ini) if there is one, then the .iso files next
    // to the executable, in the data directory and in the working directory. Files whose paths the narrow code page
    // can't represent are left out, since the runtime's paths are narrow strings.
    std::vector<std::string> FindDiscImages(const std::string& first, const std::string& dataDirectory)
    {
        std::vector<std::string> images;
        std::vector<std::filesystem::path> seen;
        const auto add = [&](const std::filesystem::path& path) {
            std::error_code error;
            std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
            if (error)
                canonical = path;
            if (std::find(seen.begin(), seen.end(), canonical) != seen.end())
                return;
            images.push_back(path.string());
            seen.push_back(canonical);
        };
        try
        {
            if (!first.empty())
                add(first);
            for (const std::string& directory : { ExecutableDirectory(), dataDirectory, std::string(".") })
            {
                std::error_code error;
                if (directory.empty())
                    continue;
                for (const auto& entry : std::filesystem::directory_iterator(directory, error))
                {
                    if (entry.is_regular_file(error) && _wcsicmp(entry.path().extension().c_str(), L".iso") == 0)
                    {
                        try
                        {
                            add(entry.path());
                        }
                        catch (const std::exception&)
                        {
                        }
                    }
                }
            }
        }
        catch (const std::exception&)
        {
            // A folder that changed or became unreadable while being listed: what was found so far.
        }
        return images;
    }

    // The game's executable (default.xex) and image (default.pe): the data directory's if it has them (a development
    // checkout), else read from a disc image, checked and unpacked in memory. `discImage` is the disc image to try first
    // (from the command line or settings.ini, or empty), and is set to the one used. Returns an empty string, or what's
    // wrong.
    std::string ReadGame(const std::string& dataDirectory, std::string& discImage, std::vector<uint8_t>& xex,
        std::vector<uint8_t>& image)
    {
        const std::vector<std::string> images = FindDiscImages(discImage, dataDirectory);
        const auto exists = [](const std::string& path) {
            std::error_code error;
            return std::filesystem::is_regular_file(path, error);
        };
        if (ReadWholeFile(dataDirectory + "/default.xex", xex) && ReadWholeFile(dataDirectory + "/default.pe", image))
        {
            for (const std::string& candidate : images)
            {
                if (exists(candidate))
                {
                    discImage = candidate;
                    return {};
                }
            }
            return "No " GAME_DISC " disc image was found.";
        }

        const std::string folder = ExecutableDirectory();
        const std::string where = "Put your " GAME_DISC " disc image (.iso) in this folder:\n\n" +
            (folder.empty() ? std::string("the game's folder") : folder);
        std::string problems;
        for (const std::string& candidate : images)
        {
            if (!exists(candidate))
            {
                problems += "\n- " + candidate + " doesn't exist.";
                continue;
            }
            const std::string name = std::filesystem::path(candidate).filename().string();
            switch (disc::ReadExecutable(candidate, xex))
            {
            case disc::Check::kOk:
                if (!disc::Unpack(xex, image))
                    return "Can't unpack the game's executable from " + name + ".";
                discImage = candidate;
                LOG_INFO("Game from %s", candidate.c_str());
                return {};
            case disc::Check::kUnreadable:
            case disc::Check::kOtherGame:
                problems += "\n- " + name + " doesn't appear to be a valid " GAME_DISC " disc image.";
                break;
            }
        }
        return problems.empty() ? where : "No " GAME_DISC " disc image was found:" + problems + "\n\n" + where;
    }

    bool MountDevices(const std::string& dataDirectory, const std::string& discImage)
    {
        auto disc = vfs::OpenIsoDevice(discImage);
        if (!disc)
            return false;
        vfs::Mount("\\Device\\Cdrom0", std::move(disc));

        // Hard disk partitions: their file systems are host folders under <data>/hdd, and their raw sectors (what XAPI
        // reads and writes when it formats and mounts the cache) are <data>/hdd/<partition>.raw. The layout is a retail
        // 20 GB drive's; Partition0 is the whole disk, with no file system of its own. A game whose utility drive is off
        // (GAME_UTILITY_DRIVE in CMakeLists.txt) has no Partition0: XAPI can't read the cache's header there, so the
        // utility drive (cache:) doesn't mount and the game reads its files from the disc.
        constexpr uint64_t kDiskSize = 20ull << 30;
        struct Partition
        {
            const char* name;
            vfs::PartitionLayout layout;
            bool fileSystem;
        };
        const Partition partitions[] = {
#if GAME_UTILITY_DRIVE
            { "Partition0", { 0, kDiskSize }, false },
#endif
            { "Cache0", { 0x80000, 0x80000000 }, true },
            { "Cache1", { 0x80080000, 0x80000000 }, true },
            { "Partition1", { 0x130EB0000, kDiskSize - 0x130EB0000 }, true },
        };
        for (const Partition& partition : partitions)
        {
            const std::string folder = dataDirectory + "/hdd/" + partition.name;
            auto device = vfs::OpenHardDiskPartition(std::string("hard disk ") + partition.name, partition.fileSystem ? folder : std::string(),
                folder + ".raw", partition.layout);
            if (!device)
                return false;
            vfs::Mount(std::string("\\Device\\Harddisk0\\") + partition.name, std::move(device));
        }

        // The title was "launched" from the disc.
        vfs::AddSymbolicLink("\\??\\D:", "\\Device\\Cdrom0");
        vfs::AddSymbolicLink("\\??\\game:", "\\Device\\Cdrom0");
        return true;
    }

    void RegisterFunctions()
    {
        for (const PPCFuncMapping* mapping = g_gameCode.functions; mapping->host != nullptr; ++mapping)
            PPC_LOOKUP_FUNC(guest::g_base, mapping->guest) = mapping->host;

        imports::RegisterThunks();
    }
}

int main(int argc, char** argv)
{
    // The C runtime's fma, which the generated code calls for every guest multiply-add (the guest's sin, cos and atan
    // among them), and its exp2f, log2f and log10 take an FMA3 path or a software one by CPU, and the two differ for NaN
    // and infinite operands and the sign of a product that underflows: in a netplay session (NETPLAY) and the fingerprint
    // (FINGERPRINT=1) every PC takes the software path, so that two PCs compute alike, and otherwise the CPU's choice
    // stands, which costs nothing. DEBUG_FMA3=1 leaves the choice to the CPU there too (to measure it).
    const char* fingerprinting = std::getenv(GAME_PREFIX "_FINGERPRINT");
    if (netplay::session::Configured() || (fingerprinting && std::atoi(fingerprinting) != 0))
    {
        if (const char* fma3 = std::getenv(GAME_PREFIX "_DEBUG_FMA3"); !fma3 || std::atoi(fma3) == 0)
            _set_FMA3_enable(0);
    }
    g_showFailures = Interactive();
    const std::string dataDirectory = argc > 1 ? argv[1] : DefaultDataDirectory();
    if (std::error_code error; !std::filesystem::create_directories(dataDirectory, error) && error)
        return Fail("Can't create the folder " + dataDirectory + " for saves and settings.\n\nPut the game in a folder you can change, such as one in Documents.");
    // Without a console or a redirected stderr (a Release build started from Explorer), log to <data>/<executable>.log.
    if (const HANDLE error = GetStdHandle(STD_ERROR_HANDLE); !error || error == INVALID_HANDLE_VALUE)
        freopen((dataDirectory + "/" GAME_EXECUTABLE ".log").c_str(), "w", stderr);
    config::SetDataDirectory(dataDirectory);
    settings::Load();
    // Sizes in pixels, so a fullscreen window gets the monitor's resolution rather than a scaled one to be stretched.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    profile::NameThread("Main (window)");

    std::string discImage = argc > 2 ? argv[2] : settings::DiscImage();
    std::vector<uint8_t> xex, image;
    if (const std::string problem = ReadGame(dataDirectory, discImage, xex, image); !problem.empty())
        return Fail(problem);
    config::SetDiscImage(discImage);
    // The title's SPA data among the executable's resources: the window's icon, and the achievements' names and pictures.
    spa::Load(xex, image);

    // The recompiled code, built from the player's game into a module of its own, checked before any of it runs.
    if (const std::string problem = module::Load(g_gameCode); !problem.empty())
        return Fail(problem);
    crash::Install(g_gameCode.functions);
    // After the crash handler, so it runs first: GPU memory watch faults are routine.
    gpu::watch::Install();
    debug::InstallWriteWatch();
    guest::SetPhysicalFreeHook(gpu::watch::InvalidatePhysical);

    if (!guest::Initialize())
        return Fail("Can't reserve the 4 GB of address space the game needs.");

    // The image, the indirect call table after it and the GPU's and the XMA decoder's registers are never handed out.
    constexpr uint32_t kLookupTableSize = uint32_t(PPC_CODE_SIZE * 2);
    if (!guest::ReserveFixed(uint32_t(PPC_IMAGE_BASE), uint32_t(PPC_IMAGE_SIZE), guest::kPageReadWrite) ||
        !guest::ReserveFixed(uint32_t(PPC_IMAGE_BASE + PPC_IMAGE_SIZE), kLookupTableSize, guest::kPageReadOnly) ||
        !guest::ReserveFixed(gpu::kRegisterWindow, gpu::kRegisterWindowSize, guest::kPageReadWrite) ||
        !guest::ReserveFixed(audio::kXmaRegisterPage, audio::kXmaRegisterPageSize, guest::kPageReadWrite))
        return Fail("Can't reserve the address space for the game's image.");
    // A game whose XAudio drives the XMA decoder through its registers (DOAX2's, of a later XDK) reads the context
    // array's address from them as it starts, so the decoder starts now. One that uses the XMA context exports instead
    // (DOA4's imports XMAInitializeContext, the kernel's ordinal 549) starts it with its first context, as before, so its
    // physical memory is laid out as before.
    if (!imports::FunctionThunk("Kernel", 549))
        audio::InitializeXmaRegisters();

    // 1 ms timer resolution, so guest sleeps and the time stamp bundle are accurate.
    timeBeginPeriod(1);

    if (!LoadImage(image))
        return Fail("The game's image doesn't match this build of the game.");
    debug::WriteWatchOnImageLoaded();

    imports::ResolveSlots();
    RegisterFunctions();

    if (!kernel::InitializeExecutableModule(xex, uint32_t(PPC_IMAGE_BASE), uint32_t(PPC_IMAGE_SIZE)))
        return Fail("Can't load the game's executable.");
    kernel::InitializeKernelVariables();

    // FINGERPRINT=1: what this PC computes where PCs could differ, for comparing two before they play (fingerprint.h).
    if (const char* fingerprint = std::getenv(GAME_PREFIX "_FINGERPRINT"); fingerprint && std::atoi(fingerprint) != 0)
        return netplay::fingerprint::Run();

    if (!MountDevices(dataDirectory, discImage))
        return Fail("Can't open the disc image " + discImage + ".");

    // The window and renderer exist before the game initializes the GPU.
    const char* headlessSetting = std::getenv(GAME_PREFIX "_HEADLESS");
    const bool headless = headlessSetting && std::atoi(headlessSetting) != 0;
    ui::Window window;
    if (!headless)
    {
        // WINDOW_SIZE=WxH sets the window's client size in pixels (default the game's 1280x720, at the display's scale).
        const uint32_t dpi = GetDpiForSystem();
        uint32_t windowWidth = uint32_t(MulDiv(1280, int(dpi), 96)), windowHeight = uint32_t(MulDiv(720, int(dpi), 96));
        if (const char* size = std::getenv(GAME_PREFIX "_WINDOW_SIZE"))
        {
            unsigned width = 0, height = 0;
            if (sscanf(size, "%ux%u", &width, &height) == 2 && width >= 320 && height >= 180)
                windowWidth = width, windowHeight = height;
        }
        // The game's own image (its dashboard tile, from the player's game) as the window's icon.
        window.SetIcon(spa::TitleImage());
        if (!window.Create(L"" GAME_TITLE, windowWidth, windowHeight, settings::GetGraphics().fullscreen))
            return Fail("Can't create the window.");
        auto renderer = gpu::d3d12::CreateRenderer(window);
        if (!renderer)
            return Fail("Can't initialize Direct3D 12: the game needs a graphics card and driver that support it.");
        renderer->PrecompileShaders(guest::ToHost(uint32_t(PPC_IMAGE_BASE)), size_t(PPC_IMAGE_SIZE));
        gpu::SetRenderer(std::move(renderer));
    }

    // The title's entry point runs as the first guest thread (default XEX stack size, its own PCR and KTHREAD).
    kernel::GuestThread::Params params;
    params.startAddress = kernel::GetXexOptionalHeader(kernel::ExecutableXexHeader(), 0x00010100);
    auto mainThread = kernel::GuestThread::Create(params);
    if (!mainThread)
    {
        LOG_ERROR("Can't create the main thread");
        return 1;
    }

    // Headless progress report: frames presented, draws and GPU packets processed. WATCH, a
    // comma-separated list of guest addresses (hex), adds the 32-bit values stored there.
    std::vector<uint32_t> watches;
    if (const char* watch = std::getenv(GAME_PREFIX "_WATCH"))
    {
        for (const char* p = watch; *p;)
        {
            char* end;
            watches.push_back(uint32_t(strtoul(p, &end, 16)));
            p = *end ? end + 1 : end;
        }
    }
    // THREAD_DUMP=<seconds> prints every guest thread's call chain and wait, the command processor's last packets and every
    // thread's host call stack once, after that long.
    const char* threadDump = std::getenv(GAME_PREFIX "_THREAD_DUMP");
    const int threadDumpSeconds = threadDump ? std::atoi(threadDump) : 0;
    profile::Start([] { return uint64_t(gpu::GetStatistics().swaps.load()); });
    std::thread([watches, threadDumpSeconds] {
        profile::NameThread("Status");
        auto& stats = gpu::GetStatistics();
        uint64_t lastSwaps = 0;
        int elapsed = 0;
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            elapsed += 5;
            if (threadDumpSeconds && elapsed >= threadDumpSeconds && elapsed - 5 < threadDumpSeconds)
            {
                kernel::DumpGuestThreads();
                gpu::DumpRecentPackets(60);
                profile::DumpHostStacks();
            }
            const uint64_t swaps = stats.swaps;
            LOG_INFO("GPU: %llu frames (%.1f fps), %llu draws, %llu packets, %llu vblanks; audio: %llu frames", (unsigned long long)swaps,
                (swaps - lastSwaps) / 5.0, (unsigned long long)stats.draws.load(), (unsigned long long)stats.packets.load(),
                (unsigned long long)stats.vblanks.load(), (unsigned long long)audio::SubmittedFrames());
            // A game that stopped making frames once it had (hung, or a netplay session waiting for the other machine; loading
            // screens are frames too): where its threads and the command processor are, once. And outside a session, whose
            // own status says when it waits for the other machine, the window's title says so after 10 s, until frames come.
            static bool hangDumped = false;
            static int stoppedSeconds = 0;
            static bool titleSaysStopped = false;
            if (swaps && swaps == lastSwaps)
            {
                stoppedSeconds += 5;
                if (!hangDumped)
                {
                    hangDumped = true;
                    LOG_WARN("GPU: no frame in 5 s (a hang, or a session waiting): the guest threads' call chains and waits, the command "
                             "processor's last packets and every thread's host call stack follow");
                    kernel::DumpGuestThreads();
                    gpu::DumpRecentPackets(60);
                    profile::DumpHostStacks();
                }
                if (stoppedSeconds >= 10 && !netplay::session::Configured())
                {
                    titleSaysStopped = true;
                    ui::SetTitleStatus("not responding: no new frame for " + std::to_string(stoppedSeconds) + " s (" GAME_EXECUTABLE
                                       ".log says what each thread waits for)");
                }
            }
            else
            {
                if (stoppedSeconds >= 10)
                    LOG_INFO("GPU: frames again, after %d s without one", stoppedSeconds);
                if (titleSaysStopped)
                    ui::SetTitleStatus("");
                stoppedSeconds = 0;
                titleSaysStopped = false;
            }
            for (uint32_t address : watches)
                LOG_INFO("  [%08X] = %08X", address, guest::LoadU32(address));
            gpu::watch::DumpWriters();
            lastSwaps = swaps;
        }
    }).detach();

    if (headless)
    {
        mainThread->Join();
    }
    else if (window.RunMessageLoop(mainThread->NativeHandle()))
    {
        // Closing the window ends the process; guest threads can't be stopped cleanly.
        LOG_INFO("Window closed");
        netplay::session::Shutdown();
        settings::Save();
        fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 0);
    }
    LOG_INFO("Main thread exited with 0x%08X", mainThread->ExitCode());
    return 0;
}
