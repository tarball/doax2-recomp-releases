// XamContent*: save games and other content packages, stored as host folders:
//   <data>/content/<XUID>/<title ID>/<content type>/<file name>/
// with the package's display name kept in "<file name>.name" next to it. There is one storage device,
// the hard drive (device ID 1).
#include "config.h"
#include "guest_abi.h"
#include "kernel/objects.h"
#include "kernel/threads.h"
#include "log.h"
#include "netplay/frame.h"
#include "vfs/iso_device.h"
#include "vfs/vfs.h"
#include "xam/xam.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using namespace kernel;

    constexpr uint32_t kHardDriveId = 1;
    constexpr uint32_t kErrorPathNotFound = 3;
    constexpr uint32_t kErrorFileNotFound = 2;
    constexpr uint32_t kErrorAlreadyExists = 183;

    // XCONTENT_DATA
    struct ContentData
    {
        be<uint32_t> deviceId;
        be<uint32_t> contentType;
        be<uint16_t> displayName[128];
        char fileName[42];
        uint8_t padding[2];
    };
    static_assert(sizeof(ContentData) == 0x134);

    // XDEVICE_DATA
    struct DeviceData
    {
        be<uint32_t> deviceId;
        be<uint32_t> deviceType;
        be<uint64_t> totalBytes;
        be<uint64_t> freeBytes;
        be<uint16_t> name[28];
    };
    static_assert(sizeof(DeviceData) == 0x50);

    std::string FileName(const ContentData& data)
    {
        return std::string(data.fileName, strnlen(data.fileName, sizeof(data.fileName)));
    }

    fs::path TypeDirectory(uint32_t contentType)
    {
        char xuid[17], title[9], type[9];
        snprintf(xuid, sizeof(xuid), "%016llX", (unsigned long long)xam::kLocalXuid);
        snprintf(title, sizeof(title), "%08X", xam::kTitleId);
        snprintf(type, sizeof(type), "%08X", contentType);
        return fs::path(config::DataDirectory()) / "content" / xuid / title / type;
    }

    fs::path PackagePath(const ContentData& data)
    {
        return TypeDirectory(data.contentType) / FileName(data);
    }

    // ---------------------------------------------------------------------------------------------
    // Enumerators (also used by XamEnumerate for other enumerations).

    class Enumerator : public KernelObject
    {
    public:
        Enumerator(uint32_t itemSize, uint32_t itemsPerCall) : itemSize(itemSize), itemsPerCall(itemsPerCall) {}
        const char* TypeName() const override { return "Enumerator"; }

        uint32_t itemSize;
        uint32_t itemsPerCall;
        std::vector<std::vector<uint8_t>> items;
        size_t next = 0;
        std::mutex lock;
    };

    // An enumeration an XAM app serves (XamCreateEnumeratorHandle; DOAX2's XMP library asks for the user's playlists): its
    // guest object is a 24-byte header (app, open and close messages, user, items per call, flags) and `extraSize` bytes
    // the title and the app share (XamGetPrivateEnumStructureFromHandle). The runtime's apps have nothing to enumerate.
    class AppEnumerator : public Enumerator
    {
    public:
        AppEnumerator(uint32_t itemsPerCall, uint32_t extraSize) : Enumerator(1, itemsPerCall), m_address(guest::SystemAlloc(24 + extraSize)) {}
        ~AppEnumerator() override
        {
            if (m_address)
                guest::SystemFree(m_address);
        }
        uint32_t GuestObject() const override { return m_address; }
        bool IsWaitable() const override { return false; }

    private:
        uint32_t m_address; // (zeroed)
    };

    uint32_t XamCreateEnumeratorHandle(uint32_t userIndex, uint32_t appId, uint32_t openMessage, uint32_t closeMessage, uint32_t extraSize,
        uint32_t itemsPerCall, uint32_t flags, be<uint32_t>* handle)
    {
        if (!handle || extraSize > 0x100000)
            return xam::kErrorInvalidParameter;
        auto enumerator = std::make_shared<AppEnumerator>(std::max(itemsPerCall, 1u), extraSize);
        const uint32_t header = enumerator->GuestObject();
        if (!header)
            return xam::kErrorNotEnoughMemory;
        const uint32_t fields[6] = { appId, openMessage, closeMessage, userIndex, itemsPerCall, flags };
        for (uint32_t i = 0; i < 6; i++)
            guest::StoreU32(header + i * 4, fields[i]);
        LOG_INFO("XamCreateEnumeratorHandle(app 0x%X, messages 0x%X/0x%X, %u items, %u bytes shared): nothing to enumerate", appId, openMessage,
            closeMessage, itemsPerCall, extraSize);
        *handle = CreateHandle(enumerator);
        return xam::kErrorSuccess;
    }

    // The guest object of an enumerator from XamCreateEnumeratorHandle, with a reference ObDereferenceObject releases.
    uint32_t XamGetPrivateEnumStructureFromHandle(uint32_t handle, be<uint32_t>* structure)
    {
        if (!LookupHandleAs<AppEnumerator>(handle))
            return 0xC0000008; // STATUS_INVALID_HANDLE
        const uint32_t object = kernel::ReferenceObject(handle);
        if (structure)
            *structure = object;
        return 0;
    }

    uint32_t XamContentCreateEnumerator(uint32_t userIndex, uint32_t deviceId, uint32_t contentType, uint32_t /*contentFlags*/,
        uint32_t itemsPerCall, be<uint32_t>* bufferSize, be<uint32_t>* handle)
    {
        if (!handle || itemsPerCall == 0)
            return xam::kErrorInvalidParameter;
        if (deviceId != 0 && deviceId != kHardDriveId)
            return xam::kErrorInvalidParameter; // no such device
        if (bufferSize)
            *bufferSize = itemsPerCall * sizeof(ContentData);

        auto enumerator = std::make_shared<Enumerator>(uint32_t(sizeof(ContentData)), itemsPerCall);
        std::error_code error;
        if (userIndex == 0xFF || xam::IsUserSignedIn(userIndex))
        {
            for (const auto& entry : fs::directory_iterator(TypeDirectory(contentType), error))
            {
                if (!entry.is_directory())
                    continue;
                std::vector<uint8_t> bytes(sizeof(ContentData));
                auto* data = reinterpret_cast<ContentData*>(bytes.data());
                data->deviceId = kHardDriveId;
                data->contentType = contentType;
                const std::string name = entry.path().filename().string();
                strncpy(data->fileName, name.c_str(), sizeof(data->fileName));
                std::ifstream displayFile(entry.path().string() + ".name", std::ios::binary);
                std::u16string display;
                for (char16_t c; displayFile.read(reinterpret_cast<char*>(&c), 2) && display.size() < 127;)
                    display.push_back(c);
                for (size_t i = 0; i < display.size(); i++)
                    data->displayName[i] = uint16_t(display[i]);
                enumerator->items.push_back(std::move(bytes));
            }
        }
        LOG_INFO("XamContentCreateEnumerator(type 0x%08X): %zu packages", contentType, enumerator->items.size());
        *handle = CreateHandle(enumerator);
        return xam::kErrorSuccess;
    }

    uint32_t XamEnumerate(uint32_t handle, uint32_t /*flags*/, uint8_t* buffer, uint32_t bufferLength, be<uint32_t>* itemsReturned, xam::Overlapped* overlapped)
    {
        auto enumerator = LookupHandleAs<Enumerator>(handle);
        if (!enumerator)
            return xam::kErrorInvalidParameter;

        std::lock_guard lock(enumerator->lock);
        uint32_t count = 0;
        while (enumerator->next < enumerator->items.size() && count < enumerator->itemsPerCall &&
               (count + 1) * enumerator->itemSize <= bufferLength)
        {
            memcpy(buffer + count * enumerator->itemSize, enumerator->items[enumerator->next].data(), enumerator->itemSize);
            enumerator->next++;
            count++;
        }

        // An app's enumeration ends with success and no items: DOAX2's radio menu waits on its XMP enumeration until
        // XGetOverlappedResult gives 0, then takes no items as the end. Content enumerations end with no more files.
        const bool appEnumeration = std::dynamic_pointer_cast<AppEnumerator>(enumerator) != nullptr;
        const uint32_t result = count || appEnumeration ? xam::kErrorSuccess : xam::kErrorNoMoreFiles;
        if (itemsReturned)
        {
            *itemsReturned = count;
            return result;
        }
        return xam::Complete(overlapped, result, count);
    }

    // ---------------------------------------------------------------------------------------------
    // Opening packages as drives ("save:").

    std::mutex g_openLock;
    std::map<std::string, std::string> g_openPackages; // lower-case root name -> mount point
    uint32_t g_nextMount = 0;

    std::string LowerRoot(const char* rootName)
    {
        std::string root = rootName ? rootName : "";
        while (!root.empty() && root.back() == ':')
            root.pop_back();
        for (char& c : root)
            c = char(tolower((unsigned char)c));
        return root;
    }

    // Flags' low bits are the disposition: 1 create new, 2 create always, 3 open existing, 4 open always, 5 truncate existing.
    uint32_t XamContentCreate(uint32_t userIndex, const char* rootName, const ContentData* data, uint32_t flags,
        be<uint32_t>* disposition, be<uint32_t>* licenseMask, xam::Overlapped* overlapped)
    {
        if (!data || !rootName || !xam::IsUserSignedIn(userIndex))
            return xam::Complete(overlapped, data ? xam::kErrorNoSuchUser : xam::kErrorInvalidParameter);

        const fs::path path = PackagePath(*data);
        std::error_code error;
        const bool exists = fs::is_directory(path, error);
        const uint32_t mode = flags & 0xF;
        uint32_t result = xam::kErrorSuccess;
        bool created = false;
        switch (mode)
        {
        case 1: // create new
            if (exists)
                result = kErrorAlreadyExists;
            created = true;
            break;
        case 2: // create always
        case 5: // truncate existing
            if (mode == 5 && !exists)
            {
                result = kErrorFileNotFound;
                break;
            }
            fs::remove_all(path, error);
            created = true;
            break;
        case 3: // open existing
            if (!exists)
                result = kErrorPathNotFound;
            break;
        case 4: // open always
            created = !exists;
            break;
        default:
            result = xam::kErrorInvalidParameter;
            break;
        }

        if (result == xam::kErrorSuccess)
        {
            if (created)
            {
                fs::create_directories(path, error);
                std::ofstream displayFile(path.string() + ".name", std::ios::binary);
                for (int i = 0; i < 128 && data->displayName[i] != 0; i++)
                {
                    const char16_t c = char16_t(uint16_t(data->displayName[i]));
                    displayFile.write(reinterpret_cast<const char*>(&c), 2);
                }
            }

            std::lock_guard lock(g_openLock);
            const std::string root = LowerRoot(rootName);
            const std::string mountPoint = "\\Device\\Content\\" + std::to_string(g_nextMount++);
            auto device = vfs::OpenHostDevice("content " + FileName(*data), path.string());
            if (!device)
            {
                result = xam::kErrorFunctionFailed;
            }
            else
            {
                vfs::Mount(mountPoint, std::move(device));
                vfs::AddSymbolicLink("\\??\\" + root + ":", mountPoint);
                g_openPackages[root] = mountPoint;
                if (disposition)
                    *disposition = created ? 1 : 2;
                if (licenseMask)
                    *licenseMask = 0xFFFFFFFF;
                const kernel::GuestThread* thread = kernel::CurrentThread();
                LOG_INFO("XamContentCreate: %s package \"%s\" as %s: (thread %08X, frame %llu)", created ? "created" : "opened",
                    FileName(*data).c_str(), root.c_str(), thread ? thread->StartAddress() : 0, (unsigned long long)netplay::Frame());
            }
        }
        else
        {
            LOG_INFO("XamContentCreate(\"%s\", mode %u): error %u", FileName(*data).c_str(), mode, result);
        }
        return xam::Complete(overlapped, result);
    }

    // Later XDKs' form (DOAX2's): with the cache size and the content's size, which host folders don't need.
    uint32_t XamContentCreateEx(uint32_t userIndex, const char* rootName, const ContentData* data, uint32_t flags, be<uint32_t>* disposition,
        be<uint32_t>* licenseMask, uint32_t /*cacheSize*/, uint64_t /*contentSize*/, xam::Overlapped* overlapped)
    {
        return XamContentCreate(userIndex, rootName, data, flags, disposition, licenseMask, overlapped);
    }

    uint32_t XamContentClose(const char* rootName, xam::Overlapped* overlapped)
    {
        std::lock_guard lock(g_openLock);
        const std::string root = LowerRoot(rootName);
        const auto it = g_openPackages.find(root);
        if (it == g_openPackages.end())
            return xam::Complete(overlapped, kErrorPathNotFound);
        vfs::RemoveSymbolicLink("\\??\\" + root + ":");
        vfs::Unmount(it->second);
        g_openPackages.erase(it);
        return xam::Complete(overlapped, xam::kErrorSuccess);
    }

    uint32_t XamContentDelete(uint32_t /*userIndex*/, const ContentData* data, xam::Overlapped* overlapped)
    {
        if (!data)
            return xam::Complete(overlapped, xam::kErrorInvalidParameter);
        std::error_code error;
        const fs::path path = PackagePath(*data);
        const bool removed = fs::remove_all(path, error) > 0;
        fs::remove(path.string() + ".name", error);
        return xam::Complete(overlapped, removed ? xam::kErrorSuccess : kErrorFileNotFound);
    }

    uint32_t XamContentGetCreator(uint32_t userIndex, const ContentData* data, be<uint32_t>* isCreator, be<uint64_t>* creatorXuid, xam::Overlapped* overlapped)
    {
        if (!data)
            return xam::Complete(overlapped, xam::kErrorInvalidParameter);
        if (isCreator)
            *isCreator = xam::IsUserSignedIn(userIndex) ? 1 : 0;
        if (creatorXuid)
            *creatorXuid = xam::kLocalXuid;
        return xam::Complete(overlapped, xam::kErrorSuccess);
    }

    uint32_t XamContentGetDeviceState(uint32_t deviceId, xam::Overlapped* overlapped)
    {
        return xam::Complete(overlapped, deviceId == kHardDriveId ? xam::kErrorSuccess : xam::kErrorDeviceNotConnected);
    }

    uint32_t XamContentGetDeviceData(uint32_t deviceId, DeviceData* data)
    {
        if (deviceId != kHardDriveId)
            return xam::kErrorDeviceNotConnected;
        if (data)
        {
            memset(data, 0, sizeof(*data));
            data->deviceId = kHardDriveId;
            data->deviceType = 1; // hard drive
            data->totalBytes = 20ull * 1024 * 1024 * 1024;
            data->freeBytes = 16ull * 1024 * 1024 * 1024;
            const char* name = "Hard Drive";
            for (int i = 0; name[i]; i++)
                data->name[i] = uint16_t(name[i]);
        }
        return xam::kErrorSuccess;
    }

    // Thumbnails are a "thumbnail.png" inside the package folder.
    uint32_t XamContentGetThumbnail(uint32_t /*userIndex*/, const ContentData* data, uint8_t* buffer, be<uint32_t>* bufferSize, xam::Overlapped* overlapped)
    {
        if (!data || !bufferSize)
            return xam::Complete(overlapped, xam::kErrorInvalidParameter);
        std::ifstream file(PackagePath(*data) / "thumbnail.png", std::ios::binary | std::ios::ate);
        if (!file)
            return xam::Complete(overlapped, kErrorFileNotFound);
        const uint32_t size = uint32_t(file.tellg());
        if (buffer && *bufferSize >= size)
        {
            file.seekg(0);
            file.read(reinterpret_cast<char*>(buffer), size);
        }
        *bufferSize = size;
        return xam::Complete(overlapped, xam::kErrorSuccess);
    }

    uint32_t XamContentSetThumbnail(uint32_t /*userIndex*/, const ContentData* data, const uint8_t* buffer, uint32_t size, xam::Overlapped* overlapped)
    {
        if (!data || !buffer)
            return xam::Complete(overlapped, xam::kErrorInvalidParameter);
        std::error_code error;
        fs::create_directories(PackagePath(*data), error);
        std::ofstream file(PackagePath(*data) / "thumbnail.png", std::ios::binary);
        file.write(reinterpret_cast<const char*>(buffer), size);
        return xam::Complete(overlapped, xam::kErrorSuccess);
    }

    // The device selector picks the hard drive without showing UI.
    uint32_t XamShowDeviceSelectorUI(uint32_t /*userIndex*/, uint32_t /*contentType*/, uint32_t /*contentFlags*/, uint64_t /*totalRequested*/,
        be<uint32_t>* deviceId, xam::Overlapped* overlapped)
    {
        if (deviceId)
            *deviceId = kHardDriveId;
        return xam::Complete(overlapped, xam::kErrorSuccess);
    }
}

namespace xam
{
    uint32_t CreateEnumerator(uint32_t itemSize, uint32_t itemsPerCall, std::vector<std::vector<uint8_t>> items)
    {
        auto enumerator = std::make_shared<Enumerator>(std::max(itemSize, 1u), std::max(itemsPerCall, 1u));
        enumerator->items = std::move(items);
        return CreateHandle(enumerator);
    }
}

GUEST_EXPORT(XamContentCreateEnumerator, XamContentCreateEnumerator)
GUEST_EXPORT(XamCreateEnumeratorHandle, XamCreateEnumeratorHandle)
GUEST_EXPORT(XamGetPrivateEnumStructureFromHandle, XamGetPrivateEnumStructureFromHandle)
GUEST_EXPORT(XamEnumerate, XamEnumerate)
GUEST_EXPORT(XamContentCreate, XamContentCreate)
GUEST_EXPORT(XamContentCreateEx, XamContentCreateEx)
GUEST_EXPORT(XamContentClose, XamContentClose)
GUEST_EXPORT(XamContentDelete, XamContentDelete)
GUEST_EXPORT(XamContentGetCreator, XamContentGetCreator)
GUEST_EXPORT(XamContentGetDeviceState, XamContentGetDeviceState)
GUEST_EXPORT(XamContentGetDeviceData, XamContentGetDeviceData)
GUEST_EXPORT(XamContentGetThumbnail, XamContentGetThumbnail)
GUEST_EXPORT(XamContentSetThumbnail, XamContentSetThumbnail)
GUEST_EXPORT(XamShowDeviceSelectorUI, XamShowDeviceSelectorUI)
