// Cryptography exports: SHA-1, and console signatures. There are no console keys here, so a "signature" is the hash
// itself in the signature's place: verification accepts exactly what signing produced, which is what XAPI checks when
// it reads back what it signed (the cache partition header on the hard disk).
#include "guest_abi.h"
#include "log.h"

#include <algorithm>
#include <cstring>

#include <windows.h>
#include <bcrypt.h>

namespace
{
    constexpr uint32_t kShaSize = 20;

    // XE_CONSOLE_SIGNATURE: the console's certificate, then its signature of the hash.
    struct ConsoleSignature
    {
        be<uint16_t> certificateSize;
        uint8_t certificate[0x1A6];
        uint8_t signature[0x80];
    };
    static_assert(sizeof(ConsoleSignature) == 0x228);

    // XeCryptSha(input1, size1, input2, size2, input3, size3, digest, digestSize): SHA-1 of up to three buffers.
    void XeCryptSha(const uint8_t* input1, uint32_t size1, const uint8_t* input2, uint32_t size2, const uint8_t* input3, uint32_t size3,
        uint8_t* digest, uint32_t digestSize)
    {
        uint8_t hash[kShaSize] = {};
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE handle = nullptr;
        bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0)) &&
            BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &handle, nullptr, 0, nullptr, 0, 0));
        for (const auto& [input, size] : { std::pair{ input1, size1 }, std::pair{ input2, size2 }, std::pair{ input3, size3 } })
        {
            if (ok && input && size)
                ok = BCRYPT_SUCCESS(BCryptHashData(handle, const_cast<PUCHAR>(input), size, 0));
        }
        if (ok)
            ok = BCRYPT_SUCCESS(BCryptFinishHash(handle, hash, kShaSize, 0));
        if (handle)
            BCryptDestroyHash(handle);
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
        if (!ok)
            LOG_ERROR("XeCryptSha: BCrypt failed");
        if (digest)
        {
            guest::BeforeHostWriteTo(digest, std::min(digestSize, kShaSize));
            memcpy(digest, hash, std::min(digestSize, kShaSize));
        }
    }

    uint32_t XeKeysConsolePrivateKeySign(const uint8_t* hash, ConsoleSignature* signature)
    {
        if (!hash || !signature)
            return 0;
        guest::BeforeHostWriteTo(signature, sizeof(*signature));
        memset(signature, 0, sizeof(*signature));
        signature->certificateSize = uint16_t(sizeof(signature->certificateSize) + sizeof(signature->certificate));
        memcpy(signature->signature, hash, kShaSize);
        return 1;
    }

    // Returns TRUE if it could check, and whether the signature is valid in *valid.
    uint32_t XeKeysConsoleSignatureVerification(const uint8_t* hash, const ConsoleSignature* signature, be<uint32_t>* valid)
    {
        if (!hash || !signature)
            return 0;
        if (valid)
            *valid = memcmp(signature->signature, hash, kShaSize) == 0 ? 1 : 0;
        return 1;
    }
}

GUEST_EXPORT(XeCryptSha, XeCryptSha)
GUEST_EXPORT(XeKeysConsolePrivateKeySign, XeKeysConsolePrivateKeySign)
GUEST_EXPORT(XeKeysConsoleSignatureVerification, XeKeysConsoleSignatureVerification)
