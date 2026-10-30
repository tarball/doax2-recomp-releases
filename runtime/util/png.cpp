#include "util/png.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace util
{
    namespace
    {
        uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0)
        {
            static const auto table = [] {
                std::vector<uint32_t> entries(256);
                for (uint32_t i = 0; i < 256; i++)
                {
                    uint32_t c = i;
                    for (int k = 0; k < 8; k++)
                        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    entries[i] = c;
                }
                return entries;
            }();
            crc = ~crc;
            for (size_t i = 0; i < size; i++)
                crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
            return ~crc;
        }

        void PutU32(std::vector<uint8_t>& out, uint32_t value)
        {
            out.push_back(uint8_t(value >> 24));
            out.push_back(uint8_t(value >> 16));
            out.push_back(uint8_t(value >> 8));
            out.push_back(uint8_t(value));
        }

        void WriteChunk(FILE* file, const char type[4], const std::vector<uint8_t>& data)
        {
            std::vector<uint8_t> chunk;
            chunk.reserve(data.size() + 12);
            PutU32(chunk, uint32_t(data.size()));
            chunk.insert(chunk.end(), type, type + 4);
            chunk.insert(chunk.end(), data.begin(), data.end());
            PutU32(chunk, Crc32(chunk.data() + 4, data.size() + 4));
            fwrite(chunk.data(), 1, chunk.size(), file);
        }
    }

    bool WritePng(const char* path, const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t rowPitch)
    {
        FILE* file = fopen(path, "wb");
        if (!file)
            return false;

        static const uint8_t kSignature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        fwrite(kSignature, 1, sizeof(kSignature), file);

        std::vector<uint8_t> header;
        PutU32(header, width);
        PutU32(header, height);
        header.insert(header.end(), { 8, 2, 0, 0, 0 }); // 8-bit RGB, deflate, no filter, no interlace
        WriteChunk(file, "IHDR", header);

        // Scanlines (filter type 0) in stored deflate blocks.
        std::vector<uint8_t> raw;
        raw.reserve(size_t(height) * (width * 3 + 1));
        for (uint32_t y = 0; y < height; y++)
        {
            raw.push_back(0);
            const uint8_t* row = rgba + size_t(y) * rowPitch;
            for (uint32_t x = 0; x < width; x++)
                raw.insert(raw.end(), { row[x * 4], row[x * 4 + 1], row[x * 4 + 2] });
        }

        std::vector<uint8_t> zlib = { 0x78, 0x01 };
        for (size_t offset = 0; offset < raw.size() || offset == 0;)
        {
            const size_t length = std::min<size_t>(raw.size() - offset, 65535);
            const bool last = offset + length == raw.size();
            zlib.push_back(last ? 1 : 0);
            zlib.insert(zlib.end(), { uint8_t(length), uint8_t(length >> 8), uint8_t(~length), uint8_t(~length >> 8) });
            zlib.insert(zlib.end(), raw.begin() + offset, raw.begin() + offset + length);
            offset += length;
            if (last)
                break;
        }
        uint32_t a = 1, b = 0;
        for (uint8_t byte : raw)
        {
            a = (a + byte) % 65521;
            b = (b + a) % 65521;
        }
        PutU32(zlib, (b << 16) | a);
        WriteChunk(file, "IDAT", zlib);
        WriteChunk(file, "IEND", {});

        const bool ok = ferror(file) == 0;
        fclose(file);
        return ok;
    }
}
