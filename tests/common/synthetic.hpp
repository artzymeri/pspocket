#pragma once

// Synthetic PS3 binaries built in code (CLAUDE.md rule 1: no game data in tests). They follow the layouts the
// loader reads (RPCS3 rpcs3/Emu/Cell/PPUModule.cpp, rpcs3/Loader/ELF.h, rpcs3/Crypto/unself.h) closely enough to
// exercise every path of recomp/common, and record what a correct parser must find.

#include <span>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/npd.hpp"
#include "pspocket/recomp/pkg.hpp"
#include "pspocket/recomp/self_decrypt.hpp"

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
    // Bytes the module carries at segment 1 + kPrxExtraData (vaddr 0x1200), after the segment's own 0x200 (an SPU ELF,
    // as the firmware's modules carry their SPU programs). Segment 1 grows to hold them; the relocations move after.
    std::vector<u8> extra_data;
};
inline constexpr u32 kPrxExtraData = 0x200;
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

// ---- encrypted SELF (D-121, D-582) ----

struct SelfEncryption {
    recomp::SelfKey key{};    // the SELF key the metadata info is encrypted with
    u16 revision = 0;         // the SCE header's key revision (the key's)
    // A PSN executable: an NPDRM control header of this licence type (wrap_in_self's content ID), and the AES-128-CBC
    // layer over the metadata info keyed with `npdrm_layer_key` (its content key decrypted with NP_KLIC_KEY).
    bool npdrm = false;
    s32 npdrm_license = 3;
    recomp::Key128 npdrm_layer_key{};
};

// `elf` as a retail SELF, as Sony makes them and recomp::decrypt_self reads them: wrap_in_self's headers (an APP
// executable, or an NPDRM one), real metadata (one section per segment with bytes, each with its own made-up key and
// counter), and the segments compressed (zlib) and encrypted. Empty in a build without zlib.
std::vector<u8> encrypt_self(std::span<const u8> elf, const SelfEncryption& encryption);

// ---- NPD files: SDATA (D-165) and EDATA (D-581) ----

struct NpdOptions {
    s32 version = 3;          // NPD version: 0 and 1 have no IV and keep compressed blocks' metadata plain; 4 uses
                              // EDAT_KEY_1 for an encrypted key
    u32 extra_flags = 0;      // recomp::kNpd*Flag besides the SDAT flag
    u32 block_size = 0x400;
    // An EDATA file (no SDAT flag) instead, encrypted with `key`, its content key (a free file's klicensee, else the
    // RIF key of the player's RAP), of this licence type and content ID. With `npdrm`, the header carries the hashes
    // RPCS3 checks: the title hash over the content ID and `file_name`, and a free file's dev hash keyed with `key`;
    // without it, made-up bytes.
    bool edata = false;
    s32 license = 3;
    std::string content_id = std::string(kSelfContentId);
    std::string file_name{};
    recomp::Key128 key{};
    const recomp::NpdrmKeys* npdrm = nullptr;
};
using SdataOptions = NpdOptions;

// `plain` as an NPD file, encrypted, hashed and laid out the way recomp::NpdFile reads Sony's (RPCS3 unedat.cpp in
// reverse), with `keys`. A compressed file (kNpdCompressedFlag) holds its blocks in the decoder's plain form (a byte
// above 0x80, the length, the bytes), every other block marked compressed. build_sdata makes an SDATA file whatever
// `options.edata` says.
std::vector<u8> build_npd(std::span<const u8> plain, const recomp::NpdKeys& keys, const NpdOptions& options = {});
std::vector<u8> build_sdata(std::span<const u8> plain, const recomp::NpdKeys& keys, const NpdOptions& options = {});

// ---- PARAM.SFO ----

// A PARAM.SFO (PSF 1.1) of string entries (key, value), the keys sorted as the PS3 keeps them, each value in a field
// of its length rounded up to 4 with a NUL.
std::vector<u8> build_param_sfo(const std::vector<std::pair<std::string, std::string>>& strings);

// ---- PS3 packages (D-583) ----

struct PkgItem {
    std::string name;                   // in the install folder: "USRDIR/EBOOT.BIN"
    u32 type = recomp::kPkgEntryRegular; // an entry type, with kPkgEntryOverwrite and the like
    std::vector<u8> bytes;              // a file's (a folder has none)
};

struct PkgSpec {
    std::string content_id = "UP0001-NPXX00000_00-0000000000000001";
    bool retail = true;                   // false: a debug package
    u16 platform = 1;                     // 1: PS3; 2: PSP or PS Vita
    recomp::Key128 key{};                 // a retail package's: PKG_AES_KEY, PKG_AES_KEY_IDU or made up
    std::array<u8, 16> counter{};         // the data's first counter block
    std::array<u8, 16> qa_digest{};       // a debug package's key stream comes from it
    u32 content_type = recomp::kPkgContentGameExec; // 0: no metadata packet 2
    u32 drm_type = 3;
    u32 package_flags = 0;
    std::string install_dir;              // an add-on's own folder (metadata packet 0xA), when not empty
    std::vector<PkgItem> items;
    usize align = 16;                     // where names and files start (a power of 2): Sony's packages, 16
};

// `spec` as a PS3 package, as RPCS3's package_reader reads them (rpcs3/Crypto/unpkg.cpp in reverse): the 0xC0-byte
// header, metadata packets (1, 2, 3 and 0xA as asked, and two RPCS3 passes over), the data (the entry table, then
// each name and file on an `align`-byte boundary) encrypted as the spec says, and 0x60 made-up bytes where Sony's
// digests are. Nothing of Sony's: the test gives the key.
std::vector<u8> build_pkg(const PkgSpec& spec);

} // namespace pspocket::test
