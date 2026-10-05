#pragma once

// Synthetic PS3 binaries built in code (CLAUDE.md rule 1: no game data in tests). They follow the layouts the
// loader reads (RPCS3 rpcs3/Emu/Cell/PPUModule.cpp, rpcs3/Loader/ELF.h, rpcs3/Crypto/unself.h) closely enough to
// exercise every path of recomp/common, and record what a correct parser must find.

#include <span>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::test {

// Growable big-endian byte buffer.
class Buffer {
public:
    std::vector<u8> bytes;

    void reserve_to(usize size);
    void put8(usize offset, u8 v);
    void put16(usize offset, u16 v);
    void put32(usize offset, u32 v);
    void put64(usize offset, u64 v);
    void put(usize offset, std::span<const u8> data);
    void put_str(usize offset, std::string_view text); // with a terminating NUL
};

// ---- SPU ELF ----

struct SpuElfSpec {
    std::string spuname;        // PT_NOTE "SPUNAME" content; empty = no note
    std::string data_text;      // text placed in the data segment (middleware signatures go here)
    u32 entry = 0;
    bool sections = true;       // section headers (+ .shstrtab) at the end of the image
};

struct SyntheticSpuElf {
    std::vector<u8> bytes;
    u64 extent = 0; // where the image ends (== bytes.size())
};

SyntheticSpuElf build_spu_elf(const SpuElfSpec& spec);

// ---- PPU executable ----

struct PpuExecOptions {
    bool sections = true;      // section header table
    bool section_names = true; // .shstrtab referenced by e_shstrndx (false: e_shstrndx = 0)
    bool symbols = true;       // .symtab with _start, main and the _binary_..._start symbol of SPU image 2
};

struct ExpectedImport {
    std::string module;
    std::vector<std::string> functions; // names whose NIDs are imported (resolve exactly)
    std::vector<u32> raw_function_nids; // NIDs imported verbatim (unknown or from another module)
    std::vector<std::string> variables; // explicit-NID names (see kVarNids)
    std::vector<u32> variable_nids;
    u16 attributes = 0;
};

struct SyntheticPpuExec {
    std::vector<u8> bytes;

    // Layout facts the parser must recover.
    u32 entry = 0;            // e_entry: the _start descriptor
    u32 entry_func = 0;       // _start's code
    u32 toc = 0;
    u32 opd_start = 0;
    u32 opd_entries = 0;
    u32 opd_distinct = 0;
    u32 libent_start = 0, libent_end = 0, libstub_start = 0, libstub_end = 0;
    u32 sdk_version = 0;
    s32 primary_prio = 0;
    u32 primary_stacksize = 0;
    u32 malloc_pagesize = 0;
    u32 tls_vaddr = 0, tls_filesz = 0, tls_memsz = 0;
    std::vector<ExpectedImport> imports;
    u32 first_stub_slot = 0;  // stub table entry of the first imported function
    u32 first_stub_code = 0;  // its initial value
    u32 spurs_refs = 0;       // reference table pointer of cellSpurs' function (attribute 0x2000)
    u32 var_ref_table = 0;    // sys_libc variable reference table
    u32 var_ref_target = 0;   // the address it patches
    u32 module_start_descriptor = 0;
    u32 export_func_descriptor = 0;
    u32 export_func_code = 0;
    u32 export_var_addr = 0;

    // Embedded SPU images (file offsets) and their bytes.
    std::vector<u64> spu_offsets;
    std::vector<std::vector<u8>> spu_images;
    std::vector<u32> spu_guest_addrs;
};

SyntheticPpuExec build_ppu_exec(const PpuExecOptions& options = {});

// ---- PPU PRX ----

struct SyntheticPrx {
    std::vector<u8> bytes;
    u32 module_info_vaddr = 0;
    u32 toc = 0;
    u32 exports_start = 0, exports_end = 0, imports_start = 0, imports_end = 0;
    u32 module_start_descriptor = 0;
    u32 module_start_code = 0;
    u32 prx_func_code = 0;
    u32 stub_slot0 = 0;   // relocated stub table entry
    u32 stub_code0 = 0;   // its relocated value
    u32 lis_insn_addr = 0;   // lis r12, slot@ha (ADDR16_HA)
    u32 lwz_insn_addr = 0;   // lwz r12, slot@l(r12) (ADDR16_LO)
    u32 relocations = 0;     // entries in the PT_SCE_PPURELA segment
    u32 unsupported_relocations = 0;
    u32 out_of_range_relocations = 0;
};

struct PrxOptions {
    // synthPrxLib also exports a variable (kPrxVariableNid at segment 1 + 0x60, holding kPrxVariableValue), and the
    // module imports that NID from library "synthVarLib" through a reference table that patches segment 1 + 0xC0.
    bool variables = false;
};
inline constexpr u32 kPrxVariableNid = 0x5EED0001;
inline constexpr u32 kPrxVariableValue = 0xC0FFEE01;

SyntheticPrx build_ppu_prx(const PrxOptions& options = {});

// ---- SELF wrapper ----

struct SelfOptions {
    bool debug = false;     // key revision 0x8000: the ELF is stored in plain text after the headers
    bool npdrm = true;      // add an NPDRM control info (supplemental header type 3)
    s32 npdrm_license = 3;  // 3 = free
    u32 self_type = 8;      // app info program type (8 = NPDRM application)
};

// Wraps an ELF in a fake SCE container. The header fields are real; for a retail-style (non-debug) SELF the
// "encrypted" metadata and segment data are pseudo-random filler. No keys, no encryption.
std::vector<u8> wrap_in_self(std::span<const u8> elf, const SelfOptions& options = {});

inline constexpr std::string_view kSelfContentId = "XX0000-TEST00000_00-0000000000000000";

} // namespace pspocket::test
