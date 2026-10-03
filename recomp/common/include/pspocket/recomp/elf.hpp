#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/result.hpp"

// Big-endian ELF32 / ELF64 reader for PS3 binaries: PPU executables (ET_EXEC), PRX modules (ET_SCE_PPURELEXEC
// 0xFFA4), SPU programs (ELF32, EM_SPU). Structures follow RPCS3 rpcs3/Loader/ELF.h; the SCE-specific type
// values come from rpcs3/Crypto/unself.h.
//
// Everything is bounds-checked: a malformed file yields an Error (for the header and program header table, which
// nothing works without) or a warning (section headers, section names, symbols, which are optional), never UB.
namespace pspocket::recomp {

namespace elf {

inline constexpr u8 kClass32 = 1;
inline constexpr u8 kClass64 = 2;
inline constexpr u8 kDataMsb = 2;
inline constexpr u8 kOsAbiCellLv2 = 0x66;

// e_type
inline constexpr u16 kTypeRel = 1;
inline constexpr u16 kTypeExec = 2;
inline constexpr u16 kTypeDyn = 3;
inline constexpr u16 kTypeSceExec = 0xFE00;
inline constexpr u16 kTypeSceRelExec = 0xFE04;
inline constexpr u16 kTypeSceStubLib = 0xFE0C;
inline constexpr u16 kTypeSceDynExec = 0xFE10;
inline constexpr u16 kTypeSceDynamic = 0xFE18;
inline constexpr u16 kTypeSceIopRelExec = 0xFF80;
inline constexpr u16 kTypeSceIopRelExec2 = 0xFF81;
inline constexpr u16 kTypeSceEeRelExec = 0xFF90;
inline constexpr u16 kTypeSceEeRelExec2 = 0xFF91;
inline constexpr u16 kTypeScePspRelExec = 0xFFA0;
inline constexpr u16 kTypeScePpuRelExec = 0xFFA4; // PS3 PRX
inline constexpr u16 kTypeSceArmRelExec = 0xFFA5;
inline constexpr u16 kTypeScePspOverlay = 0xFFA8;

// e_machine
inline constexpr u16 kMachinePpc = 20;
inline constexpr u16 kMachinePpc64 = 21;
inline constexpr u16 kMachineSpu = 23;
inline constexpr u16 kMachineArm = 40;

// p_type
inline constexpr u32 kPtNull = 0;
inline constexpr u32 kPtLoad = 1;
inline constexpr u32 kPtDynamic = 2;
inline constexpr u32 kPtInterp = 3;
inline constexpr u32 kPtNote = 4;
inline constexpr u32 kPtShlib = 5;
inline constexpr u32 kPtPhdr = 6;
inline constexpr u32 kPtTls = 7;
inline constexpr u32 kPtSceRela = 0x60000000;
inline constexpr u32 kPtProcParam = 0x60000001; // sys_process_param (RPCS3 ppu_load_exec "LOOS+1")
inline constexpr u32 kPtProcPrx = 0x60000002;   // ppu_proc_prx_param_t: libent/libstub ranges ("LOOS+2")
inline constexpr u32 kPtSceDynLibData = 0x61000000;
inline constexpr u32 kPtSceProcParam = 0x61000001;
inline constexpr u32 kPtSceUnk61000010 = 0x61000010;
inline constexpr u32 kPtSceComment = 0x6FFFFF00;
inline constexpr u32 kPtSceLibVersion = 0x6FFFFF01;
inline constexpr u32 kPtSceUnk70000001 = 0x70000001;
inline constexpr u32 kPtSceIopMod = 0x70000080;
inline constexpr u32 kPtSceEeMod = 0x70000090;
inline constexpr u32 kPtScePspRela = 0x700000A0;
inline constexpr u32 kPtScePspRela2 = 0x700000A1;
inline constexpr u32 kPtScePpuRela = 0x700000A4; // PRX relocations
inline constexpr u32 kPtSceSegSym = 0x700000A8;

// p_flags
inline constexpr u32 kPfX = 0x1;
inline constexpr u32 kPfW = 0x2;
inline constexpr u32 kPfR = 0x4;
inline constexpr u32 kPfSpuX = 0x00100000;
inline constexpr u32 kPfSpuW = 0x00200000;
inline constexpr u32 kPfSpuR = 0x00400000;
inline constexpr u32 kPfRsxX = 0x01000000;
inline constexpr u32 kPfRsxW = 0x02000000;
inline constexpr u32 kPfRsxR = 0x04000000;

// sh_type
inline constexpr u32 kShtNull = 0;
inline constexpr u32 kShtProgbits = 1;
inline constexpr u32 kShtSymtab = 2;
inline constexpr u32 kShtStrtab = 3;
inline constexpr u32 kShtRela = 4;
inline constexpr u32 kShtHash = 5;
inline constexpr u32 kShtDynamic = 6;
inline constexpr u32 kShtNote = 7;
inline constexpr u32 kShtNobits = 8;
inline constexpr u32 kShtRel = 9;
inline constexpr u32 kShtDynsym = 11;
inline constexpr u32 kShtSceRela = 0x60000000;
inline constexpr u32 kShtSceNid = 0x61000001;
inline constexpr u32 kShtSceIopMod = 0x70000080;
inline constexpr u32 kShtSceEeMod = 0x70000090;
inline constexpr u32 kShtScePspRela = 0x700000A0;
inline constexpr u32 kShtScePpuRela = 0x700000A4;

// sh_flags
inline constexpr u64 kShfWrite = 0x1;
inline constexpr u64 kShfAlloc = 0x2;
inline constexpr u64 kShfExecInstr = 0x4;

inline constexpr usize kHeaderSize32 = 52;
inline constexpr usize kHeaderSize64 = 64;
inline constexpr usize kPhdrSize32 = 32;
inline constexpr usize kPhdrSize64 = 56;
inline constexpr usize kShdrSize32 = 40;
inline constexpr usize kShdrSize64 = 64;
inline constexpr usize kSymSize32 = 16;
inline constexpr usize kSymSize64 = 24;

std::string_view type_name(u16 type);
std::string_view machine_name(u16 machine);
std::string_view program_type_name(u32 type);
std::string_view section_type_name(u32 type);
// "R-X", plus " spu:R--" / " rsx:RW-" when those bits are set, plus "+0x..." for unknown bits.
std::string segment_flags_string(u32 flags);

} // namespace elf

struct ElfHeader {
    u8 elf_class = 0; // elf::kClass32 / kClass64
    u8 data = 0;      // elf::kDataMsb for big-endian
    u8 ident_version = 0;
    u8 os_abi = 0;
    u8 abi_version = 0;
    u16 type = 0;
    u16 machine = 0;
    u32 version = 0;
    u64 entry = 0;
    u64 phoff = 0;
    u64 shoff = 0;
    u32 flags = 0;
    u16 ehsize = 0;
    u16 phentsize = 0;
    u16 phnum = 0;
    u16 shentsize = 0;
    u16 shnum = 0;
    u16 shstrndx = 0;

    bool is64() const { return elf_class == elf::kClass64; }
    usize header_size() const { return is64() ? elf::kHeaderSize64 : elf::kHeaderSize32; }
    usize phdr_size() const { return is64() ? elf::kPhdrSize64 : elf::kPhdrSize32; }
    usize shdr_size() const { return is64() ? elf::kShdrSize64 : elf::kShdrSize32; }
};

struct ProgramHeader {
    u32 type = 0;
    u32 flags = 0;
    u64 offset = 0;
    u64 vaddr = 0;
    u64 paddr = 0;
    u64 filesz = 0;
    u64 memsz = 0;
    u64 align = 0;
    bool data_in_file = false; // [offset, offset + filesz) lies inside the file
};

struct SectionHeader {
    u32 name_offset = 0;
    std::string name; // empty when the section name table is missing (e.g. a decrypted SELF)
    u32 type = 0;
    u64 flags = 0;
    u64 addr = 0;
    u64 offset = 0;
    u64 size = 0;
    u32 link = 0;
    u32 info = 0;
    u64 addralign = 0;
    u64 entsize = 0;
    bool data_in_file = false; // NOBITS sections are never "in file"
};

struct Symbol {
    std::string name;
    u64 value = 0;
    u64 size = 0;
    u8 info = 0;
    u8 other = 0;
    u16 shndx = 0;

    u8 bind() const { return info >> 4; }
    u8 type() const { return info & 0xF; }
};

// Parses the identification and header fields only. Requires the ELF magic and a big-endian 32- or 64-bit
// header. Used on its own for ELF headers embedded in SELF files.
Result<ElfHeader> parse_elf_header(std::span<const u8> data);

// Reads `count` program headers of the header's class starting at data[offset]. Fails when the table is out of
// bounds or the entry size is not the standard one (as RPCS3's loader requires).
Result<std::vector<ProgramHeader>> parse_program_headers(std::span<const u8> data, const ElfHeader& header, u64 offset,
                                                         u64 file_size_for_data_check);
Result<std::vector<SectionHeader>> parse_section_headers(std::span<const u8> data, const ElfHeader& header, u64 offset,
                                                         u64 file_size_for_data_check);

struct ElfParseOptions {
    bool symbols = true; // read SYMTAB/DYNSYM entries
};

class ElfFile {
public:
    // The ElfFile refers to `data`; the caller keeps the bytes alive.
    static Result<ElfFile> parse(std::span<const u8> data, const ElfParseOptions& options = {});

    const ElfHeader& header() const { return header_; }
    std::span<const u8> data() const { return data_; }
    const std::vector<ProgramHeader>& segments() const { return segments_; }
    const std::vector<SectionHeader>& sections() const { return sections_; }
    const std::vector<Symbol>& symbols() const { return symbols_; }
    const std::vector<std::string>& warnings() const { return warnings_; }

    bool is_ppu() const { return header_.is64() && header_.machine == elf::kMachinePpc64; }
    bool is_spu() const { return !header_.is64() && header_.machine == elf::kMachineSpu; }

    // File offset of [vaddr, vaddr + size) when it lies inside one LOAD segment's file data.
    std::optional<u64> vaddr_to_offset(u64 vaddr, u64 size = 1) const;
    std::optional<std::span<const u8>> read_vaddr(u64 vaddr, u64 size) const;

    const SectionHeader* find_section(std::string_view name) const;
    // File bytes of a section or segment; empty when not in the file.
    std::span<const u8> section_data(const SectionHeader& section) const;
    std::span<const u8> segment_data(const ProgramHeader& segment) const;

    // End of the furthest byte the image claims: headers, header tables, segment file data and non-NOBITS
    // sections. May exceed data().size() for a truncated image (see extent_truncated()).
    u64 claimed_extent() const { return claimed_extent_; }
    // claimed_extent() clamped to the bytes available.
    u64 extent() const;
    bool extent_truncated() const { return claimed_extent_ > data_.size(); }

private:
    std::span<const u8> data_;
    ElfHeader header_;
    std::vector<ProgramHeader> segments_;
    std::vector<SectionHeader> sections_;
    std::vector<Symbol> symbols_;
    std::vector<std::string> warnings_;
    u64 claimed_extent_ = 0;
};

} // namespace pspocket::recomp
