#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/result.hpp"

// SCE container ("SCE\0") / SELF: the plaintext headers only. Structures follow RPCS3 rpcs3/Crypto/unself.h and
// unself.cpp (SceHeader, ext_hdr, program_identification_header, segment_ext_header, version_header,
// supplemental_header, NPD_HEADER).
//
// This never decrypts anything and holds no keys. A retail SELF's segments (and its metadata) are encrypted; the
// report says so and points at RPCS3's "Decrypt PS3 Binaries" utility (docs/phase0.md). The one exception is a
// debug ("fake signed") SELF, whose ELF is stored in plain text after the headers (RPCS3 CheckDebugSelf): its
// offset is reported so the plain ELF can be analyzed directly.
namespace pspocket::recomp {

inline constexpr u32 kSceMagic = 0x53434500; // "SCE\0"

struct SceHeader {
    u32 magic = 0;
    u32 version = 0;
    u16 key_revision = 0; // se_flags; 0x8000 = debug
    u16 type = 0;         // 1 = SELF
    u32 metadata_offset = 0;
    u64 header_size = 0;
    u64 data_size = 0;
};

struct SelfExtHeader {
    u64 version = 0;
    u64 app_info_offset = 0;
    u64 elf_offset = 0;
    u64 phdr_offset = 0;
    u64 shdr_offset = 0;
    u64 segment_info_offset = 0;
    u64 version_info_offset = 0;
    u64 control_info_offset = 0;
    u64 control_info_size = 0;
};

// "program identification header" in RPCS3, "app info" in most tools.
struct SelfAppInfo {
    u64 auth_id = 0;
    u32 vendor_id = 0;
    u32 self_type = 0; // 1 LV0, 2 LV1, 3 LV2, 4 APP, 5 ISO, 6 LDR, 7 UNK7, 8 NPDRM (RPCS3 key_vault.h SELF_KEY_TYPE)
    u64 version = 0;
};

struct SelfSegmentInfo {
    u64 offset = 0;      // where the (encrypted, maybe compressed) segment data sits in the SELF
    u64 size = 0;
    u32 compression = 0; // 1 = plain, 2 = zlib
    u32 unknown = 0;
    u64 encryption = 0;  // RPCS3's note: 0 = unrequested, 1 = completed, 2 = requested
};

struct SelfVersionInfo {
    u32 subheader_type = 0;
    u32 present = 0;
    u32 size = 0;
    u32 unknown = 0;
};

struct NpdrmInfo {
    u32 magic = 0;    // "NPD\0"
    s32 version = 0;
    s32 license = 0;  // 1 network, 2 local (both need a RAP), 3 free
    s32 type = 0;
    std::string content_id;
    s64 activate_time = 0;
    s64 expire_time = 0;
};

// One supplemental header ("control info"): type 1 = plaintext capability flags, 2 = ELF digest, 3 = NPDRM.
struct SelfControlInfo {
    u32 type = 0;
    u32 size = 0;
    u64 next = 0;
    std::optional<u32> ctrl_flag1;              // type 1
    std::optional<u64> required_system_version; // type 2, 0x40 bytes
    std::optional<NpdrmInfo> npdrm;             // type 3
};

struct SelfInfo {
    SceHeader sce;
    std::optional<SelfExtHeader> ext;
    std::optional<SelfAppInfo> app;
    std::optional<ElfHeader> elf;
    std::vector<ProgramHeader> phdrs; // copies of the original ELF's headers (offsets refer to the ELF, not the SELF)
    std::vector<SectionHeader> shdrs;
    std::vector<SelfSegmentInfo> segments; // one per program header
    std::optional<SelfVersionInfo> version;
    std::vector<SelfControlInfo> control;
    bool debug = false;                      // key revision 0x8000 / 0xC000
    std::optional<u64> plaintext_elf_offset; // debug SELF: where the plain ELF starts
    bool encrypted = true;                   // false only for a debug SELF
    std::vector<std::string> warnings;

    const NpdrmInfo* npdrm() const;
};

bool has_sce_magic(std::span<const u8> data);
Result<SelfInfo> parse_self(std::span<const u8> data);

std::string_view self_type_name(u32 self_type);
std::string_view npdrm_license_name(s32 license);

} // namespace pspocket::recomp
