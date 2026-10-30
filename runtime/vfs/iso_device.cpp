// XDVDFS disc image device. The directory tree is parsed once at mount; reads go straight to the image.
#include "vfs/iso_device.h"

#include "kernel/status.h"
#include "log.h"

#include <algorithm>
#include <cstring>
#include <map>

#include <windows.h>

namespace vfs
{
    namespace
    {
        constexpr uint64_t kSectorSize = 2048;
        constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
        // Game partition offsets: plain XISO, XGD1, XGD2, XGD3.
        constexpr uint64_t kPartitionOffsets[] = { 0, 0x18300000, 0xFD90000, 0x2080000 };

        std::string Lower(std::string_view text)
        {
            std::string result(text);
            std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return result;
        }

        struct Node
        {
            EntryInfo info;
            uint64_t offset = 0; // byte offset of the data in the image
            std::vector<std::string> children; // lower-case paths of direct children
        };

        class IsoImage
        {
        public:
            HANDLE file = INVALID_HANDLE_VALUE;
            uint64_t imageSize = 0;
            uint64_t partition = 0;
            uint64_t creationTime = 0;
            std::map<std::string, Node> nodes; // lower-case relative path -> node ("" is the root)

            ~IsoImage()
            {
                if (file != INVALID_HANDLE_VALUE)
                    CloseHandle(file);
            }

            bool ReadAt(uint64_t offset, void* buffer, uint32_t length, uint32_t& transferred) const
            {
                OVERLAPPED overlapped{};
                overlapped.Offset = DWORD(offset);
                overlapped.OffsetHigh = DWORD(offset >> 32);
                DWORD read = 0;
                const BOOL ok = ReadFile(file, buffer, length, &read, &overlapped);
                transferred = read;
                return ok || GetLastError() == ERROR_HANDLE_EOF;
            }

            bool ReadExact(uint64_t offset, void* buffer, uint32_t length) const
            {
                uint32_t transferred = 0;
                return ReadAt(offset, buffer, length, transferred) && transferred == length;
            }

            // Parses a directory's binary tree of entries into `nodes`.
            bool ParseDirectory(const std::string& path, uint32_t sector, uint32_t size, int depth)
            {
                if (depth > 32 || size == 0)
                    return true;
                std::vector<uint8_t> table(size);
                if (!ReadExact(partition + sector * kSectorSize, table.data(), size))
                    return false;

                std::vector<uint32_t> stack{ 0 };
                std::vector<bool> seen(size / 4 + 1);
                while (!stack.empty())
                {
                    const uint32_t offset = stack.back();
                    stack.pop_back();
                    if (offset + 14 > size || seen[offset / 4])
                        continue;
                    seen[offset / 4] = true;

                    uint16_t left, right;
                    uint32_t entrySector, entrySize;
                    memcpy(&left, &table[offset], 2);
                    memcpy(&right, &table[offset + 2], 2);
                    memcpy(&entrySector, &table[offset + 4], 4);
                    memcpy(&entrySize, &table[offset + 8], 4);
                    const uint8_t attributes = table[offset + 12];
                    const uint8_t nameLength = table[offset + 13];
                    if (left == 0xFFFF) // padding
                        continue;
                    if (offset + 14 + nameLength > size)
                        continue;

                    const std::string name(reinterpret_cast<const char*>(&table[offset + 14]), nameLength);
                    const std::string childPath = path.empty() ? name : path + "\\" + name;
                    Node node;
                    node.info.name = name;
                    node.info.isDirectory = attributes & kAttributeDirectory;
                    node.info.size = node.info.isDirectory ? 0 : entrySize;
                    node.info.allocationSize = (uint64_t(entrySize) + kSectorSize - 1) / kSectorSize * kSectorSize;
                    node.info.attributes = (attributes & kAttributeDirectory) ? kAttributeDirectory : kAttributeReadOnly;
                    node.info.creationTime = node.info.accessTime = node.info.writeTime = creationTime;
                    node.offset = partition + uint64_t(entrySector) * kSectorSize;
                    nodes[Lower(path)].children.push_back(Lower(childPath));
                    const std::string key = Lower(childPath);
                    nodes[key] = node;

                    if (node.info.isDirectory && !ParseDirectory(childPath, entrySector, entrySize, depth + 1))
                        return false;
                    if (left)
                        stack.push_back(uint32_t(left) * 4);
                    if (right)
                        stack.push_back(uint32_t(right) * 4);
                }
                return true;
            }
        };

        class IsoFile : public File
        {
        public:
            IsoFile(std::shared_ptr<const IsoImage> image, const Node& node) : m_image(std::move(image)), m_node(node) {}

            const EntryInfo& Info() const override { return m_node.info; }

            uint32_t Read(void* buffer, uint32_t length, uint64_t offset, uint32_t& transferred) override
            {
                transferred = 0;
                if (m_node.info.isDirectory)
                    return status::kInvalidParameter;
                if (offset >= m_node.info.size)
                    return status::kEndOfFile;
                const uint32_t count = uint32_t(std::min<uint64_t>(length, m_node.info.size - offset));
                return m_image->ReadAt(m_node.offset + offset, buffer, count, transferred) ? status::kSuccess : status::kUnsuccessful;
            }

            uint32_t Write(const void*, uint32_t, uint64_t, uint32_t& transferred) override
            {
                transferred = 0;
                return status::kAccessDenied;
            }

            uint32_t SetSize(uint64_t) override { return status::kAccessDenied; }
            uint32_t SetDeleteOnClose() override { return status::kAccessDenied; }

            std::vector<EntryInfo> List() const override
            {
                std::vector<EntryInfo> entries;
                for (const std::string& child : m_node.children)
                    entries.push_back(m_image->nodes.at(child).info);
                return entries;
            }

        private:
            std::shared_ptr<const IsoImage> m_image;
            const Node& m_node;
        };

        class IsoDevice : public Device
        {
        public:
            explicit IsoDevice(std::shared_ptr<IsoImage> image) : m_image(std::move(image)) {}

            const char* Name() const override { return "disc image"; }
            bool ReadOnly() const override { return true; }

            uint32_t Open(const OpenRequest& request, std::unique_ptr<File>& file, OpenResult& result) override
            {
                const auto it = m_image->nodes.find(Lower(request.path));
                if (it == m_image->nodes.end())
                {
                    // Distinguish a missing parent directory from a missing file, as the kernel does.
                    const size_t slash = request.path.rfind('\\');
                    const bool parentExists = slash == std::string::npos || m_image->nodes.count(Lower(request.path.substr(0, slash)));
                    if (request.disposition != Disposition::Open && request.disposition != Disposition::Overwrite)
                        return status::kAccessDenied; // can't create on a read-only disc
                    return parentExists ? status::kObjectNameNotFound : status::kObjectPathNotFound;
                }
                const Node& node = it->second;
                if (request.write || (request.disposition != Disposition::Open && request.disposition != Disposition::OpenIf))
                    return status::kAccessDenied;
                if (request.directory && !node.info.isDirectory)
                    return 0xC0000103; // STATUS_NOT_A_DIRECTORY
                if (request.nonDirectory && node.info.isDirectory)
                    return 0xC00000BA; // STATUS_FILE_IS_A_DIRECTORY
                file = std::make_unique<IsoFile>(m_image, node);
                result = OpenResult::Opened;
                return status::kSuccess;
            }

            uint32_t Query(const std::string& path, EntryInfo& info) override
            {
                const auto it = m_image->nodes.find(Lower(path));
                if (it == m_image->nodes.end())
                    return status::kObjectNameNotFound;
                info = it->second.info;
                return status::kSuccess;
            }

            uint64_t TotalBytes() const override { return m_image->imageSize - m_image->partition; }
            uint64_t FreeBytes() const override { return 0; }
            const char* FileSystemName() const override { return "GDFX"; }

        private:
            std::shared_ptr<IsoImage> m_image;
        };
    }

    std::unique_ptr<Device> OpenIsoDevice(const std::string& isoPath)
    {
        auto image = std::make_shared<IsoImage>();
        image->file = CreateFileA(isoPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (image->file == INVALID_HANDLE_VALUE)
        {
            LOG_ERROR("Can't open disc image %s", isoPath.c_str());
            return nullptr;
        }

        LARGE_INTEGER size;
        GetFileSizeEx(image->file, &size);
        image->imageSize = uint64_t(size.QuadPart);
        FILETIME writeTime;
        GetFileTime(image->file, nullptr, nullptr, &writeTime);
        image->creationTime = (uint64_t(writeTime.dwHighDateTime) << 32) | writeTime.dwLowDateTime;

        bool found = false;
        uint8_t descriptor[0x800];
        for (uint64_t partition : kPartitionOffsets)
        {
            if (partition + 33 * kSectorSize <= image->imageSize && image->ReadExact(partition + 32 * kSectorSize, descriptor, sizeof(descriptor)) &&
                memcmp(descriptor, kMagic, sizeof(kMagic) - 1) == 0)
            {
                image->partition = partition;
                found = true;
                break;
            }
        }
        if (!found)
        {
            LOG_ERROR("%s isn't an Xbox disc image (no XDVDFS volume descriptor)", isoPath.c_str());
            return nullptr;
        }

        uint32_t rootSector, rootSize;
        memcpy(&rootSector, descriptor + 20, 4);
        memcpy(&rootSize, descriptor + 24, 4);
        // Files' times are the volume's, from when the disc was mastered (the image file's own time if it has none):
        // the same wherever the image is kept, which matters to a game comparing them with files it copied.
        uint64_t volumeTime;
        memcpy(&volumeTime, descriptor + 28, 8);
        if (volumeTime)
            image->creationTime = volumeTime;

        Node& root = image->nodes[""];
        root.info.isDirectory = true;
        root.info.attributes = kAttributeDirectory;
        root.info.creationTime = root.info.accessTime = root.info.writeTime = image->creationTime;
        if (!image->ParseDirectory("", rootSector, rootSize, 0))
        {
            LOG_ERROR("Failed to read the directory tree of %s", isoPath.c_str());
            return nullptr;
        }

        LOG_INFO("Disc image %s: partition at 0x%llX, %zu entries", isoPath.c_str(), (unsigned long long)image->partition, image->nodes.size() - 1);
        return std::make_unique<IsoDevice>(std::move(image));
    }
}
