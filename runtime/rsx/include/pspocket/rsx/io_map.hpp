#pragma once

// RSX IO address space: main memory the RSX can reach, mapped in 1 MB pages ("IO offsets" 0..512 MB) to guest
// effective addresses. lv2 (sys_rsx_context_iomap / iounmap) and cellGcmMapMainMemory / cellGcmMapEaIoAddress
// fill it; the RSX translates FIFO addresses, CELL_GCM_LOCATION_MAIN offsets and report/notify addresses through it.
// Semantics follow RPCS3 rpcs3/Emu/RSX/Core/RSXIOMap.hpp and sys_rsx.cpp sys_rsx_context_iomap: each entry holds the
// EA of a 1 MB page, a mapping replaces any previous mapping of that IO page and of that EA page.
//
// Thread safety: entries are atomics, so the RSX thread may translate while a PPU thread maps or unmaps.

#include <array>
#include <atomic>
#include <optional>

#include "pspocket/core/types.hpp"

namespace pspocket::rsx {

class IoMap {
public:
    static constexpr u32 kPageShift = 20;
    static constexpr u32 kPageSize = 1u << kPageShift;
    static constexpr u32 kIoPages = 512;   // 512 MB IO space (CELL_GCM_SYSTEM_MODE_IOMAP_512MB)
    static constexpr u32 kEaPages = 4096;  // 4 GB / 1 MB
    static constexpr u32 kUnmapped = 0xFFFFFFFFu;

    IoMap();

    // Maps [io, io + size) to [ea, ea + size). io, ea and size must be 1 MB aligned and within range; returns false
    // (and changes nothing) otherwise.
    bool map(u32 io, u32 ea, u32 size);
    bool unmap(u32 io, u32 size);
    void clear();

    // IO offset -> guest EA, or nullopt when the page is unmapped.
    // Inline: the FIFO reader translates every command word's address (3% of the RSX thread out of line, run 221).
    std::optional<u32> io_to_ea(u32 io) const
    {
        const u32 page = io >> kPageShift;
        if (page >= kIoPages) return std::nullopt;
        const u32 ea = io_to_ea_[page].load(std::memory_order_acquire);
        if (ea == kUnmapped) return std::nullopt;
        return ea | (io & (kPageSize - 1));
    }
    // Guest EA -> IO offset, or nullopt.
    std::optional<u32> ea_to_io(u32 ea) const;

    // True when every 1 MB page touched by [io, io + size) is mapped (not necessarily contiguous in EA space).
    bool is_mapped(u32 io, u32 size) const;

private:
    std::array<std::atomic<u32>, kIoPages> io_to_ea_{}; // EA of the page (page aligned), or kUnmapped
    std::array<std::atomic<u32>, kEaPages> ea_to_io_{}; // IO offset of the page, or kUnmapped
};

} // namespace pspocket::rsx
