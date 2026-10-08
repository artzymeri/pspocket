// cellSpudll: SPU shared objects (".so" SPU ELFs loaded and linked at run time by SPU code), the PPU side's two
// functions. Part of Sony's libsre; RPCS3 has the error codes and CellSpudllHandleConfig (cellSpudll.h) and a TODO for
// the size. Both are pspocket's own, read from libsre (fw 4.92) and compared with it in tests/hle/test_spurs_compare.cpp
// (D-508).

#include <format>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

enum : u32 {
    CELL_SPUDLL_ERROR_INVAL = 0x80410602,
    CELL_SPUDLL_ERROR_NULL_POINTER = 0x80410611,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

// CellSpudllHandleConfig (64 bytes): mode, dmaTag, numMaxReferred, numMaxDepend, three unresolved-symbol values, 9
// reserved words.
constexpr u32 kConfigSize = 64;

void default_config(u8* base, u32 config)
{
    for (u32 i = 0; i < kConfigSize; i += 4) ps_write32(base, config + i, 0);
    ps_write32(base, config + 8, 16);  // numMaxReferred
    ps_write32(base, config + 12, 16); // numMaxDepend
}

// cellSpudllHandleConfigSetDefaultValues(config), as libsre (0x1D6B4): NULL_POINTER, else every field 0 but the two
// maximum counts, 16 each.
s32 cellSpudllHandleConfigSetDefaultValues(PPUContext& ctx, u32 config)
{
    if (!config) return error(CELL_SPUDLL_ERROR_NULL_POINTER);
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_readable(t, config, kConfigSize)) {
        log_call_site(ctx, t.kernel.base(), std::format("cellSpudllHandleConfigSetDefaultValues: {:#x} is not guest memory; "
            "refused with NULL_POINTER", config));
        return error(CELL_SPUDLL_ERROR_NULL_POINTER);
    }
    default_config(t.kernel.base(), config);
    return lv2::CELL_OK;
}

// The span of an SPU shared object's loadable segments, as libsre measures it (0x1D564, 0x1D490): an ELF32, big-endian,
// ET_DYN, for the SPU, with 32-byte program headers; from the lowest loadable address (below 0x40000) to the highest
// end, over at most four loadable segments, with a dynamic segment at a non-zero address. INVAL otherwise (and for an
// empty span).
s32 image_span(lv2::PpuThread& t, u32 elf, u32& span)
{
    u8* base = t.kernel.base();
    if (!guest_readable(t, elf, 0x34)) return error(CELL_SPUDLL_ERROR_INVAL);
    if (ps_read32(base, elf) != 0x7F454C46 || ps_read8(base, elf + 4) != 1 || ps_read8(base, elf + 5) != 2 ||
        ps_read16(base, elf + 16) != 3 || ps_read16(base, elf + 18) != 23 || ps_read16(base, elf + 42) != 32) {
        return error(CELL_SPUDLL_ERROR_INVAL);
    }
    const u32 phnum = ps_read16(base, elf + 44), ph = elf + ps_read32(base, elf + 28);
    if (!phnum || !guest_readable(t, ph, 32 * phnum)) return error(CELL_SPUDLL_ERROR_INVAL);
    u32 lo = 0x40000, hi = 0, dynamic = 0, loads = 0;
    for (u32 i = 0; i < phnum; i++) {
        const u32 e = ph + 32 * i, type = ps_read32(base, e);
        if (type == 2) {
            dynamic = ps_read32(base, e + 8);
        } else if (type == 1) {
            if (loads++ > 3) return error(CELL_SPUDLL_ERROR_INVAL);
            const u32 vaddr = ps_read32(base, e + 8), end = vaddr + ps_read32(base, e + 20);
            lo = std::min(lo, vaddr);
            hi = std::max(hi, end);
        }
    }
    if (!dynamic || hi == lo) return error(CELL_SPUDLL_ERROR_INVAL);
    span = hi - lo;
    return lv2::CELL_OK;
}

// cellSpudllGetImageSize(psize, soElf, config), as libsre (0x1D5DC): NULL_POINTER for psize or soElf; a NULL config is
// the default one. The size is the image's span and the tables of referred and depended-on objects (4 bytes each,
// 16-aligned), each part rounded up to 128 bytes.
s32 cellSpudllGetImageSize(PPUContext& ctx, u32 psize, u32 so_elf, u32 config)
{
    if (!psize || !so_elf) return error(CELL_SPUDLL_ERROR_NULL_POINTER);
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    u32 referred = 16, depend = 16;
    if (config) {
        if (!guest_readable(t, config, 16)) {
            log_call_site(ctx, base, std::format("cellSpudllGetImageSize: config {:#x} is not guest memory; refused with "
                "NULL_POINTER", config));
            return error(CELL_SPUDLL_ERROR_NULL_POINTER);
        }
        referred = ps_read32(base, config + 8);
        depend = ps_read32(base, config + 12);
    }
    u32 span = 0;
    if (const s32 rc = image_span(t, so_elf, span); rc != lv2::CELL_OK) return rc;
    if (!guest_readable(t, psize, 4)) {
        log_call_site(ctx, base, std::format("cellSpudllGetImageSize: psize {:#x} is not guest memory; refused with "
            "NULL_POINTER", psize));
        return error(CELL_SPUDLL_ERROR_NULL_POINTER);
    }
    const u32 tables = (((referred << 2) + 15) & ~15u) + 127 + (((depend << 2) + 15) & ~15u);
    ps_write32(base, psize, ((span + 127) & ~127u) + (tables & ~127u));
    return lv2::CELL_OK;
}

} // namespace

void register_cellSpudll(Registry& r)
{
    r.add("cellSpudll", {
        PS_HLE_FUNC(0xCCCD3257, cellSpudllGetImageSize),
        PS_HLE_FUNC(0x3FBCF1D6, cellSpudllHandleConfigSetDefaultValues),
    });
}

} // namespace pspocket::hle
