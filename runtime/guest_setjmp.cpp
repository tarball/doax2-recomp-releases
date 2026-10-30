// The host jmp_buf for each guest setjmp buffer (PPC_JMP_BUF, ppc_overrides.h): the host's is larger than the guest's
// and needs 16-byte alignment, so the guest's buffer stays the guest's and the host's lives here, one per guest address,
// for as long as the game runs. XenonRecomp's default keeps this table in the generated code, which would need the C++
// standard library there; the game's code module is built without it (ANALYSIS.md, Phase 5, The game's code module).
#include <csetjmp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

extern "C" void* PPCJmpBuf(uint32_t guestBuffer)
{
    struct Buffer
    {
        jmp_buf value;
    };
    static std::mutex lock;
    static std::unordered_map<uint32_t, std::unique_ptr<Buffer>> buffers;
    std::lock_guard guard(lock);
    std::unique_ptr<Buffer>& buffer = buffers[guestBuffer];
    if (!buffer)
        buffer = std::make_unique<Buffer>();
    return &buffer->value;
}
