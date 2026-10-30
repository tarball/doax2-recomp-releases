#include "netplay/fingerprint.h"

#include "config.h"
#include "guest_abi.h"
#include "kernel/threads.h"
#include "log.h"
#include "netplay/game_state.h"
#include "netplay/identity.h"

#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <math.h>
#include <string>
#include <type_traits>
#include <vector>

#include <immintrin.h>
#include <intrin.h>
#include <windows.h>

extern "C" int __isa_available; // the static C runtime's CPU level (memcpy, memset and the STL dispatch on it)

namespace netplay::fingerprint
{
    namespace
    {
        FILE* g_file = nullptr;

        // A line in dumps/fingerprint.txt and the log.
        void Line(const char* format, ...)
        {
            char text[512];
            va_list arguments;
            va_start(arguments, format);
            vsnprintf(text, sizeof(text), format, arguments);
            va_end(arguments);
            if (g_file)
                fprintf(g_file, "%s\n", text);
            LOG_INFO("fingerprint: %s", text);
        }

        // The results hashed, in 64-bit words.
        struct Hash
        {
            uint64_t value = 0xCBF29CE484222325ull;
            uint64_t count = 0;
            void Word(uint64_t word)
            {
                value = (value ^ word) * 0x100000001B3ull;
                value ^= value >> 29;
            }
            template <typename T> void Add(const T& item)
            {
                static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= 32);
                uint64_t words[4] = {};
                memcpy(words, &item, sizeof(T));
                for (size_t i = 0; i < (sizeof(T) + 7) / 8; i++)
                    Word(words[i]);
                count++;
            }
        };

        struct Result
        {
            std::string name;
            Hash hash;
        };

        // SplitMix64: the same inputs on every PC.
        struct Inputs
        {
            uint64_t state;
            explicit Inputs(uint64_t seed) : state(seed) {}
            uint64_t Next()
            {
                uint64_t z = (state += 0x9E3779B97F4A7C15ull);
                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
                z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
                return z ^ (z >> 31);
            }
            // A random sign and significand, with an exponent in [lowExponent, highExponent].
            double Double(int lowExponent, int highExponent)
            {
                const uint64_t bits = Next();
                const int exponent = lowExponent + int(bits % uint64_t(highExponent - lowExponent + 1));
                const double significand = 1.0 + double(Next() >> 11) * 0x1p-53;
                return std::ldexp((bits & (1ull << 63)) ? -significand : significand, exponent);
            }
            float FloatBits() { return std::bit_cast<float>(uint32_t(Next())); }
        };

        // Zeros, infinities, NaNs with signs and payloads (quiet and signaling), denormals, extremes and ones.
        constexpr uint64_t kSpecialDoubles[] = { 0x0000000000000000, 0x8000000000000000, 0x7FF0000000000000, 0xFFF0000000000000,
            0x7FF8000000000000, 0xFFF8000000000000, 0x7FF8000000001234, 0xFFF800000000ABCD, 0x7FF0000000000042, 0x0000000000000001,
            0x800FFFFFFFFFFFFF, 0x7FEFFFFFFFFFFFFF, 0x3FF0000000000000, 0xBFF0000000000000 };
        constexpr uint32_t kSpecialFloats[] = { 0x00000000, 0x80000000, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7FC01234,
            0xFFC0ABCD, 0x7F800042, 0x00000001, 0x807FFFFF, 0x7F7FFFFF, 0x3F800000, 0xBF800000 };

        void PrintMachine()
        {
            int info[4] = {};
            char vendor[13] = {};
            __cpuid(info, 0);
            memcpy(vendor, &info[1], 4);
            memcpy(vendor + 4, &info[3], 4);
            memcpy(vendor + 8, &info[2], 4);
            __cpuid(info, 1);
            const int signature = info[0];
            const bool fma3 = info[2] & (1 << 12), avx = info[2] & (1 << 28), f16c = info[2] & (1 << 29);
            __cpuidex(info, 7, 0);
            const bool avx2 = info[1] & (1 << 5), avx512f = info[1] & (1 << 16);
            Line("machine: %s", identity::Machine().c_str());
            Line("cpu: %s, signature %08X, AVX %d FMA3 %d F16C %d AVX2 %d AVX-512F %d", vendor, signature, avx, fma3, f16c, avx2, avx512f);
            Line("build: %016llX (%s); C runtime: CPU level %d, FMA3 paths %d", (unsigned long long)identity::BuildId(),
                identity::BuildConfiguration(), __isa_available, _get_FMA3_enable());
        }

        // MXCSR as threads and fibers start: what the generated code's cached FPSCR assumes.
        void PrintModes()
        {
            const unsigned mainThread = _mm_getcsr() & ~0x3Fu;
            unsigned threadMxcsr = 0;
            HANDLE thread = CreateThread(nullptr, 0, [](void* out) -> DWORD {
                *static_cast<unsigned*>(out) = _mm_getcsr() & ~0x3Fu;
                return 0;
            }, &threadMxcsr, 0, nullptr);
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
            // A fiber created as the game's are (game/fibers.cpp) while its creator runs with flush-to-zero, denormals
            // as zero and rounding down: does it start with Windows' 1F80, or with its creator's modes?
            struct FiberTest
            {
                void* creator = nullptr;
                unsigned mxcsr = 0;
            } test;
            HANDLE fiberThread = CreateThread(nullptr, 0, [](void* parameter) -> DWORD {
                auto& test = *static_cast<FiberTest*>(parameter);
                test.creator = ConvertThreadToFiber(nullptr);
                const unsigned saved = _mm_getcsr();
                _mm_setcsr(0x1F80 | 0x8040 | 0x2000);
                void* fiber = CreateFiberEx(64 * 1024, 64 * 1024, FIBER_FLAG_FLOAT_SWITCH, [](void* parameter) {
                    auto& test = *static_cast<FiberTest*>(parameter);
                    test.mxcsr = _mm_getcsr() & ~0x3Fu;
                    SwitchToFiber(test.creator);
                }, &test);
                SwitchToFiber(fiber);
                _mm_setcsr(saved);
                DeleteFiber(fiber);
                ConvertFiberToThread();
                return 0;
            }, &test, 0, nullptr);
            WaitForSingleObject(fiberThread, INFINITE);
            CloseHandle(fiberThread);
            Line("mxcsr: main thread %08X, new thread %08X, new fiber (its creator at 0000BFC0) %08X", mainThread, threadMxcsr, test.mxcsr);
        }

        // The C runtime's fma (the generated code's fmadd, fmsub, fnmadd, fnmsub), by kind of operands.
        void HashFma(std::vector<Result>& results)
        {
            Hash numeric, cancelling, ties, tiny, special;
            Inputs in(1);
            for (int i = 0; i < 2000000; i++)
            {
                const double a = in.Double(-30, 30), b = in.Double(-30, 30), c = in.Double(-60, 60);
                numeric.Add(std::fma(a, b, c));
                const double p = a * b;
                cancelling.Add(std::fma(a, b, -p * (1.0 + double(int(in.Next() % 17) - 8) * 0x1p-52)));
                const double x = double((in.Next() >> 37) | 1) * std::ldexp(1.0, -int(in.Next() % 40));
                const double y = double((in.Next() >> 37) | 1) * std::ldexp(1.0, -int(in.Next() % 40));
                const double ulp = std::nextafter(std::fabs(x * y), INFINITY) - std::fabs(x * y);
                const double half = (in.Next() & 1 ? 0.5 : -0.5) * ulp;
                ties.Add(std::fma(x, y, half * ((in.Next() & 1) ? 1.0 : std::ldexp(1.0, -int(in.Next() % 60)))));
                tiny.Add(std::fma(in.Double(-540, -500), in.Double(-540, -500), in.Double(-1080, -1022)));
            }
            for (const uint64_t a : kSpecialDoubles)
                for (const uint64_t b : kSpecialDoubles)
                    for (const uint64_t c : kSpecialDoubles)
                        special.Add(std::fma(std::bit_cast<double>(a), std::bit_cast<double>(b), std::bit_cast<double>(c)));
            for (int i = 0; i < 10000; i++) // products that underflow to zero, plus a zero of either sign
                for (const double c : { 0.0, -0.0 })
                    special.Add(std::fma(in.Double(-600, -540), in.Double(-600, -540), c));
            results.push_back({ "fma numeric", numeric });
            results.push_back({ "fma cancelling", cancelling });
            results.push_back({ "fma near ties", ties });
            results.push_back({ "fma tiny", tiny });
            results.push_back({ "fma special", special });
        }

        // The C runtime's exp2f and log2f (vexptefp, vlogefp), every `stride`th float.
        void HashExpLog(std::vector<Result>& results, uint32_t stride)
        {
            Hash exp2, log2;
            for (uint64_t bits = 0; bits <= 0xFFFFFFFFull; bits += stride)
            {
                const float x = std::bit_cast<float>(uint32_t(bits));
                exp2.Add(exp2f(x));
                log2.Add(log2f(x));
            }
            results.push_back({ "exp2f", exp2 });
            results.push_back({ "log2f", log2 });
        }

        // DPPS (vmsum4fp128: 0xFF, vmsum3fp128: 0xEF), with the VMX flush mode as the generated code sets it, and without.
        void HashDotProducts(std::vector<Result>& results)
        {
            Hash finite, denormal, nonFinite;
            Inputs in(2);
            const unsigned saved = _mm_getcsr();
            for (const unsigned csr : { 0x1F80u | 0x8040u, 0x1F80u })
            {
                _mm_setcsr(csr);
                for (int i = 0; i < 1000000; i++)
                {
                    alignas(16) float a[4], b[4];
                    for (int j = 0; j < 4; j++)
                        a[j] = float(in.Double(-20, 20)), b[j] = float(in.Double(-20, 20));
                    __m128 va = _mm_load_ps(a), vb = _mm_load_ps(b);
                    finite.Add(_mm_dp_ps(va, vb, 0xFF));
                    finite.Add(_mm_dp_ps(va, vb, 0xEF));
                    for (int j = 0; j < 4; j++) // products and sums near the denormal range
                        a[j] = float(in.Double(-70, -60)), b[j] = float(in.Double(-70, -60));
                    va = _mm_load_ps(a), vb = _mm_load_ps(b);
                    denormal.Add(_mm_dp_ps(va, vb, 0xFF));
                    denormal.Add(_mm_dp_ps(va, vb, 0xEF));
                    for (int j = 0; j < 4; j++) // a lane or two special: which NaN comes out is the implementation's choice
                    {
                        a[j] = float(in.Double(-5, 5)), b[j] = float(in.Double(-5, 5));
                        if (in.Next() % 4 == 0)
                            a[j] = std::bit_cast<float>(kSpecialFloats[in.Next() % std::size(kSpecialFloats)]);
                        if (in.Next() % 6 == 0)
                            b[j] = std::bit_cast<float>(kSpecialFloats[in.Next() % std::size(kSpecialFloats)]);
                    }
                    va = _mm_load_ps(a), vb = _mm_load_ps(b);
                    nonFinite.Add(_mm_dp_ps(va, vb, 0xFF));
                    nonFinite.Add(_mm_dp_ps(va, vb, 0xEF));
                }
            }
            _mm_setcsr(saved);
            results.push_back({ "dpps finite", finite });
            results.push_back({ "dpps near denormal", denormal });
            results.push_back({ "dpps with nan or inf", nonFinite });
        }

        // The generated code's other float patterns (XenonRecomp's recompiler.cpp). IEEE operations, which shouldn't
        // differ: a difference would mean a compiler or CPU problem.
        void HashVmxPatterns(std::vector<Result>& results)
        {
            Hash hash;
            Inputs in(3);
            const unsigned saved = _mm_getcsr();
            _mm_setcsr(0x1F80 | 0x8040);
            for (int i = 0; i < 1000000; i++)
            {
                const __m128 a = _mm_set_ps(in.FloatBits(), float(in.Double(-30, 30)), float(in.Double(-130, -120)), float(in.Double(-5, 5)));
                const __m128 b = _mm_set_ps(float(in.Double(-30, 30)), in.FloatBits(), float(in.Double(-5, 5)), float(in.Double(-130, -120)));
                const __m128 c = _mm_set_ps(float(in.Double(-30, 30)), float(in.Double(-30, 30)), in.FloatBits(), float(in.Double(-5, 5)));
                // vmaddfp: in double, rounded once to float
                hash.Add(_mm256_cvtpd_ps(_mm256_add_pd(_mm256_mul_pd(_mm256_cvtps_pd(a), _mm256_cvtps_pd(b)), _mm256_cvtps_pd(c))));
                hash.Add(_mm_div_ps(_mm_set1_ps(1), a));              // vrefp
                hash.Add(_mm_div_ps(_mm_set1_ps(1), _mm_sqrt_ps(a))); // vrsqrtefp
                hash.Add(_mm_round_ps(a, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
                hash.Add(_mm_round_ps(a, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC));
                hash.Add(_mm_max_ps(a, b));
                hash.Add(_mm_min_ps(a, b));
                hash.Add(_mm_cvttps_epi32(a));
            }
            for (const uint32_t rounding : { 0x0000u, 0x2000u, 0x4000u, 0x6000u }) // fctid: the current rounding mode
            {
                _mm_setcsr(0x1F80 | rounding);
                for (int i = 0; i < 100000; i++)
                    hash.Add(_mm_cvtsd_si64(_mm_set_sd(in.Double(-10, 60))));
            }
            _mm_setcsr(saved);
            results.push_back({ "vmx and fpu patterns", hash });
        }

        // The guest's math functions through the recompiled code, on a guest thread of its own. Valid domains only: an
        // error would go to the guest C runtime's errno, which needs its per-thread data.
        void HashGuestMath(std::vector<Result>& results)
        {
            const GameState* game = GetGameState();
            if (!game || game->mathFunctions.empty())
                return;
            const std::vector<GameState::MathFunction> functions = game->mathFunctions;
            std::vector<Result> hashed(functions.size());
            kernel::GuestThread::Params params;
            params.stackSize = 0x10000;
            params.hostEntry = [&](PPCContext& ctx) {
                const uint64_t stack = ctx.r1.u64;
                for (size_t f = 0; f < functions.size(); f++)
                {
                    const GameState::MathFunction& function = functions[f];
                    PPCFunc* host = PPC_LOOKUP_FUNC(guest::g_base, function.address);
                    char name[64];
                    snprintf(name, sizeof(name), "guest %s %08X", function.name, function.address);
                    hashed[f].name = name;
                    Inputs in(function.address);
                    for (int i = 0; i < 200000 && host; i++)
                    {
                        ctx.r1.u64 = stack;
                        const double first = in.Double(function.lowExponent, function.highExponent);
                        ctx.f1.f64 = function.positive ? std::fabs(first) : first;
                        ctx.f2.f64 = in.Double(function.secondLowExponent, function.secondHighExponent);
                        host(ctx, guest::g_base);
                        hashed[f].hash.Add(ctx.f1.u64);
                    }
                }
            };
            if (const std::shared_ptr<kernel::GuestThread> thread = kernel::GuestThread::Create(params))
                thread->Join();
            results.insert(results.end(), hashed.begin(), hashed.end());
        }

        std::vector<Result> HashAll()
        {
            std::vector<Result> results;
            HashFma(results);
            HashExpLog(results, 7);
            HashDotProducts(results);
            HashVmxPatterns(results);
            HashGuestMath(results);
            for (const Result& result : results)
                Line("%-32s %016llX %llu", result.name.c_str(), (unsigned long long)result.hash.value, (unsigned long long)result.hash.count);
            return results;
        }
    }

    int Run()
    {
        const std::string path = config::DumpPath("fingerprint.txt");
        g_file = fopen(path.c_str(), "w");
        LOG_INFO("fingerprint: writing %s (a minute or so)", path.c_str());
        PrintMachine();
        PrintModes();
        const int asRun = _get_FMA3_enable();
        _set_FMA3_enable(1); // as the CPU allows them: on with FMA3 (and the AVX the game needs), else it stays off
        Line("-- the C runtime's FMA3 paths as this CPU allows them (%d)", _get_FMA3_enable());
        const std::vector<Result> allowed = HashAll();
        _set_FMA3_enable(0);
        Line("-- the C runtime's FMA3 paths off (%d), as the game runs them and a CPU without FMA3 would", _get_FMA3_enable());
        const std::vector<Result> off = HashAll();
        _set_FMA3_enable(asRun);
        std::string differing;
        for (size_t i = 0; i < allowed.size() && i < off.size(); i++)
        {
            if (allowed[i].hash.value != off[i].hash.value)
                differing += (differing.empty() ? "" : ", ") + allowed[i].name;
        }
        Line("-- with the FMA3 paths on and off: %s%s", differing.empty() ? "the same" : "differ in ", differing.c_str());
        if (g_file)
            fclose(g_file);
        g_file = nullptr;
        return 0;
    }
}
