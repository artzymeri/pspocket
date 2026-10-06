// cellOvis: SPU overlay tables, a library of libsre, which pspocket serves itself (D-037). An SPU ELF that keeps code
// in overlays places those segments at physical addresses past the 256 KB local store; the overlay table, in main
// memory, holds them, and the SPU program's overlay manager copies them in when called. Sony's libvdec sets up its
// H.264 decoder's SPU tasks this way (D-195).
// RPCS3 has the four functions as TODOs (rpcs3/Emu/Cell/Modules/cellOvis.cpp, which RPCS3 leaves to libsre), so
// everything here follows Sony's libsre (fw 4.92, read as the reference, D-046; never copied): GetOverlayTableSize
// 0x6EA0, InitializeOverlayTable 0x6F8C, FixSpuSegments 0x6E90, InvalidateOverlappedSegments 0x6D48.
//
// Where libsre would read a pointer that is not guest memory, pspocket logs the call site and refuses the call with
// 0x8041040D (the FAULT of libsre's other libraries), or for the two functions without a result leaves everything as
// it is (D-042).

#include <cstring>
#include <format>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellOvis";

enum : u32 {
    CELL_OVIS_ERROR_INVAL = 0x80410402,
    CELL_OVIS_ERROR_FAULT = 0x8041040D,
    CELL_OVIS_ERROR_ALIGN = 0x80410416, // libsre's InitializeOverlayTable (RPCS3 names 0x80410410)
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr u32 kLocalStore = 0x40000;
constexpr u32 PT_LOAD = 1;
constexpr s32 SYS_SPU_SEGMENT_TYPE_INFO = 4;
constexpr u32 kSegmentSize = 0x18; // sys_spu_segment: type, ls, size, (+0xC), the address or value at +0x10

bool readable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (addr && guest_readable(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::format("cellOvis: {} ({:#x}, {} bytes) is not guest memory", what, addr, size));
    return false;
}

// libsre's ELF check: the magic and big-endian data (EI_DATA 2); the class is not looked at.
bool is_spu_elf(const u8* base, u32 elf) { return ps_read32(const_cast<u8*>(base), elf) == 0x7F454C46 && base[elf + 5] == 2; }

// The program headers of a 32-bit ELF, as libsre walks them (e_phoff at 28, e_phentsize at 42, e_phnum at 44).
bool headers_readable(PPUContext& ctx, u32 elf, std::string_view what)
{
    if (!readable(ctx, elf, 0x34, what)) return false;
    u8* base = guest_base(ctx);
    const u32 phoff = ps_read32(base, elf + 28), phentsize = ps_read16(base, elf + 42), phnum = ps_read16(base, elf + 44);
    return phnum == 0 || readable(ctx, elf + phoff, phentsize * (phnum - 1) + 0x20, "the ELF's program headers");
}

} // namespace

// cellOvisGetOverlayTableSize(elf): INVAL unless an ELF; else how far the loaded segments with data reach past the
// local store (the largest p_paddr + p_filesz of a PT_LOAD with p_filesz, minus 256 KB), 0 when none does.
s32 cellOvisGetOverlayTableSize(PPUContext& ctx, u32 elf)
{
    if (!headers_readable(ctx, elf, "the ELF")) return error(CELL_OVIS_ERROR_FAULT);
    u8* base = guest_base(ctx);
    if (!is_spu_elf(base, elf)) return error(CELL_OVIS_ERROR_INVAL);
    const u32 phoff = ps_read32(base, elf + 28), phentsize = ps_read16(base, elf + 42), phnum = ps_read16(base, elf + 44);
    s32 end = 0;
    for (u32 i = 0; i < phnum; i++) {
        const u32 ph = elf + phoff + i * phentsize;
        if (ps_read32(base, ph) != PT_LOAD) continue;
        const u32 filesz = ps_read32(base, ph + 16);
        const s32 seg_end = static_cast<s32>(ps_read32(base, ph + 12) + filesz);
        if (filesz && end < seg_end) end = seg_end; // a signed comparison, as libsre's
    }
    const s32 size = end <= static_cast<s32>(kLocalStore - 1) ? 0 : end - static_cast<s32>(kLocalStore);
    PS_LOG_DEBUG(kChan, "cellOvisGetOverlayTableSize({:#x}): {:#x}", elf, static_cast<u32>(size));
    return size;
}

// cellOvisInitializeOverlayTable(table, elf): the table 128-aligned (else 0x80410416), the ELF checked (INVAL); then
// every PT_LOAD placed past the local store is copied from the ELF to table + p_paddr - 256 KB.
s32 cellOvisInitializeOverlayTable(PPUContext& ctx, u32 table, u32 elf)
{
    if (table & 0x7F) return error(CELL_OVIS_ERROR_ALIGN);
    if (!headers_readable(ctx, elf, "the ELF")) return error(CELL_OVIS_ERROR_FAULT);
    u8* base = guest_base(ctx);
    if (!is_spu_elf(base, elf)) return error(CELL_OVIS_ERROR_INVAL);
    const u32 phoff = ps_read32(base, elf + 28), phentsize = ps_read16(base, elf + 42), phnum = ps_read16(base, elf + 44);
    for (u32 i = 0; i < phnum; i++) {
        const u32 ph = elf + phoff + i * phentsize;
        if (ps_read32(base, ph) != PT_LOAD) continue;
        const u32 paddr = ps_read32(base, ph + 12), filesz = ps_read32(base, ph + 16), offset = ps_read32(base, ph + 4);
        if (paddr < kLocalStore) continue;
        const u32 dst = table + paddr - kLocalStore, src = elf + offset;
        if (!readable(ctx, src, filesz, "an overlay segment") || !readable(ctx, dst, filesz, "the overlay table"))
            return error(CELL_OVIS_ERROR_FAULT);
        std::memmove(base + dst, base + src, filesz);
    }
    PS_LOG_DEBUG(kChan, "cellOvisInitializeOverlayTable({:#x}, {:#x})", table, elf);
    return lv2::CELL_OK;
}

// cellOvisInvalidateOverlappedSegments(segs, nsegs): each segment, from the second on, is compared with the ones
// before it; one that overlaps an earlier one in the local store (INFO segments never do) swaps places with the last
// counted segment and is no longer counted, and the comparison starts over. libsre's swap moves the type, ls, size and
// the address doubleword at +0x10, not the word at +0xC.
void cellOvisInvalidateOverlappedSegments(PPUContext& ctx, u32 segs, u32 nsegs)
{
    if (!readable(ctx, nsegs, 4, "the segment count")) return;
    u8* base = guest_base(ctx);
    s32 n = static_cast<s32>(ps_read32(base, nsegs));
    if (n > 0 && !readable(ctx, segs, static_cast<u32>(n) * kSegmentSize, "the segments")) return;
    const auto at = [&](s32 k) { return segs + static_cast<u32>(k) * kSegmentSize; };
    for (s32 i = 1; i < n; i++) {
        for (s32 j = 0; j < i && j < n && i < n;) {
            const u32 si = at(i), sj = at(j);
            j++;
            const s32 ti = static_cast<s32>(ps_read32(base, si)), tj = static_cast<s32>(ps_read32(base, sj));
            if (ti == SYS_SPU_SEGMENT_TYPE_INFO || tj == SYS_SPU_SEGMENT_TYPE_INFO) continue;
            const u32 ls_i = ps_read32(base, si + 4), ls_j = ps_read32(base, sj + 4);
            if (ls_j + ps_read32(base, sj + 8) <= ls_i || ls_i + ps_read32(base, si + 8) <= ls_j) continue;
            const u32 last = at(n - 1);
            for (const u32 off : {0u, 4u, 8u}) {
                const u32 v = ps_read32(base, si + off);
                ps_write32(base, si + off, ps_read32(base, last + off));
                ps_write32(base, last + off, v);
            }
            const u64 v = ps_read64(base, si + 0x10);
            ps_write64(base, si + 0x10, ps_read64(base, last + 0x10));
            ps_write64(base, last + 0x10, v);
            n--;
            j = 0;
        }
    }
    ps_write32(base, nsegs, static_cast<u32>(n));
}

// cellOvisFixSpuSegments(image): the overlapping segments of a sys_spu_image (segs at +8, nsegs at +0xC) invalidated.
void cellOvisFixSpuSegments(PPUContext& ctx, u32 image)
{
    if (!readable(ctx, image, 0x10, "the SPU image")) return;
    cellOvisInvalidateOverlappedSegments(ctx, ps_read32(guest_base(ctx), image + 8), image + 0xC);
}

void register_cellOvis(Registry& r)
{
    r.add("cellOvis", {
        PS_HLE_FUNC(0x82F294B2, cellOvisGetOverlayTableSize),
        PS_HLE_FUNC(0xA876C911, cellOvisInitializeOverlayTable),
        PS_HLE_FUNC(0xCE6CB776, cellOvisFixSpuSegments),
        PS_HLE_FUNC(0x629BA0C0, cellOvisInvalidateOverlappedSegments),
    });
}

} // namespace pspocket::hle
