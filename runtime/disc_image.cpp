#include "disc_image.h"

#include "log.h"
#include "vfs/iso_device.h"

#include <cstdio>
#include <cstring>
#include <memory>

#include <windows.h>
#include <bcrypt.h>

namespace disc
{
    namespace
    {
        // default.xex of Dead or Alive Xtreme 2 (USA), title 544307D2: the version config/DOAX2.toml describes.
        constexpr char kSupportedXex[] = "c4a14853e1f7af951be34f380604113703638780b36d5e74e674fe7431992870";
        // The XEX retail key, which decrypts each executable's session key.
        constexpr uint8_t kRetailKey[16] = { 0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3, 0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91 };
        constexpr uint32_t kFileFormatInfo = 0x3FF; // optional header key
        constexpr uint16_t kEncrypted = 1, kBasicCompression = 1;

        // AES-128-CBC with a zero IV, in place, over whole blocks.
        bool Decrypt(const uint8_t key[16], uint8_t* data, size_t size)
        {
            BCRYPT_ALG_HANDLE algorithm = nullptr;
            BCRYPT_KEY_HANDLE handle = nullptr;
            bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0)) &&
                BCRYPT_SUCCESS(BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                    sizeof(BCRYPT_CHAIN_MODE_CBC), 0)) &&
                BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(algorithm, &handle, nullptr, 0, const_cast<PUCHAR>(key), 16, 0));
            if (ok)
            {
                uint8_t iv[16] = {};
                ULONG written = 0;
                ok = BCRYPT_SUCCESS(BCryptDecrypt(handle, data, ULONG(size), nullptr, iv, sizeof(iv), data, ULONG(size), &written, 0)) &&
                    written == size;
            }
            if (handle)
                BCryptDestroyKey(handle);
            if (algorithm)
                BCryptCloseAlgorithmProvider(algorithm, 0);
            return ok;
        }
    }

    std::string Sha256(const uint8_t* data, size_t size)
    {
        uint8_t hash[32];
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
            return {};
        const bool ok = BCRYPT_SUCCESS(BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(data), ULONG(size), hash, sizeof(hash)));
        BCryptCloseAlgorithmProvider(algorithm, 0);
        if (!ok)
            return {};
        char text[65];
        for (int i = 0; i < 32; i++)
            snprintf(text + i * 2, 3, "%02x", hash[i]);
        return text;
    }

    bool IsSupported(const std::vector<uint8_t>& xex)
    {
        return Sha256(xex.data(), xex.size()) == kSupportedXex;
    }

    Check ReadExecutable(const std::string& isoPath, std::vector<uint8_t>& xex)
    {
        xex.clear();
        const std::unique_ptr<vfs::Device> device = vfs::OpenIsoDevice(isoPath);
        if (!device)
            return Check::kUnreadable;
        vfs::OpenRequest request;
        request.path = "default.xex";
        request.nonDirectory = true;
        std::unique_ptr<vfs::File> file;
        vfs::OpenResult result;
        if (device->Open(request, file, result) != 0 || !file)
            return Check::kUnreadable;
        const uint64_t size = file->Info().size;
        if (size == 0 || size > (256u << 20))
            return Check::kUnreadable;
        xex.resize(size_t(size));
        uint32_t transferred = 0;
        if (file->Read(xex.data(), uint32_t(size), 0, transferred) != 0 || transferred != size)
            return Check::kUnreadable;
        return IsSupported(xex) ? Check::kOk : Check::kOtherGame;
    }

    bool Unpack(const std::vector<uint8_t>& xex, std::vector<uint8_t>& image)
    {
        const auto be32 = [&](size_t offset) -> uint32_t {
            if (offset + 4 > xex.size())
                return 0;
            return uint32_t(xex[offset]) << 24 | uint32_t(xex[offset + 1]) << 16 | uint32_t(xex[offset + 2]) << 8 | xex[offset + 3];
        };
        if (xex.size() < 24 || memcmp(xex.data(), "XEX2", 4) != 0)
            return false;
        const uint32_t headerSize = be32(8), security = be32(16), headers = be32(20);
        uint32_t info = 0; // the file format info
        for (uint32_t i = 0; i < headers && 32 + i * 8 <= headerSize; i++)
        {
            if (be32(24 + i * 8) == kFileFormatInfo)
                info = be32(28 + i * 8);
        }
        if (!info || headerSize > xex.size() || info + 8 > headerSize)
            return false;
        const uint32_t infoSize = be32(info);
        const uint16_t encryption = uint16_t(be32(info + 4) >> 16), compression = uint16_t(be32(info + 4));
        if (compression != kBasicCompression || infoSize < 8 || info + infoSize > headerSize)
        {
            LOG_ERROR("default.xex: compression %u isn't handled (only basic compression is)", compression);
            return false;
        }

        std::vector<uint8_t> payload(xex.begin() + headerSize, xex.end());
        if (encryption == kEncrypted)
        {
            uint8_t sessionKey[16];
            if (size_t(security) + 0x150 + 16 > xex.size())
                return false;
            memcpy(sessionKey, xex.data() + security + 0x150, 16);
            if (!Decrypt(kRetailKey, sessionKey, 16) || !Decrypt(sessionKey, payload.data(), payload.size() & ~size_t(15)))
                return false;
        }

        // Basic compression: blocks of data, each followed by a run of zeros. The image is the size the security info
        // gives, which can be more than the blocks cover (DOAX2's last 0x8000 bytes are zeros they leave out).
        const uint32_t blocks = infoSize / 8 - 1;
        size_t total = 0;
        for (uint32_t i = 0; i < blocks; i++)
            total += size_t(be32(info + 8 + i * 8)) + be32(info + 12 + i * 8);
        const uint32_t imageSize = be32(size_t(security) + 4);
        if (total > imageSize)
            return false;
        image.assign(imageSize, 0);
        size_t source = 0, destination = 0;
        for (uint32_t i = 0; i < blocks; i++)
        {
            const uint32_t data = be32(info + 8 + i * 8), zeros = be32(info + 12 + i * 8);
            if (source + data > payload.size())
                return false;
            memcpy(image.data() + destination, payload.data() + source, data);
            source += data;
            destination += size_t(data) + zeros;
        }
        return true;
    }
}
