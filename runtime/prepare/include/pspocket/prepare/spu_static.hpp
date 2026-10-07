#pragma once

// SPU programs found by their ELF headers in code nothing has run yet: the firmware's modules for the system library
// (D-191) and a game's executable (D-226). Each program is saved as the runtime saves an image it meets (hle
// spu_programs.cpp: spu_<hash>_<ls>.bin and .txt), so its name is the one the runtime looks up, and lifted with the
// images the game's runs saved (lift_spu_images).

#include <cstddef>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/elf.hpp"

namespace pspocket::prepare {

struct SpuStaticCount {
    size_t images = 0, programs = 0, overlays = 0, skipped = 0;
};

// Bytes as they sit in guest memory from `addr`.
struct SpuSegment {
    u32 addr = 0;
    std::span<const u8> bytes;
};

struct SpuStaticOptions {
    bool save = true; // false: only tell whether there are any
    // An image entered below 0x3000 cannot be a SPURS task (SPURS refuses such an entry) and is position independent
    // code a program copies in (Assassin's Creed's mstream_dsp_reverb.pic at 0x80): saved as a module, lifted
    // relocatable, found at any local store address. Off for the system library, whose output stays as it was.
    bool low_entry_relocatable = false;
    std::string_view who = "system library"; // for the log
};

// Every SPU ELF in `segments`: the executable PT_LOAD holding its entry is the program a task or thread runs (SPURS's
// task start and sys_spu_thread_group_start identify that segment at its vaddr); its other executable PT_LOADs are
// overlays, each a program at its vaddr, entered at its start. `source` names where they came from. Whether there was
// any SPU ELF.
bool save_spu_images(std::span<const SpuSegment> segments, std::string_view source, const std::filesystem::path& out,
    const SpuStaticOptions& options, SpuStaticCount& count);

// An executable's PT_LOADs with their file bytes, at their vaddr.
std::vector<SpuSegment> executable_segments(const recomp::ElfFile& elf);

} // namespace pspocket::prepare
