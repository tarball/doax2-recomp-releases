// Writable device backed by a host directory (hard disk partitions: saves, cache), and the partitions' raw sectors.
#include "vfs/iso_device.h"

#include "kernel/status.h"
#include "log.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <optional>
#include <system_error>

#include <windows.h>
#include <winioctl.h>

namespace vfs
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr uint32_t kSectorSize = 512;
        constexpr uint32_t kDefaultBytesPerCluster = 0x4000;

        uint64_t ToFileTime(const FILETIME& time)
        {
            return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        }

        void StoreBig32(uint8_t* out, uint32_t value)
        {
            value = __builtin_bswap32(value);
            memcpy(out, &value, 4);
        }

        void StoreBig64(uint8_t* out, uint64_t value)
        {
            value = __builtin_bswap64(value);
            memcpy(out, &value, 8);
        }

        uint32_t StatusFromWin32(DWORD error)
        {
            switch (error)
            {
            case ERROR_FILE_NOT_FOUND: return status::kObjectNameNotFound;
            case ERROR_PATH_NOT_FOUND: return status::kObjectPathNotFound;
            case ERROR_ACCESS_DENIED: return status::kAccessDenied;
            case ERROR_FILE_EXISTS:
            case ERROR_ALREADY_EXISTS: return 0xC0000035; // STATUS_OBJECT_NAME_COLLISION
            case ERROR_DISK_FULL: return 0xC000007F;      // STATUS_DISK_FULL
            case ERROR_DIR_NOT_EMPTY: return 0xC0000101;  // STATUS_DIRECTORY_NOT_EMPTY
            default: return status::kUnsuccessful;
            }
        }

        bool FillInfo(const fs::path& path, EntryInfo& info)
        {
            WIN32_FILE_ATTRIBUTE_DATA data;
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
                return false;
            info.name = path.filename().string();
            info.isDirectory = data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
            info.size = info.isDirectory ? 0 : (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            info.allocationSize = (info.size + 0xFFF) & ~uint64_t(0xFFF);
            info.creationTime = ToFileTime(data.ftCreationTime);
            info.accessTime = ToFileTime(data.ftLastAccessTime);
            info.writeTime = ToFileTime(data.ftLastWriteTime);
            info.attributes = info.isDirectory ? kAttributeDirectory : kAttributeNormal;
            return true;
        }

        class HostFile : public File
        {
        public:
            HostFile(fs::path path, HANDLE handle, EntryInfo info) : m_path(std::move(path)), m_handle(handle), m_info(std::move(info)) {}

            ~HostFile() override
            {
                if (m_handle != INVALID_HANDLE_VALUE)
                    CloseHandle(m_handle);
                if (m_deleteOnClose)
                {
                    std::error_code error;
                    fs::remove(m_path, error);
                }
            }

            const EntryInfo& Info() const override
            {
                FillInfo(m_path, m_info); // sizes and times change as the file is written
                return m_info;
            }

            uint32_t Read(void* buffer, uint32_t length, uint64_t offset, uint32_t& transferred) override
            {
                transferred = 0;
                if (m_info.isDirectory)
                    return status::kInvalidParameter;
                OVERLAPPED overlapped{};
                overlapped.Offset = DWORD(offset);
                overlapped.OffsetHigh = DWORD(offset >> 32);
                DWORD read = 0;
                if (!ReadFile(m_handle, buffer, length, &read, &overlapped))
                    return GetLastError() == ERROR_HANDLE_EOF ? status::kEndOfFile : StatusFromWin32(GetLastError());
                transferred = read;
                return (read == 0 && length != 0) ? status::kEndOfFile : status::kSuccess;
            }

            uint32_t Write(const void* buffer, uint32_t length, uint64_t offset, uint32_t& transferred) override
            {
                transferred = 0;
                if (m_info.isDirectory)
                    return status::kInvalidParameter;
                OVERLAPPED overlapped{};
                overlapped.Offset = DWORD(offset);
                overlapped.OffsetHigh = DWORD(offset >> 32);
                DWORD written = 0;
                if (!WriteFile(m_handle, buffer, length, &written, &overlapped))
                    return StatusFromWin32(GetLastError());
                transferred = written;
                return status::kSuccess;
            }

            uint32_t SetSize(uint64_t size) override
            {
                FILE_END_OF_FILE_INFO info{};
                info.EndOfFile.QuadPart = LONGLONG(size);
                return SetFileInformationByHandle(m_handle, FileEndOfFileInfo, &info, sizeof(info)) ? status::kSuccess : StatusFromWin32(GetLastError());
            }

            uint32_t Flush() override
            {
                return (m_info.isDirectory || FlushFileBuffers(m_handle)) ? status::kSuccess : StatusFromWin32(GetLastError());
            }

            std::vector<EntryInfo> List() const override
            {
                std::vector<EntryInfo> entries;
                std::error_code error;
                for (const auto& entry : fs::directory_iterator(m_path, error))
                {
                    EntryInfo info;
                    if (FillInfo(entry.path(), info))
                        entries.push_back(std::move(info));
                }
                return entries;
            }

            uint32_t SetDeleteOnClose() override
            {
                m_deleteOnClose = true;
                return status::kSuccess;
            }

            uint32_t SetTimes(uint64_t creation, uint64_t access, uint64_t write, uint64_t change) override
            {
                if (m_handle == INVALID_HANDLE_VALUE)
                    return status::kSuccess; // directories keep the host's times
                FILE_BASIC_INFO info{};
                info.CreationTime.QuadPart = LONGLONG(creation);
                info.LastAccessTime.QuadPart = LONGLONG(access);
                info.LastWriteTime.QuadPart = LONGLONG(write);
                info.ChangeTime.QuadPart = LONGLONG(change);
                return SetFileInformationByHandle(m_handle, FileBasicInfo, &info, sizeof(info)) ? status::kSuccess : StatusFromWin32(GetLastError());
            }

        private:
            fs::path m_path;
            HANDLE m_handle;
            mutable EntryInfo m_info;
            bool m_deleteOnClose = false;
        };

        // A partition's raw sectors, kept in a sparse host file: what was never written reads as zeros.
        class RawPartitionFile : public File
        {
        public:
            RawPartitionFile(HANDLE store, PartitionLayout layout) : m_store(store), m_layout(layout)
            {
                m_info.size = layout.length;
                m_info.allocationSize = layout.length;
                m_info.attributes = kAttributeNormal;
            }

            ~RawPartitionFile() override
            {
                CloseHandle(m_store);
            }

            const EntryInfo& Info() const override { return m_info; }

            uint32_t Read(void* buffer, uint32_t length, uint64_t offset, uint32_t& transferred) override
            {
                transferred = 0;
                if (offset >= m_layout.length)
                    return status::kEndOfFile;
                length = uint32_t(std::min<uint64_t>(length, m_layout.length - offset));
                OVERLAPPED overlapped{};
                overlapped.Offset = DWORD(offset);
                overlapped.OffsetHigh = DWORD(offset >> 32);
                DWORD read = 0;
                if (!ReadFile(m_store, buffer, length, &read, &overlapped) && GetLastError() != ERROR_HANDLE_EOF)
                    return StatusFromWin32(GetLastError());
                memset(static_cast<uint8_t*>(buffer) + read, 0, length - read);
                transferred = length;
                return status::kSuccess;
            }

            uint32_t Write(const void* buffer, uint32_t length, uint64_t offset, uint32_t& transferred) override
            {
                transferred = 0;
                if (offset >= m_layout.length)
                    return 0xC000007F; // STATUS_DISK_FULL
                length = uint32_t(std::min<uint64_t>(length, m_layout.length - offset));
                OVERLAPPED overlapped{};
                overlapped.Offset = DWORD(offset);
                overlapped.OffsetHigh = DWORD(offset >> 32);
                DWORD written = 0;
                if (!WriteFile(m_store, buffer, length, &written, &overlapped))
                    return StatusFromWin32(GetLastError());
                transferred = written;
                return status::kSuccess;
            }

            uint32_t SetSize(uint64_t /*size*/) override { return status::kInvalidParameter; }

            uint32_t Flush() override
            {
                return FlushFileBuffers(m_store) ? status::kSuccess : StatusFromWin32(GetLastError());
            }

            uint32_t SetDeleteOnClose() override { return status::kAccessDenied; }

            // The disk queries XAPI's FATX format asks (output layouts as Xenia answers them).
            uint32_t IoControl(uint32_t code, void* output, uint32_t outputLength, uint32_t& returned) override
            {
                constexpr uint32_t kGetDriveGeometry = 0x70000;  // { sectors, bytes per sector }
                constexpr uint32_t kGetPartitionInfo = 0x74004;  // { 64-bit starting offset, 64-bit length }
                constexpr uint32_t kBufferTooSmall = 0xC0000023;
                auto* out = static_cast<uint8_t*>(output);
                switch (code)
                {
                case kGetDriveGeometry:
                    if (!out || outputLength < 8)
                        return kBufferTooSmall;
                    StoreBig32(out, uint32_t(m_layout.length / kSectorSize));
                    StoreBig32(out + 4, kSectorSize);
                    returned = 8;
                    return status::kSuccess;
                case kGetPartitionInfo:
                    if (!out || outputLength < 16)
                        return kBufferTooSmall;
                    StoreBig64(out, m_layout.offset);
                    StoreBig64(out + 8, m_layout.length);
                    returned = 16;
                    return status::kSuccess;
                default:
                    return File::IoControl(code, output, outputLength, returned);
                }
            }

        private:
            HANDLE m_store;
            PartitionLayout m_layout;
            EntryInfo m_info;
        };

        // The raw view of a hard disk partition: its sectors' host file and its place on the disk.
        struct RawPartition
        {
            fs::path store;
            PartitionLayout layout;
        };

        class HostDevice : public Device
        {
        public:
            HostDevice(std::string name, fs::path root, std::optional<RawPartition> raw = std::nullopt)
                : m_name(std::move(name)), m_root(std::move(root)), m_raw(std::move(raw))
            {
            }

            const char* Name() const override { return m_name.c_str(); }
            bool ReadOnly() const override { return false; }

            uint32_t Open(const OpenRequest& request, std::unique_ptr<File>& file, OpenResult& result) override
            {
                if (m_raw && (request.raw || m_root.empty()))
                {
                    if (!request.raw)
                        return 0xC000014F; // STATUS_UNRECOGNIZED_VOLUME: no file system (Partition0, the whole disk)
                    HANDLE store = OpenStore();
                    if (store == INVALID_HANDLE_VALUE)
                        return StatusFromWin32(GetLastError());
                    file = std::make_unique<RawPartitionFile>(store, m_raw->layout);
                    result = OpenResult::Opened;
                    return status::kSuccess;
                }

                fs::path path;
                if (!HostPath(request.path, path))
                    return status::kObjectPathNotFound;

                std::error_code error;
                const bool exists = fs::exists(path, error);
                const bool isDirectory = exists && fs::is_directory(path, error);

                if (request.directory || isDirectory)
                {
                    if (request.nonDirectory && isDirectory)
                        return 0xC00000BA; // STATUS_FILE_IS_A_DIRECTORY
                    if (!exists)
                    {
                        if (request.disposition != Disposition::Create && request.disposition != Disposition::OpenIf)
                            return fs::exists(path.parent_path(), error) ? status::kObjectNameNotFound : status::kObjectPathNotFound;
                        if (!fs::create_directory(path, error))
                            return fs::exists(path.parent_path()) ? status::kUnsuccessful : status::kObjectPathNotFound;
                        result = OpenResult::Created;
                    }
                    else
                    {
                        if (request.disposition == Disposition::Create)
                            return 0xC0000035; // STATUS_OBJECT_NAME_COLLISION
                        if (!isDirectory)
                            return 0xC0000103; // STATUS_NOT_A_DIRECTORY
                        result = OpenResult::Opened;
                    }
                    EntryInfo info;
                    FillInfo(path, info);
                    file = std::make_unique<HostFile>(path, INVALID_HANDLE_VALUE, info);
                    return status::kSuccess;
                }

                DWORD creation;
                switch (request.disposition)
                {
                case Disposition::Supersede: creation = CREATE_ALWAYS; result = exists ? OpenResult::Superseded : OpenResult::Created; break;
                case Disposition::Open: creation = OPEN_EXISTING; result = OpenResult::Opened; break;
                case Disposition::Create: creation = CREATE_NEW; result = OpenResult::Created; break;
                case Disposition::OpenIf: creation = OPEN_ALWAYS; result = exists ? OpenResult::Opened : OpenResult::Created; break;
                case Disposition::Overwrite: creation = TRUNCATE_EXISTING; result = OpenResult::Overwritten; break;
                case Disposition::OverwriteIf: creation = CREATE_ALWAYS; result = exists ? OpenResult::Overwritten : OpenResult::Created; break;
                default: return status::kInvalidParameter;
                }

                // Times can be set through any handle (DOAX2 sets those of the files it copies to the cache through the
                // handle it read them back with), so every one can write attributes.
                const bool write = request.write || creation != OPEN_EXISTING;
                HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | FILE_WRITE_ATTRIBUTES | (write ? GENERIC_WRITE : 0),
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (handle == INVALID_HANDLE_VALUE)
                    return StatusFromWin32(GetLastError());

                EntryInfo info;
                FillInfo(path, info);
                file = std::make_unique<HostFile>(path, handle, info);
                return status::kSuccess;
            }

            uint32_t Query(const std::string& relative, EntryInfo& info) override
            {
                if (m_root.empty())
                    return status::kObjectPathNotFound;
                fs::path path;
                if (!HostPath(relative, path))
                    return status::kObjectPathNotFound;
                if (!FillInfo(path, info))
                {
                    std::error_code error;
                    return fs::exists(path.parent_path(), error) ? status::kObjectNameNotFound : status::kObjectPathNotFound;
                }
                return status::kSuccess;
            }

            // A partition is its own size; other host devices (content packages) report a 20 GB retail drive.
            uint64_t TotalBytes() const override { return m_raw ? m_raw->layout.length : 20ull * 1024 * 1024 * 1024; }

            // What the host has, up to what's left of the partition after the files already in it.
            uint64_t FreeBytes() const override
            {
                if (m_root.empty())
                    return 0;
                std::error_code error;
                const auto space = fs::space(m_root, error);
                if (error)
                    return 0;
                uint64_t used = 0;
                if (m_raw)
                {
                    const uint64_t cluster = BytesPerCluster();
                    for (auto it = fs::recursive_directory_iterator(m_root, error); !error && it != fs::recursive_directory_iterator(); it.increment(error))
                    {
                        if (it->is_regular_file(error))
                            used += (it->file_size(error) + cluster - 1) / cluster * cluster;
                    }
                }
                return std::min<uint64_t>(space.available, TotalBytes() - std::min(used, TotalBytes()));
            }

            const char* FileSystemName() const override { return "FATX"; }

            // A FATX superblock ("XTAF", serial, sectors per cluster, ...) at the start of the raw partition gives the
            // cluster size it was formatted with; XAPI checks it against the one it asks for when it mounts the cache.
            uint32_t BytesPerCluster() const override
            {
                if (!m_raw)
                    return kDefaultBytesPerCluster;
                HANDLE store = CreateFileW(m_raw->store.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (store == INVALID_HANDLE_VALUE)
                    return kDefaultBytesPerCluster;
                uint8_t superblock[12] = {};
                DWORD read = 0;
                const bool ok = ReadFile(store, superblock, sizeof(superblock), &read, nullptr) && read == sizeof(superblock);
                CloseHandle(store);
                if (!ok || memcmp(superblock, "XTAF", 4) != 0)
                    return kDefaultBytesPerCluster;
                const uint32_t sectorsPerCluster = (uint32_t(superblock[8]) << 24) | (uint32_t(superblock[9]) << 16) | (uint32_t(superblock[10]) << 8) | superblock[11];
                return sectorsPerCluster != 0 && sectorsPerCluster <= 1024 ? sectorsPerCluster * kSectorSize : kDefaultBytesPerCluster;
            }

        private:
            HANDLE OpenStore() const
            {
                HANDLE store = CreateFileW(m_raw->store.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (store != INVALID_HANDLE_VALUE && GetLastError() != ERROR_ALREADY_EXISTS)
                {
                    // New: sparse, so writes far into the partition don't allocate everything before them.
                    DWORD returned = 0;
                    DeviceIoControl(store, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
                }
                return store;
            }

            // Maps a guest relative path into the root, refusing anything that escapes it.
            bool HostPath(const std::string& relative, fs::path& out) const
            {
                fs::path path = m_root;
                size_t start = 0;
                while (start <= relative.size())
                {
                    const size_t end = std::min(relative.find('\\', start), relative.size());
                    const std::string part = relative.substr(start, end - start);
                    if (part == "..")
                        return false;
                    if (!part.empty() && part != ".")
                        path /= part;
                    start = end + 1;
                }
                out = path;
                return true;
            }

            std::string m_name;
            fs::path m_root; // empty: no file system
            std::optional<RawPartition> m_raw;
        };
    }

    std::unique_ptr<Device> OpenHostDevice(const std::string& name, const std::string& hostDirectory)
    {
        std::error_code error;
        fs::create_directories(hostDirectory, error);
        if (error)
        {
            LOG_ERROR("Can't create %s: %s", hostDirectory.c_str(), error.message().c_str());
            return nullptr;
        }
        return std::make_unique<HostDevice>(name, fs::absolute(hostDirectory));
    }

    std::unique_ptr<Device> OpenHardDiskPartition(const std::string& name, const std::string& hostDirectory, const std::string& rawStore,
        PartitionLayout layout)
    {
        std::error_code error;
        if (!hostDirectory.empty())
            fs::create_directories(hostDirectory, error);
        if (!error)
            fs::create_directories(fs::path(rawStore).parent_path(), error);
        if (error)
        {
            LOG_ERROR("Can't create the folders for %s: %s", name.c_str(), error.message().c_str());
            return nullptr;
        }
        return std::make_unique<HostDevice>(name, hostDirectory.empty() ? fs::path() : fs::absolute(hostDirectory),
            RawPartition{ fs::absolute(rawStore), layout });
    }
}
