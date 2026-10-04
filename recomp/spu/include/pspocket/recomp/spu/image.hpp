#pragma once

// Input of the SPU analyzer and lifter: a local-store image (256 KB), the LS range that holds code (the program key
// covers it: ps_spu_hash64 over [code_start, code_end)), and the entry points.
//
// Sources: an SPU ELF (PT_LOAD segments placed at p_vaddr, through recomp/common's ELF parser) or a raw image loaded
// at a base address.

#include <span>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/result.hpp"

namespace pspocket::recomp::spu {

struct LsImage {
    std::vector<u8> ls = std::vector<u8>(0x40000, 0); // big-endian guest bytes
    u32 code_start = 0;
    u32 code_end = 0; // exclusive
    std::vector<u32> entries;
    // Addresses execution entered from outside lifted code (spu-lift --seed: misses of earlier runs, module entries).
    // One inside a function the entries reach becomes a label of it; only the others start functions (analyzer.hpp).
    std::vector<u32> seeds;
    std::string name;

    u32 code_size() const { return code_end - code_start; }
    bool in_code(u32 addr) const { return addr >= code_start && addr < code_end && (addr & 3) == 0; }
    u32 word(u32 addr) const;
    u64 hash() const; // ps_spu_hash64 of the code range: the runtime program key
};

// SPU ELF (ELF32, big-endian, EM_SPU). Segments must fit in the LS; the code range spans the executable PT_LOAD
// segments; the entry point is e_entry.
Result<LsImage> load_spu_elf(std::span<const u8> data, std::string name = {});

// Raw image: bytes at LS address base, code range = the image, entries as given.
Result<LsImage> load_raw_image(std::span<const u8> bytes, u32 base, std::vector<u32> entries, std::string name = {});

} // namespace pspocket::recomp::spu
