#include "pspocket/recomp/elf.hpp"

#include <algorithm>
#include <format>

#include "pspocket/recomp/bytes.hpp"

namespace pspocket::recomp {

namespace elf {

std::string_view type_name(u16 type)
{
    switch (type) {
    case 0: return "NONE";
    case kTypeRel: return "REL";
    case kTypeExec: return "EXEC";
    case kTypeDyn: return "DYN";
    case 4: return "CORE";
    case kTypeSceExec: return "SCE_EXEC";
    case kTypeSceRelExec: return "SCE_RELEXEC";
    case kTypeSceStubLib: return "SCE_STUBLIB";
    case kTypeSceDynExec: return "SCE_DYNEXEC";
    case kTypeSceDynamic: return "SCE_DYNAMIC";
    case kTypeSceIopRelExec: return "SCE_IOPRELEXEC";
    case kTypeSceIopRelExec2: return "SCE_IOPRELEXEC2";
    case kTypeSceEeRelExec: return "SCE_EERELEXEC";
    case kTypeSceEeRelExec2: return "SCE_EERELEXEC2";
    case kTypeScePspRelExec: return "SCE_PSPRELEXEC";
    case kTypeScePpuRelExec: return "SCE_PPURELEXEC (PRX)";
    case kTypeSceArmRelExec: return "SCE_ARMRELEXEC";
    case kTypeScePspOverlay: return "SCE_PSPOVERLAY";
    default: return "?";
    }
}

std::string_view machine_name(u16 machine)
{
    switch (machine) {
    case 0: return "NONE";
    case 8: return "MIPS";
    case kMachinePpc: return "PPC";
    case kMachinePpc64: return "PPC64";
    case kMachineSpu: return "SPU";
    case kMachineArm: return "ARM";
    default: return "?";
    }
}

std::string_view program_type_name(u32 type)
{
    switch (type) {
    case kPtNull: return "NULL";
    case kPtLoad: return "LOAD";
    case kPtDynamic: return "DYNAMIC";
    case kPtInterp: return "INTERP";
    case kPtNote: return "NOTE";
    case kPtShlib: return "SHLIB";
    case kPtPhdr: return "PHDR";
    case kPtTls: return "TLS";
    case kPtSceRela: return "SCE_RELA";
    case kPtProcParam: return "PROC_PARAM";
    case kPtProcPrx: return "PROC_PRX";
    case kPtSceDynLibData: return "SCE_DYNLIBDATA";
    case kPtSceProcParam: return "SCE_PROCPARAM";
    case kPtSceUnk61000010: return "SCE_UNK_61000010";
    case kPtSceComment: return "SCE_COMMENT";
    case kPtSceLibVersion: return "SCE_LIBVERSION";
    case kPtSceUnk70000001: return "SCE_UNK_70000001";
    case kPtSceIopMod: return "SCE_IOPMOD";
    case kPtSceEeMod: return "SCE_EEMOD";
    case kPtScePspRela: return "SCE_PSPRELA";
    case kPtScePspRela2: return "SCE_PSPRELA2";
    case kPtScePpuRela: return "SCE_PPURELA";
    case kPtSceSegSym: return "SCE_SEGSYM";
    case 0x6474E550: return "GNU_EH_FRAME";
    case 0x6474E551: return "GNU_STACK";
    case 0x6474E552: return "GNU_RELRO";
    default: return "?";
    }
}

std::string_view section_type_name(u32 type)
{
    switch (type) {
    case kShtNull: return "NULL";
    case kShtProgbits: return "PROGBITS";
    case kShtSymtab: return "SYMTAB";
    case kShtStrtab: return "STRTAB";
    case kShtRela: return "RELA";
    case kShtHash: return "HASH";
    case kShtDynamic: return "DYNAMIC";
    case kShtNote: return "NOTE";
    case kShtNobits: return "NOBITS";
    case kShtRel: return "REL";
    case kShtDynsym: return "DYNSYM";
    case 14: return "INIT_ARRAY";
    case 15: return "FINI_ARRAY";
    case 16: return "PREINIT_ARRAY";
    case 17: return "GROUP";
    case 18: return "SYMTAB_SHNDX";
    case kShtSceRela: return "SCE_RELA";
    case kShtSceNid: return "SCE_NID";
    case kShtSceIopMod: return "SCE_IOPMOD";
    case kShtSceEeMod: return "SCE_EEMOD";
    case kShtScePspRela: return "SCE_PSPRELA";
    case kShtScePpuRela: return "SCE_PPURELA";
    default: return "?";
    }
}

std::string segment_flags_string(u32 flags)
{
    auto rwx = [](u32 bits) {
        std::string s = "---";
        if (bits & 4) s[0] = 'R';
        if (bits & 2) s[1] = 'W';
        if (bits & 1) s[2] = 'X';
        return s;
    };
    std::string out = rwx(flags & 7);
    if (const u32 spu = (flags >> 20) & 7) out += " spu:" + rwx(spu);
    if (const u32 rsx = (flags >> 24) & 7) out += " rsx:" + rwx(rsx);
    if (const u32 other = flags & ~(0x7u | 0x00700000u | 0x07000000u)) out += std::format(" +{:#x}", other);
    return out;
}

} // namespace elf

namespace {

constexpr usize kMaxSymbols = usize{1} << 22;

u64 saturating_add(u64 a, u64 b)
{
    const auto sum = checked_add(a, b);
    return sum ? *sum : ~u64{0};
}

} // namespace

Result<ElfHeader> parse_elf_header(std::span<const u8> data)
{
    const ByteReader r(data);
    if (data.size() < 16) return make_error("too small for an ELF header ({} bytes)", data.size());
    if (data[0] != 0x7F || data[1] != 'E' || data[2] != 'L' || data[3] != 'F') return make_error("not an ELF file");

    ElfHeader h;
    h.elf_class = data[4];
    h.data = data[5];
    h.ident_version = data[6];
    h.os_abi = data[7];
    h.abi_version = data[8];
    if (h.elf_class != elf::kClass32 && h.elf_class != elf::kClass64) {
        return make_error("unsupported ELF class {}", h.elf_class);
    }
    if (h.data != elf::kDataMsb) {
        return make_error("ELF data encoding {} is not big-endian (PS3 binaries are big-endian)", h.data);
    }
    if (data.size() < h.header_size()) {
        return make_error("truncated ELF header ({} of {} bytes)", data.size(), h.header_size());
    }

    h.type = *r.be16(16);
    h.machine = *r.be16(18);
    h.version = *r.be32(20);
    if (h.is64()) {
        h.entry = *r.be64(24);
        h.phoff = *r.be64(32);
        h.shoff = *r.be64(40);
        h.flags = *r.be32(48);
        h.ehsize = *r.be16(52);
        h.phentsize = *r.be16(54);
        h.phnum = *r.be16(56);
        h.shentsize = *r.be16(58);
        h.shnum = *r.be16(60);
        h.shstrndx = *r.be16(62);
    } else {
        h.entry = *r.be32(24);
        h.phoff = *r.be32(28);
        h.shoff = *r.be32(32);
        h.flags = *r.be32(36);
        h.ehsize = *r.be16(40);
        h.phentsize = *r.be16(42);
        h.phnum = *r.be16(44);
        h.shentsize = *r.be16(46);
        h.shnum = *r.be16(48);
        h.shstrndx = *r.be16(50);
    }
    return h;
}

Result<std::vector<ProgramHeader>> parse_program_headers(std::span<const u8> data, const ElfHeader& header, u64 offset,
                                                         u64 file_size_for_data_check)
{
    std::vector<ProgramHeader> out;
    if (header.phnum == 0) return out;
    if (header.phentsize != header.phdr_size()) {
        return make_error("unexpected program header entry size {} (expected {})", header.phentsize, header.phdr_size());
    }
    const u64 table_size = u64{header.phnum} * header.phdr_size();
    const ByteReader r(data);
    if (!r.in_bounds(offset, table_size)) {
        return make_error("program header table ({} entries at {:#x}) is outside the data ({:#x} bytes)", header.phnum,
            offset, data.size());
    }
    out.reserve(header.phnum);
    for (u64 i = 0; i < header.phnum; i++) {
        const u64 base = offset + i * header.phdr_size();
        ProgramHeader p;
        if (header.is64()) {
            p.type = *r.be32(base + 0);
            p.flags = *r.be32(base + 4);
            p.offset = *r.be64(base + 8);
            p.vaddr = *r.be64(base + 16);
            p.paddr = *r.be64(base + 24);
            p.filesz = *r.be64(base + 32);
            p.memsz = *r.be64(base + 40);
            p.align = *r.be64(base + 48);
        } else {
            p.type = *r.be32(base + 0);
            p.offset = *r.be32(base + 4);
            p.vaddr = *r.be32(base + 8);
            p.paddr = *r.be32(base + 12);
            p.filesz = *r.be32(base + 16);
            p.memsz = *r.be32(base + 20);
            p.flags = *r.be32(base + 24);
            p.align = *r.be32(base + 28);
        }
        p.data_in_file = range_ok(file_size_for_data_check, p.offset, p.filesz);
        out.push_back(p);
    }
    return out;
}

Result<std::vector<SectionHeader>> parse_section_headers(std::span<const u8> data, const ElfHeader& header, u64 offset,
                                                         u64 file_size_for_data_check)
{
    std::vector<SectionHeader> out;
    if (header.shnum == 0) return out;
    if (header.shentsize != header.shdr_size()) {
        return make_error("unexpected section header entry size {} (expected {})", header.shentsize, header.shdr_size());
    }
    const u64 table_size = u64{header.shnum} * header.shdr_size();
    const ByteReader r(data);
    if (!r.in_bounds(offset, table_size)) {
        return make_error("section header table ({} entries at {:#x}) is outside the data ({:#x} bytes)", header.shnum,
            offset, data.size());
    }
    out.reserve(header.shnum);
    for (u64 i = 0; i < header.shnum; i++) {
        const u64 base = offset + i * header.shdr_size();
        SectionHeader s;
        if (header.is64()) {
            s.name_offset = *r.be32(base + 0);
            s.type = *r.be32(base + 4);
            s.flags = *r.be64(base + 8);
            s.addr = *r.be64(base + 16);
            s.offset = *r.be64(base + 24);
            s.size = *r.be64(base + 32);
            s.link = *r.be32(base + 40);
            s.info = *r.be32(base + 44);
            s.addralign = *r.be64(base + 48);
            s.entsize = *r.be64(base + 56);
        } else {
            s.name_offset = *r.be32(base + 0);
            s.type = *r.be32(base + 4);
            s.flags = *r.be32(base + 8);
            s.addr = *r.be32(base + 12);
            s.offset = *r.be32(base + 16);
            s.size = *r.be32(base + 20);
            s.link = *r.be32(base + 24);
            s.info = *r.be32(base + 28);
            s.addralign = *r.be32(base + 32);
            s.entsize = *r.be32(base + 36);
        }
        s.data_in_file = s.type != elf::kShtNobits && range_ok(file_size_for_data_check, s.offset, s.size);
        out.push_back(std::move(s));
    }
    return out;
}

Result<ElfFile> ElfFile::parse(std::span<const u8> data, const ElfParseOptions& options)
{
    ElfFile f;
    f.data_ = data;

    auto header = parse_elf_header(data);
    if (!header) return header.error();
    f.header_ = *header;
    const ElfHeader& h = f.header_;

    if (h.ehsize != h.header_size()) {
        f.warnings_.push_back(std::format("e_ehsize is {} (expected {})", h.ehsize, h.header_size()));
    }
    if (h.ident_version != 1 || h.version != 1) {
        f.warnings_.push_back(std::format("unexpected ELF version (ident {}, e_version {})", h.ident_version, h.version));
    }

    auto segments = parse_program_headers(data, h, h.phoff, data.size());
    if (!segments) return make_error("program headers: {}", segments.error().message);
    f.segments_ = std::move(*segments);
    for (usize i = 0; i < f.segments_.size(); i++) {
        const auto& p = f.segments_[i];
        if (p.filesz != 0 && !p.data_in_file) {
            f.warnings_.push_back(std::format("segment {} data ({:#x} bytes at {:#x}) is outside the file", i, p.filesz, p.offset));
        }
        if (p.type == elf::kPtLoad && p.filesz > p.memsz) {
            f.warnings_.push_back(std::format("segment {} has p_filesz {:#x} > p_memsz {:#x}", i, p.filesz, p.memsz));
        }
    }

    if (h.shnum != 0) {
        auto sections = parse_section_headers(data, h, h.shoff, data.size());
        if (sections) {
            f.sections_ = std::move(*sections);
        } else {
            f.warnings_.push_back("section headers ignored: " + sections.error().message);
        }
    }

    // Section names.
    if (!f.sections_.empty() && h.shstrndx != 0) {
        if (h.shstrndx < f.sections_.size() && f.sections_[h.shstrndx].data_in_file) {
            const ByteReader strtab(f.section_data(f.sections_[h.shstrndx]));
            usize bad = 0;
            for (auto& s : f.sections_) {
                if (s.name_offset >= strtab.size()) {
                    if (s.name_offset != 0) bad++;
                    continue;
                }
                s.name = std::string(strtab.cstring_lenient(s.name_offset, 256));
            }
            if (bad != 0) f.warnings_.push_back(std::format("{} section names point outside the name table", bad));
        } else {
            f.warnings_.push_back("section name table is unavailable");
        }
    }

    // Symbols (SYMTAB and DYNSYM sections).
    for (const auto& s : f.sections_) {
        if (!options.symbols) break;
        if ((s.type != elf::kShtSymtab && s.type != elf::kShtDynsym) || !s.data_in_file) continue;
        const usize entsize = h.is64() ? elf::kSymSize64 : elf::kSymSize32;
        if (s.entsize != 0 && s.entsize != entsize) {
            f.warnings_.push_back(std::format("symbol table '{}' has entry size {} (expected {}); skipped", s.name, s.entsize, entsize));
            continue;
        }
        u64 count = s.size / entsize;
        if (f.symbols_.size() + count > kMaxSymbols) {
            f.warnings_.push_back(std::format("symbol table '{}' truncated to {} symbols", s.name, kMaxSymbols));
            count = kMaxSymbols - f.symbols_.size();
        }
        const ByteReader syms(f.section_data(s));
        ByteReader strtab;
        if (s.link < f.sections_.size() && f.sections_[s.link].data_in_file) strtab = ByteReader(f.section_data(f.sections_[s.link]));
        for (u64 i = 0; i < count; i++) {
            const u64 base = i * entsize;
            Symbol sym;
            const u32 name = *syms.be32(base);
            if (h.is64()) {
                sym.info = *syms.u8_at(base + 4);
                sym.other = *syms.u8_at(base + 5);
                sym.shndx = *syms.be16(base + 6);
                sym.value = *syms.be64(base + 8);
                sym.size = *syms.be64(base + 16);
            } else {
                sym.value = *syms.be32(base + 4);
                sym.size = *syms.be32(base + 8);
                sym.info = *syms.u8_at(base + 12);
                sym.other = *syms.u8_at(base + 13);
                sym.shndx = *syms.be16(base + 14);
            }
            sym.name = std::string(strtab.cstring_lenient(name, 1024));
            f.symbols_.push_back(std::move(sym));
        }
    }

    // Claimed extent.
    u64 extent = h.header_size();
    if (h.phnum != 0) extent = std::max(extent, saturating_add(h.phoff, u64{h.phnum} * h.phentsize));
    // The section header table counts even when it could not be parsed: a truncated image must look truncated.
    if (h.shnum != 0) extent = std::max(extent, saturating_add(h.shoff, u64{h.shnum} * h.shentsize));
    for (const auto& p : f.segments_) {
        if (p.filesz != 0) extent = std::max(extent, saturating_add(p.offset, p.filesz));
    }
    for (const auto& s : f.sections_) {
        if (s.type != elf::kShtNobits && s.type != elf::kShtNull && s.size != 0) {
            extent = std::max(extent, saturating_add(s.offset, s.size));
        }
    }
    f.claimed_extent_ = extent;
    return f;
}

std::optional<u64> ElfFile::vaddr_to_offset(u64 vaddr, u64 size) const
{
    for (const auto& p : segments_) {
        if (p.type != elf::kPtLoad || !p.data_in_file || p.filesz == 0) continue;
        if (vaddr < p.vaddr) continue;
        const u64 delta = vaddr - p.vaddr;
        if (delta > p.filesz || size > p.filesz - delta) continue;
        return p.offset + delta;
    }
    return std::nullopt;
}

std::optional<std::span<const u8>> ElfFile::read_vaddr(u64 vaddr, u64 size) const
{
    const auto offset = vaddr_to_offset(vaddr, size);
    if (!offset) return std::nullopt;
    return ByteReader(data_).bytes(*offset, size);
}

const SectionHeader* ElfFile::find_section(std::string_view name) const
{
    for (const auto& s : sections_) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

std::span<const u8> ElfFile::section_data(const SectionHeader& section) const
{
    if (!section.data_in_file) return {};
    return data_.subspan(static_cast<usize>(section.offset), static_cast<usize>(section.size));
}

std::span<const u8> ElfFile::segment_data(const ProgramHeader& segment) const
{
    if (!segment.data_in_file) return {};
    return data_.subspan(static_cast<usize>(segment.offset), static_cast<usize>(segment.filesz));
}

u64 ElfFile::extent() const { return std::min<u64>(claimed_extent_, data_.size()); }

} // namespace pspocket::recomp
