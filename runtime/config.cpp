#include "config.h"

#include <filesystem>
#include <system_error>

namespace config
{
    namespace
    {
        std::string g_dataDirectory = "private";
        std::string g_discImage;
    }

    void SetDiscImage(std::string path)
    {
        g_discImage = std::move(path);
    }

    const std::string& DiscImage()
    {
        return g_discImage;
    }

    void SetDataDirectory(std::string directory)
    {
        g_dataDirectory = std::move(directory);
    }

    const std::string& DataDirectory()
    {
        return g_dataDirectory;
    }

    std::string DumpPath(const std::string& name)
    {
        const std::string directory = g_dataDirectory + "/dumps";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        return directory + "/" + name;
    }
}
