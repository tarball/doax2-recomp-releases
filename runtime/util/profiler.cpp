#include "util/profiler.h"

#include "config.h"
#include "crash_handler.h"
#include "log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>
#include <dbghelp.h>
#include <intrin.h>
#include <psapi.h>
#include <tlhelp32.h>

namespace profile
{
    namespace
    {
        constexpr uint32_t kZoneCount = uint32_t(Zone::kCount);
        constexpr uint32_t kCounterCount = uint32_t(Counter::kCount);

        const char* const kZoneNames[kZoneCount] = {
            "idle (ring empty)", "execute packets", "wait: WAIT_REG_MEM", "wait: interrupt callback", "shader loads", "draws", "swaps",
            "draw: resolves", "draw: pipelines", "draw: vertex/index uploads", "draw: textures", "draw: constants", "draw: recording",
            "swap: submit", "swap: present", "swap: wait for GPU",
        };
        const char* const kCounterNames[kCounterCount] = {
            "memory watch faults", "VirtualProtect calls", "guest memory uploaded (KB)", "upload heap allocated (KB)", "texture loads", "resolves",
            "draw packets", "draws recorded", "pipelines created", "shader loads (IM_LOAD)", "shader load dwords", "interrupt callbacks",
        };
        // Counters kept in bytes and reported in KB.
        bool InKilobytes(uint32_t counter)
        {
            return counter == uint32_t(Counter::kSharedUploadBytes) || counter == uint32_t(Counter::kUploadHeapBytes);
        }

        std::atomic<uint64_t> g_counters[kCounterCount];

        // Command processor accounting (that thread only), in TSC ticks.
        struct FrameRecord
        {
            uint64_t zones[kZoneCount];
            uint64_t counters[kCounterCount];
        };
        Zone g_phase = Zone::kIdle;
        uint64_t g_phaseStart = 0;
        uint64_t g_zones[kZoneCount] = {};
        uint64_t g_counterSnapshot[kCounterCount] = {};

        std::mutex g_frameLock;
        std::vector<FrameRecord> g_frames; // since the last report
        std::vector<double> g_gpuTimes;    // ms, since the last report

        uint64_t (*g_framesPresented)() = nullptr;

        uint64_t Ticks()
        {
            return __rdtsc();
        }

        std::string Utf8(const wchar_t* text)
        {
            const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
            std::string result(size > 0 ? size - 1 : 0, '\0');
            if (size > 1)
                WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
            return result;
        }

        void Appendf(std::string& out, const char* format, ...)
        {
            char buffer[1024];
            va_list arguments;
            va_start(arguments, format);
            vsnprintf(buffer, sizeof(buffer), format, arguments);
            va_end(arguments);
            out += buffer;
        }

        // Threads: CPU time per thread between reports.

        struct ThreadTimes
        {
            uint64_t cycles = 0;
            uint64_t user = 0, kernel = 0; // 100 ns units
            std::string name;
        };

        std::unordered_map<DWORD, ThreadTimes> SnapshotThreads()
        {
            std::unordered_map<DWORD, ThreadTimes> threads;
            const DWORD process = GetCurrentProcessId();
            const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                return threads;
            THREADENTRY32 entry{ sizeof(entry) };
            for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
            {
                if (entry.th32OwnerProcessID != process)
                    continue;
                const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
                if (!thread)
                    continue;
                ThreadTimes& times = threads[entry.th32ThreadID];
                ULONG64 cycles = 0;
                QueryThreadCycleTime(thread, &cycles);
                times.cycles = cycles;
                FILETIME creation, exit, kernel, user;
                if (GetThreadTimes(thread, &creation, &exit, &kernel, &user))
                {
                    times.kernel = (uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
                    times.user = (uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime;
                }
                PWSTR description = nullptr;
                if (SUCCEEDED(GetThreadDescription(thread, &description)) && description)
                {
                    times.name = Utf8(description);
                    LocalFree(description);
                }
                CloseHandle(thread);
            }
            CloseHandle(snapshot);
            return threads;
        }

        // Sampling: call stacks of the busiest threads, unwound with the modules' function tables.

        struct Module
        {
            uintptr_t base = 0, end = 0;
            const RUNTIME_FUNCTION* functions = nullptr;
            uint32_t functionCount = 0;
            std::string name;
        };
        using ModuleList = std::vector<Module>; // sorted by base

        std::shared_ptr<const ModuleList> LoadModules()
        {
            auto modules = std::make_shared<ModuleList>();
            HMODULE handles[1024];
            DWORD needed = 0;
            if (!EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles), &needed))
                return modules;
            for (DWORD i = 0; i < std::min<DWORD>(needed / sizeof(HMODULE), 1024); i++)
            {
                MODULEINFO info{};
                if (!GetModuleInformation(GetCurrentProcess(), handles[i], &info, sizeof(info)))
                    continue;
                Module module;
                module.base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
                module.end = module.base + info.SizeOfImage;
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module.base + dos->e_lfanew);
                const IMAGE_DATA_DIRECTORY& exceptions = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
                if (exceptions.VirtualAddress && exceptions.Size)
                {
                    module.functions = reinterpret_cast<const RUNTIME_FUNCTION*>(module.base + exceptions.VirtualAddress);
                    module.functionCount = exceptions.Size / sizeof(RUNTIME_FUNCTION);
                }
                wchar_t name[MAX_PATH];
                if (GetModuleBaseNameW(GetCurrentProcess(), handles[i], name, MAX_PATH))
                    module.name = Utf8(name);
                modules->push_back(std::move(module));
            }
            std::sort(modules->begin(), modules->end(), [](const Module& a, const Module& b) { return a.base < b.base; });
            return modules;
        }

        const Module* FindModule(const ModuleList& modules, uintptr_t address)
        {
            auto it = std::upper_bound(modules.begin(), modules.end(), address, [](uintptr_t value, const Module& m) { return value < m.base; });
            if (it == modules.begin())
                return nullptr;
            --it;
            return address < it->end ? &*it : nullptr;
        }

        // The function table entry covering `address` (no locks: safe while other threads are suspended).
        const RUNTIME_FUNCTION* FindFunction(const Module& module, uintptr_t address)
        {
            const uint32_t rva = uint32_t(address - module.base);
            uint32_t low = 0, high = module.functionCount;
            while (low < high)
            {
                const uint32_t middle = (low + high) / 2;
                const RUNTIME_FUNCTION& function = module.functions[middle];
                if (rva < function.BeginAddress)
                    high = middle;
                else if (rva >= function.EndAddress)
                    low = middle + 1;
                else
                    return &function;
            }
            return nullptr;
        }

        constexpr int kMaxDepth = 192;

        // Walks a suspended thread's stack from `context`. No allocation and no locks: the thread may hold
        // the heap or stdio lock. Faults (a bad frame) end the walk.
        int Unwind(CONTEXT& context, const NT_TIB* tib, const ModuleList& modules, uint64_t* frames)
        {
            int depth = 0;
            __try
            {
                const uintptr_t stackBase = reinterpret_cast<uintptr_t>(tib->StackBase);
                const uintptr_t stackLimit = reinterpret_cast<uintptr_t>(tib->StackLimit);
                while (depth < kMaxDepth && context.Rip)
                {
                    frames[depth++] = context.Rip;
                    if (context.Rsp < stackLimit || context.Rsp >= stackBase)
                        break;
                    const Module* module = FindModule(modules, context.Rip);
                    if (!module)
                        break;
                    if (const RUNTIME_FUNCTION* function = FindFunction(*module, context.Rip))
                    {
                        void* handlerData = nullptr;
                        DWORD64 establisherFrame = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, module->base, context.Rip, const_cast<RUNTIME_FUNCTION*>(function), &context, &handlerData,
                            &establisherFrame, nullptr);
                    }
                    else
                    {
                        // A leaf function: the return address is on top of the stack.
                        context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
                        context.Rsp += 8;
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            return depth;
        }

        struct Target
        {
            DWORD tid = 0;
            HANDLE handle = nullptr;
            const NT_TIB* tib = nullptr;
            std::string name;
        };

        struct TargetList
        {
            std::vector<Target> threads;
            std::shared_ptr<const ModuleList> modules;
            ~TargetList()
            {
                for (Target& target : threads)
                    CloseHandle(target.handle);
            }
        };

        // NtQueryInformationThread(ThreadBasicInformation): the TEB, whose NT_TIB has the current stack bounds.
        struct ThreadBasicInformation
        {
            LONG exitStatus;
            PVOID tebBaseAddress;
            HANDLE clientProcess, clientThread;
            KAFFINITY affinityMask;
            LONG priority;
            LONG basePriority;
        };
        using NtQueryInformationThreadFunction = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

        std::mutex g_targetLock;
        std::shared_ptr<const TargetList> g_targets;

        struct Sample
        {
            DWORD tid;
            uint32_t first; // index in g_sampleFrames
            uint32_t depth;
        };
        std::mutex g_sampleLock;
        std::vector<Sample> g_samples;
        std::vector<uint64_t> g_sampleFrames;
        std::unordered_map<DWORD, std::string> g_sampleNames;

        void SetTargets(const std::vector<std::pair<DWORD, std::string>>& threads)
        {
            static const auto queryThread =
                reinterpret_cast<NtQueryInformationThreadFunction>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
            auto targets = std::make_shared<TargetList>();
            targets->modules = LoadModules();
            for (const auto& [tid, name] : threads)
            {
                const HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
                if (!handle)
                    continue;
                ThreadBasicInformation information{};
                if (!queryThread || queryThread(handle, 0, &information, sizeof(information), nullptr) < 0 || !information.tebBaseAddress)
                {
                    CloseHandle(handle);
                    continue;
                }
                targets->threads.push_back({ tid, handle, static_cast<const NT_TIB*>(information.tebBaseAddress), name });
            }
            {
                std::lock_guard lock(g_sampleLock);
                for (const Target& target : targets->threads)
                    g_sampleNames[target.tid] = target.name;
            }
            std::lock_guard lock(g_targetLock);
            g_targets = std::move(targets);
        }

        void SampleThread(int hertz)
        {
            NameThread("Profiler sampler");
            crash::SuppressReportsOnThisThread();
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            const auto period = std::chrono::nanoseconds(1000000000 / std::max(hertz, 1));
            auto next = std::chrono::steady_clock::now();
            uint64_t frames[kMaxDepth];
            for (;;)
            {
                next += period;
                const auto now = std::chrono::steady_clock::now();
                if (next < now)
                    next = now;
                std::this_thread::sleep_until(next);

                std::shared_ptr<const TargetList> targets;
                {
                    std::lock_guard lock(g_targetLock);
                    targets = g_targets;
                }
                if (!targets)
                    continue;
                for (const Target& target : targets->threads)
                {
                    if (SuspendThread(target.handle) == DWORD(-1))
                        continue;
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                    int depth = 0;
                    if (GetThreadContext(target.handle, &context))
                        depth = Unwind(context, target.tib, *targets->modules, frames);
                    ResumeThread(target.handle);
                    if (!depth)
                        continue;
                    std::lock_guard lock(g_sampleLock);
                    g_samples.push_back({ target.tid, uint32_t(g_sampleFrames.size()), uint32_t(depth) });
                    g_sampleFrames.insert(g_sampleFrames.end(), frames, frames + depth);
                }
            }
        }

        // Symbols (report thread only: DbgHelp isn't thread-safe).
        class Symbolizer
        {
        public:
            Symbolizer()
            {
                wchar_t path[MAX_PATH];
                GetModuleFileNameW(nullptr, path, MAX_PATH);
                const std::string directory = std::filesystem::path(path).parent_path().string();
                SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
                // Only the executable's directory: no symbol servers.
                m_ready = SymInitialize(GetCurrentProcess(), directory.c_str(), TRUE) != FALSE;
            }

            // A function's start (the key samples are counted by) for a code address.
            uintptr_t FunctionStart(const ModuleList& modules, uintptr_t address)
            {
                if (const Module* module = FindModule(modules, address))
                {
                    if (const RUNTIME_FUNCTION* function = FindFunction(*module, address))
                        return module->base + function->BeginAddress;
                }
                // No function table entry (a leaf function): ask DbgHelp for the symbol's start.
                auto it = m_leafStarts.find(address);
                if (it != m_leafStarts.end())
                    return it->second;
                uintptr_t start = address;
                alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
                auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 512;
                DWORD64 displacement = 0;
                if (m_ready && SymFromAddr(GetCurrentProcess(), address, &displacement, symbol))
                    start = uintptr_t(symbol->Address);
                m_leafStarts[address] = start;
                return start;
            }

            const std::string& Name(const ModuleList& modules, uintptr_t start)
            {
                auto it = m_names.find(start);
                if (it != m_names.end())
                    return it->second;
                const Module* module = FindModule(modules, start);
                std::string name = module ? module->name : "?";
                alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
                auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 512;
                DWORD64 displacement = 0;
                // Generated functions by the function table, which names them without the module's PDB (a release's
                // module has none).
                if (const uint32_t guest = crash::GuestFunctionAt(start))
                    Appendf(name, "!__imp__sub_%08X", guest);
                else if (m_ready && SymFromAddr(GetCurrentProcess(), start, &displacement, symbol))
                {
                    name += "!";
                    name += symbol->Name;
                    if (displacement)
                        Appendf(name, "+0x%llX", (unsigned long long)displacement);
                }
                else if (module)
                {
                    Appendf(name, "+0x%llX", (unsigned long long)(start - module->base));
                }
                return m_names.emplace(start, std::move(name)).first->second;
            }

            // A code address in a call stack: its function and the offset into it (generated functions by the function
            // table, as sub_XXXXXXXX), or the module and the offset into it.
            std::string Describe(const ModuleList& modules, uintptr_t address)
            {
                const Module* module = FindModule(modules, address);
                std::string text = module ? module->name : "";
                if (const uint32_t guest = crash::GuestFunctionAt(address))
                {
                    Appendf(text, "!sub_%08X", guest);
                    return text;
                }
                alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
                auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 512;
                DWORD64 displacement = 0;
                const bool named = module && m_ready && SymFromAddr(GetCurrentProcess(), address, &displacement, symbol);
                // Without a PDB (the runtime's in a release, Windows' own DLLs) DbgHelp names the nearest export, which may
                // be another function: then the offset into the module, and the export only beside it, if it's near.
                IMAGEHLP_MODULE64 info{};
                info.SizeOfStruct = sizeof(info);
                const bool pdb = named && SymGetModuleInfo64(GetCurrentProcess(), address, &info) && (info.SymType == SymPdb || info.SymType == SymDia);
                if (pdb)
                    Appendf(text, "!%s+0x%llX", symbol->Name, (unsigned long long)displacement);
                else if (module)
                {
                    Appendf(text, "+0x%llX", (unsigned long long)(address - module->base));
                    if (named && displacement < 0x1000)
                        Appendf(text, " (%s+0x%llX)", symbol->Name, (unsigned long long)displacement);
                }
                else
                    Appendf(text, "%016llX", (unsigned long long)address);
                return text;
            }

        private:
            bool m_ready = false;
            std::unordered_map<uintptr_t, uintptr_t> m_leafStarts;
            std::unordered_map<uintptr_t, std::string> m_names;
        };

        // DbgHelp isn't thread-safe: one symbolizer for the profiler's reports and the host stacks' dumps, one at a time.
        std::mutex g_symbolLock;
        Symbolizer& SharedSymbols() // (g_symbolLock held)
        {
            static Symbolizer symbols;
            return symbols;
        }

        double Percentile(std::vector<double>& values, double fraction)
        {
            if (values.empty())
                return 0.0;
            const size_t index = std::min(values.size() - 1, size_t(fraction * double(values.size() - 1) + 0.5));
            std::nth_element(values.begin(), values.begin() + index, values.end());
            return values[index];
        }

        void AppendStatistics(std::string& out, const char* label, std::vector<double> values, double scale = 1.0)
        {
            if (values.empty())
                return;
            double sum = 0.0, maximum = 0.0;
            for (double& value : values)
            {
                value *= scale;
                sum += value;
                maximum = std::max(maximum, value);
            }
            const double mean = sum / double(values.size());
            const double p50 = Percentile(values, 0.5), p95 = Percentile(values, 0.95);
            Appendf(out, "[profile]   %-30s %8.2f %8.2f %8.2f %8.2f\n", label, mean, p50, p95, maximum);
        }

        class Reporter
        {
        public:
            Reporter(int seconds, int sampleHertz) : m_seconds(seconds), m_sampleHertz(sampleHertz) {}

            void Run()
            {
                NameThread("Profiler report");
                crash::SuppressReportsOnThisThread();
                // TSC rate, for converting the command processor's tick counts.
                LARGE_INTEGER frequency, q0, q1;
                QueryPerformanceFrequency(&frequency);
                QueryPerformanceCounter(&q0);
                const uint64_t t0 = Ticks();
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                QueryPerformanceCounter(&q1);
                const uint64_t t1 = Ticks();
                m_ticksPerMs = double(t1 - t0) / (double(q1.QuadPart - q0.QuadPart) * 1000.0 / double(frequency.QuadPart));
                LOG_INFO("Profiler: reports every %d s%s (TSC %.0f MHz)", m_seconds,
                    m_sampleHertz ? (", sampling at " + std::to_string(m_sampleHertz) + " Hz").c_str() : "", m_ticksPerMs / 1000.0);

                auto previousThreads = SnapshotThreads();
                uint64_t previousFrames = g_framesPresented ? g_framesPresented() : 0;
                LARGE_INTEGER previousTime;
                QueryPerformanceCounter(&previousTime);
                for (uint32_t report = 1;; report++)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(m_seconds));
                    LARGE_INTEGER now;
                    QueryPerformanceCounter(&now);
                    const double elapsedMs = double(now.QuadPart - previousTime.QuadPart) * 1000.0 / double(frequency.QuadPart);
                    const uint64_t frames = g_framesPresented ? g_framesPresented() : 0;
                    auto threads = SnapshotThreads();

                    std::string out;
                    const uint64_t frameCount = frames - previousFrames;
                    Appendf(out, "[profile] ===== report %u: %.1f s, %llu frames (%.1f fps)\n", report, elapsedMs / 1000.0,
                        (unsigned long long)frameCount, double(frameCount) * 1000.0 / elapsedMs);
                    const auto busy = ReportThreads(out, previousThreads, threads, elapsedMs, frameCount);
                    ReportCommandProcessor(out);
                    if (m_sampleHertz)
                        ReportSamples(out, report);
                    fwrite(out.data(), 1, out.size(), stderr);
                    fflush(stderr);

                    if (m_sampleHertz)
                        SetTargets(busy);
                    previousThreads = std::move(threads);
                    previousFrames = frames;
                    previousTime = now;
                }
            }

        private:
            // Returns the threads that used at least 3% of a CPU (the next interval's sampling targets).
            std::vector<std::pair<DWORD, std::string>> ReportThreads(std::string& out, const std::unordered_map<DWORD, ThreadTimes>& before,
                const std::unordered_map<DWORD, ThreadTimes>& after, double elapsedMs, uint64_t frames)
            {
                struct Row
                {
                    DWORD tid;
                    double percent, kernelShare;
                    const std::string* name;
                };
                std::vector<Row> rows;
                double total = 0.0;
                const DWORD self = GetCurrentThreadId();
                for (const auto& [tid, times] : after)
                {
                    const auto it = before.find(tid);
                    const uint64_t cycles = times.cycles - (it != before.end() ? it->second.cycles : 0);
                    const uint64_t user = times.user - (it != before.end() ? it->second.user : 0);
                    const uint64_t kernel = times.kernel - (it != before.end() ? it->second.kernel : 0);
                    const double percent = 100.0 * double(cycles) / (m_ticksPerMs * elapsedMs);
                    total += percent;
                    rows.push_back({ tid, percent, user + kernel ? double(kernel) / double(user + kernel) : 0.0, &times.name });
                }
                std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.percent > b.percent; });
                Appendf(out, "[profile] CPU by thread (%% of one core; ms per frame; kernel share): %.0f%% in %zu threads\n", total, rows.size());
                std::vector<std::pair<DWORD, std::string>> busy;
                double rest = 0.0;
                uint32_t restCount = 0;
                for (const Row& row : rows)
                {
                    if (row.percent < 1.0)
                    {
                        rest += row.percent;
                        restCount++;
                        continue;
                    }
                    Appendf(out, "[profile]   %6.1f%% %7.2f ms  kernel %3.0f%%  %s [%lu]\n", row.percent,
                        frames ? row.percent / 100.0 * elapsedMs / double(frames) : 0.0, row.kernelShare * 100.0,
                        row.name->empty() ? "(unnamed)" : row.name->c_str(), row.tid);
                    if (row.percent >= 3.0 && row.tid != self && *row.name != "Profiler sampler")
                        busy.emplace_back(row.tid, row.name->empty() ? "thread " + std::to_string(row.tid) : *row.name);
                }
                Appendf(out, "[profile]   %6.1f%% in %u threads under 1%% each\n", rest, restCount);
                return busy;
            }

            void ReportCommandProcessor(std::string& out)
            {
                std::vector<FrameRecord> frames;
                std::vector<double> gpuTimes;
                {
                    std::lock_guard lock(g_frameLock);
                    frames.swap(g_frames);
                    gpuTimes.swap(g_gpuTimes);
                }
                if (frames.empty())
                    return;
                Appendf(out, "[profile] command processor, ms per frame over %zu frames:  %8s %8s %8s %8s\n", frames.size(), "mean", "p50", "p95", "max");
                const double scale = 1.0 / m_ticksPerMs;
                std::vector<double> values(frames.size());
                for (size_t i = 0; i < frames.size(); i++)
                {
                    uint64_t sum = 0;
                    for (uint32_t phase = 0; phase < uint32_t(Zone::kPhaseCount); phase++)
                        sum += frames[i].zones[phase];
                    values[i] = double(sum);
                }
                AppendStatistics(out, "frame (all phases)", values, scale);
                for (uint32_t zone = 0; zone < kZoneCount; zone++)
                {
                    bool any = false;
                    for (size_t i = 0; i < frames.size(); i++)
                    {
                        values[i] = double(frames[i].zones[zone]);
                        any |= frames[i].zones[zone] != 0;
                    }
                    if (any)
                        AppendStatistics(out, kZoneNames[zone], values, scale);
                }
                if (!gpuTimes.empty())
                {
                    Appendf(out, "[profile] GPU, ms per frame over %zu frames:\n", gpuTimes.size());
                    AppendStatistics(out, "GPU time (timestamps)", gpuTimes);
                }
                Appendf(out, "[profile] counters per frame:\n");
                for (uint32_t counter = 0; counter < kCounterCount; counter++)
                {
                    bool any = false;
                    for (size_t i = 0; i < frames.size(); i++)
                    {
                        values[i] = double(frames[i].counters[counter]);
                        any |= frames[i].counters[counter] != 0;
                    }
                    if (any)
                        AppendStatistics(out, kCounterNames[counter], values, InKilobytes(counter) ? 1.0 / 1024.0 : 1.0);
                }
            }

            void ReportSamples(std::string& out, uint32_t report)
            {
                std::vector<Sample> samples;
                std::vector<uint64_t> frames;
                std::unordered_map<DWORD, std::string> names;
                {
                    std::lock_guard lock(g_sampleLock);
                    samples.swap(g_samples);
                    frames.swap(g_sampleFrames);
                    names = g_sampleNames;
                }
                std::shared_ptr<const ModuleList> modules;
                {
                    std::lock_guard lock(g_targetLock);
                    if (g_targets)
                        modules = g_targets->modules;
                }
                if (samples.empty() || !modules)
                    return;
                std::lock_guard symbolLock(g_symbolLock);
                Symbolizer& symbols = SharedSymbols();

                // Per thread: samples by leaf function (self) and by every function on the stack (inclusive),
                // and the leaf functions' callers.
                struct Counts
                {
                    uint32_t total = 0;
                    std::unordered_map<uintptr_t, uint32_t> self, inclusive;
                    std::unordered_map<uintptr_t, std::unordered_map<uintptr_t, uint32_t>> callers; // leaf -> caller -> samples
                    std::map<std::string, uint32_t> modules;
                };
                std::map<DWORD, Counts> threads;
                std::string folded;
                const bool writeFolded = std::getenv(GAME_PREFIX "_PROFILE_FOLDED") != nullptr;
                std::unordered_map<std::string, uint32_t> foldedStacks;
                std::vector<uintptr_t> stack;
                for (const Sample& sample : samples)
                {
                    Counts& counts = threads[sample.tid];
                    counts.total++;
                    stack.clear();
                    for (uint32_t i = 0; i < sample.depth; i++)
                    {
                        // Return addresses point after the call; look up the call itself.
                        const uintptr_t address = uintptr_t(frames[sample.first + i]) - (i ? 1 : 0);
                        stack.push_back(symbols.FunctionStart(*modules, address));
                    }
                    counts.self[stack[0]]++;
                    if (stack.size() > 1)
                        counts.callers[stack[0]][stack[1]]++;
                    const Module* module = FindModule(*modules, stack[0]);
                    counts.modules[module ? module->name : "?"]++;
                    for (size_t i = 0; i < stack.size(); i++)
                    {
                        if (std::find(stack.begin(), stack.begin() + i, stack[i]) == stack.begin() + i)
                            counts.inclusive[stack[i]]++;
                    }
                    if (writeFolded)
                    {
                        std::string line = names[sample.tid];
                        for (size_t i = stack.size(); i-- > 0;)
                        {
                            line += ';';
                            line += symbols.Name(*modules, stack[i]);
                        }
                        foldedStacks[line]++;
                    }
                }

                for (auto& [tid, counts] : threads)
                {
                    Appendf(out, "[profile] samples of %s [%lu]: %u\n", names[tid].c_str(), tid, counts.total);
                    std::string moduleLine;
                    std::vector<std::pair<std::string, uint32_t>> moduleRows(counts.modules.begin(), counts.modules.end());
                    std::sort(moduleRows.begin(), moduleRows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                    for (const auto& [name, samplesInModule] : moduleRows)
                        Appendf(moduleLine, " %s %.1f%%,", name.c_str(), 100.0 * samplesInModule / counts.total);
                    if (!moduleLine.empty())
                        moduleLine.pop_back();
                    Appendf(out, "[profile]   self by module:%s\n", moduleLine.c_str());

                    std::vector<std::pair<uintptr_t, uint32_t>> rows(counts.self.begin(), counts.self.end());
                    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                    Appendf(out, "[profile]   self:\n");
                    for (size_t i = 0; i < rows.size() && i < 30; i++)
                    {
                        Appendf(out, "[profile]     %5.1f%%  %s", 100.0 * rows[i].second / counts.total, symbols.Name(*modules, rows[i].first).c_str());
                        // The top callers of the hottest functions.
                        if (i < 12)
                        {
                            const auto& callers = counts.callers[rows[i].first];
                            std::vector<std::pair<uintptr_t, uint32_t>> callerRows(callers.begin(), callers.end());
                            std::sort(callerRows.begin(), callerRows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                            for (size_t c = 0; c < callerRows.size() && c < 3; c++)
                                Appendf(out, "%s %s %.0f%%", c ? "," : "  <-", symbols.Name(*modules, callerRows[c].first).c_str(),
                                    100.0 * callerRows[c].second / rows[i].second);
                        }
                        out += "\n";
                    }
                    rows.assign(counts.inclusive.begin(), counts.inclusive.end());
                    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                    Appendf(out, "[profile]   inclusive:\n");
                    for (size_t i = 0, shown = 0; i < rows.size() && shown < 40; i++)
                    {
                        Appendf(out, "[profile]     %5.1f%%  %s\n", 100.0 * rows[i].second / counts.total, symbols.Name(*modules, rows[i].first).c_str());
                        shown++;
                    }
                }

                if (writeFolded)
                {
                    const std::string path = config::DataDirectory() + "/dumps/profile_" + std::to_string(report) + ".folded";
                    std::error_code error;
                    std::filesystem::create_directories(config::DataDirectory() + "/dumps", error);
                    if (FILE* file = fopen(path.c_str(), "wb"))
                    {
                        for (const auto& [line, count] : foldedStacks)
                            fprintf(file, "%s %u\n", line.c_str(), count);
                        fclose(file);
                        Appendf(out, "[profile] folded stacks written to %s\n", path.c_str());
                    }
                }
            }

            int m_seconds;
            int m_sampleHertz;
            double m_ticksPerMs = 1.0;
        };
    }

    bool Enabled()
    {
        static const bool enabled = std::getenv(GAME_PREFIX "_PROFILE") != nullptr;
        return enabled;
    }

    void DumpHostStacks(uint32_t maxDepth)
    {
        // On a thread of its own, which handles the faults of a bad frame's walk itself (no crash report for them).
        std::thread([maxDepth] {
            NameThread("Host stacks' dump");
            crash::SuppressReportsOnThisThread();
            DumpHostStacksHere(maxDepth);
        }).join();
    }

    void DumpHostStacksHere(uint32_t maxDepth)
    {
        static const auto queryThread =
            reinterpret_cast<NtQueryInformationThreadFunction>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
        maxDepth = std::min<uint32_t>(std::max<uint32_t>(maxDepth, 1), kMaxDepth);
        // The threads and their names first: nothing is allocated while a thread is suspended (it may hold the heap's lock).
        struct Thread
        {
            DWORD tid = 0;
            HANDLE handle = nullptr;
            const NT_TIB* tib = nullptr;
            std::string name;
            std::vector<uint64_t> frames;
        };
        std::vector<Thread> threads;
        const DWORD process = GetCurrentProcessId(), self = GetCurrentThreadId();
        if (const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); snapshot != INVALID_HANDLE_VALUE)
        {
            THREADENTRY32 entry{ sizeof(entry) };
            for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
            {
                if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self)
                    continue;
                const HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
                if (!handle)
                    continue;
                ThreadBasicInformation information{};
                if (!queryThread || queryThread(handle, 0, &information, sizeof(information), nullptr) < 0 || !information.tebBaseAddress)
                {
                    CloseHandle(handle);
                    continue;
                }
                Thread& thread = threads.emplace_back();
                thread.tid = entry.th32ThreadID;
                thread.handle = handle;
                thread.tib = static_cast<const NT_TIB*>(information.tebBaseAddress);
                thread.frames.resize(kMaxDepth);
                PWSTR description = nullptr;
                if (SUCCEEDED(GetThreadDescription(handle, &description)) && description)
                {
                    thread.name = Utf8(description);
                    LocalFree(description);
                }
            }
            CloseHandle(snapshot);
        }
        const std::shared_ptr<const ModuleList> modules = LoadModules();
        // Each stack walked with its thread suspended, one thread at a time (Unwind neither allocates nor locks).
        for (Thread& thread : threads)
        {
            int depth = 0;
            if (SuspendThread(thread.handle) != DWORD(-1))
            {
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                if (GetThreadContext(thread.handle, &context))
                    depth = Unwind(context, thread.tib, *modules, thread.frames.data());
                ResumeThread(thread.handle);
            }
            thread.frames.resize(size_t(depth));
            CloseHandle(thread.handle);
        }
        std::string out;
        {
            std::lock_guard lock(g_symbolLock);
            Symbolizer& symbols = SharedSymbols();
            for (const Thread& thread : threads)
            {
                Appendf(out, "[host] thread %lu (%s):\n", thread.tid, thread.name.empty() ? "unnamed" : thread.name.c_str());
                for (size_t i = 0; i < thread.frames.size() && i < maxDepth; i++)
                {
                    // Return addresses point after the call: the call itself is the byte before.
                    const uintptr_t address = uintptr_t(thread.frames[i]) - (i ? 1 : 0);
                    Appendf(out, "[host]   #%zu %s\n", i, symbols.Describe(*modules, address).c_str());
                }
            }
        }
        fputs(out.c_str(), stderr);
        fflush(stderr);
    }

    void NameThread(const char* format, ...)
    {
        char name[128];
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(name, sizeof(name), format, arguments);
        va_end(arguments);
        wchar_t wide[128];
        if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 128))
            SetThreadDescription(GetCurrentThread(), wide);
    }

    void Start(uint64_t (*framesPresented)())
    {
        if (!Enabled())
            return;
        g_framesPresented = framesPresented;
        const int seconds = std::max(1, std::atoi(std::getenv(GAME_PREFIX "_PROFILE")));
        const char* sample = std::getenv(GAME_PREFIX "_PROFILE_SAMPLE");
        const int sampleHertz = sample ? std::max(0, std::atoi(sample)) : 0;
        std::thread([seconds, sampleHertz] { Reporter(seconds, sampleHertz).Run(); }).detach();
        if (sampleHertz)
            std::thread(SampleThread, sampleHertz).detach();
    }

    Zone SetPhase(Zone phase)
    {
        if (!Enabled())
            return phase;
        const uint64_t now = Ticks();
        if (g_phaseStart)
            g_zones[uint32_t(g_phase)] += now - g_phaseStart;
        const Zone previous = g_phase;
        g_phase = phase;
        g_phaseStart = now;
        return previous;
    }

    void EndFrame()
    {
        if (!Enabled())
            return;
        const uint64_t now = Ticks();
        if (g_phaseStart)
            g_zones[uint32_t(g_phase)] += now - g_phaseStart;
        g_phaseStart = now;
        FrameRecord record;
        std::copy(std::begin(g_zones), std::end(g_zones), record.zones);
        std::fill(std::begin(g_zones), std::end(g_zones), 0);
        for (uint32_t i = 0; i < kCounterCount; i++)
        {
            const uint64_t value = g_counters[i].load(std::memory_order_relaxed);
            record.counters[i] = value - g_counterSnapshot[i];
            g_counterSnapshot[i] = value;
        }
        std::lock_guard lock(g_frameLock);
        if (g_frames.size() < 1000000)
            g_frames.push_back(record);
    }

    ScopedZone::ScopedZone(Zone zone) : m_zone(zone), m_start(Enabled() ? Ticks() : 0)
    {
    }

    void ScopedZone::Stop()
    {
        if (m_start)
            g_zones[uint32_t(m_zone)] += Ticks() - m_start;
        m_start = 0;
    }

    void RecordGpuTime(double milliseconds)
    {
        if (!Enabled())
            return;
        std::lock_guard lock(g_frameLock);
        if (g_gpuTimes.size() < 1000000)
            g_gpuTimes.push_back(milliseconds);
    }

    void Count(Counter counter, uint64_t amount)
    {
        if (Enabled())
            g_counters[uint32_t(counter)].fetch_add(amount, std::memory_order_relaxed);
    }
}
