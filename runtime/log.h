#pragma once

#include <cstdio>
#include <cstdlib>

// Minimal logging to stderr. LOG_TRACE is compiled in but only prints when the TRACE environment variable is set.
// The runtime's environment variables are named with the game's prefix, GAME_PREFIX from CMakeLists.txt, and an
// underscore (TRACE is DOAX2_TRACE for DOAX2); comments name them without it.
namespace logging
{
    inline bool TraceEnabled()
    {
        static const bool enabled = std::getenv(GAME_PREFIX "_TRACE") != nullptr;
        return enabled;
    }
}

#define LOG_INFO(fmt, ...) (fprintf(stderr, "[info] " fmt "\n" __VA_OPT__(,) __VA_ARGS__), fflush(stderr))
#define LOG_WARN(fmt, ...) (fprintf(stderr, "[warn] " fmt "\n" __VA_OPT__(,) __VA_ARGS__), fflush(stderr))
#define LOG_ERROR(fmt, ...) (fprintf(stderr, "[error] " fmt "\n" __VA_OPT__(,) __VA_ARGS__), fflush(stderr))
#define LOG_TRACE(fmt, ...)                                                       \
    do                                                                            \
    {                                                                             \
        if (logging::TraceEnabled())                                              \
            fprintf(stderr, "[trace] " fmt "\n" __VA_OPT__(,) __VA_ARGS__);       \
    } while (0)
