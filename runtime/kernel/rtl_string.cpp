// Rtl ANSI/Unicode string helpers. Guest wide strings are big-endian UTF-16.
#include "guest_abi.h"
#include "kernel/status.h"

#include <algorithm>
#include <cstring>

namespace
{
    struct CountedString // ANSI_STRING and UNICODE_STRING share this layout
    {
        be<uint16_t> length;        // bytes, excluding the terminator
        be<uint16_t> maximumLength; // bytes, including the terminator
        be<uint32_t> buffer;
    };
    static_assert(sizeof(CountedString) == 8);

    size_t WideLength(const be<uint16_t>* text)
    {
        size_t n = 0;
        while (text[n] != 0)
            n++;
        return n;
    }

    void RtlInitAnsiString(CountedString* destination, const char* source)
    {
        if (!destination)
            return;
        const size_t length = source ? strlen(source) : 0;
        destination->length = uint16_t(length);
        destination->maximumLength = uint16_t(source ? length + 1 : 0);
        destination->buffer = guest::ToGuest(source);
    }

    void RtlInitUnicodeString(CountedString* destination, const be<uint16_t>* source)
    {
        if (!destination)
            return;
        const size_t length = source ? WideLength(source) * 2 : 0;
        destination->length = uint16_t(length);
        destination->maximumLength = uint16_t(source ? length + 2 : 0);
        destination->buffer = guest::ToGuest(source);
    }

    void RtlFreeAnsiString(CountedString* string)
    {
        if (!string || string->buffer == 0)
            return;
        guest::SystemFree(string->buffer);
        string->buffer = 0;
        string->length = 0;
        string->maximumLength = 0;
    }

    // Narrows UTF-16 to single bytes; characters above 0xFF become '?', as a non-Unicode code page would.
    uint8_t Narrow(uint16_t c)
    {
        return c <= 0xFF ? uint8_t(c) : uint8_t('?');
    }

    uint32_t RtlUnicodeStringToAnsiString(CountedString* destination, const CountedString* source, uint32_t allocate)
    {
        if (!destination || !source)
            return status::kInvalidParameter;
        const uint32_t chars = source->length / 2;
        const auto* in = guest::Translate<be<uint16_t>>(source->buffer);

        if (allocate)
        {
            const uint32_t buffer = guest::SystemAlloc(chars + 1);
            if (!buffer)
                return status::kNoMemory;
            destination->buffer = buffer;
            destination->maximumLength = uint16_t(chars + 1);
        }
        else if (destination->maximumLength < chars + 1)
        {
            return status::kBufferOverflow;
        }

        auto* out = guest::Translate<uint8_t>(destination->buffer);
        for (uint32_t i = 0; i < chars; i++)
            out[i] = Narrow(in[i]);
        out[chars] = 0;
        destination->length = uint16_t(chars);
        return status::kSuccess;
    }

    uint32_t RtlMultiByteToUnicodeN(be<uint16_t>* destination, uint32_t destinationBytes, be<uint32_t>* resultBytes, const uint8_t* source, uint32_t sourceBytes)
    {
        const uint32_t count = std::min(destinationBytes / 2, sourceBytes);
        for (uint32_t i = 0; i < count; i++)
            destination[i] = source[i];
        if (resultBytes)
            *resultBytes = count * 2;
        return 0;
    }

    uint32_t RtlUnicodeToMultiByteN(uint8_t* destination, uint32_t destinationBytes, be<uint32_t>* resultBytes, const be<uint16_t>* source, uint32_t sourceBytes)
    {
        const uint32_t count = std::min(destinationBytes, sourceBytes / 2);
        for (uint32_t i = 0; i < count; i++)
            destination[i] = Narrow(source[i]);
        if (resultBytes)
            *resultBytes = count;
        return 0;
    }
}

GUEST_EXPORT(RtlInitAnsiString, RtlInitAnsiString)
GUEST_EXPORT(RtlInitUnicodeString, RtlInitUnicodeString)
GUEST_EXPORT(RtlFreeAnsiString, RtlFreeAnsiString)
GUEST_EXPORT(RtlUnicodeStringToAnsiString, RtlUnicodeStringToAnsiString)
GUEST_EXPORT(RtlMultiByteToUnicodeN, RtlMultiByteToUnicodeN)
GUEST_EXPORT(RtlUnicodeToMultiByteN, RtlUnicodeToMultiByteN)
