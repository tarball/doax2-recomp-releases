// File system exports over the VFS. All I/O completes synchronously. DEBUG_READ_DELAY=ms[,from[,to]] delays each read from
// game frame `from` (netplay::Frame; default 0) until frame `to` (default: on), for debugging: loading that takes longer,
// as on a slower machine.
#include "gpu/memory_watch.h"
#include "guest_abi.h"
#include "kernel/objects.h"
#include "kernel/status.h"
#include "kernel/threads.h"
#include "log.h"
#include "netplay/frame.h"
#include "netplay/replay.h"
#include "netplay/write_tracker.h"
#include "util/write_watch.h"
#include "vfs/vfs.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>

#include <windows.h>

namespace
{
    using namespace kernel;

    constexpr uint32_t kNoMoreFiles = 0x80000006;
    constexpr uint32_t kNotADirectory = 0xC0000103;
    constexpr uint32_t kInfoLengthMismatch = 0xC0000004;
    constexpr uint64_t kUseFilePointer = 0xFFFFFFFFFFFFFFFEull; // FILE_USE_FILE_POINTER_POSITION

    struct CountedString
    {
        be<uint16_t> length;
        be<uint16_t> maximumLength;
        be<uint32_t> buffer;
    };

    struct ObjectAttributes
    {
        be<uint32_t> rootDirectory;
        be<uint32_t> objectName; // ANSI_STRING*
        be<uint32_t> attributes;
    };

    struct IoStatusBlock
    {
        be<uint32_t> status;
        be<uint32_t> information;
    };

    std::string ReadString(const CountedString* string)
    {
        if (!string || string->buffer == 0)
            return {};
        return std::string(guest::Translate<char>(string->buffer), string->length);
    }

    class FileObject : public GuestBackedObject
    {
    public:
        FileObject(std::string path, std::unique_ptr<vfs::File> file, vfs::Device* device)
            : GuestBackedObject(sizeof(DispatcherHeader)), path(std::move(path)), file(std::move(file)), device(device)
        {
            // File objects are waitable; I/O is synchronous, so they're always signaled.
            Header()->type = uint8_t(DispatcherType::NotificationEvent);
            Header()->size = sizeof(DispatcherHeader) / 4;
            Header()->signalState = 1;
        }
        const char* TypeName() const override { return "File"; }

        std::string path; // guest path as opened (used for root-relative opens)
        std::unique_ptr<vfs::File> file;
        vfs::Device* device;
        std::mutex lock;
        uint64_t position = 0;
        // Directory enumeration state.
        std::vector<vfs::EntryInfo> listing;
        size_t listingIndex = 0;
        bool listingStarted = false;
    };

    void SetIoStatus(IoStatusBlock* iosb, uint32_t result, uint32_t information)
    {
        if (iosb)
        {
            iosb->status = result;
            iosb->information = information;
        }
    }

    // Signals the optional completion event, as asynchronous callers expect.
    void SignalCompletion(uint32_t event)
    {
        if (!event)
            return;
        if (auto object = LookupHandle(event); object && object->IsWaitable())
            kernel::SetEvent(object->GuestObject());
    }

    bool BuildPath(const ObjectAttributes* attributes, std::string& path)
    {
        if (!attributes)
            return false;
        path = ReadString(guest::Translate<CountedString>(attributes->objectName));
        constexpr uint32_t kDosDevicesRoot = 0xFFFFFFFD; // XAPI's root for drive-letter paths: the \?? directory
        const uint32_t rootDirectory = attributes->rootDirectory;
        if (rootDirectory == kDosDevicesRoot)
        {
            if (path.rfind("\\", 0) != 0)
                path = "\\??\\" + path;
        }
        else if (rootDirectory != 0)
        {
            auto root = LookupHandleAs<FileObject>(rootDirectory);
            if (!root)
            {
                LOG_WARN("File path \"%s\" relative to unknown root handle 0x%08X", path.c_str(), rootDirectory);
                return false;
            }
            path = vfs::JoinPath(root->path, path);
        }
        return true;
    }

    // DEBUG_REDIRECT=from=to (debugging): a path ending in `from` (case-insensitively) opens the same path
    // ending in `to` instead, e.g. "\movie\ninja_vi_hd.sfd=\movie\kasumi.sfd" plays an ending as the boot movie.
    void RedirectPath(std::string& path)
    {
        static const std::pair<std::string, std::string> redirect = [] {
            const char* setting = std::getenv(GAME_PREFIX "_DEBUG_REDIRECT");
            const char* equals = setting ? std::strchr(setting, '=') : nullptr;
            if (!equals)
                return std::pair<std::string, std::string>{};
            return std::pair<std::string, std::string>{ std::string(setting, equals), std::string(equals + 1) };
        }();
        const auto& [from, to] = redirect;
        if (from.empty() || path.size() < from.size())
            return;
        const size_t start = path.size() - from.size();
        for (size_t i = 0; i < from.size(); i++)
        {
            if (std::tolower(static_cast<unsigned char>(path[start + i])) != std::tolower(static_cast<unsigned char>(from[i])))
                return;
        }
        LOG_INFO("NtCreateFile: redirecting \"%s\" to \"%s\"", path.c_str(), to.c_str());
        path = path.substr(0, start) + to;
    }

    uint32_t NtCreateFile(be<uint32_t>* handle, uint32_t desiredAccess, const ObjectAttributes* attributes, IoStatusBlock* iosb,
        const be<uint64_t>* /*allocationSize*/, uint32_t /*fileAttributes*/, uint32_t /*shareAccess*/, uint32_t disposition, uint32_t createOptions)
    {
        std::string path;
        if (!handle || !BuildPath(attributes, path))
            return status::kInvalidParameter;
        RedirectPath(path);

        std::string relative;
        vfs::Device* device = vfs::Resolve(path, relative);
        if (!device)
        {
            LOG_WARN("NtCreateFile: no device for \"%s\"", path.c_str());
            SetIoStatus(iosb, status::kObjectPathNotFound, 0);
            return status::kObjectPathNotFound;
        }

        constexpr uint32_t kWriteAccess = 0x40000000 /* GENERIC_WRITE */ | 0x10000000 /* GENERIC_ALL */ | 0x2 /* FILE_WRITE_DATA */ | 0x4 /* FILE_APPEND_DATA */;
        vfs::OpenRequest request;
        request.path = relative;
        request.disposition = vfs::Disposition(disposition);
        request.directory = createOptions & 0x1;       // FILE_DIRECTORY_FILE
        request.nonDirectory = createOptions & 0x40;   // FILE_NON_DIRECTORY_FILE
        request.write = desiredAccess & kWriteAccess;
        request.raw = relative.empty() && !path.empty() && path.back() != '\\' && !request.directory;

        std::unique_ptr<vfs::File> file;
        vfs::OpenResult result = vfs::OpenResult::Opened;
        const uint32_t openStatus = device->Open(request, file, result);
        if (openStatus != status::kSuccess)
        {
            LOG_WARN("NtCreateFile(\"%s\", access 0x%08X, disposition %u, options 0x%X): 0x%08X", path.c_str(), desiredAccess, disposition, createOptions, openStatus);
            SetIoStatus(iosb, openStatus, 0);
            return openStatus;
        }

        if (createOptions & 0x1000) // FILE_DELETE_ON_CLOSE
            file->SetDeleteOnClose();

        auto object = std::make_shared<FileObject>(path, std::move(file), device);
        RegisterGuestObject(object);
        *handle = CreateHandle(object);
        SetIoStatus(iosb, status::kSuccess, uint32_t(result));
        LOG_TRACE("NtCreateFile(\"%s\") -> 0x%08X", path.c_str(), uint32_t(*handle));
        return status::kSuccess;
    }

    uint32_t NtOpenFile(be<uint32_t>* handle, uint32_t desiredAccess, const ObjectAttributes* attributes, IoStatusBlock* iosb, uint32_t shareAccess, uint32_t openOptions)
    {
        return NtCreateFile(handle, desiredAccess, attributes, iosb, nullptr, 0, shareAccess, uint32_t(vfs::Disposition::Open), openOptions);
    }

    // DEBUG_READ_DELAY and DEBUG_WRITE_DELAY=ms[,from[,to]]: each read or write waits `ms` from game frame `from` until
    // `to`, as a slower machine's disk would.
    struct IoDelay
    {
        DWORD ms = 0;
        uint64_t from = 0, to = ~0ull;
        explicit IoDelay(const char* name)
        {
            if (const char* setting = std::getenv(name))
            {
                char* end = nullptr;
                ms = DWORD(strtoul(setting, &end, 10));
                if (end && *end == ',')
                    from = strtoull(end + 1, &end, 10);
                if (end && *end == ',')
                    to = strtoull(end + 1, &end, 10);
            }
        }
        void Wait() const
        {
            if (ms && netplay::Frame() >= from && netplay::Frame() < to)
                Sleep(ms);
        }
    };

    uint64_t ResolveOffset(FileObject& file, const be<uint64_t>* byteOffset)
    {
        if (!byteOffset)
            return file.position;
        const uint64_t offset = *byteOffset;
        return offset == kUseFilePointer ? file.position : offset;
    }

    uint32_t NtReadFile(uint32_t handle, uint32_t event, uint32_t apcRoutine, uint32_t /*apcContext*/, IoStatusBlock* iosb,
        void* buffer, uint32_t length, const be<uint64_t>* byteOffset)
    {
        auto file = LookupHandleAs<FileObject>(handle);
        if (!file)
            return status::kInvalidHandle;
        if (apcRoutine)
            LOG_WARN("NtReadFile: APC completion routine not supported");

        static const IoDelay delay(GAME_PREFIX "_DEBUG_READ_DELAY");
        delay.Wait();
        std::lock_guard lock(file->lock);
        const uint64_t offset = ResolveOffset(*file, byteOffset);
        uint32_t transferred = 0;
        // The host kernel writes the buffer, so pages the GPU watches must be unprotected first, and netplay's page log
        // copies out the pages it watches (guest_memory.h).
        guest::BeforeHostWriteTo(buffer, length);
        gpu::watch::PrepareHostWrite(uint32_t(static_cast<uint8_t*>(buffer) - guest::g_base), length);
        netplay::tracker::PrepareHostWrite(uint32_t(static_cast<uint8_t*>(buffer) - guest::g_base), length);
        debug::WriteWatchHostWrite(uint32_t(static_cast<uint8_t*>(buffer) - guest::g_base), length, false);
        // (Kept while netplay's journal records, for replays: netplay/replay.h.)
        netplay::journal::BeforeHostWrite(uint32_t(static_cast<uint8_t*>(buffer) - guest::g_base), length);
        const uint32_t result = file->file->Read(buffer, length, offset, transferred);
        netplay::journal::AfterHostWrite(uint32_t(static_cast<uint8_t*>(buffer) - guest::g_base), length);
        netplay::tracker::FinishHostWrite(uint32_t(static_cast<uint8_t*>(buffer) - guest::g_base), length);
        debug::WriteWatchHostWrite(uint32_t(static_cast<uint8_t*>(buffer) - guest::g_base), length, true);
        file->position = offset + transferred;
        if (logging::TraceEnabled())
        {
            // With the game frame and the reading thread's start address: which thread loads what, and when.
            const kernel::GuestThread* thread = kernel::CurrentThread();
            LOG_TRACE("NtReadFile(\"%s\", offset 0x%llX, %u bytes to %08X) -> 0x%08X, %u bytes, frame %llu, thread %08X", file->path.c_str(),
                static_cast<unsigned long long>(offset), length, uint32_t(static_cast<uint8_t*>(buffer) - guest::g_base), result, transferred,
                (unsigned long long)netplay::Frame(), thread ? thread->StartAddress() : 0);
        }
        SetIoStatus(iosb, result, transferred);
        SignalCompletion(event);
        return result;
    }

    uint32_t NtWriteFile(uint32_t handle, uint32_t event, uint32_t apcRoutine, uint32_t /*apcContext*/, IoStatusBlock* iosb,
        const void* buffer, uint32_t length, const be<uint64_t>* byteOffset)
    {
        auto file = LookupHandleAs<FileObject>(handle);
        if (!file)
            return status::kInvalidHandle;
        if (apcRoutine)
            LOG_WARN("NtWriteFile: APC completion routine not supported");

        static const IoDelay delay(GAME_PREFIX "_DEBUG_WRITE_DELAY");
        delay.Wait();
        std::lock_guard lock(file->lock);
        const uint64_t offset = ResolveOffset(*file, byteOffset);
        uint32_t transferred = 0;
        const uint32_t result = file->file->Write(buffer, length, offset, transferred);
        file->position = offset + transferred;
        SetIoStatus(iosb, result, transferred);
        SignalCompletion(event);
        return result;
    }

    // ---------------------------------------------------------------------------------------------
    // Information classes

    struct NetworkOpenInformation
    {
        be<uint64_t> creationTime;
        be<uint64_t> lastAccessTime;
        be<uint64_t> lastWriteTime;
        be<uint64_t> changeTime;
        be<uint64_t> allocationSize;
        be<uint64_t> endOfFile;
        be<uint32_t> attributes;
        be<uint32_t> reserved;
    };
    static_assert(sizeof(NetworkOpenInformation) == 0x38);

    void FillNetworkOpen(const vfs::EntryInfo& info, NetworkOpenInformation* out)
    {
        out->creationTime = info.creationTime;
        out->lastAccessTime = info.accessTime;
        out->lastWriteTime = info.writeTime;
        out->changeTime = info.writeTime;
        out->allocationSize = info.allocationSize;
        out->endOfFile = info.size;
        out->attributes = info.attributes;
        out->reserved = 0;
    }

    enum FileInformationClass : uint32_t
    {
        FileBasicInformation = 4,
        FileStandardInformation = 5,
        FileInternalInformation = 6,
        FileDispositionInformation = 13,
        FilePositionInformation = 14,
        FileAllocationInformation = 19,
        FileEndOfFileInformation = 20,
        FileNetworkOpenInformation = 34,
    };

    uint32_t NtQueryInformationFile(uint32_t handle, IoStatusBlock* iosb, uint8_t* information, uint32_t length, uint32_t informationClass)
    {
        auto file = LookupHandleAs<FileObject>(handle);
        if (!file)
            return status::kInvalidHandle;
        if (!information)
            return status::kInvalidParameter;

        std::lock_guard lock(file->lock);
        const vfs::EntryInfo& info = file->file->Info();
        uint32_t written = 0;
        switch (informationClass)
        {
        case FileBasicInformation:
            if (length < 0x28)
                return kInfoLengthMismatch;
            *reinterpret_cast<be<uint64_t>*>(information + 0x00) = info.creationTime;
            *reinterpret_cast<be<uint64_t>*>(information + 0x08) = info.accessTime;
            *reinterpret_cast<be<uint64_t>*>(information + 0x10) = info.writeTime;
            *reinterpret_cast<be<uint64_t>*>(information + 0x18) = info.writeTime;
            *reinterpret_cast<be<uint32_t>*>(information + 0x20) = info.attributes;
            *reinterpret_cast<be<uint32_t>*>(information + 0x24) = 0;
            written = 0x28;
            break;
        case FileStandardInformation:
            if (length < 0x18)
                return kInfoLengthMismatch;
            *reinterpret_cast<be<uint64_t>*>(information + 0x00) = info.allocationSize;
            *reinterpret_cast<be<uint64_t>*>(information + 0x08) = info.size;
            *reinterpret_cast<be<uint32_t>*>(information + 0x10) = 1; // links
            information[0x14] = 0;                                    // delete pending
            information[0x15] = info.isDirectory ? 1 : 0;
            information[0x16] = information[0x17] = 0;
            written = 0x18;
            break;
        case FileInternalInformation:
            if (length < 8)
                return kInfoLengthMismatch;
            *reinterpret_cast<be<uint64_t>*>(information) = std::hash<std::string>{}(file->path);
            written = 8;
            break;
        case FilePositionInformation:
            if (length < 8)
                return kInfoLengthMismatch;
            *reinterpret_cast<be<uint64_t>*>(information) = file->position;
            written = 8;
            break;
        case FileEndOfFileInformation:
        case FileAllocationInformation:
            if (length < 8)
                return kInfoLengthMismatch;
            *reinterpret_cast<be<uint64_t>*>(information) = informationClass == FileEndOfFileInformation ? info.size : info.allocationSize;
            written = 8;
            break;
        case FileNetworkOpenInformation:
            if (length < sizeof(NetworkOpenInformation))
                return kInfoLengthMismatch;
            FillNetworkOpen(info, reinterpret_cast<NetworkOpenInformation*>(information));
            written = sizeof(NetworkOpenInformation);
            break;
        default:
            LOG_WARN("NtQueryInformationFile: unsupported class %u on \"%s\"", informationClass, file->path.c_str());
            return 0xC0000003; // STATUS_INVALID_INFO_CLASS
        }
        SetIoStatus(iosb, status::kSuccess, written);
        return status::kSuccess;
    }

    uint32_t NtSetInformationFile(uint32_t handle, IoStatusBlock* iosb, const uint8_t* information, uint32_t length, uint32_t informationClass)
    {
        auto file = LookupHandleAs<FileObject>(handle);
        if (!file)
            return status::kInvalidHandle;
        if (!information)
            return status::kInvalidParameter;

        std::lock_guard lock(file->lock);
        uint32_t result = status::kSuccess;
        switch (informationClass)
        {
        case FilePositionInformation:
            file->position = *reinterpret_cast<const be<uint64_t>*>(information);
            break;
        case FileEndOfFileInformation:
            result = file->file->SetSize(*reinterpret_cast<const be<uint64_t>*>(information));
            break;
        case FileAllocationInformation:
            break; // preallocation is a hint
        case FileBasicInformation: // creation, access, write and change times (0: unchanged), attributes (not kept)
        {
            if (length < 0x28)
                return kInfoLengthMismatch;
            const auto* times = reinterpret_cast<const be<uint64_t>*>(information);
            result = file->file->SetTimes(times[0], times[1], times[2], times[3]);
            break;
        }
        case FileDispositionInformation:
            if (information[0])
                result = file->file->SetDeleteOnClose();
            break;
        default:
            LOG_WARN("NtSetInformationFile: unsupported class %u on \"%s\"", informationClass, file->path.c_str());
            return 0xC0000003; // STATUS_INVALID_INFO_CLASS
        }
        SetIoStatus(iosb, result, result == status::kSuccess ? length : 0);
        return result;
    }

    uint32_t NtQueryFullAttributesFile(const ObjectAttributes* attributes, NetworkOpenInformation* information)
    {
        std::string path;
        if (!BuildPath(attributes, path) || !information)
            return status::kInvalidParameter;
        std::string relative;
        vfs::Device* device = vfs::Resolve(path, relative);
        if (!device)
            return status::kObjectPathNotFound;
        vfs::EntryInfo info;
        const uint32_t result = device->Query(relative, info);
        if (result == status::kSuccess)
            FillNetworkOpen(info, information);
        else
            LOG_TRACE("NtQueryFullAttributesFile(\"%s\"): 0x%08X", path.c_str(), result);
        return result;
    }

    enum FsInformationClass : uint32_t
    {
        FileFsVolumeInformation = 1,
        FileFsSizeInformation = 3,
        FileFsDeviceInformation = 4,
        FileFsAttributeInformation = 5,
    };

    uint32_t NtQueryVolumeInformationFile(uint32_t handle, IoStatusBlock* iosb, uint8_t* information, uint32_t length, uint32_t informationClass)
    {
        auto file = LookupHandleAs<FileObject>(handle);
        if (!file)
            return status::kInvalidHandle;
        if (!information)
            return status::kInvalidParameter;

        constexpr uint32_t kSectorSize = 512;
        const uint32_t sectorsPerUnit = std::max<uint32_t>(1, file->device->BytesPerCluster() / kSectorSize);
        uint32_t written = 0;
        switch (informationClass)
        {
        case FileFsVolumeInformation:
            if (length < 0x12)
                return kInfoLengthMismatch;
            memset(information, 0, 0x12);
            *reinterpret_cast<be<uint32_t>*>(information + 0x08) = 0x12345678; // serial number
            written = 0x12;
            break;
        case FileFsSizeInformation:
            if (length < 0x18)
                return kInfoLengthMismatch;
            *reinterpret_cast<be<uint64_t>*>(information + 0x00) = file->device->TotalBytes() / (kSectorSize * sectorsPerUnit);
            *reinterpret_cast<be<uint64_t>*>(information + 0x08) = file->device->FreeBytes() / (kSectorSize * sectorsPerUnit);
            *reinterpret_cast<be<uint32_t>*>(information + 0x10) = sectorsPerUnit;
            *reinterpret_cast<be<uint32_t>*>(information + 0x14) = kSectorSize;
            written = 0x18;
            break;
        case FileFsDeviceInformation:
            if (length < 8)
                return kInfoLengthMismatch;
            *reinterpret_cast<be<uint32_t>*>(information + 0) = file->device->ReadOnly() ? 0x02 /* CD-ROM */ : 0x07 /* disk */;
            *reinterpret_cast<be<uint32_t>*>(information + 4) = file->device->ReadOnly() ? 0x01 /* removable */ : 0;
            written = 8;
            break;
        case FileFsAttributeInformation:
        {
            const char* name = file->device->FileSystemName();
            const uint32_t nameLength = uint32_t(strlen(name));
            if (length < 12 + nameLength)
                return kInfoLengthMismatch;
            *reinterpret_cast<be<uint32_t>*>(information + 0) = 0;   // attributes
            *reinterpret_cast<be<uint32_t>*>(information + 4) = 255; // max component length
            *reinterpret_cast<be<uint32_t>*>(information + 8) = nameLength;
            memcpy(information + 12, name, nameLength);
            written = 12 + nameLength;
            break;
        }
        default:
            LOG_WARN("NtQueryVolumeInformationFile: unsupported class %u", informationClass);
            return 0xC0000003; // STATUS_INVALID_INFO_CLASS
        }
        SetIoStatus(iosb, status::kSuccess, written);
        return status::kSuccess;
    }

    // Case-insensitive match with '*' and '?' wildcards.
    bool MatchPattern(std::string_view pattern, std::string_view name)
    {
        if (pattern.empty())
            return true;
        if (pattern.front() == '*')
        {
            for (size_t i = 0; i <= name.size(); i++)
            {
                if (MatchPattern(pattern.substr(1), name.substr(i)))
                    return true;
            }
            return false;
        }
        if (name.empty())
            return false;
        if (pattern.front() != '?' && std::tolower((unsigned char)pattern.front()) != std::tolower((unsigned char)name.front()))
            return false;
        return MatchPattern(pattern.substr(1), name.substr(1));
    }

    // FILE_DIRECTORY_INFORMATION with an ANSI name; one entry per call.
    uint32_t NtQueryDirectoryFile(uint32_t handle, uint32_t event, uint32_t /*apcRoutine*/, uint32_t /*apcContext*/, IoStatusBlock* iosb,
        uint8_t* information, uint32_t length, const CountedString* fileName, uint32_t restartScan)
    {
        auto file = LookupHandleAs<FileObject>(handle);
        if (!file)
            return status::kInvalidHandle;
        if (!file->file->Info().isDirectory)
            return kNotADirectory;

        std::lock_guard lock(file->lock);
        const std::string pattern = fileName ? ReadString(fileName) : "*";
        const bool first = !file->listingStarted || restartScan;
        if (first)
        {
            file->listing.clear();
            for (vfs::EntryInfo& entry : file->file->List())
            {
                if (MatchPattern(pattern, entry.name))
                    file->listing.push_back(std::move(entry));
            }
            file->listingIndex = 0;
            file->listingStarted = true;
        }

        if (file->listingIndex >= file->listing.size())
        {
            const uint32_t result = (first && file->listing.empty()) ? status::kNoSuchFile : kNoMoreFiles;
            SetIoStatus(iosb, result, 0);
            SignalCompletion(event);
            return result;
        }

        const vfs::EntryInfo& entry = file->listing[file->listingIndex];
        const uint32_t nameLength = uint32_t(entry.name.size());
        constexpr uint32_t kHeaderSize = 0x40;
        if (length < kHeaderSize + nameLength)
            return status::kBufferOverflow;

        memset(information, 0, kHeaderSize);
        *reinterpret_cast<be<uint32_t>*>(information + 0x00) = 0; // next entry offset: single entry
        *reinterpret_cast<be<uint32_t>*>(information + 0x04) = uint32_t(file->listingIndex);
        *reinterpret_cast<be<uint64_t>*>(information + 0x08) = entry.creationTime;
        *reinterpret_cast<be<uint64_t>*>(information + 0x10) = entry.accessTime;
        *reinterpret_cast<be<uint64_t>*>(information + 0x18) = entry.writeTime;
        *reinterpret_cast<be<uint64_t>*>(information + 0x20) = entry.writeTime;
        *reinterpret_cast<be<uint64_t>*>(information + 0x28) = entry.size;
        *reinterpret_cast<be<uint64_t>*>(information + 0x30) = entry.allocationSize;
        *reinterpret_cast<be<uint32_t>*>(information + 0x38) = entry.attributes;
        *reinterpret_cast<be<uint32_t>*>(information + 0x3C) = nameLength;
        memcpy(information + kHeaderSize, entry.name.data(), nameLength);
        file->listingIndex++;

        SetIoStatus(iosb, status::kSuccess, kHeaderSize + nameLength);
        SignalCompletion(event);
        return status::kSuccess;
    }

    uint32_t NtFlushBuffersFile(uint32_t handle, IoStatusBlock* iosb)
    {
        auto file = LookupHandleAs<FileObject>(handle);
        if (!file)
            return status::kInvalidHandle;
        const uint32_t result = file->file->Flush();
        SetIoStatus(iosb, result, 0);
        return result;
    }

    uint32_t NtDeviceIoControlFile(uint32_t handle, uint32_t event, uint32_t /*apcRoutine*/, uint32_t /*apcContext*/, IoStatusBlock* iosb,
        uint32_t ioControlCode, uint32_t /*input*/, uint32_t /*inputLength*/, void* output, uint32_t outputLength)
    {
        auto file = LookupHandleAs<FileObject>(handle);
        if (!file)
            return status::kInvalidHandle;
        uint32_t returned = 0;
        guest::BeforeHostWriteTo(output, outputLength);
        const uint32_t result = file->file->IoControl(ioControlCode, output, outputLength, returned);
        if (result != status::kSuccess)
            LOG_WARN("NtDeviceIoControlFile(\"%s\", 0x%08X): 0x%08X", file->path.c_str(), ioControlCode, result);
        SetIoStatus(iosb, result, returned);
        SignalCompletion(event);
        return result;
    }

    uint32_t IoDismountVolumeByFileHandle(uint32_t /*handle*/)
    {
        return status::kSuccess;
    }

    uint32_t ObCreateSymbolicLink(const CountedString* link, const CountedString* target)
    {
        const std::string linkPath = ReadString(link), targetPath = ReadString(target);
        if (linkPath.empty() || targetPath.empty())
            return status::kInvalidParameter;
        LOG_INFO("Symbolic link %s -> %s", linkPath.c_str(), targetPath.c_str());
        vfs::AddSymbolicLink(linkPath, targetPath);
        return status::kSuccess;
    }

    uint32_t ObDeleteSymbolicLink(const CountedString* link)
    {
        return vfs::RemoveSymbolicLink(ReadString(link)) ? status::kSuccess : status::kObjectNameNotFound;
    }

    // ---------------------------------------------------------------------------------------------
    // STFS devices. XAPI's secure utility drive (XMountUtilityDrive for titles without the insecure utility drive
    // privilege, DOAX2's cache:) formats a cache partition as FATX, then has a XAM task create an STFS volume over the raw
    // partition and link cache: to it. The volume's files are kept as the partition's host files instead: the STFS
    // device's name becomes a link to the partition's file system.

    // What XAPI's utility drive task passes (the fields it sets; 0x58 bytes).
    struct StfsCreateParameters
    {
        CountedString deviceName;     // 0x00 "\Device\cache0"
        be<uint64_t> blockSize;       // 0x08 4096
        be<uint64_t> volumeSize;      // 0x10 the partition's size
        be<uint32_t> partitionHandle; // 0x18 the raw partition, opened read/write
        uint8_t volumeDescriptor[36]; // 0x1C kept in Partition0's cache header; zeros for a new volume
        be<uint32_t> cacheBuffer;     // 0x40 memory for the volume's block cache
        be<uint32_t> unknown44;
        be<uint32_t> deviceObject;    // 0x48 returned: StfsControlDevice and ObDereferenceObject take it
    };
    static_assert(offsetof(StfsCreateParameters, deviceObject) == 0x48);

    uint32_t StfsCreateDevice(StfsCreateParameters* parameters, uint32_t /*parametersSize*/)
    {
        if (!parameters)
            return status::kInvalidParameter;
        auto partition = LookupHandleAs<FileObject>(parameters->partitionHandle);
        const std::string name = ReadString(&parameters->deviceName);
        if (!partition || name.empty())
            return status::kInvalidParameter;
        vfs::AddSymbolicLink(name, partition->path);
        static const uint32_t deviceObject = guest::KernelAlloc(0x40);
        parameters->deviceObject = deviceObject;
        LOG_INFO("STFS device %s over %s, as host files", name.c_str(), partition->path.c_str());
        return status::kSuccess;
    }

    // Flushes, and reading and writing the volume descriptor: the files are host files, so there's nothing to do.
    uint32_t StfsControlDevice(uint32_t /*deviceObject*/, uint32_t /*code*/, uint32_t /*buffer*/)
    {
        return status::kSuccess;
    }

    // XSetFileCacheSize: the file system cache's size in 4 KB pages. Host files are cached by the host.
    uint32_t FscSetCacheElementCount(uint32_t /*unknown*/, uint32_t pages)
    {
        LOG_TRACE("FscSetCacheElementCount(%u pages)", pages);
        return status::kSuccess;
    }
}

GUEST_EXPORT(NtCreateFile, NtCreateFile)
GUEST_EXPORT(NtOpenFile, NtOpenFile)
GUEST_EXPORT(NtReadFile, NtReadFile)
GUEST_EXPORT(NtWriteFile, NtWriteFile)
GUEST_EXPORT(NtQueryInformationFile, NtQueryInformationFile)
GUEST_EXPORT(NtSetInformationFile, NtSetInformationFile)
GUEST_EXPORT(NtQueryFullAttributesFile, NtQueryFullAttributesFile)
GUEST_EXPORT(NtQueryVolumeInformationFile, NtQueryVolumeInformationFile)
GUEST_EXPORT(NtQueryDirectoryFile, NtQueryDirectoryFile)
GUEST_EXPORT(NtFlushBuffersFile, NtFlushBuffersFile)
GUEST_EXPORT(NtDeviceIoControlFile, NtDeviceIoControlFile)
GUEST_EXPORT(IoDismountVolumeByFileHandle, IoDismountVolumeByFileHandle)
GUEST_EXPORT(ObCreateSymbolicLink, ObCreateSymbolicLink)
GUEST_EXPORT(ObDeleteSymbolicLink, ObDeleteSymbolicLink)
GUEST_EXPORT(StfsCreateDevice, StfsCreateDevice)
GUEST_EXPORT(StfsControlDevice, StfsControlDevice)
GUEST_EXPORT(FscSetCacheElementCount, FscSetCacheElementCount)
