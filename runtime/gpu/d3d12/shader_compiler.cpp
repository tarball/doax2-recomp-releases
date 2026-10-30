#include "gpu/d3d12/shader_compiler.h"

#include "log.h"
#include "util/profiler.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include <d3dcompiler.h>

namespace gpu::d3d12
{
    namespace
    {
        uint64_t Hash(const std::string& a, const std::string& b)
        {
            uint64_t hash = 0xCBF29CE484222325ull;
            for (const std::string* text : { &a, &b })
            {
                for (unsigned char c : *text)
                    hash = (hash ^ c) * 0x100000001B3ull;
                hash = (hash ^ 0xFF) * 0x100000001B3ull;
            }
            return hash;
        }
    }

    void ShaderCompiler::Start(std::string cacheDirectory)
    {
        m_cacheDirectory = std::move(cacheDirectory);
        if (!m_cacheDirectory.empty())
        {
            std::error_code error;
            std::filesystem::create_directories(m_cacheDirectory, error);
        }
        const uint32_t threads = std::max(1u, std::thread::hardware_concurrency() > 2 ? std::thread::hardware_concurrency() - 2 : 1u);
        m_backgroundLimit = std::max(1u, std::thread::hardware_concurrency() / 4);
        for (uint32_t i = 0; i < threads; i++)
            m_workers.emplace_back([this] { Worker(); });
        LOG_INFO("GPU: %u shader compiler threads, cache in %s", threads, m_cacheDirectory.empty() ? "(none)" : m_cacheDirectory.c_str());
    }

    ShaderCompiler::~ShaderCompiler()
    {
        {
            std::lock_guard lock(m_lock);
            m_stopping = true;
        }
        m_ready.notify_all();
        for (std::thread& worker : m_workers)
            worker.join();
    }

    std::shared_ptr<ShaderCompiler::Job> ShaderCompiler::Compile(std::string source, const char* target, std::string name, bool background)
    {
        const uint64_t key = Hash(target, source);
        std::shared_ptr<Job> job;
        {
            std::lock_guard lock(m_lock);
            for (auto [it, end] = m_jobs.equal_range(key); it != end; ++it)
            {
                const std::shared_ptr<Job>& known = it->second;
                if (known->target != target || known->source != source)
                    continue;
                if (!background && known->background)
                {
                    known->background = false;
                    if (const auto queued = std::find(m_backgroundQueue.begin(), m_backgroundQueue.end(), known); queued != m_backgroundQueue.end())
                    {
                        m_backgroundQueue.erase(queued);
                        m_queue.push_back(known);
                        m_ready.notify_one(); // idle workers may be waiting for a background slot
                    }
                }
                return known;
            }
            job = std::make_shared<Job>();
            job->source = std::move(source);
            job->target = target;
            job->name = std::move(name);
            job->precompiled = job->background = background;
            m_jobs.emplace(key, job);
            (background ? m_backgroundQueue : m_queue).push_back(job);
        }
        m_ready.notify_one();
        return job;
    }

    void ShaderCompiler::Wait(Job& job)
    {
        std::unique_lock lock(m_lock);
        m_finished.wait(lock, [&] { return job.done.load(); });
    }

    void ShaderCompiler::Worker()
    {
        profile::NameThread("Shader compiler");
        for (;;)
        {
            std::shared_ptr<Job> job;
            bool background;
            {
                std::unique_lock lock(m_lock);
                m_ready.wait(lock, [this] {
                    return m_stopping || !m_queue.empty() || (!m_backgroundQueue.empty() && m_backgroundRunning < m_backgroundLimit);
                });
                if (m_stopping)
                    return;
                background = m_queue.empty();
                auto& queue = background ? m_backgroundQueue : m_queue;
                job = std::move(queue.front());
                queue.pop_front();
                m_backgroundRunning += background;
            }
            // Precompilation shouldn't take the game's cores.
            SetThreadPriority(GetCurrentThread(), background ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL);
            Run(*job);
            {
                std::lock_guard lock(m_lock);
                job->done = true;
                m_backgroundRunning -= background;
            }
            m_finished.notify_all();
            if (background)
                m_ready.notify_one(); // a background slot is free
        }
    }

    void ShaderCompiler::Run(Job& job)
    {
        // The cache key covers the HLSL, so translator changes invalidate entries naturally.
        std::string path;
        if (!m_cacheDirectory.empty())
        {
            char name[32];
            snprintf(name, sizeof(name), "/%016llX.dxbc", (unsigned long long)Hash(job.target, job.source));
            path = m_cacheDirectory + name;
            if (FILE* file = fopen(path.c_str(), "rb"))
            {
                fseek(file, 0, SEEK_END);
                const long size = ftell(file);
                fseek(file, 0, SEEK_SET);
                ComPtr<ID3DBlob> blob;
                if (size > 0 && SUCCEEDED(D3DCreateBlob(size_t(size), &blob)) && fread(blob->GetBufferPointer(), 1, size_t(size), file) == size_t(size))
                {
                    fclose(file);
                    job.bytecode = blob;
                    cacheHits++;
                    return;
                }
                fclose(file);
            }
        }

        job.bytecode = CompileShader(job.source.c_str(), "main", job.target.c_str(), job.name.c_str());
        compiled++;
        if (job.bytecode && !path.empty())
        {
            const std::string temporary = path + ".tmp";
            if (FILE* file = fopen(temporary.c_str(), "wb"))
            {
                const bool ok = fwrite(job.bytecode->GetBufferPointer(), 1, job.bytecode->GetBufferSize(), file) == job.bytecode->GetBufferSize();
                fclose(file);
                std::error_code error;
                if (ok)
                    std::filesystem::rename(temporary, path, error);
                else
                    std::filesystem::remove(temporary, error);
            }
        }
    }
}
