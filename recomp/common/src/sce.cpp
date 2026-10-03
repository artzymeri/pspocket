#include "pspocket/recomp/sce.hpp"

#include <algorithm>
#include <format>

#include "pspocket/recomp/bytes.hpp"

namespace pspocket::recomp {

namespace {

constexpr usize kMaxControlInfos = 64;

} // namespace

const NpdrmInfo* SelfInfo::npdrm() const
{
    for (const auto& c : control) {
        if (c.npdrm) return &*c.npdrm;
    }
    return nullptr;
}

bool has_sce_magic(std::span<const u8> data)
{
    const auto magic = ByteReader(data).be32(0);
    return magic && *magic == kSceMagic;
}

std::string_view self_type_name(u32 self_type)
{
    switch (self_type) {
    case 1: return "LV0";
    case 2: return "LV1";
    case 3: return "LV2";
    case 4: return "APP";
    case 5: return "ISO";
    case 6: return "LDR";
    case 7: return "UNK7";
    case 8: return "NPDRM";
    default: return "?";
    }
}

std::string_view npdrm_license_name(s32 license)
{
    switch (license) {
    case 1: return "network (needs a RAP)";
    case 2: return "local (needs a RAP)";
    case 3: return "free (no RAP)";
    default: return "invalid";
    }
}

// Layouts: RPCS3 rpcs3/Crypto/unself.cpp (SceHeader::Load, ext_hdr::Load, program_identification_header::Load,
// segment_ext_header::Load, version_header::Load, supplemental_header::Load) and SELFDecrypter::LoadHeaders.
Result<SelfInfo> parse_self(std::span<const u8> data)
{
    const ByteReader r(data);
    if (!has_sce_magic(data)) return make_error("not an SCE container");
    if (!r.in_bounds(0, 0x20)) return make_error("truncated SCE header ({} bytes)", data.size());

    SelfInfo info;
    info.sce.magic = *r.be32(0x00);
    info.sce.version = *r.be32(0x04);
    info.sce.key_revision = *r.be16(0x08);
    info.sce.type = *r.be16(0x0A);
    info.sce.metadata_offset = *r.be32(0x0C);
    info.sce.header_size = *r.be64(0x10);
    info.sce.data_size = *r.be64(0x18);

    // RPCS3 CheckDebugSelf: key version 0x80 / 0xC0 (read little-endian at 0x08) marks a debug SELF whose ELF
    // is stored in plain text at the offset in 0x10 (big-endian for 0x80, little-endian for 0xC0).
    if (info.sce.key_revision == 0x8000 || info.sce.key_revision == 0xC000) {
        info.debug = true;
        info.encrypted = false;
        info.plaintext_elf_offset = info.sce.key_revision == 0x8000 ? *r.be64(0x10) : *r.le64(0x10);
        if (!r.in_bounds(*info.plaintext_elf_offset, 4)) {
            info.warnings.push_back(std::format("debug SELF: plain ELF offset {:#x} is outside the file", *info.plaintext_elf_offset));
        }
    }

    if (info.sce.type != 1) {
        info.warnings.push_back(std::format("SCE container type {} is not a SELF (1)", info.sce.type));
        return info;
    }
    if (!r.in_bounds(0x20, 0x50)) {
        info.warnings.push_back("truncated SELF extended header");
        return info;
    }
    SelfExtHeader ext;
    ext.version = *r.be64(0x20);
    ext.app_info_offset = *r.be64(0x28);
    ext.elf_offset = *r.be64(0x30);
    ext.phdr_offset = *r.be64(0x38);
    ext.shdr_offset = *r.be64(0x40);
    ext.segment_info_offset = *r.be64(0x48);
    ext.version_info_offset = *r.be64(0x50);
    ext.control_info_offset = *r.be64(0x58);
    ext.control_info_size = *r.be64(0x60);
    info.ext = ext;

    if (r.in_bounds(ext.app_info_offset, 0x20)) {
        SelfAppInfo app;
        app.auth_id = *r.be64(ext.app_info_offset);
        app.vendor_id = *r.be32(ext.app_info_offset + 8);
        app.self_type = *r.be32(ext.app_info_offset + 12);
        app.version = *r.be64(ext.app_info_offset + 16);
        info.app = app;
    } else {
        info.warnings.push_back("app info is outside the file");
    }

    // The original ELF header and program headers, stored in plain text.
    if (ext.elf_offset < data.size()) {
        auto elf = parse_elf_header(data.subspan(static_cast<usize>(ext.elf_offset)));
        if (elf) {
            info.elf = *elf;
            auto phdrs = parse_program_headers(data, *elf, ext.phdr_offset, 0);
            if (phdrs) {
                info.phdrs = std::move(*phdrs);
            } else {
                info.warnings.push_back("program headers: " + phdrs.error().message);
            }
            if (ext.shdr_offset != 0 && elf->shnum != 0) {
                auto shdrs = parse_section_headers(data, *elf, ext.shdr_offset, 0);
                if (shdrs) {
                    info.shdrs = std::move(*shdrs);
                } else {
                    info.warnings.push_back("section headers: " + shdrs.error().message);
                }
            }
        } else {
            info.warnings.push_back("embedded ELF header: " + elf.error().message);
        }
    } else {
        info.warnings.push_back("embedded ELF header is outside the file");
    }

    // Segment info: one 0x20-byte entry per program header.
    const u16 phnum = info.elf ? info.elf->phnum : 0;
    for (u64 i = 0; i < phnum; i++) {
        const auto at = checked_add(ext.segment_info_offset, i * 0x20);
        if (!at || !r.in_bounds(*at, 0x20)) {
            info.warnings.push_back(std::format("segment info {} of {} is outside the file", i, phnum));
            break;
        }
        SelfSegmentInfo s;
        s.offset = *r.be64(*at);
        s.size = *r.be64(*at + 8);
        s.compression = *r.be32(*at + 16);
        s.unknown = *r.be32(*at + 20);
        s.encryption = *r.be64(*at + 24);
        info.segments.push_back(s);
    }

    if (ext.version_info_offset != 0) {
        if (r.in_bounds(ext.version_info_offset, 0x10)) {
            info.version = SelfVersionInfo{*r.be32(ext.version_info_offset), *r.be32(ext.version_info_offset + 4),
                *r.be32(ext.version_info_offset + 8), *r.be32(ext.version_info_offset + 12)};
        } else {
            info.warnings.push_back("version info is outside the file");
        }
    }

    // Supplemental headers.
    u64 consumed = 0;
    while (consumed < ext.control_info_size) {
        if (info.control.size() == kMaxControlInfos) {
            info.warnings.push_back("too many control info entries");
            break;
        }
        const auto at = checked_add(ext.control_info_offset, consumed);
        if (!at || !r.in_bounds(*at, 0x10)) {
            info.warnings.push_back("control info is outside the file");
            break;
        }
        SelfControlInfo c;
        c.type = *r.be32(*at);
        c.size = *r.be32(*at + 4);
        c.next = *r.be64(*at + 8);
        if (c.size < 0x10 || !r.in_bounds(*at, c.size)) {
            info.warnings.push_back(std::format("control info entry of type {} has bad size {:#x}", c.type, c.size));
            break;
        }
        if (c.type == 1 && c.size >= 0x14) {
            c.ctrl_flag1 = *r.be32(*at + 0x10);
        } else if (c.type == 2 && c.size == 0x40) {
            c.required_system_version = *r.be64(*at + 0x38);
        } else if (c.type == 3 && c.size >= 0x90) {
            NpdrmInfo n;
            n.magic = *r.be32(*at + 0x10);
            n.version = static_cast<s32>(*r.be32(*at + 0x14));
            n.license = static_cast<s32>(*r.be32(*at + 0x18));
            n.type = static_cast<s32>(*r.be32(*at + 0x1C));
            n.content_id = std::string(r.cstring_lenient(*at + 0x20, 0x30));
            n.activate_time = static_cast<s64>(*r.be64(*at + 0x80));
            n.expire_time = static_cast<s64>(*r.be64(*at + 0x88));
            c.npdrm = std::move(n);
        }
        info.control.push_back(std::move(c));
        consumed += info.control.back().size;
    }
    return info;
}

} // namespace pspocket::recomp
