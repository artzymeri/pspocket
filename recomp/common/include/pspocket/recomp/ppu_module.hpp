#pragma once

#include <optional>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/guest_image.hpp"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/recomp/result.hpp"

// PPU module metadata: what lv2 reads from an executable or PRX before running it.
//
// Executables: PT_PROC_PARAM (sys_process_param) and PT_PROC_PRX (ppu_proc_prx_param_t), whose libent / libstub
// ranges hold the export and import library tables. PRX modules: the module info structure, located at file offset
// p_paddr of the first program header, whose export / import ranges hold the same library tables.
// Structures and semantics: RPCS3 rpcs3/Emu/Cell/PPUModule.cpp (ppu_load_exec, ppu_load_prx, ppu_load_exports,
// ppu_load_imports, ppu_prx_module_info) and rpcs3/Emu/Cell/lv2/sys_process.h.
namespace pspocket::recomp {

inline constexpr u32 kProcessParamMagic = 0x13BCC5F6; // SYS_PROCESS_PARAM_MAGIC
inline constexpr u32 kProcPrxParamMagic = 0x1B434CEC;

// Special NIDs of the nameless module-management export library (RPCS3 ppu_load_prx / ppu_get_function_name).
inline constexpr u32 kNidModuleStart = 0xBC9A0086;
inline constexpr u32 kNidModuleStop = 0xAB779874;
inline constexpr u32 kNidModuleExit = 0x3AB9A95E;
inline constexpr u32 kNidModulePrologue = 0x0D10FD3F;
inline constexpr u32 kNidModuleEpilogue = 0x330F7005;

// Library attribute bits (RPCS3 PRX_EXPORT_ATTRIBUTES, ppu_load_imports).
inline constexpr u16 kLibAttrLibrary = 0x0001;    // a named library
inline constexpr u16 kLibAttrFuncRefs = 0x2000;   // imports: per-function reference tables follow the NID table
inline constexpr u16 kLibAttrManagement = 0x8000; // exports: module_start / module_stop / ...

struct ProcessParam {
    u64 vaddr = 0;
    u32 size = 0;
    u32 magic = 0;
    u32 version = 0;
    u32 sdk_version = 0;
    s32 primary_prio = 0;
    u32 primary_stacksize = 0;
    u32 malloc_pagesize = 0;
    u32 ppc_seg = 0;
    std::optional<u32> crash_dump_param_addr; // present when size >= 0x24

    bool magic_ok() const { return magic == kProcessParamMagic; }
};

struct ProcPrxParam {
    u64 vaddr = 0;
    u32 size = 0;
    u32 magic = 0;
    u32 version = 0;
    u32 unk0 = 0;
    u32 libent_start = 0;
    u32 libent_end = 0;
    u32 libstub_start = 0;
    u32 libstub_end = 0;
    u16 ver = 0;
    u16 unk1 = 0;
    u32 unk2 = 0;

    bool magic_ok() const { return magic == kProcPrxParamMagic; }
};

// PRX module info (RPCS3 ppu_prx_library_info).
struct PrxModuleInfo {
    u32 vaddr = 0;
    u64 file_offset = 0;
    u16 attributes = 0;
    u8 version_major = 0;
    u8 version_minor = 0;
    std::string name;
    u32 toc = 0;
    u32 exports_start = 0;
    u32 exports_end = 0;
    u32 imports_start = 0;
    u32 imports_end = 0;
    std::string located_by; // "first phdr p_paddr" or "section .rodata.sceModuleInfo"
};

struct TlsInfo {
    u32 vaddr = 0;
    u32 image_size = 0; // p_filesz: initialized TLS image
    u32 size = 0;       // p_memsz: per-thread TLS size
    u32 align = 0;
};

// An entry of a reference table (RPCS3 ppu_patch_refs ref_t): the loader patches `addr` with the resolved value.
struct RefEntry {
    u32 type = 0;
    u32 addr = 0;
    u32 addend = 0;
};

struct ResolvedName {
    std::string name; // empty when unknown
    NidMatch match = NidMatch::Unknown;
};

struct ImportedFunction {
    u32 nid = 0;
    u32 slot = 0;              // address of this function's entry in the stub table (patched with the descriptor)
    u32 stub = 0;              // initial value of that entry (RPCS3 records it as the import's use address; in SDK
                               // binaries it points at the .sceStub.text stub that calls through the slot)
    std::optional<u32> refs;   // reference table, when the library has attribute 0x2000
    ResolvedName resolved;
};

struct ImportedVariable {
    u32 nid = 0;
    u32 ref_table = 0;          // address of the reference table listing the locations to patch
    std::vector<RefEntry> refs; // decoded, up to the terminating type-0 entry
    bool refs_unreadable = false;
    ResolvedName resolved;
};

// TLS variable imports: count from the library header; the tables are read best-effort from the two words after
// the variable stub table (RPCS3 calls them unk4/unk5 and does not parse them, logging "Unexpected num_tlsvar").
struct ImportedTlsVariable {
    u32 nid = 0;
    u32 ref = 0;
};

struct ImportedLibrary {
    u32 addr = 0;
    std::string name;
    u8 struct_size = 0;
    u16 version = 0;
    u16 attributes = 0;
    u16 num_func = 0;
    u16 num_var = 0;
    u16 num_tlsvar = 0;
    u32 fnid_table = 0;
    u32 fstub_table = 0;
    u32 vnid_table = 0;
    u32 vstub_table = 0;
    u32 tls_table = 0;      // unk4
    u32 tls_stub_table = 0; // unk5
    std::vector<ImportedFunction> functions;
    std::vector<ImportedVariable> variables;
    std::vector<ImportedTlsVariable> tls_variables;
    std::vector<std::string> problems;
};

struct ExportedFunction {
    u32 nid = 0;
    u32 descriptor = 0; // address of the function descriptor (OPD)
    std::optional<u32> entry;
    std::optional<u32> toc;
    ResolvedName resolved;
};

struct ExportedVariable {
    u32 nid = 0;
    u32 addr = 0;
    ResolvedName resolved;
};

struct ExportedLibrary {
    u32 addr = 0;
    std::string name;        // empty for the nameless management library
    bool management = false; // attribute 0x8000 without 0x0001: module_start, module_stop, ...
    bool skipped = false;    // neither flag set: the loader ignores the entry
    u8 struct_size = 0;
    u16 version = 0;
    u16 attributes = 0;
    u16 num_func = 0;
    u16 num_var = 0;
    u16 num_tlsvar = 0;
    u32 nid_table = 0;
    u32 addr_table = 0;
    std::vector<ExportedFunction> functions;
    std::vector<ExportedVariable> variables;
    std::vector<std::string> problems;
};

struct OpdEntry {
    u32 addr = 0; // where the descriptor lives
    u32 func = 0;
    u32 toc = 0;
};

// Function descriptors. Sony SDK binaries use 8-byte {u32 func, u32 toc} entries; PSL1GHT homebrew uses 24-byte
// entries {u64 func, u64 toc, u32 func, u32 toc} (RPCS3 PPUAnalyser.cpp "special OPD format case").
struct OpdInfo {
    enum class Source : u8 { None, Section, SectionProbe, EntrySegment };
    Source source = Source::None;
    std::string section_name;
    u32 start = 0;
    u32 size = 0;
    u32 entry_size = 0;
    std::vector<OpdEntry> entries;
    std::optional<u32> toc;        // the TOC of the anchor descriptor
    std::optional<u32> anchor;     // the descriptor the search started from (e_entry for executables)
    std::optional<u32> entry_func; // the entry point's code address
    u32 distinct_functions = 0;    // distinct code addresses among valid entries
    u32 invalid_entries = 0;       // entries in an .opd section whose code address is not executable
};

std::string_view opd_source_name(OpdInfo::Source source);

struct PpuModule {
    bool is_prx = false;
    std::optional<ProcessParam> process_param;
    std::optional<ProcPrxParam> prx_param;
    std::optional<PrxModuleInfo> module_info;
    std::optional<TlsInfo> tls;
    std::vector<ImportedLibrary> imports;
    std::vector<ExportedLibrary> exports;
    OpdInfo opd;
    GuestImage::RelocationSummary relocations;
    std::vector<std::string> warnings;

    usize imported_function_count() const;
    usize imported_variable_count() const;
};

// Analyzes a 64-bit PowerPC ELF (executable or PRX). Fails only when the ELF is not a PPU image or its segments
// cannot be placed; problems in the tables become warnings or per-library problems.
Result<PpuModule> analyze_ppu_module(const ElfFile& elf);
Result<PpuModule> analyze_ppu_module(const ElfFile& elf, const GuestImage& image);

// Fills ResolvedName fields from the NID database.
void resolve_names(PpuModule& module, const NidDb& db);

} // namespace pspocket::recomp
