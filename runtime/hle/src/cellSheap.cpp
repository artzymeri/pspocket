// cellSheap: the SPURS shared heap (memory in main storage that PPU and SPU code allocate from). Part of Sony's libsre;
// RPCS3 has no implementation (every function is UNIMPLEMENTED and returns CELL_OK) and only the error codes come from
// it (rpcs3/Emu/Cell/Modules/cellSheap.cpp). Native SPURS (D-037), assumed semantics until libsre can be checked:
//
// - cellSheapInitialize(ea, size, dmaTag): a heap over [ea, ea + size), ea 128-byte aligned. Three arguments: GTA V
//   passes a non-tag value in r6 (run 009). The heap is kept on the host; nothing is written into the guest range.
// - cellSheapAllocate / cellSheapFree: not implemented yet. GTA V imports them but has not called them in a boot log;
//   their exact signatures (return value or out pointer) are taken from the first call that does (CLAUDE.md rule 4).

#include <map>
#include <mutex>

#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellSheap";

enum : u32 {
    CELL_SHEAP_ERROR_INVAL = 0x80410302,
    CELL_SHEAP_ERROR_BUSY = 0x8041030A,
    CELL_SHEAP_ERROR_ALIGN = 0x80410310,
    CELL_SHEAP_ERROR_SHORTAGE = 0x80410312,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

struct Heap {
    u32 size = 0;
};

struct SheapState {
    std::mutex mutex;
    std::map<u32, Heap> heaps; // by guest address
};

s32 cellSheapInitialize(PPUContext&, u32 ea, u32 size, u32 dma_tag)
{
    PS_LOG_INFO(kChan, "cellSheapInitialize(ea {:#x}, size {:#x}, dmaTag {}) [native, assumed semantics]", ea, size,
        dma_tag);
    if (!ea || !size) return error(CELL_SHEAP_ERROR_INVAL);
    if (ea % 128 || size % 128) return error(CELL_SHEAP_ERROR_ALIGN);
    SheapState& s = HleProcess::current()->state<SheapState>();
    std::lock_guard lock(s.mutex);
    s.heaps[ea] = Heap{size};
    return lv2::CELL_OK;
}

} // namespace

void register_cellSheap(Registry& r)
{
    r.add("cellSheap", {
        PS_HLE_FUNC(0xBBB47CD8, cellSheapInitialize),
    });
}

} // namespace pspocket::hle
