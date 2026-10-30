#include "vfs/vfs.h"

#include "log.h"

#include <algorithm>
#include <map>
#include <mutex>

namespace vfs
{
    namespace
    {
        std::string Lower(std::string_view text)
        {
            std::string result(text);
            std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return result;
        }

        // Converts '/' to '\', collapses repeated separators and removes "." components.
        std::string Normalize(std::string_view path)
        {
            std::string result;
            result.reserve(path.size());
            for (char c : path)
            {
                if (c == '/')
                    c = '\\';
                if (c == '\\' && !result.empty() && result.back() == '\\')
                    continue;
                result.push_back(c);
            }
            for (size_t dot; (dot = result.find("\\.\\")) != std::string::npos;)
                result.erase(dot, 2);
            if (result.size() > 1 && result.back() == '\\')
                result.pop_back();
            return result;
        }

        struct State
        {
            std::mutex lock;
            std::map<std::string, std::unique_ptr<Device>> mounts; // lower-case mount point
            std::map<std::string, std::string> links;              // lower-case link -> target
        };

        State& GetState()
        {
            static State state;
            return state;
        }

        // Drive-style names ("D:\foo", "game:\foo") are links under \??\.
        std::string ExpandDrive(const std::string& path)
        {
            const size_t colon = path.find(':');
            if (colon != std::string::npos && path.find('\\') > colon && path.rfind("\\??\\", 0) != 0)
                return "\\??\\" + path;
            return path;
        }
    }

    std::string JoinPath(std::string_view directory, std::string_view name)
    {
        std::string result(directory);
        if (!result.empty() && result.back() != '\\' && !name.empty() && name.front() != '\\')
            result.push_back('\\');
        result.append(name);
        return result;
    }

    void Mount(std::string_view mountPoint, std::unique_ptr<Device> device)
    {
        State& state = GetState();
        std::lock_guard lock(state.lock);
        LOG_INFO("Mounted %s at %.*s", device->Name(), int(mountPoint.size()), mountPoint.data());
        state.mounts[Lower(Normalize(mountPoint))] = std::move(device);
    }

    void Unmount(std::string_view mountPoint)
    {
        State& state = GetState();
        std::lock_guard lock(state.lock);
        const auto it = state.mounts.find(Lower(Normalize(mountPoint)));
        if (it == state.mounts.end())
            return;
        // Open files keep a pointer to their device, so unmounted devices are retired, not destroyed.
        static std::vector<std::unique_ptr<Device>> retired;
        retired.push_back(std::move(it->second));
        state.mounts.erase(it);
    }

    bool AddSymbolicLink(std::string_view link, std::string_view target)
    {
        State& state = GetState();
        std::lock_guard lock(state.lock);
        const auto [it, inserted] = state.links.emplace(Lower(Normalize(link)), Normalize(target));
        if (!inserted)
            it->second = Normalize(target); // relinking replaces, as the kernel's XAPI usage expects
        LOG_TRACE("Symbolic link %.*s -> %.*s", int(link.size()), link.data(), int(target.size()), target.data());
        return true;
    }

    bool RemoveSymbolicLink(std::string_view link)
    {
        State& state = GetState();
        std::lock_guard lock(state.lock);
        return state.links.erase(Lower(Normalize(link))) != 0;
    }

    Device* Resolve(std::string_view guestPath, std::string& relativePath)
    {
        State& state = GetState();
        std::lock_guard lock(state.lock);

        std::string path = ExpandDrive(Normalize(guestPath));

        // Follow symbolic links on the longest matching prefix (a few levels at most).
        for (int depth = 0; depth < 8; depth++)
        {
            const std::string lower = Lower(path);
            const std::pair<const std::string, std::string>* longest = nullptr;
            for (const auto& entry : state.links)
            {
                const std::string& link = entry.first;
                if (lower.rfind(link, 0) == 0 && (lower.size() == link.size() || lower[link.size()] == '\\') &&
                    (!longest || link.size() > longest->first.size()))
                    longest = &entry;
            }
            if (!longest)
                break;
            path = longest->second + path.substr(longest->first.size());
        }

        const std::string lower = Lower(path);
        Device* best = nullptr;
        size_t bestLength = 0;
        for (auto& [mountPoint, device] : state.mounts)
        {
            if (lower.rfind(mountPoint, 0) == 0 && (lower.size() == mountPoint.size() || lower[mountPoint.size()] == '\\') &&
                mountPoint.size() > bestLength)
            {
                best = device.get();
                bestLength = mountPoint.size();
            }
        }
        if (!best)
            return nullptr;

        relativePath = path.substr(bestLength);
        while (!relativePath.empty() && relativePath.front() == '\\')
            relativePath.erase(relativePath.begin());
        return best;
    }
}
