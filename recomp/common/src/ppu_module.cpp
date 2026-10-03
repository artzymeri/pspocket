#include "pspocket/recomp/ppu_module.hpp"

#include <algorithm>
#include <format>
#include <set>

#include "pspocket/core/be.h"
#include "pspocket/recomp/bytes.hpp"

namespace pspocket::recomp {

namespace {

constexpr usize kMaxLibraries = 4096;
constexpr usize kMaxTableEntries = 1'000'000; // all NIDs of all libraries of one module
constexpr usize kMaxRefs = 4096;
constexpr usize kMaxOpdEntries = usize{1} << 22;
constexpr usize kMaxLibraryName = 255;
constexpr u32 kModuleInfoSize = 0x34;       // ppu_prx_library_info
constexpr u32 kLibraryStructSize = 0x2C;    // sizeof(ppu_prx_module_info), RPCS3's stride when size is 0
constexpr u32 kExportStructMinSize = 0x1C;  // through the address table pointer
constexpr u32 kImportStructMinSize = 0x24;  // through the variable stub table pointer
constexpr u64 kGuestSpace = 0x1'0000'0000ull;

struct Budget {
    usize remaining = kMaxTableEntries;
    bool exhausted = false;
    bool take()
    {
        if (remaining == 0) {
            exhausted = true;
            return false;
        }
        remaining--;
        return true;
    }
};

std::optional<ProcessParam> read_process_param(const ElfFile& elf, const ProgramHeader& p, std::vector<std::string>& warnings)
{
    // RPCS3 ppu_load_exec copies the structure out of the segment's file data.
    if (!p.data_in_file || p.filesz < 0x20) {
        warnings.push_back(std::format("PT_PROC_PARAM segment holds {:#x} bytes, less than sys_process_param (0x20)", p.filesz));
        return std::nullopt;
    }
    const ByteReader r(elf.segment_data(p));
    ProcessParam pp;
    pp.vaddr = p.vaddr;
    pp.size = *r.be32(0x00);
    pp.magic = *r.be32(0x04);
    pp.version = *r.be32(0x08);
    pp.sdk_version = *r.be32(0x0C);
    pp.primary_prio = static_cast<s32>(*r.be32(0x10));
    pp.primary_stacksize = *r.be32(0x14);
    pp.malloc_pagesize = *r.be32(0x18);
    pp.ppc_seg = *r.be32(0x1C);
    if (pp.size >= 0x24) {
        if (const auto v = r.be32(0x20)) pp.crash_dump_param_addr = *v;
    }
    if (!pp.magic_ok()) warnings.push_back(std::format("sys_process_param has bad magic {:#010x}", pp.magic));
    if (pp.size < 0x20) warnings.push_back(std::format("sys_process_param size {:#x} is smaller than 0x20", pp.size));
    return pp;
}

std::optional<ProcPrxParam> read_prx_param(const ElfFile& elf, const GuestImage& image, const ProgramHeader& p,
                                           std::vector<std::string>& warnings)
{
    // RPCS3 reads ppu_proc_prx_param_t through the loaded image at p_vaddr; fall back to the file data.
    u8 buf[0x28];
    if (!image.read_bytes(p.vaddr, buf)) {
        const auto data = elf.segment_data(p);
        if (data.size() < sizeof(buf)) {
            warnings.push_back(std::format("PT_PROC_PRX at {:#x} is unreadable", p.vaddr));
            return std::nullopt;
        }
        std::copy_n(data.begin(), sizeof(buf), buf);
    }
    ProcPrxParam pp;
    pp.vaddr = p.vaddr;
    pp.size = ps_ld32(buf + 0x00);
    pp.magic = ps_ld32(buf + 0x04);
    pp.version = ps_ld32(buf + 0x08);
    pp.unk0 = ps_ld32(buf + 0x0C);
    pp.libent_start = ps_ld32(buf + 0x10);
    pp.libent_end = ps_ld32(buf + 0x14);
    pp.libstub_start = ps_ld32(buf + 0x18);
    pp.libstub_end = ps_ld32(buf + 0x1C);
    pp.ver = ps_ld16(buf + 0x20);
    pp.unk1 = ps_ld16(buf + 0x22);
    pp.unk2 = ps_ld32(buf + 0x24);
    if (!pp.magic_ok()) warnings.push_back(std::format("ppu_proc_prx_param has bad magic {:#010x} (RPCS3 refuses to load)", pp.magic));
    return pp;
}

// Walks a libent/libstub table: entries are `size` bytes long (first byte), or 0x2C when that byte is 0.
template <class Fn>
void walk_library_table(const GuestImage& image, u32 start, u32 end, std::string_view what,
                        std::vector<std::string>& warnings, Fn&& fn)
{
    if (start > end) {
        warnings.push_back(std::format("{} table range [{:#x}, {:#x}) is inverted", what, start, end));
        return;
    }
    u64 addr = start;
    usize count = 0;
    while (addr < end) {
        if (count == kMaxLibraries) {
            warnings.push_back(std::format("{} table: stopped after {} libraries", what, kMaxLibraries));
            return;
        }
        const auto size = image.read8(addr);
        if (!size) {
            warnings.push_back(std::format("{} table: entry at {:#x} is unreadable", what, addr));
            return;
        }
        fn(static_cast<u32>(addr), *size);
        addr += *size != 0 ? *size : kLibraryStructSize;
        count++;
    }
}

ImportedLibrary read_import_library(const GuestImage& image, u32 addr, u8 size, Budget& budget)
{
    ImportedLibrary lib;
    lib.addr = addr;
    lib.struct_size = size;

    u8 hdr[kLibraryStructSize] = {};
    const bool full = image.read_bytes(addr, std::span<u8>(hdr, kLibraryStructSize));
    if (!full && !image.read_bytes(addr, std::span<u8>(hdr, kImportStructMinSize))) {
        lib.problems.push_back("library header is unreadable");
        return lib;
    }
    lib.version = ps_ld16(hdr + 0x02);
    lib.attributes = ps_ld16(hdr + 0x04);
    lib.num_func = ps_ld16(hdr + 0x06);
    lib.num_var = ps_ld16(hdr + 0x08);
    lib.num_tlsvar = ps_ld16(hdr + 0x0A);
    const u32 name_ptr = ps_ld32(hdr + 0x10);
    lib.fnid_table = ps_ld32(hdr + 0x14);
    lib.fstub_table = ps_ld32(hdr + 0x18);
    lib.vnid_table = ps_ld32(hdr + 0x1C);
    lib.vstub_table = ps_ld32(hdr + 0x20);
    if (full) {
        lib.tls_table = ps_ld32(hdr + 0x24);
        lib.tls_stub_table = ps_ld32(hdr + 0x28);
    }
    if (size != 0 && size < kImportStructMinSize) lib.problems.push_back(std::format("unexpected structure size {:#x}", size));

    if (name_ptr != 0) {
        if (auto name = image.read_cstring(name_ptr, kMaxLibraryName)) {
            lib.name = std::move(*name);
        } else {
            lib.problems.push_back(std::format("name at {:#x} is unreadable", name_ptr));
        }
    } else {
        lib.problems.push_back("library has no name");
    }

    for (u32 i = 0; i < lib.num_func; i++) {
        if (!budget.take()) break;
        const auto nid = image.read32(u64{lib.fnid_table} + 4ull * i);
        const auto stub = image.read32(u64{lib.fstub_table} + 4ull * i);
        if (!nid || !stub) {
            lib.problems.push_back(std::format("function {} of {}: NID or stub table entry unreadable", i, lib.num_func));
            break;
        }
        ImportedFunction f;
        f.nid = *nid;
        f.slot = static_cast<u32>(u64{lib.fstub_table} + 4ull * i);
        f.stub = *stub;
        if (lib.attributes & kLibAttrFuncRefs) {
            // RPCS3 ppu_load_imports: "0x2000 seems to be correct flag indicating the presence of additional info"
            f.refs = image.read32(u64{lib.fnid_table} + 4ull * (u64{lib.num_func} + i));
        }
        lib.functions.push_back(f);
    }

    for (u32 i = 0; i < lib.num_var; i++) {
        if (!budget.take()) break;
        const auto nid = image.read32(u64{lib.vnid_table} + 4ull * i);
        const auto ref_table = image.read32(u64{lib.vstub_table} + 4ull * i);
        if (!nid || !ref_table) {
            lib.problems.push_back(std::format("variable {} of {}: NID or reference table entry unreadable", i, lib.num_var));
            break;
        }
        ImportedVariable v;
        v.nid = *nid;
        v.ref_table = *ref_table;
        for (u32 k = 0; k < kMaxRefs && v.ref_table != 0; k++) {
            if (!budget.take()) break;
            const u64 at = u64{v.ref_table} + 12ull * k;
            const auto type = image.read32(at);
            const auto ref_addr = image.read32(at + 4);
            const auto addend = image.read32(at + 8);
            if (!type || !ref_addr || !addend) {
                v.refs_unreadable = true;
                break;
            }
            if (*type == 0) break;
            v.refs.push_back({*type, *ref_addr, *addend});
        }
        lib.variables.push_back(std::move(v));
    }

    if (lib.num_tlsvar != 0) {
        lib.problems.push_back(std::format("{} TLS variable imports (tables read best-effort; RPCS3 does not support them)", lib.num_tlsvar));
        for (u32 i = 0; i < lib.num_tlsvar && full; i++) {
            if (!budget.take()) break;
            const auto nid = image.read32(u64{lib.tls_table} + 4ull * i);
            const auto ref = image.read32(u64{lib.tls_stub_table} + 4ull * i);
            if (!nid || !ref) {
                lib.problems.push_back("TLS variable tables are unreadable");
                break;
            }
            lib.tls_variables.push_back({*nid, *ref});
        }
    }
    return lib;
}

ExportedLibrary read_export_library(const GuestImage& image, u32 addr, u8 size, Budget& budget)
{
    ExportedLibrary lib;
    lib.addr = addr;
    lib.struct_size = size;

    u8 hdr[kExportStructMinSize] = {};
    if (!image.read_bytes(addr, hdr)) {
        lib.problems.push_back("library header is unreadable");
        return lib;
    }
    lib.version = ps_ld16(hdr + 0x02);
    lib.attributes = ps_ld16(hdr + 0x04);
    lib.num_func = ps_ld16(hdr + 0x06);
    lib.num_var = ps_ld16(hdr + 0x08);
    lib.num_tlsvar = ps_ld16(hdr + 0x0A);
    const u32 name_ptr = ps_ld32(hdr + 0x10);
    lib.nid_table = ps_ld32(hdr + 0x14);
    lib.addr_table = ps_ld32(hdr + 0x18);

    // RPCS3 ppu_load_exports: a library has flag 0x0001; without it, 0x8000 marks the management functions.
    const bool is_library = (lib.attributes & kLibAttrLibrary) != 0;
    lib.management = !is_library && (lib.attributes & kLibAttrManagement) != 0;
    lib.skipped = !is_library && !lib.management;

    if (name_ptr != 0) {
        if (auto name = image.read_cstring(name_ptr, kMaxLibraryName)) {
            lib.name = std::move(*name);
        } else {
            lib.problems.push_back(std::format("name at {:#x} is unreadable", name_ptr));
        }
    } else if (is_library) {
        lib.problems.push_back("library has no name");
    }
    if (lib.num_tlsvar != 0) lib.problems.push_back(std::format("{} TLS variable exports (unexpected)", lib.num_tlsvar));

    for (u32 i = 0; i < lib.num_func; i++) {
        if (!budget.take()) break;
        const auto nid = image.read32(u64{lib.nid_table} + 4ull * i);
        const auto descriptor = image.read32(u64{lib.addr_table} + 4ull * i);
        if (!nid || !descriptor) {
            lib.problems.push_back(std::format("function {} of {}: NID or address table entry unreadable", i, lib.num_func));
            break;
        }
        ExportedFunction f;
        f.nid = *nid;
        f.descriptor = *descriptor;
        f.entry = image.read32(*descriptor);
        f.toc = image.read32(u64{*descriptor} + 4);
        lib.functions.push_back(f);
    }
    for (u32 i = 0; i < lib.num_var; i++) {
        if (!budget.take()) break;
        const u64 index = u64{lib.num_func} + i;
        const auto nid = image.read32(u64{lib.nid_table} + 4 * index);
        const auto var_addr = image.read32(u64{lib.addr_table} + 4 * index);
        if (!nid || !var_addr) {
            lib.problems.push_back(std::format("variable {} of {}: NID or address table entry unreadable", i, lib.num_var));
            break;
        }
        lib.variables.push_back({*nid, *var_addr, {}});
    }
    return lib;
}

void read_library_tables(const GuestImage& image, u32 exports_start, u32 exports_end, u32 imports_start,
                         u32 imports_end, PpuModule& m)
{
    Budget budget;
    walk_library_table(image, exports_start, exports_end, "export", m.warnings,
        [&](u32 addr, u8 size) { m.exports.push_back(read_export_library(image, addr, size, budget)); });
    walk_library_table(image, imports_start, imports_end, "import", m.warnings,
        [&](u32 addr, u8 size) { m.imports.push_back(read_import_library(image, addr, size, budget)); });
    if (budget.exhausted) m.warnings.push_back(std::format("library tables: stopped after {} entries", kMaxTableEntries));
}

std::optional<PrxModuleInfo> read_module_info(const ElfFile& elf, const GuestImage& image, std::vector<std::string>& warnings)
{
    std::optional<u64> vaddr;
    std::string located_by;
    u64 file_offset = 0;

    // RPCS3 ppu_load_prx: the module info lives at segs[0].addr + progs[0].p_paddr - progs[0].p_offset, i.e. at
    // file offset p_paddr of the first program header.
    const auto& phdrs = elf.segments();
    if (!phdrs.empty() && phdrs[0].paddr != 0) {
        const ProgramHeader& p0 = phdrs[0];
        u64 base = p0.vaddr;
        for (const auto& p : phdrs) {
            if (p.type == elf::kPtLoad) {
                base = p.vaddr;
                break;
            }
        }
        const u64 candidate = base + p0.paddr - p0.offset; // wraps like RPCS3's u32 arithmetic for odd inputs
        if (candidate < kGuestSpace && image.readable(candidate, kModuleInfoSize)) {
            vaddr = candidate;
            file_offset = p0.paddr;
            located_by = "first phdr p_paddr";
        } else {
            warnings.push_back(std::format("module info at p_paddr {:#x} is outside the image", p0.paddr));
        }
    }
    if (!vaddr) {
        if (const SectionHeader* s = elf.find_section(".rodata.sceModuleInfo")) {
            if (image.readable(s->addr, kModuleInfoSize)) {
                vaddr = s->addr;
                file_offset = s->offset;
                located_by = "section .rodata.sceModuleInfo";
            }
        }
    }
    if (!vaddr) {
        warnings.push_back("PRX module info not found");
        return std::nullopt;
    }

    u8 buf[kModuleInfoSize];
    if (!image.read_bytes(*vaddr, buf)) return std::nullopt;
    PrxModuleInfo info;
    info.vaddr = static_cast<u32>(*vaddr);
    info.file_offset = file_offset;
    info.attributes = ps_ld16(buf + 0x00);
    info.version_major = buf[0x02];
    info.version_minor = buf[0x03];
    const auto* name = reinterpret_cast<const char*>(buf + 0x04);
    info.name.assign(name, std::find(name, name + 28, '\0'));
    info.toc = ps_ld32(buf + 0x20);
    info.exports_start = ps_ld32(buf + 0x24);
    info.exports_end = ps_ld32(buf + 0x28);
    info.imports_start = ps_ld32(buf + 0x2C);
    info.imports_end = ps_ld32(buf + 0x30);
    info.located_by = std::move(located_by);
    return info;
}

// ---- function descriptors ----

struct Descriptor {
    u32 func = 0;
    u32 toc = 0;
};

// Reads one descriptor of the given entry size at addr.
std::optional<Descriptor> read_descriptor(const GuestImage& image, u64 addr, u32 entry_size)
{
    if (entry_size == 8) {
        const auto func = image.read32(addr);
        const auto toc = image.read32(addr + 4);
        if (!func || !toc) return std::nullopt;
        return Descriptor{*func, *toc};
    }
    // 24 bytes: {u64 func, u64 toc, u32 func, u32 toc}; the 32-bit copy is what lv2 uses.
    const auto hi_func = image.read32(addr);
    const auto func64 = image.read32(addr + 4);
    const auto hi_toc = image.read32(addr + 8);
    const auto toc64 = image.read32(addr + 12);
    const auto func = image.read32(addr + 16);
    const auto toc = image.read32(addr + 20);
    if (!hi_func || !func64 || !hi_toc || !toc64 || !func || !toc) return std::nullopt;
    if (*hi_func != 0 || *hi_toc != 0 || *func64 != *func || *toc64 != *toc) return std::nullopt;
    return Descriptor{*func, *toc};
}

bool plausible_code(const GuestImage& image, u32 func) { return func % 4 == 0 && image.executable(func); }

// RPCS3 PPUAnalyser.cpp "Probe": the TOC base (toc - 0x8000) must be readable data, below 0x40000000.
bool plausible_toc(const GuestImage& image, u32 toc)
{
    if (toc < 0x8000) return false;
    const u32 base = toc - 0x8000;
    return base % 4 == 0 && base < 0x40000000 && image.readable(base, 4) && !image.executable(base);
}

void finish_opd(OpdInfo& opd, const GuestImage& image)
{
    std::set<u32> funcs;
    for (const auto& e : opd.entries) {
        if (plausible_code(image, e.func)) {
            funcs.insert(e.func);
        } else {
            opd.invalid_entries++;
        }
    }
    opd.distinct_functions = static_cast<u32>(funcs.size());
}

OpdInfo find_opd(const ElfFile& elf, const GuestImage& image, std::optional<u32> anchor, std::optional<u32> toc_hint)
{
    OpdInfo opd;

    // The anchor descriptor tells the entry format and the TOC.
    u32 entry_size = 8;
    if (anchor) {
        opd.anchor = anchor;
        if (const auto first = image.read32(*anchor); first && *first == 0) entry_size = 24;
        if (const auto d = read_descriptor(image, *anchor, entry_size)) {
            opd.entry_func = d->func;
            opd.toc = d->toc;
        } else if (entry_size == 24) {
            entry_size = 8;
            if (const auto d8 = read_descriptor(image, *anchor, 8)) {
                opd.entry_func = d8->func;
                opd.toc = d8->toc;
            }
        }
    }
    if (!opd.toc && toc_hint) opd.toc = toc_hint;

    // Descriptors only exist in file-backed bytes; bounding every walk by them (and by kMaxOpdEntries) keeps a
    // malformed section size or a 4 GB memsz from turning into a multi-gigabyte loop.
    auto file_end = [&](u64 addr) -> u64 {
        const GuestImage::Segment* seg = image.find(addr, 1);
        return seg ? u64{seg->vaddr} + seg->data.size() : 0;
    };
    auto read_all = [&](u64 start, u64 size, u32 es) {
        opd.entries.clear();
        const u64 end = std::min(start + size, file_end(start));
        for (u64 at = start; at + es <= end && opd.entries.size() < kMaxOpdEntries; at += es) {
            if (const auto d = read_descriptor(image, at, es)) opd.entries.push_back({static_cast<u32>(at), d->func, d->toc});
        }
    };

    // 1. A section named .opd.
    if (const SectionHeader* s = elf.find_section(".opd"); s && s->size != 0 && s->addr < kGuestSpace) {
        u32 es = (s->entsize == 8 || s->entsize == 24) ? static_cast<u32>(s->entsize) : entry_size;
        if (s->entsize == 0 && s->size % 24 == 0 && read_descriptor(image, s->addr, 24)) es = 24;
        opd.source = OpdInfo::Source::Section;
        opd.section_name = s->name;
        opd.start = static_cast<u32>(s->addr);
        opd.size = static_cast<u32>(std::min<u64>(s->size, kGuestSpace - s->addr));
        opd.entry_size = es;
        read_all(opd.start, opd.size, es);
        finish_opd(opd, image);
        return opd;
    }

    if (!anchor || !opd.toc) return opd;
    const u32 toc = *opd.toc;
    const u64 anchor_end = file_end(*anchor);
    auto valid = [&](u64 at, u32 es) {
        if (at + es > anchor_end) return false;
        const auto d = read_descriptor(image, at, es);
        return d && d->toc == toc && plausible_code(image, d->func);
    };
    if (!valid(*anchor, entry_size)) return opd;

    // 2. The (unnamed) section that holds the anchor, if every entry in it looks like a descriptor.
    for (usize i = 0; i < elf.sections().size(); i++) {
        const SectionHeader& s = elf.sections()[i];
        if (s.type != elf::kShtProgbits || s.size == 0 || s.size % entry_size != 0 || s.addr >= kGuestSpace) continue;
        if (*anchor < s.addr || *anchor - s.addr >= s.size || (*anchor - s.addr) % entry_size != 0) continue;
        if (s.addr + s.size > file_end(s.addr) || s.size / entry_size > kMaxOpdEntries) continue;
        bool all = true;
        for (u64 at = s.addr; at + entry_size <= s.addr + s.size && all; at += entry_size) {
            const auto d = read_descriptor(image, at, entry_size);
            all = d && plausible_code(image, d->func) && plausible_toc(image, d->toc);
        }
        if (!all) break;
        opd.source = OpdInfo::Source::SectionProbe;
        opd.section_name = s.name.empty() ? std::format("[{}]", i) : s.name;
        opd.start = static_cast<u32>(s.addr);
        opd.size = static_cast<u32>(s.size);
        opd.entry_size = entry_size;
        read_all(opd.start, opd.size, entry_size);
        finish_opd(opd, image);
        return opd;
    }

    // 3. No usable sections: the contiguous run of descriptors with the anchor's TOC around the anchor, inside
    //    the segment that holds it.
    const GuestImage::Segment* seg = image.find(*anchor, entry_size);
    if (!seg) return opd;
    u64 lo = *anchor;
    u64 count = 1;
    while (lo >= u64{seg->vaddr} + entry_size && count < kMaxOpdEntries && valid(lo - entry_size, entry_size)) {
        lo -= entry_size;
        count++;
    }
    u64 hi = u64{*anchor} + entry_size;
    while (count < kMaxOpdEntries && valid(hi, entry_size)) {
        hi += entry_size;
        count++;
    }
    opd.source = OpdInfo::Source::EntrySegment;
    opd.start = static_cast<u32>(lo);
    opd.size = static_cast<u32>(hi - lo);
    opd.entry_size = entry_size;
    read_all(lo, hi - lo, entry_size);
    finish_opd(opd, image);
    return opd;
}

void resolve(ResolvedName& out, const NidDb& db, std::string_view module, u32 nid)
{
    const NidResolution r = db.resolve(module, nid);
    out.name = std::string(r.name);
    out.match = r.match;
}

} // namespace

std::string_view opd_source_name(OpdInfo::Source source)
{
    switch (source) {
    case OpdInfo::Source::None: return "none";
    case OpdInfo::Source::Section: return "section";
    case OpdInfo::Source::SectionProbe: return "section probe";
    case OpdInfo::Source::EntrySegment: return "entry segment scan";
    }
    return "none";
}

usize PpuModule::imported_function_count() const
{
    usize n = 0;
    for (const auto& lib : imports) n += lib.functions.size();
    return n;
}

usize PpuModule::imported_variable_count() const
{
    usize n = 0;
    for (const auto& lib : imports) n += lib.variables.size();
    return n;
}

Result<PpuModule> analyze_ppu_module(const ElfFile& elf)
{
    if (!elf.is_ppu()) return make_error("not a 64-bit PowerPC ELF");
    auto image = GuestImage::from_elf(elf);
    if (!image) return image.error();
    return analyze_ppu_module(elf, *image);
}

Result<PpuModule> analyze_ppu_module(const ElfFile& elf, const GuestImage& image)
{
    if (!elf.is_ppu()) return make_error("not a 64-bit PowerPC ELF");

    PpuModule m;
    m.is_prx = elf.header().type == elf::kTypeScePpuRelExec;
    m.relocations = image.relocations();
    m.warnings = image.warnings();

    for (const auto& p : elf.segments()) {
        if (p.type == elf::kPtTls) {
            if (p.vaddr >= kGuestSpace || p.filesz > 0xFFFFFFFF || p.memsz > 0xFFFFFFFF) {
                m.warnings.push_back("PT_TLS segment does not fit 32 bits");
                continue;
            }
            m.tls = TlsInfo{static_cast<u32>(p.vaddr), static_cast<u32>(p.filesz), static_cast<u32>(p.memsz),
                static_cast<u32>(std::min<u64>(p.align, 0xFFFFFFFF))};
        } else if (p.type == elf::kPtProcParam && !m.is_prx) {
            if (m.process_param) {
                m.warnings.push_back("more than one PT_PROC_PARAM segment");
                continue;
            }
            m.process_param = read_process_param(elf, p, m.warnings);
        } else if (p.type == elf::kPtProcPrx && !m.is_prx) {
            if (m.prx_param) {
                m.warnings.push_back("more than one PT_PROC_PRX segment");
                continue;
            }
            if (p.filesz == 0) continue; // RPCS3 ignores an empty PT_PROC_PRX (overlays)
            m.prx_param = read_prx_param(elf, image, p, m.warnings);
        }
    }

    std::optional<u32> anchor;
    std::optional<u32> toc_hint;
    if (m.is_prx) {
        m.module_info = read_module_info(elf, image, m.warnings);
        if (m.module_info) {
            read_library_tables(image, m.module_info->exports_start, m.module_info->exports_end,
                m.module_info->imports_start, m.module_info->imports_end, m);
            toc_hint = m.module_info->toc;
        }
        // Anchor the descriptor search on module_start, else on any exported function.
        for (const auto& lib : m.exports) {
            for (const auto& f : lib.functions) {
                if (!anchor || (lib.management && f.nid == kNidModuleStart)) anchor = f.descriptor;
            }
        }
    } else {
        if (m.prx_param) {
            read_library_tables(image, m.prx_param->libent_start, m.prx_param->libent_end, m.prx_param->libstub_start,
                m.prx_param->libstub_end, m);
        } else if (elf.header().type == elf::kTypeExec) {
            m.warnings.push_back("no PT_PROC_PRX segment: import and export tables unavailable");
        }
        if (elf.header().entry != 0 && elf.header().entry < kGuestSpace) anchor = static_cast<u32>(elf.header().entry);
    }

    m.opd = find_opd(elf, image, anchor, toc_hint);
    return m;
}

void resolve_names(PpuModule& module, const NidDb& db)
{
    for (auto& lib : module.imports) {
        for (auto& f : lib.functions) resolve(f.resolved, db, lib.name, f.nid);
        for (auto& v : lib.variables) resolve(v.resolved, db, lib.name, v.nid);
    }
    for (auto& lib : module.exports) {
        const std::string_view name = lib.management ? std::string_view() : std::string_view(lib.name);
        for (auto& f : lib.functions) resolve(f.resolved, db, name, f.nid);
        for (auto& v : lib.variables) resolve(v.resolved, db, name, v.nid);
    }
}

} // namespace pspocket::recomp
