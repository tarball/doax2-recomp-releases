#pragma once

// Compiles translated shaders on worker threads, with a disk cache of the results. FXC takes
// seconds for the game's large shaders, so draws don't wait for compilation: they are skipped
// until their shaders are ready (SYNC_SHADERS=1 waits instead). The game's shaders are
// precompiled in the background at startup, so that rarely happens.
#include "gpu/d3d12/device.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gpu::d3d12
{
    class ShaderCompiler
    {
    public:
        struct Job
        {
            std::string source;
            std::string target;
            std::string name;
            ComPtr<ID3DBlob> bytecode; // null if compilation failed
            std::atomic<bool> done{ false };
            bool precompiled = false; // created by a background request
            bool background = false;  // still at background priority (guarded by the compiler's lock)
        };

        // `cacheDirectory` may be empty (no disk cache).
        void Start(std::string cacheDirectory);
        ~ShaderCompiler();

        // Compiles `source` for `target`, or returns the job that already does: shaders that translate the same
        // share one. Background jobs (precompilation) run after the others, on at most a quarter of the hardware
        // threads and at below normal priority, so they leave the game its cores; asking for one in the
        // foreground moves it to the front.
        std::shared_ptr<Job> Compile(std::string source, const char* target, std::string name, bool background = false);
        // Blocks until the job has finished.
        void Wait(Job& job);

        std::atomic<uint64_t> cacheHits{ 0 }, compiled{ 0 }; // statistics

    private:
        void Worker();
        void Run(Job& job);

        std::string m_cacheDirectory;
        std::mutex m_lock;
        std::condition_variable m_ready, m_finished;
        std::deque<std::shared_ptr<Job>> m_queue, m_backgroundQueue;
        std::unordered_multimap<uint64_t, std::shared_ptr<Job>> m_jobs; // every job, by Hash(target, source)
        std::vector<std::thread> m_workers;
        uint32_t m_backgroundRunning = 0, m_backgroundLimit = 1;
        bool m_stopping = false;
    };
}
