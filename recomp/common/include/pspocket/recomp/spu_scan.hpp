#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/hash.hpp"

// Embedded SPU ELF images (plan section 6, "Finding SPU code", static part).
//
// PPU executables and PRX modules carry SPU programs as complete ELF32 images (ELFCLASS32, ELFDATA2MSB,
// e_machine 23) inside their segments. The scan finds every "\x7fELF" in a buffer that parses as such an image and
// measures it: extent (headers, program and section header tables, segment and section data), SHA-1 of those
// bytes, entry point, segments and a best-effort name.
//
// Two hashes are reported. `sha1` covers the image bytes and is the key for the game manifest's SPU table.
// `rpcs3_hash` is RPCS3's "SPU-<sha1>" patch hash (rpcs3/Emu/Cell/PPUModule.cpp, ppu_check_patch_spu_images:
// p_vaddr, p_memsz, p_filesz of every program header, plus LOAD and NOTE data), so images can be matched with
// RPCS3 logs and patch files.
//
// SPU code that is not stored as an ELF (raw SPURS job binaries, "roaming" code RPCS3 finds by instruction
// patterns) is not found here; the runtime traces cover it (docs/phase0.md).
namespace pspocket::recomp {

struct SpuImage {
    u64 offset = 0;              // offset in the scanned buffer's coordinates (file offset for scan_spu_images)
    std::optional<u32> guest_addr; // address in the containing PPU image, when inside a LOAD segment
    std::optional<u32> container_segment; // program header index of the containing PPU segment
    u64 size = 0;                // extent in bytes, clamped to the bytes available
    bool truncated = false;      // the image claims more bytes than are available
    Sha1Digest sha1{};           // SHA-1 of [offset, offset + size)
    std::optional<Sha1Digest> rpcs3_hash; // unavailable when program data is missing
    u16 type = 0;
    u32 entry = 0;
    u16 phnum = 0;
    u16 shnum = 0;
    u64 load_bytes = 0;          // sum of LOAD p_filesz
    std::vector<ProgramHeader> segments;
    std::string name;
    std::string name_source;     // "SPUNAME note", "symbol", "section", "nearby string" or empty
    std::vector<std::string> warnings;
};

// Scans every PT_LOAD segment's file data of a PPU ELF. Offsets are file offsets; guest_addr is set.
std::vector<SpuImage> scan_spu_images(const ElfFile& ppu);

// Scans a raw buffer; offsets are relative to `data` plus `base_offset`.
std::vector<SpuImage> scan_spu_images(std::span<const u8> data, u64 base_offset = 0);

// Measures a single SPU ELF that starts at data[0] (e.g. a standalone SPU program file).
std::optional<SpuImage> measure_spu_image(std::span<const u8> data, u64 offset = 0);

// "SPU-<hex>" as RPCS3 prints it.
std::string rpcs3_spu_hash_string(const Sha1Digest& digest);

} // namespace pspocket::recomp
