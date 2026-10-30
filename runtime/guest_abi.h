#pragma once

// Calling recompiled-guest-facing C++ code: big-endian values, guest pointers, and an adapter
// that turns a plain C++ function into an import implementation.
//
//   uint32_t NtFoo(uint32_t handle, be<uint32_t>* outValue) { ... }
//   GUEST_EXPORT(NtFoo, NtFoo);
//
// Arguments are taken from r3..r10 in order, then from the stack: integers/enums by value, pointers
// translated from guest addresses (0 becomes nullptr). The return value goes to r3.
#include "guest_memory.h"
#include "ppc_recomp_shared.h"

#include <bit>
#include <cstdint>
#include <cstring>
#include <tuple>
#include <type_traits>

// A big-endian value stored in guest memory.
template <typename T>
struct be
{
    static_assert(std::is_trivially_copyable_v<T> && (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8));

    T raw;

    static T Swap(T value)
    {
        if constexpr (sizeof(T) == 1)
            return value;
        else
        {
            using U = std::conditional_t<sizeof(T) == 2, uint16_t, std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>>;
            U bits = std::bit_cast<U>(value);
            if constexpr (sizeof(T) == 2)
                bits = __builtin_bswap16(bits);
            else if constexpr (sizeof(T) == 4)
                bits = __builtin_bswap32(bits);
            else
                bits = __builtin_bswap64(bits);
            return std::bit_cast<T>(bits);
        }
    }

    T get() const { return Swap(raw); }
    operator T() const { return get(); }

    be& operator=(T value)
    {
        // In guest memory, a host write the store map may want to see first (netplay's page log, guest_memory.h).
        guest::BeforeHostWriteTo(this, sizeof(T));
        raw = Swap(value);
        return *this;
    }
};

static_assert(sizeof(be<uint32_t>) == 4);

namespace guest
{
    inline uint32_t ToGuest(const void* host)
    {
        return host ? uint32_t(static_cast<const uint8_t*>(host) - g_base) : 0;
    }

    template <typename T>
    inline T* Translate(uint32_t address)
    {
        return address ? reinterpret_cast<T*>(g_base + address) : nullptr;
    }

    namespace detail
    {
        inline PPCRegister& ArgumentRegister(PPCContext& ctx, size_t index)
        {
            switch (index)
            {
            case 0: return ctx.r3;
            case 1: return ctx.r4;
            case 2: return ctx.r5;
            case 3: return ctx.r6;
            case 4: return ctx.r7;
            case 5: return ctx.r8;
            case 6: return ctx.r9;
            default: return ctx.r10;
            }
        }

        // Arguments 0-7 are in r3..r10. Later ones are on the caller's stack in 8-byte parameter
        // save slots starting at r1 + 0x50, with a 32-bit value in the slot's low word.
        inline uint64_t ArgumentValue(PPCContext& ctx, size_t index)
        {
            if (index < 8)
                return ArgumentRegister(ctx, index).u64;
            return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(g_base + ctx.r1.u32 + 0x54 + (index - 8) * 8));
        }

        template <typename T>
        T ReadArgument(PPCContext& ctx, size_t index)
        {
            if constexpr (std::is_same_v<T, PPCContext*>)
                return &ctx; // doesn't consume a register; must be the last parameter
            else if constexpr (std::is_pointer_v<T>)
                return Translate<std::remove_pointer_t<T>>(uint32_t(ArgumentValue(ctx, index)));
            else if constexpr (std::is_enum_v<T> || std::is_integral_v<T>)
                return static_cast<T>(ArgumentValue(ctx, index));
            else
                static_assert(sizeof(T) == 0, "unsupported argument type");
        }

        template <typename T>
        void WriteResult(PPCContext& ctx, T value)
        {
            if constexpr (std::is_pointer_v<T>)
                ctx.r3.u64 = ToGuest(value);
            else if constexpr (std::is_signed_v<T>)
                ctx.r3.s64 = static_cast<int64_t>(value);
            else
                ctx.r3.u64 = static_cast<uint64_t>(value);
        }

        template <typename>
        struct FunctionTraits;

        template <typename R, typename... Args>
        struct FunctionTraits<R (*)(Args...)>
        {
            using Return = R;
            using Arguments = std::tuple<Args...>;
        };
    }

    // Calls F with arguments taken from the guest's argument registers and stores its result in r3.
    template <auto F>
    void Invoke(PPCContext& ctx)
    {
        using Traits = detail::FunctionTraits<decltype(F)>;
        using Arguments = typename Traits::Arguments;

        auto call = [&]<size_t... I>(std::index_sequence<I...>)
        {
            return F(detail::ReadArgument<std::tuple_element_t<I, Arguments>>(ctx, I)...);
        };

        using Indices = std::make_index_sequence<std::tuple_size_v<Arguments>>;
        if constexpr (std::is_void_v<typename Traits::Return>)
            call(Indices{});
        else
            detail::WriteResult(ctx, call(Indices{}));
    }
}

namespace guest
{
    // With TRACE_IMPORTS set, every import call is logged with its caller and result.
    bool TraceImports();
    void TraceImportCall(const char* name, const PPCContext& ctx);
    void TraceImportResult(const char* name, const PPCContext& ctx);
}

// Defines the import __imp__<name>, overriding its weak "unimplemented" stub.
#define GUEST_EXPORT(name, function)                \
    PPC_FUNC(__imp__##name)                         \
    {                                               \
        (void)base;                                 \
        const bool trace = guest::TraceImports();   \
        if (trace)                                  \
            guest::TraceImportCall(#name, ctx);     \
        guest::Invoke<&function>(ctx);              \
        if (trace)                                  \
            guest::TraceImportResult(#name, ctx);   \
    }
