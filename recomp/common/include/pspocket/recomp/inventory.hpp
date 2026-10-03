#pragma once

#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/hash.hpp"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/recomp/ppu_module.hpp"
#include "pspocket/recomp/sce.hpp"
#include "pspocket/recomp/spu_scan.hpp"
#include "pspocket/recomp/strings.hpp"

// The Phase 0 inventory of one file (plan section 10): what it is, its headers and segments, the PPU module
// metadata with NIDs resolved to names, embedded SPU programs split into Sony middleware / third-party / custom,
// middleware signatures, and a summary. ps3inspect prints and serializes this; tests assert on it.
namespace pspocket::recomp {

enum class FileKind : u8 {
    PpuExecutable, // ELF64 PowerPC, ET_EXEC
    PpuPrx,        // ELF64 PowerPC, ET_SCE_PPURELEXEC (0xFFA4)
    PpuOther,      // other ELF64 PowerPC (relocatable objects, ...)
    SpuElf,        // ELF32 SPU program
    Self,          // SCE container (SELF / SPRX); encrypted unless it is a debug SELF
    OtherElf,      // big-endian ELF for another machine
    Unknown,
};

std::string_view file_kind_name(FileKind kind); // stable identifiers used in JSON: "ppu_executable", ...
std::string_view file_kind_description(FileKind kind);

struct SpuProgram {
    SpuImage image;
    std::vector<MiddlewareHit> middleware; // signatures found in the image bytes or its name
    SpuProgramClass classification = SpuProgramClass::Custom;
};

struct UnresolvedNid {
    std::string module;
    u32 nid = 0;
    bool variable = false;
};

struct ModuleImportSummary {
    u32 functions = 0;
    u32 variables = 0;
    u32 unresolved = 0;      // no name known at all
    u32 other_module = 0;    // named only through another module's registration
};

struct Phase0Summary {
    std::optional<u32> function_estimate; // distinct functions with a descriptor (.opd)
    std::string function_estimate_source;
    std::map<std::string, ModuleImportSummary> imports; // by library
    std::vector<UnresolvedNid> unresolved;
    u32 spu_programs = 0;
    u32 spu_sony = 0;
    u32 spu_third_party = 0;
    u32 spu_custom = 0;
    u64 spu_bytes_sony = 0;
    u64 spu_bytes_third_party = 0;
    u64 spu_bytes_custom = 0;
};

struct FileReport {
    std::string path;
    u64 size = 0;
    Sha1Digest sha1{};
    Sha256Digest sha256{};
    FileKind kind = FileKind::Unknown;
    std::string detail; // e.g. "PARAM.SFO" for an unknown file with a known magic

    // ELF (for a debug SELF: the plain ELF inside it; elf_offset says where it starts)
    std::optional<ElfHeader> elf;
    u64 elf_offset = 0;
    std::vector<ProgramHeader> segments;
    std::vector<SectionHeader> sections;
    usize symbol_count = 0;
    std::optional<Sha1Digest> rpcs3_ppu_hash; // RPCS3's "PPU-<sha1>" executable hash

    std::optional<SelfInfo> self;
    std::optional<PpuModule> ppu;
    std::vector<SpuProgram> spu_programs;  // embedded in a PPU file, or the file itself when it is an SPU ELF
    std::vector<MiddlewareHit> middleware; // over the whole file
    std::vector<FoundString> strings;      // only when requested
    Phase0Summary phase0;

    std::vector<std::string> warnings;
    std::vector<std::string> errors; // the file could not be analyzed (fully)

    bool encrypted() const { return kind == FileKind::Self && self && self->encrypted; }
};

struct InspectOptions {
    const NidDb* nids = nullptr; // defaults to NidDb::builtin()
    bool collect_strings = false;
    usize min_string_length = 6;
    usize max_strings = 1'000'000;
};

FileReport inspect_bytes(std::string path, std::span<const u8> data, const InspectOptions& options = {});

// RPCS3's PPU executable hash (rpcs3/Emu/Cell/PPUModule.cpp, ppu_load_exec): SHA-1 over p_type and p_flags of
// every program header, plus p_vaddr, p_memsz and the file data of each LOAD segment with p_memsz != 0.
std::optional<Sha1Digest> rpcs3_ppu_executable_hash(const ElfFile& elf);
std::string rpcs3_ppu_hash_string(const Sha1Digest& digest); // "PPU-<hex>"

} // namespace pspocket::recomp
