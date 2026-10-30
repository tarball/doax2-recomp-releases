#include "guest_memory.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include <windows.h>

namespace guest
{
    uint8_t* g_base = nullptr;

    namespace
    {
        constexpr uint32_t k4KB = 0x1000;
        constexpr uint32_t k64KB = 0x10000;
        constexpr uint32_t k16MB = 0x1000000;
        constexpr uint64_t kPhysicalSize = 0x20000000; // 512 MB

        struct Heaps
        {
            std::unique_ptr<PageHeap> v00000000;
            std::unique_ptr<PageHeap> v40000000;
            std::unique_ptr<PageHeap> v7F000000;
            std::unique_ptr<PageHeap> v80000000;
            std::unique_ptr<PageHeap> v90000000;
            std::unique_ptr<PageHeap> physicalPool;
            // Physical views: virtual = view base + physical. Both map the same host memory.
            std::unique_ptr<PageHeap> vA0000000;
            std::unique_ptr<PageHeap> vC0000000;
        };

        Heaps g_heaps;

        // TRACE_PHYSICAL=1 logs physical memory allocations and frees.
        bool TracingPhysical()
        {
            static const bool tracing = [] {
                const char* setting = std::getenv(GAME_PREFIX "_TRACE_PHYSICAL");
                return setting && std::atoi(setting) != 0;
            }();
            return tracing;
        }

        PageHeap* LookupView(uint32_t address)
        {
            for (PageHeap* view : { g_heaps.vA0000000.get(), g_heaps.vC0000000.get() })
            {
                if (view->Contains(address))
                    return view;
            }
            return nullptr;
        }
    }

    std::recursive_mutex& HeapLock()
    {
        static std::recursive_mutex lock;
        return lock;
    }

    namespace
    {
        using VirtualAlloc2Function = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
        using MapViewOfFile3Function = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);

        // Reserves the 4 GB space as placeholders: committed memory below A0000000, the A0000000 and
        // C0000000 windows both mapping one 512 MB section (Windows 10 1803+), and nothing from
        // E0000000 up (see guest_memory.h). Below it, the store map, committed (zero: nothing watched).
        uint8_t* ReserveAddressSpace()
        {
            const HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
            const auto virtualAlloc2 = reinterpret_cast<VirtualAlloc2Function>(GetProcAddress(kernelBase, "VirtualAlloc2"));
            const auto mapViewOfFile3 = reinterpret_cast<MapViewOfFile3Function>(GetProcAddress(kernelBase, "MapViewOfFile3"));
            if (!virtualAlloc2 || !mapViewOfFile3)
            {
                fprintf(stderr, "VirtualAlloc2/MapViewOfFile3 unavailable (Windows 10 1803 or later is required)\n");
                return nullptr;
            }

            const HANDLE process = GetCurrentProcess();
            auto* map = static_cast<uint8_t*>(virtualAlloc2(process, nullptr, kStoreMapSize + kAddressSpaceSize,
                MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
            if (!map)
                return nullptr;
            uint8_t* const base = map + kStoreMapSize;
            if (!VirtualFree(map, kStoreMapSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) ||
                !virtualAlloc2(process, map, kStoreMapSize, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0))
            {
                fprintf(stderr, "Can't set up the store map (error %lu)\n", GetLastError());
                return nullptr;
            }

            // Split into [0, A0000000), [A0000000, C0000000), [C0000000, E0000000) and [E0000000, 4 GB).
            const bool split = VirtualFree(base, kSharedViewA, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) &&
                VirtualFree(base + kSharedViewA, kPhysicalSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) &&
                VirtualFree(base + kSharedViewC, kPhysicalSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
            const HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, 0, uint32_t(kPhysicalSize), nullptr);
            if (!split || !section ||
                !virtualAlloc2(process, base, kSharedViewA, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) ||
                !mapViewOfFile3(section, process, base + kSharedViewA, 0, kPhysicalSize, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) ||
                !mapViewOfFile3(section, process, base + kSharedViewC, 0, kPhysicalSize, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0))
            {
                fprintf(stderr, "Can't set up the guest address space (error %lu)\n", GetLastError());
                return nullptr;
            }
            return base;
        }
    }

    bool Initialize()
    {
        // Committed pages are backed lazily, so this only costs commit charge until touched.
        // The base is 64 KB aligned, which satisfies the generated code's 32-byte alignment assumption.
        g_base = ReserveAddressSpace();
        if (!g_base)
            return false;

        g_heaps.v00000000 = std::make_unique<PageHeap>(0x00000000, 0x40000000, k4KB);
        g_heaps.v40000000 = std::make_unique<PageHeap>(0x40000000, 0x3F000000, k64KB);
        g_heaps.v7F000000 = std::make_unique<PageHeap>(0x7F000000, 0x01000000, k64KB);
        g_heaps.v80000000 = std::make_unique<PageHeap>(0x80000000, 0x10000000, k64KB);
        g_heaps.v90000000 = std::make_unique<PageHeap>(0x90000000, 0x10000000, k4KB);
        g_heaps.physicalPool = std::make_unique<PageHeap>(0x00000000, kPhysicalSize, k4KB);
        // The A0000000 view also holds 4 KB-page allocations, so it keeps 4 KB bookkeeping.
        g_heaps.vA0000000 = std::make_unique<PageHeap>(kSharedViewA, kPhysicalSize, k4KB);
        g_heaps.vC0000000 = std::make_unique<PageHeap>(kSharedViewC, kPhysicalSize, k16MB);

        // Keep the first 64 KB unallocated and inaccessible, so guest null-pointer accesses fault
        // (reported by the crash handler) instead of silently reading or writing guest memory.
        g_heaps.v00000000->AllocFixed(0, k64KB, true, false, kPageNoAccess);
        DWORD oldProtect;
        VirtualProtect(g_base, k64KB, PAGE_NOACCESS, &oldProtect);
        // Physical page 0 is never handed out, so no allocation has physical address 0.
        g_heaps.physicalPool->AllocFixed(0, k4KB, true, false, kPageNoAccess);
        return true;
    }

    PageHeap* LookupHeap(uint32_t address)
    {
        for (PageHeap* heap : { g_heaps.v00000000.get(), g_heaps.v40000000.get(), g_heaps.v7F000000.get(),
                 g_heaps.v80000000.get(), g_heaps.v90000000.get() })
        {
            if (heap->Contains(address))
                return heap;
        }

        return LookupView(address);
    }

    PageHeap* VirtualHeap(uint32_t pageSize)
    {
        return pageSize >= k64KB ? g_heaps.v40000000.get() : g_heaps.v00000000.get();
    }

    uint32_t AllocPhysical(uint32_t size, uint32_t pageSize, uint32_t protect, uint32_t minPhysical, uint32_t maxPhysical, uint32_t alignment)
    {
        std::lock_guard lock(HeapLock());

        // 16 MB pages come from the C0000000 view, the rest from A0000000 (see guest_memory.h).
        const uint32_t page = pageSize >= k16MB ? k16MB : pageSize >= k64KB ? k64KB : k4KB;
        PageHeap* view = page == k16MB ? g_heaps.vC0000000.get() : g_heaps.vA0000000.get();
        const uint32_t adjustedSize = (size + page - 1) / page * page;
        const uint32_t adjustedAlignment = std::max(alignment, page);
        const uint64_t high = std::min<uint64_t>(uint64_t(maxPhysical) + 1, kPhysicalSize);

        // Physical allocations are placed top-down, like the 360 kernel does.
        const uint32_t physical = g_heaps.physicalPool->AllocRange(minPhysical, high, adjustedSize, adjustedAlignment, true, protect, true);
        if (physical == 0)
            return 0;

        const uint32_t address = view->Base() + physical;
        if (!view->AllocFixed(address, adjustedSize, true, true, protect))
        {
            g_heaps.physicalPool->Release(physical);
            return 0;
        }

        BeforeHostWrite(address, adjustedSize);
        memset(ToHost(address), 0, adjustedSize);
        if (TracingPhysical())
            fprintf(stderr, "[physical] alloc %08X size %08X (physical %08X, page %X)\n", address, adjustedSize, physical, pageSize);
        return address;
    }

    namespace
    {
        void (*g_physicalFreeHook)(uint32_t physical, uint32_t size) = nullptr;
    }

    void SetPhysicalFreeHook(void (*hook)(uint32_t physical, uint32_t size))
    {
        g_physicalFreeHook = hook;
    }

    bool FreePhysical(uint32_t address)
    {
        std::lock_guard lock(HeapLock());

        PageHeap* view = LookupView(address);
        uint32_t size = 0;
        if (!view || !view->Release(address, &size))
            return false;

        const uint32_t physical = PhysicalAddress(address);
        if (TracingPhysical())
            fprintf(stderr, "[physical] free %08X size %08X (physical %08X)\n", address, size, physical);
        if (g_physicalFreeHook)
            g_physicalFreeHook(physical, size);
        return g_heaps.physicalPool->Release(physical);
    }

    uint32_t PhysicalAddress(uint32_t address)
    {
        if (PageHeap* view = LookupView(address))
            return address - view->Base();
        return address;
    }

    uint32_t PhysicalToGuest(uint32_t physical)
    {
        return physical < kPhysicalSize ? kSharedViewA + physical : physical;
    }

    uint32_t SystemAlloc(uint32_t size)
    {
        std::lock_guard lock(HeapLock());

        // Top-down so runtime allocations stay away from the game's bottom-up NtAllocateVirtualMemory calls.
        const uint32_t address = g_heaps.v00000000->Alloc(size, k4KB, true, kPageReadWrite, true);
        if (address)
        {
            BeforeHostWrite(address, (size + k4KB - 1) / k4KB * k4KB);
            memset(ToHost(address), 0, (size + k4KB - 1) / k4KB * k4KB);
        }
        return address;
    }

    void SystemFree(uint32_t address)
    {
        std::lock_guard lock(HeapLock());
        g_heaps.v00000000->Release(address);
    }

    namespace
    {
        // Size classes are multiples of 16 up to 4 KB; each has a free list, refilled from 64 KB blocks.
        constexpr uint32_t kKernelGranularity = 16;
        constexpr uint32_t kKernelMaxSize = 0x1000;
        constexpr uint32_t kKernelBlockSize = 0x10000;

        struct KernelHeap
        {
            std::array<std::vector<uint32_t>, kKernelMaxSize / kKernelGranularity + 1> freeLists;
            uint32_t blockNext = 0;
            uint32_t blockEnd = 0;
        };

        KernelHeap g_kernelHeap;
    }

    uint32_t KernelAlloc(uint32_t size)
    {
        if (size == 0 || size > kKernelMaxSize)
            return 0;

        std::lock_guard lock(HeapLock());
        const uint32_t rounded = (size + kKernelGranularity - 1) / kKernelGranularity * kKernelGranularity;
        auto& freeList = g_kernelHeap.freeLists[rounded / kKernelGranularity];

        uint32_t address;
        if (!freeList.empty())
        {
            address = freeList.back();
            freeList.pop_back();
        }
        else
        {
            if (g_kernelHeap.blockEnd - g_kernelHeap.blockNext < rounded)
            {
                const uint32_t block = SystemAlloc(kKernelBlockSize);
                if (!block)
                    return 0;
                g_kernelHeap.blockNext = block;
                g_kernelHeap.blockEnd = block + kKernelBlockSize;
            }
            address = g_kernelHeap.blockNext;
            g_kernelHeap.blockNext += rounded;
        }

        BeforeHostWrite(address, rounded);
        memset(ToHost(address), 0, rounded);
        return address;
    }

    void KernelFree(uint32_t address, uint32_t size)
    {
        if (!address || size == 0 || size > kKernelMaxSize)
            return;

        std::lock_guard lock(HeapLock());
        const uint32_t rounded = (size + kKernelGranularity - 1) / kKernelGranularity * kKernelGranularity;
        g_kernelHeap.freeLists[rounded / kKernelGranularity].push_back(address);
    }

    uint32_t AllocStack(uint32_t size)
    {
        std::lock_guard lock(HeapLock());

        const uint32_t address = g_heaps.v40000000->AllocRange(0x70000000, 0x7F000000, size, k64KB, true, kPageReadWrite, true);
        if (address)
        {
            BeforeHostWrite(address, (size + k64KB - 1) / k64KB * k64KB);
            memset(ToHost(address), 0, (size + k64KB - 1) / k64KB * k64KB);
        }
        return address;
    }

    std::vector<uint32_t> CallChain(uint32_t stackPointer, int maxFrames)
    {
        std::vector<uint32_t> chain;
        uint32_t frame = stackPointer;
        for (int i = 0; i < maxFrames; i++)
        {
            // Stop on anything that isn't a plausible, readable stack address.
            if (frame < 0x10000 || frame >= 0x7F000000 || frame & 3)
                break;
            const uint32_t previous = LoadU32(frame);
            if (previous <= frame || previous - frame > 0x100000)
                break;
            const uint32_t returnAddress = LoadU32(previous - 8);
            if (returnAddress < 0x82000000 || returnAddress >= 0x84000000)
                break;
            chain.push_back(returnAddress);
            frame = previous;
        }
        return chain;
    }

    void PrintBacktrace(uint32_t stackPointer, int maxFrames)
    {
        const std::vector<uint32_t> chain = CallChain(stackPointer, maxFrames);
        for (size_t i = 0; i < chain.size(); i++)
            fprintf(stderr, "    #%zu 0x%08X\n", i, chain[i]);
    }

    bool ReserveFixed(uint32_t address, uint32_t size, uint32_t protect)
    {
        std::lock_guard lock(HeapLock());

        PageHeap* heap = LookupHeap(address);
        return heap && heap->AllocFixed(address, size, true, true, protect);
    }

    bool IsAllocated(uint32_t address)
    {
        std::lock_guard lock(HeapLock());

        const PageHeap* heap = LookupHeap(address);
        return heap && heap->IsAllocated(address);
    }
}
