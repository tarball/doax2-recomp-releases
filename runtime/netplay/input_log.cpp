#include "netplay/input_log.h"

#include "netplay/frame.h"

#include <array>
#include <map>
#include <mutex>
#include <vector>

namespace netplay::input
{
    namespace
    {
        enum class Mode
        {
            kOff,
            kRecord,
            kReplay,
        };

        constexpr uint32_t kUsers = 4;

        std::mutex g_lock;
        Mode g_mode = Mode::kOff;
        std::map<uint64_t, std::array<std::vector<PadRead>, kUsers>> g_frames; // frame -> each pad's reads, in order
        uint64_t g_from = 0, g_to = 0;
        uint64_t g_cursorFrame = ~0ull;                                       // replaying: the frame of the reads below
        std::array<size_t, kUsers> g_cursors{};                                // its reads answered so far, per pad
        uint64_t g_misses = 0;
    }

    void Record()
    {
        std::lock_guard lock(g_lock);
        g_frames.clear();
        g_mode = Mode::kRecord;
    }

    void Replay(uint64_t from, uint64_t to)
    {
        std::lock_guard lock(g_lock);
        g_mode = Mode::kReplay;
        g_from = from;
        g_to = to;
        g_cursorFrame = ~0ull;
        g_misses = 0;
    }

    void Stop()
    {
        std::lock_guard lock(g_lock);
        g_mode = Mode::kOff;
        g_frames.clear();
    }

    bool Replayed(uint32_t user, PadRead& read)
    {
        std::lock_guard lock(g_lock);
        if (g_mode != Mode::kReplay || user >= kUsers)
            return false;
        const uint64_t frame = Frame();
        if (frame < g_from || frame >= g_to)
            return false;
        if (frame != g_cursorFrame)
        {
            g_cursorFrame = frame;
            g_cursors.fill(0);
        }
        const auto it = g_frames.find(frame);
        if (it == g_frames.end() || g_cursors[user] >= it->second[user].size())
        {
            g_misses++;
            return false;
        }
        read = it->second[user][g_cursors[user]++];
        return true;
    }

    void Note(uint32_t user, const PadRead& read)
    {
        std::lock_guard lock(g_lock);
        if (g_mode == Mode::kRecord && user < kUsers)
            g_frames[Frame()][user].push_back(read);
    }

    uint64_t Misses()
    {
        std::lock_guard lock(g_lock);
        return g_misses;
    }
}
