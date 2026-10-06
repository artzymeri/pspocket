// PRX placement and relocation as data (D-178). Reference: RPCS3 rpcs3/Emu/Cell/PPUModule.cpp ppu_load_prx (the
// relocation entries and types); the loader's place() (runtime/loader/src/lle.cpp) copies the result into guest memory.

#include "pspocket/recomp/prx_place.hpp"

#include <algorithm>
#include <cstring>
#include <format>

#include "pspocket/core/be.h"

namespace pspocket::recomp {
namespace {

constexpr u32 kSceRelocations = 0x700000A4; // PT_SCE_PPURELA

u64 align_up(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }

} // namespace

std::vector<u32> prx_load_sizes(const ElfFile& elf)
{
    std::vector<u32> out;
    for (const ProgramHeader& ph : elf.segments())
        if (ph.type == 1) out.push_back(static_cast<u32>(ph.memsz));
    return out;
}

std::vector<std::vector<u32>> prx_fixed_layout(std::span<const std::vector<u32>> sizes, u32 base, u32 size)
{
    std::vector<std::vector<u32>> out;
    out.reserve(sizes.size());
    u64 cursor = base;
    for (const std::vector<u32>& module : sizes) {
        std::vector<u32> addrs;
        for (const u32 memsz : module) {
            if (!memsz) {
                addrs.push_back(0);
                continue;
            }
            const u64 addr = align_up(cursor, 0x10000);
            cursor = addr + memsz;
            if (cursor > u64{base} + size) return {};
            addrs.push_back(static_cast<u32>(addr));
        }
        out.push_back(std::move(addrs));
    }
    return out;
}

Result<PlacedPrx> place_prx(const ElfFile& elf, std::span<const u8> bytes, std::span<const u32> addrs)
{
    PlacedPrx out;
    usize index = 0;
    for (const ProgramHeader& ph : elf.segments()) {
        if (ph.type != 1) continue;
        if (index >= addrs.size()) return make_error("{} PT_LOADs, {} addresses", index + 1, addrs.size());
        PlacedSegment seg;
        seg.memsz = static_cast<u32>(ph.memsz);
        seg.executable = (ph.flags & 1) != 0;
        if (ph.memsz) {
            if (ph.filesz > ph.memsz || (ph.filesz && !ph.data_in_file))
                return make_error("segment {} lies outside the file", index);
            if (!addrs[index]) return make_error("segment {} has no address", index);
            seg.addr = addrs[index];
            seg.bytes.assign(static_cast<usize>(align_up(ph.memsz, 0x100)) + 8, 0); // + a relocation's width
            std::memcpy(seg.bytes.data(), bytes.data() + ph.offset, static_cast<usize>(ph.filesz));
        }
        out.segments.push_back(std::move(seg));
        index++;
    }
    if (out.segments.empty() || !out.segments[0].addr) return make_error("no first segment");

    // {u64 offset, u16, u8 index of the value's segment (0xFF: none), u8 index of the address's segment, u32 type,
    // u64 ptr} (ppu_prx_relocation_info).
    for (const ProgramHeader& ph : elf.segments()) {
        if (ph.type != kSceRelocations || !ph.data_in_file) continue;
        for (u64 i = 0; i + 24 <= ph.filesz; i += 24) {
            const u8* r = bytes.data() + ph.offset + i;
            const u64 offset = ps_ld64(r);
            const u8 index_value = r[10], index_addr = r[11];
            const u32 type = ps_ld32(r + 12);
            const u64 ptr = ps_ld64(r + 16);
            const auto& segs = out.segments;
            if (index_addr >= segs.size() || !segs[index_addr].addr || offset >= segs[index_addr].bytes.size() - 8 ||
                (index_value != 0xFF && (index_value >= segs.size() || !segs[index_value].addr))) {
                out.errors.push_back(std::format("relocation {} lies outside its segments", i / 24));
                continue;
            }
            u8* p = out.segments[index_addr].bytes.data() + offset;
            const u32 at = segs[index_addr].addr + static_cast<u32>(offset);
            const u64 data = (index_value == 0xFF ? 0 : segs[index_value].addr) + ptr;
            switch (type) {
            case 1: ps_st32(p, static_cast<u32>(data)); break;                                         // ADDR32
            case 4: ps_st16(p, static_cast<u16>(data)); break;                                         // ADDR16_LO
            case 5: ps_st16(p, static_cast<u16>(data >> 16)); break;                                   // ADDR16_HI
            case 6: ps_st16(p, static_cast<u16>((data >> 16) + ((data & 0x8000) ? 1 : 0))); break;     // ADDR16_HA
            case 10: ps_st32(p, (ps_ld32(p) & ~0x03FFFFFCu) | (static_cast<u32>(data - at) & 0x03FFFFFCu)); break; // REL24
            case 11: ps_st32(p, (ps_ld32(p) & ~0x0000FFFCu) | (static_cast<u32>(data - at) & 0x0000FFFCu)); break; // REL14
            case 38: ps_st64(p, data); break;                                                          // ADDR64
            case 44: ps_st64(p, data - at); break;                                                     // REL64
            case 57: ps_st16(p, static_cast<u16>((ps_ld16(p) & 3u) | (static_cast<u16>(data) & 0xFFFCu))); break; // LO_DS
            default:
                out.errors.push_back(std::format("relocation type {} at {:#x} is unknown", type, at));
                continue;
            }
            out.relocations++;
        }
    }
    return out;
}

} // namespace pspocket::recomp
