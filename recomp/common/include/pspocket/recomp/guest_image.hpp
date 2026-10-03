#pragma once

#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/result.hpp"

// A static view of a PPU ELF's LOAD segments in the 32-bit guest address space, for reading module metadata
// (process parameters, import and export tables, function descriptors) the way the loader would see them.
//
// Every LOAD segment is placed at its p_vaddr. Executables are absolute, so that is where lv2 maps them. PRX
// modules are relocatable: RPCS3 allocates each segment separately and applies the PT_SCE_PPURELA relocations
// (rpcs3/Emu/Cell/PPUModule.cpp, ppu_load_prx). Here the segments keep their p_vaddr ("identity" placement) and
// the same relocations are applied with those bases, so every pointer in the image is consistent with p_vaddr.
//
// Addresses are u64 so callers can pass `table + 4 * i` without wrapping; anything above 4 GB simply isn't mapped.
namespace pspocket::recomp {

class GuestImage {
public:
    struct Segment {
        u32 vaddr = 0;
        u64 memsz = 0;        // vaddr + memsz <= 4 GB
        u32 flags = 0;
        u32 phdr_index = 0;  // index in the ELF program header table
        u32 load_index = 0;  // index among LOAD segments (what PRX relocations refer to)
        u64 file_offset = 0;
        u64 file_size = 0;    // p_filesz
        std::vector<u8> data; // p_filesz bytes (more after relocation writes into bss); the rest of memsz reads as zero
    };

    struct RelocationSummary {
        u32 total = 0;
        u32 applied = 0;
        u32 unsupported = 0;    // unknown relocation types (counted per type in by_type)
        u32 out_of_range = 0;   // bad segment index, offset outside the segment, or target not writable
        std::map<u32, u32> by_type;
    };

    // Fails when a LOAD segment does not fit the 32-bit address space or its file data is missing.
    static Result<GuestImage> from_elf(const ElfFile& elf);

    const std::vector<Segment>& segments() const { return segments_; }
    const std::vector<std::string>& warnings() const { return warnings_; }
    const RelocationSummary& relocations() const { return relocations_; }

    // The segment containing all of [addr, addr + size) within its memsz, if any.
    const Segment* find(u64 addr, u64 size = 1) const;
    bool readable(u64 addr, u64 size) const { return find(addr, size) != nullptr; }
    // Inside a segment with PF_X (or, when no segment is marked executable, the first LOAD segment).
    bool executable(u64 addr) const;

    bool read_bytes(u64 addr, std::span<u8> out) const;
    std::optional<u8> read8(u64 addr) const;
    std::optional<u16> read16(u64 addr) const;
    std::optional<u32> read32(u64 addr) const;
    std::optional<u64> read64(u64 addr) const;
    // NUL-terminated string at addr, at most max_length characters; nullopt if unreadable or unterminated.
    std::optional<std::string> read_cstring(u64 addr, usize max_length) const;

    // Writes into segment data (materializing bss up to memsz). Used by relocation.
    bool write_bytes(u64 addr, std::span<const u8> in);
    bool write16(u64 addr, u16 value);
    bool write32(u64 addr, u32 value);
    bool write64(u64 addr, u64 value);

private:
    Segment* find_mut(u64 addr, u64 size);
    void apply_prx_relocations(const ElfFile& elf);

    std::vector<Segment> segments_;
    std::vector<std::string> warnings_;
    RelocationSummary relocations_;
    bool any_executable_ = false;
};

} // namespace pspocket::recomp
