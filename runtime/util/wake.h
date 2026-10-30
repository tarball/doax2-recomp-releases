#pragma once

// A counter that threads sleep on until it changes. For threads that wait for each other's progress where
// the 360 let them spin on hardware threads of their own: on a PC each spinning thread burns a core.
#include <atomic>
#include <cstdint>

#include <windows.h>

namespace util
{
    class WakeCounter
    {
    public:
        uint64_t Value() const { return m_count.load(); }

        // Counts a change and wakes the threads waiting for one.
        void Notify()
        {
            m_count.fetch_add(1);
            if (m_waiters.load())
                WakeByAddressAll(&m_count);
        }

        // Blocks while the count is still `seen`, for up to `milliseconds`; returns the count. A Notify between
        // reading `seen` and calling this isn't lost: the wait returns at once when the count already differs.
        // (Both sides use locked read-modify-writes, so a waiter is counted before it checks the count.)
        uint64_t Wait(uint64_t seen, uint32_t milliseconds)
        {
            m_waiters.fetch_add(1);
            WaitOnAddress(&m_count, &seen, sizeof(seen), milliseconds);
            m_waiters.fetch_sub(1);
            return m_count.load();
        }

    private:
        std::atomic<uint64_t> m_count{ 0 };
        std::atomic<uint32_t> m_waiters{ 0 };
    };
}
