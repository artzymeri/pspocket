#include "synthetic.hpp"

#include <algorithm>

#include "pspocket/core/be.h"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::test {

using recomp::nid_of;

void Buffer::reserve_to(usize size)
{
    if (bytes.size() < size) bytes.resize(size, 0);
}

void Buffer::put8(usize offset, u8 v)
{
    reserve_to(offset + 1);
    bytes[offset] = v;
}

void Buffer::put16(usize offset, u16 v)
{
    reserve_to(offset + 2);
    ps_st16(bytes.data() + offset, v);
}

void Buffer::put32(usize offset, u32 v)
{
    reserve_to(offset + 4);
    ps_st32(bytes.data() + offset, v);
}

void Buffer::put64(usize offset, u64 v)
{
    reserve_to(offset + 8);
    ps_st64(bytes.data() + offset, v);
}

void Buffer::put(usize offset, std::span<const u8> data)
{
    reserve_to(offset + data.size());
    std::copy(data.begin(), data.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

void Buffer::put_str(usize offset, std::string_view text)
{
    put(offset, std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
    put8(offset + text.size(), 0);
}

namespace {

constexpr u64 align_up(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }

// ---- ELF writers ----

void elf64_header(Buffer& b, u16 type, u8 osabi, u64 entry, u16 phnum, u64 shoff, u16 shnum, u16 shstrndx)
{
    b.put32(0, 0x7F454C46);
    b.put8(4, 2); // ELFCLASS64
    b.put8(5, 2); // ELFDATA2MSB
    b.put8(6, 1);
    b.put8(7, osabi);
    b.put16(16, type);
    b.put16(18, 21); // EM_PPC64
    b.put32(20, 1);
    b.put64(24, entry);
    b.put64(32, 0x40); // e_phoff
    b.put64(40, shoff);
    b.put32(48, 0);
    b.put16(52, 64);
    b.put16(54, 56);
    b.put16(56, phnum);
    b.put16(58, 64);
    b.put16(60, shnum);
    b.put16(62, shstrndx);
}

void phdr64(Buffer& b, usize index, u32 type, u32 flags, u64 offset, u64 vaddr, u64 paddr, u64 filesz, u64 memsz, u64 align)
{
    const usize at = 0x40 + index * 56;
    b.put32(at + 0, type);
    b.put32(at + 4, flags);
    b.put64(at + 8, offset);
    b.put64(at + 16, vaddr);
    b.put64(at + 24, paddr);
    b.put64(at + 32, filesz);
    b.put64(at + 40, memsz);
    b.put64(at + 48, align);
}

struct Section64 {
    std::string name;
    u32 type = 0;
    u64 flags = 0;
    u64 addr = 0;
    u64 offset = 0;
    u64 size = 0;
    u32 link = 0;
    u32 info = 0;
    u64 addralign = 1;
    u64 entsize = 0;
};

// Writes .shstrtab and the section header table starting at `offset`; returns {shoff, shnum, shstrndx}.
struct SectionTable {
    u64 shoff = 0;
    u16 shnum = 0;
    u16 shstrndx = 0;
};

SectionTable write_sections64(Buffer& b, u64 offset, std::vector<Section64> sections)
{
    // Section 0 (NULL) is implicit; .shstrtab is appended last.
    std::string strtab(1, '\0');
    std::vector<u32> name_offsets;
    sections.push_back({".shstrtab", 3, 0, 0, 0, 0, 0, 0, 1, 0});
    for (const auto& s : sections) {
        name_offsets.push_back(static_cast<u32>(strtab.size()));
        strtab += s.name;
        strtab.push_back('\0');
    }
    const u64 strtab_off = offset;
    b.put(strtab_off, std::span<const u8>(reinterpret_cast<const u8*>(strtab.data()), strtab.size()));
    sections.back().offset = strtab_off;
    sections.back().size = strtab.size();

    const u64 shoff = align_up(strtab_off + strtab.size(), 8);
    b.reserve_to(shoff + 64); // NULL entry
    for (usize i = 0; i < sections.size(); i++) {
        const auto& s = sections[i];
        const usize at = shoff + (i + 1) * 64;
        b.put32(at + 0, name_offsets[i]);
        b.put32(at + 4, s.type);
        b.put64(at + 8, s.flags);
        b.put64(at + 16, s.addr);
        b.put64(at + 24, s.offset);
        b.put64(at + 32, s.size);
        b.put32(at + 40, s.link);
        b.put32(at + 44, s.info);
        b.put64(at + 48, s.addralign);
        b.put64(at + 56, s.entsize);
    }
    return {shoff, static_cast<u16>(sections.size() + 1), static_cast<u16>(sections.size())};
}

// A small PowerPC function body (prologue, nops, epilogue), `words` instructions long.
void ppc_function(Buffer& b, usize at, usize words)
{
    const u32 prologue[] = {0x7C0802A6, 0xF8010010, 0xF821FF91}; // mflr r0; std r0,16(r1); stdu r1,-112(r1)
    const u32 epilogue[] = {0x38210070, 0xE8010010, 0x7C0803A6, 0x4E800020}; // addi; ld r0; mtlr r0; blr
    for (usize i = 0; i < words; i++) b.put32(at + 4 * i, 0x60000000); // nop
    for (usize i = 0; i < 3; i++) b.put32(at + 4 * i, prologue[i]);
    for (usize i = 0; i < 4; i++) b.put32(at + 4 * (words - 4 + i), epilogue[i]);
}

// SDK-style import stub: load the descriptor address from the stub table slot, then jump through it.
void ppc_import_stub(Buffer& b, usize at, u32 slot)
{
    const u32 ha = (slot >> 16) + ((slot & 0x8000) ? 1 : 0);
    b.put32(at + 0x00, 0x3D800000 | (ha & 0xFFFF));     // lis r12, slot@ha
    b.put32(at + 0x04, 0x818C0000 | (slot & 0xFFFF));   // lwz r12, slot@l(r12)
    b.put32(at + 0x08, 0xF8410028);                     // std r2, 40(r1)
    b.put32(at + 0x0C, 0x800C0000);                     // lwz r0, 0(r12)
    b.put32(at + 0x10, 0x804C0004);                     // lwz r2, 4(r12)
    b.put32(at + 0x14, 0x7C0903A6);                     // mtctr r0
    b.put32(at + 0x18, 0x4E800420);                     // bctr
    b.put32(at + 0x1C, 0x60000000);
}

// Library table entry (RPCS3 ppu_prx_module_info), 0x2C bytes.
struct LibEntry {
    u16 version = 1;
    u16 attributes = 0;
    u16 num_func = 0;
    u16 num_var = 0;
    u32 name = 0;
    u32 nids = 0;
    u32 addrs = 0;
    u32 vnids = 0;
    u32 vstubs = 0;
};

void lib_entry(Buffer& b, usize at, const LibEntry& e)
{
    b.put8(at + 0x00, 0x2C);
    b.put8(at + 0x01, 0);
    b.put16(at + 0x02, e.version);
    b.put16(at + 0x04, e.attributes);
    b.put16(at + 0x06, e.num_func);
    b.put16(at + 0x08, e.num_var);
    b.put16(at + 0x0A, 0);
    b.put32(at + 0x10, e.name);
    b.put32(at + 0x14, e.nids);
    b.put32(at + 0x18, e.addrs);
    b.put32(at + 0x1C, e.vnids);
    b.put32(at + 0x20, e.vstubs);
    b.put32(at + 0x24, 0);
    b.put32(at + 0x28, 0);
}

// Deterministic filler for "encrypted" SELF data.
void fill_noise(Buffer& b, usize at, usize size, u32 seed)
{
    u32 x = seed | 1;
    for (usize i = 0; i < size; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b.put8(at + i, static_cast<u8>(x));
    }
}

} // namespace

// ---- SPU ELF ----

SyntheticSpuElf build_spu_elf(const SpuElfSpec& spec)
{
    Buffer b;
    const bool note = !spec.spuname.empty();
    const u16 phnum = note ? 3 : 2;
    constexpr u32 kCode = 0x100;
    constexpr u32 kData = 0x180;
    constexpr u32 kNote = 0x200;

    // Code: nops and a final "bi $0".
    for (u32 i = 0; i < 32; i++) b.put32(kCode + 4 * i, i == 31 ? 0x35000000 : 0x40200000);
    b.put_str(kData, spec.data_text.substr(0, 0x7F));
    b.reserve_to(kData + 0x80);

    u32 note_size = 0;
    if (note) {
        const u32 descsz = static_cast<u32>(align_up(spec.spuname.size() + 1, 4));
        b.put32(kNote + 0, 8); // namesz "SPUNAME\0"
        b.put32(kNote + 4, descsz);
        b.put32(kNote + 8, 1);
        b.put_str(kNote + 12, "SPUNAME");
        b.put_str(kNote + 20, spec.spuname); // RPCS3 reads the name 0x14 bytes into the NOTE segment
        note_size = 20 + descsz;
        b.reserve_to(kNote + note_size);
    }

    // Program headers (ELF32: type, offset, vaddr, paddr, filesz, memsz, flags, align).
    auto phdr = [&](usize i, u32 type, u32 offset, u32 vaddr, u32 filesz, u32 memsz, u32 flags, u32 align) {
        const usize at = 52 + i * 32;
        b.put32(at + 0, type);
        b.put32(at + 4, offset);
        b.put32(at + 8, vaddr);
        b.put32(at + 12, vaddr);
        b.put32(at + 16, filesz);
        b.put32(at + 20, memsz);
        b.put32(at + 24, flags);
        b.put32(at + 28, align);
    };
    phdr(0, 1, kCode, 0x0, 0x80, 0x80, 0x5, 0x80);
    phdr(1, 1, kData, 0x80, 0x80, 0x100, 0x6, 0x80);
    if (note) phdr(2, 4, kNote, 0, note_size, 0, 0x4, 4);

    u32 shoff = 0;
    u16 shnum = 0;
    u16 shstrndx = 0;
    if (spec.sections) {
        std::string strtab(1, '\0');
        auto add_name = [&](std::string_view n) {
            const auto off = static_cast<u32>(strtab.size());
            strtab += n;
            strtab.push_back('\0');
            return off;
        };
        const u32 n_text = add_name(".text");
        const u32 n_data = add_name(".data");
        const u32 n_note = note ? add_name(".note.spu_name") : 0;
        const u32 n_shstrtab = add_name(".shstrtab");
        const u32 strtab_off = static_cast<u32>(align_up(b.bytes.size(), 4));
        b.put(strtab_off, std::span<const u8>(reinterpret_cast<const u8*>(strtab.data()), strtab.size()));
        shoff = static_cast<u32>(align_up(strtab_off + strtab.size(), 4));
        shnum = note ? 5 : 4;
        shstrndx = shnum - 1;
        auto shdr = [&](usize i, u32 name, u32 type, u32 flags, u32 addr, u32 offset, u32 size) {
            const usize at = shoff + i * 40;
            b.put32(at + 0, name);
            b.put32(at + 4, type);
            b.put32(at + 8, flags);
            b.put32(at + 12, addr);
            b.put32(at + 16, offset);
            b.put32(at + 20, size);
            b.put32(at + 24, 0);
            b.put32(at + 28, 0);
            b.put32(at + 32, 4);
            b.put32(at + 36, 0);
        };
        b.reserve_to(shoff + 40); // NULL section
        shdr(1, n_text, 1, 0x6, 0x0, kCode, 0x80);
        shdr(2, n_data, 1, 0x3, 0x80, kData, 0x80);
        if (note) shdr(3, n_note, 7, 0x0, 0, kNote, note_size);
        shdr(shstrndx, n_shstrtab, 3, 0, 0, strtab_off, static_cast<u32>(strtab.size()));
    }

    // ELF32 header.
    b.put32(0, 0x7F454C46);
    b.put8(4, 1); // ELFCLASS32
    b.put8(5, 2); // ELFDATA2MSB
    b.put8(6, 1);
    b.put16(16, 2);  // ET_EXEC
    b.put16(18, 23); // EM_SPU
    b.put32(20, 1);
    b.put32(24, spec.entry);
    b.put32(28, 52);
    b.put32(32, shoff);
    b.put32(36, 0);
    b.put16(40, 52);
    b.put16(42, 32);
    b.put16(44, phnum);
    b.put16(46, 40);
    b.put16(48, shnum);
    b.put16(50, shstrndx);

    SyntheticSpuElf out;
    out.bytes = std::move(b.bytes);
    out.extent = out.bytes.size();
    return out;
}

// ---- PPU executable ----

SyntheticPpuExec build_ppu_exec(const PpuExecOptions& options)
{
    // Layout: LOAD0 (R-X) file [0, 0x4000) at 0x10000; LOAD1 (RW-) file [0x4000, 0x5000) at 0x20000, memsz 0x2000.
    constexpr u32 kText = 0x10000;
    constexpr u32 kTextSize = 0x4000;
    constexpr u32 kData = 0x20000;
    constexpr u32 kDataOffset = 0x4000;
    constexpr u32 kDataFileSize = 0x1000;
    constexpr u32 kDataMemSize = 0x2000;
    auto text = [](u32 off) { return kText + off; };
    auto data = [](u32 off) { return kData + off; };
    auto dfile = [](u32 off) { return static_cast<usize>(kDataOffset + off); };

    Buffer b;
    b.reserve_to(kDataOffset + kDataFileSize);
    SyntheticPpuExec x;

    // Functions: _start, main, func_a, func_b, func_c, export_func, module_start.
    constexpr u32 kCode = 0x200;
    constexpr u32 kFuncCount = 7;
    for (u32 i = 0; i < kFuncCount; i++) ppc_function(b, kCode + 0x40 * i, 16);
    auto func = [&](u32 i) { return text(kCode + 0x40 * i); };

    // .opd: 8 descriptors (the last one aliases main), TOC = .toc + 0x8000.
    constexpr u32 kOpd = 0x000;
    constexpr u32 kToc = 0x100;
    x.toc = data(kToc) + 0x8000;
    const u32 opd_funcs[] = {func(0), func(1), func(2), func(3), func(4), func(5), func(6), func(1)};
    for (u32 i = 0; i < 8; i++) {
        b.put32(dfile(kOpd + 8 * i), opd_funcs[i]);
        b.put32(dfile(kOpd + 8 * i + 4), x.toc);
    }
    for (u32 i = 0; i < 0x40; i += 4) b.put32(dfile(kToc + i), data(0x300)); // a few TOC entries
    x.entry = data(kOpd);
    x.entry_func = func(0);
    x.opd_start = data(kOpd);
    x.opd_entries = 8;
    x.opd_distinct = kFuncCount;
    x.module_start_descriptor = data(kOpd + 8 * 6);
    x.export_func_descriptor = data(kOpd + 8 * 5);
    x.export_func_code = func(5);

    // Imports.
    x.imports = {
        {"cellGcmSys", {"_cellGcmInitBody", "cellGcmSetFlipMode", "cellGcmGetConfiguration"}, {}, {}, {}, 0x0009},
        {"sysPrxForUser", {"sys_lwmutex_lock", "_sys_printf", "sys_ppu_thread_exit", "sys_process_exit"}, {}, {}, {}, 0x0009},
        {"sys_io", {"cellPadInit"}, {}, {}, {}, 0x0009},
        {"sys_fs", {"cellFsOpen"}, {}, {}, {}, 0x0009},
        {"cellSysutil", {"cellSysutilRegisterCallback"}, {}, {}, {}, 0x0009},
        {"cellSpurs", {"cellSpursInitialize"}, {}, {}, {}, 0x2009},
        {"sys_libc", {"memcpy"}, {}, {"_Ctype"}, {0xD97B0687}, 0x0009},
        {"synthLib", {}, {0xDEADBEEF, nid_of("cellFsOpen")}, {}, {}, 0x0009},
    };

    constexpr u32 kStubCode = 0x400;  // text: 0x20 per stub
    constexpr u32 kStubTable = 0x200; // data: one u32 per imported function
    constexpr u32 kRefTable = 0x280;  // data: sys_libc variable reference table
    constexpr u32 kVstub = 0x2A0;     // data: variable stub table
    constexpr u32 kSpursRefs = 0x2C0; // data: cellSpurs function reference table
    constexpr u32 kVarTarget = 0x300; // data: patched by the variable reference
    constexpr u32 kLibStub = 0x400;   // data
    constexpr u32 kLibEnt = 0x600;    // data
    constexpr u32 kExportAddrs = 0x680;
    constexpr u32 kExportVars = 0x700;
    constexpr u32 kProcParam = 0x800;
    constexpr u32 kPrxParam = 0x840;
    constexpr u32 kTls = 0x900;
    constexpr u32 kRodata = 0x600; // text: strings
    constexpr u32 kNids = 0x800;   // text: NID tables

    u32 rodata = kRodata;
    auto add_string = [&](std::string_view s) {
        const u32 at = rodata;
        b.put_str(at, s);
        rodata += static_cast<u32>(s.size() + 1);
        return text(at);
    };

    u32 nid_cursor = kNids;
    u32 func_index = 0;
    for (usize li = 0; li < x.imports.size(); li++) {
        const ExpectedImport& imp = x.imports[li];
        std::vector<u32> fnids;
        for (const auto& n : imp.functions) fnids.push_back(nid_of(n));
        fnids.insert(fnids.end(), imp.raw_function_nids.begin(), imp.raw_function_nids.end());

        LibEntry e;
        e.attributes = imp.attributes;
        e.num_func = static_cast<u16>(fnids.size());
        e.num_var = static_cast<u16>(imp.variable_nids.size());
        e.name = add_string(imp.module);
        e.nids = text(nid_cursor);
        e.addrs = data(kStubTable + 4 * func_index);
        for (usize i = 0; i < fnids.size(); i++) {
            b.put32(nid_cursor, fnids[i]);
            nid_cursor += 4;
            const u32 slot = data(kStubTable + 4 * func_index);
            const u32 stub = text(kStubCode + 0x20 * func_index);
            ppc_import_stub(b, kStubCode + 0x20 * func_index, slot);
            b.put32(dfile(kStubTable + 4 * func_index), stub);
            if (func_index == 0) {
                x.first_stub_slot = slot;
                x.first_stub_code = stub;
            }
            func_index++;
        }
        if (imp.attributes & 0x2000) {
            // Reference tables follow the NIDs (RPCS3 ppu_load_imports): one entry, ADDR16_HA into the stub.
            for (usize i = 0; i < fnids.size(); i++) {
                b.put32(nid_cursor, data(kSpursRefs));
                nid_cursor += 4;
            }
            b.put32(dfile(kSpursRefs + 0), 6);
            b.put32(dfile(kSpursRefs + 4), text(kStubCode + 0x20 * (func_index - 1) + 2));
            b.put32(dfile(kSpursRefs + 8), 0);
            x.spurs_refs = data(kSpursRefs);
        }
        if (!imp.variable_nids.empty()) {
            e.vnids = text(nid_cursor);
            e.vstubs = data(kVstub);
            for (usize i = 0; i < imp.variable_nids.size(); i++) {
                b.put32(nid_cursor, imp.variable_nids[i]);
                nid_cursor += 4;
                b.put32(dfile(kVstub + 4 * static_cast<u32>(i)), data(kRefTable));
            }
            b.put32(dfile(kRefTable + 0), 1); // R_PPC64_ADDR32
            b.put32(dfile(kRefTable + 4), data(kVarTarget));
            b.put32(dfile(kRefTable + 8), 0);
            x.var_ref_table = data(kRefTable);
            x.var_ref_target = data(kVarTarget);
        }
        lib_entry(b, dfile(kLibStub + 0x2C * static_cast<u32>(li)), e);
    }
    x.libstub_start = data(kLibStub);
    x.libstub_end = data(kLibStub + 0x2C * static_cast<u32>(x.imports.size()));

    // Exports: the nameless management library (module_start + module_info) and synthLibExport.
    const u32 exp0_nids = nid_cursor;
    b.put32(nid_cursor, 0xBC9A0086); // module_start
    b.put32(nid_cursor + 4, 0xD7F43016); // module_info
    nid_cursor += 8;
    const u32 exp1_nids = nid_cursor;
    b.put32(nid_cursor, nid_of("synthetic_export_func"));
    b.put32(nid_cursor + 4, nid_of("synthetic_export_var"));
    nid_cursor += 8;
    b.put32(dfile(kExportAddrs + 0), x.module_start_descriptor);
    b.put32(dfile(kExportAddrs + 4), data(kExportVars));
    b.put32(dfile(kExportAddrs + 8), x.export_func_descriptor);
    b.put32(dfile(kExportAddrs + 12), data(kExportVars + 4));
    x.export_var_addr = data(kExportVars + 4);
    LibEntry mgmt;
    mgmt.attributes = 0x8000;
    mgmt.num_func = 1;
    mgmt.num_var = 1;
    mgmt.nids = text(exp0_nids);
    mgmt.addrs = data(kExportAddrs);
    lib_entry(b, dfile(kLibEnt), mgmt);
    LibEntry named;
    named.attributes = 0x0001;
    named.num_func = 1;
    named.num_var = 1;
    named.name = add_string("synthLibExport");
    named.nids = text(exp1_nids);
    named.addrs = data(kExportAddrs + 8);
    lib_entry(b, dfile(kLibEnt + 0x2C), named);
    x.libent_start = data(kLibEnt);
    x.libent_end = data(kLibEnt + 0x58);

    // Middleware identifiers (RAGE, NaturalMotion, Havok, Scaleform).
    add_string("rage::grcDevice::BeginFrame");
    add_string("NaturalMotion euphoria runtime");
    add_string("hkpWorld::stepDeltaTime");
    add_string("GFxMovieView");

    // sys_process_param (RPCS3 ppu_load_exec / sys_process.h).
    x.sdk_version = 0x00360001;
    x.primary_prio = 1001;
    x.primary_stacksize = 0x10000;
    x.malloc_pagesize = 0x100000;
    b.put32(dfile(kProcParam + 0x00), 0x24);
    b.put32(dfile(kProcParam + 0x04), 0x13BCC5F6);
    b.put32(dfile(kProcParam + 0x08), 0x00330000);
    b.put32(dfile(kProcParam + 0x0C), x.sdk_version);
    b.put32(dfile(kProcParam + 0x10), static_cast<u32>(x.primary_prio));
    b.put32(dfile(kProcParam + 0x14), x.primary_stacksize);
    b.put32(dfile(kProcParam + 0x18), x.malloc_pagesize);
    b.put32(dfile(kProcParam + 0x1C), 0);
    b.put32(dfile(kProcParam + 0x20), 0);

    // ppu_proc_prx_param_t.
    b.put32(dfile(kPrxParam + 0x00), 0x28);
    b.put32(dfile(kPrxParam + 0x04), 0x1B434CEC);
    b.put32(dfile(kPrxParam + 0x08), 4);
    b.put32(dfile(kPrxParam + 0x0C), 0);
    b.put32(dfile(kPrxParam + 0x10), x.libent_start);
    b.put32(dfile(kPrxParam + 0x14), x.libent_end);
    b.put32(dfile(kPrxParam + 0x18), x.libstub_start);
    b.put32(dfile(kPrxParam + 0x1C), x.libstub_end);
    b.put16(dfile(kPrxParam + 0x20), 1);

    // TLS image.
    x.tls_vaddr = data(kTls);
    x.tls_filesz = 0x20;
    x.tls_memsz = 0x40;
    for (u32 i = 0; i < x.tls_filesz; i += 4) b.put32(dfile(kTls + i), 0x11111111u * (i / 4 + 1));

    // Embedded SPU programs: a custom job (SPUNAME note) and an Edge zlib job (no note, named by its neighbours).
    constexpr u32 kSpu1 = 0xA00;
    constexpr u32 kSpu2Name = 0xFE0;
    constexpr u32 kSpu2 = 0x1000;
    const auto spu1 = build_spu_elf({"synthetic_job", "custom job data", 0x0, true});
    const auto spu2 = build_spu_elf({"", "edgezlib inflate", 0x40, true});
    b.put(kSpu1, spu1.bytes);
    b.put_str(kSpu2Name, "edge_zlib_job");
    b.put(kSpu2, spu2.bytes);
    x.spu_offsets = {kSpu1, kSpu2};
    x.spu_images = {spu1.bytes, spu2.bytes};
    x.spu_guest_addrs = {text(kSpu1), text(kSpu2)};

    // Sections, symbols.
    u64 end = kDataOffset + kDataFileSize;
    SectionTable table;
    if (options.sections) {
        std::vector<Section64> secs = {
            {".text", 1, 0x6, text(kCode), kCode, 0x40 * kFuncCount, 0, 0, 4, 0},
            {".sceStub.text", 1, 0x6, text(kStubCode), kStubCode, 0x20 * func_index, 0, 0, 4, 0},
            {".rodata", 1, 0x2, text(kRodata), kRodata, 0x200, 0, 0, 4, 0},
            {".rodata.sceFNID", 1, 0x2, text(kNids), kNids, 0x100, 0, 0, 4, 0},
            {".spu_image.job", 1, 0x2, text(kSpu1), kSpu1, spu1.bytes.size(), 0, 0, 16, 0},
            {".spu_image.edge", 1, 0x2, text(kSpu2), kSpu2, spu2.bytes.size(), 0, 0, 16, 0},
            {".opd", 1, 0x3, data(kOpd), dfile(kOpd), 0x40, 0, 0, 8, 0},
            {".toc", 1, 0x3, data(kToc), dfile(kToc), 0x100, 0, 0, 8, 0},
            {".data.sceFStub", 1, 0x3, data(kStubTable), dfile(kStubTable), 4 * func_index, 0, 0, 4, 0},
            {".lib.stub", 1, 0x3, data(kLibStub), dfile(kLibStub), 0x2C * x.imports.size(), 0, 0, 4, 0},
            {".lib.ent", 1, 0x3, data(kLibEnt), dfile(kLibEnt), 0x58, 0, 0, 4, 0},
            {".sys_proc_param", 1, 0x2, data(kProcParam), dfile(kProcParam), 0x24, 0, 0, 4, 0},
            {".sys_proc_prx_param", 1, 0x2, data(kPrxParam), dfile(kPrxParam), 0x28, 0, 0, 4, 0},
            {".tdata", 1, 0x403, data(kTls), dfile(kTls), x.tls_filesz, 0, 0, 16, 0},
            {".bss", 8, 0x3, data(kDataFileSize), end, kDataMemSize - kDataFileSize, 0, 0, 16, 0},
        };
        u64 cursor = end;
        if (options.symbols) {
            struct Sym {
                std::string name;
                u64 value;
                u8 info;
            };
            const std::vector<Sym> syms = {
                {"_start", func(0), 0x12},
                {"main", func(1), 0x12},
                {"_binary_edge_zlib_job_spu_elf_start", text(kSpu2), 0x11},
            };
            std::string strtab(1, '\0');
            const u64 symtab_off = align_up(cursor, 8);
            b.reserve_to(symtab_off + 24); // NULL symbol
            for (usize i = 0; i < syms.size(); i++) {
                const usize at = symtab_off + (i + 1) * 24;
                b.put32(at + 0, static_cast<u32>(strtab.size()));
                b.put8(at + 4, syms[i].info);
                b.put8(at + 5, 0);
                b.put16(at + 6, 1);
                b.put64(at + 8, syms[i].value);
                b.put64(at + 16, 0);
                strtab += syms[i].name;
                strtab.push_back('\0');
            }
            const u64 strtab_off = symtab_off + 24 * (syms.size() + 1);
            b.put(strtab_off, std::span<const u8>(reinterpret_cast<const u8*>(strtab.data()), strtab.size()));
            const u32 strtab_index = static_cast<u32>(secs.size() + 2); // NULL + secs + .symtab
            secs.push_back({".symtab", 2, 0, 0, symtab_off, 24 * (syms.size() + 1), strtab_index, 1, 8, 24});
            secs.push_back({".strtab", 3, 0, 0, strtab_off, strtab.size(), 0, 0, 1, 0});
            cursor = strtab_off + strtab.size();
        }
        table = write_sections64(b, align_up(cursor, 8), std::move(secs));
        if (!options.section_names) table.shstrndx = 0;
    }

    // Program headers and ELF header.
    phdr64(b, 0, 1, 0x5, 0, kText, 0, kTextSize, kTextSize, 0x10000);
    phdr64(b, 1, 1, 0x6, kDataOffset, kData, 0, kDataFileSize, kDataMemSize, 0x10000);
    phdr64(b, 2, 7, 0x4, dfile(kTls), data(kTls), 0, x.tls_filesz, x.tls_memsz, 0x10);
    phdr64(b, 3, 0x60000001, 0, dfile(kProcParam), data(kProcParam), 0, 0x24, 0x24, 4);
    phdr64(b, 4, 0x60000002, 0, dfile(kPrxParam), data(kPrxParam), 0, 0x28, 0x28, 4);
    elf64_header(b, 2, 0x66, x.entry, 5, table.shoff, table.shnum, table.shstrndx);

    x.bytes = std::move(b.bytes);
    return x;
}

// ---- PPU PRX ----

SyntheticPrx build_ppu_prx(const PrxOptions& options)
{
    // LOAD0 (R-X): file 0x100, vaddr 0, 0x700 bytes. LOAD1 (RW-): file 0x800, vaddr 0x1000, filesz 0x200, memsz 0x300.
    // Every pointer is left 0 in the file and filled by PT_SCE_PPURELA relocations, so the parse only succeeds
    // when relocations are applied.
    constexpr u32 kSeg0Off = 0x100;
    constexpr u32 kSeg0Size = 0x700;
    constexpr u32 kSeg1Off = 0x800;
    constexpr u32 kSeg1Vaddr = 0x1000;
    constexpr u32 kRelOff = 0xA00;
    auto s0 = [](u32 off) { return static_cast<usize>(kSeg0Off + off); };

    Buffer b;
    b.reserve_to(kSeg1Off + 0x200);
    SyntheticPrx x;

    // Code: module_start, module_stop, synthPrxFunc; two import stubs.
    ppc_function(b, s0(0x000), 16);
    ppc_function(b, s0(0x040), 16);
    ppc_function(b, s0(0x080), 16);
    for (u32 i = 0; i < 2; i++) {
        b.put32(s0(0x100 + 0x20 * i), 0x3D800000); // lis r12, 0       (ADDR16_HA)
        b.put32(s0(0x104 + 0x20 * i), 0x818C0000); // lwz r12, 0(r12)  (ADDR16_LO)
        b.put32(s0(0x108 + 0x20 * i), 0xF8410028);
        b.put32(s0(0x10C + 0x20 * i), 0x800C0000);
        b.put32(s0(0x110 + 0x20 * i), 0x804C0004);
        b.put32(s0(0x114 + 0x20 * i), 0x7C0903A6);
        b.put32(s0(0x118 + 0x20 * i), 0x4E800420);
    }
    b.put_str(s0(0x200), "synthPrxLib");
    b.put_str(s0(0x210), "sysPrxForUser");
    b.put32(s0(0x300), 0xBC9A0086); // module_start
    b.put32(s0(0x304), 0xAB779874); // module_stop
    b.put32(s0(0x308), nid_of("synthPrxFunc"));
    b.put32(s0(0x310), nid_of("sys_lwmutex_lock"));
    b.put32(s0(0x314), nid_of("_sys_printf"));
    if (options.variables) {
        b.put32(s0(0x30C), kPrxVariableNid); // synthPrxLib's variable, after its function
        b.put32(s0(0x318), kPrxVariableNid); // synthVarLib's, imported
        b.put_str(s0(0x220), "synthVarLib");
        b.put32(kSeg1Off + 0x60, kPrxVariableValue);
    }

    LibEntry mgmt;
    mgmt.attributes = 0x8000;
    mgmt.num_func = 2;
    lib_entry(b, s0(0x400), mgmt);
    LibEntry lib;
    lib.attributes = 0x0001;
    lib.num_func = 1;
    lib.num_var = options.variables ? 1 : 0;
    lib_entry(b, s0(0x42C), lib);
    LibEntry stub;
    stub.attributes = 0x0009;
    stub.num_func = 2;
    lib_entry(b, s0(0x480), stub);
    if (options.variables) {
        LibEntry vstub;
        vstub.attributes = 0x0009;
        vstub.num_var = 1;
        lib_entry(b, s0(0x4AC), vstub);
    }

    // Module info (RPCS3 ppu_prx_library_info) at seg0 + 0x600.
    b.put16(s0(0x600), 0x0000);
    b.put8(s0(0x602), 1);
    b.put8(s0(0x603), 1);
    b.put_str(s0(0x604), "synthetic_prx");
    b.reserve_to(s0(0x634));

    // A branch for the REL24 relocation probe.
    b.put32(s0(0x6A0), 0x48000000);
    b.put16(s0(0x698), 0x0003); // ADDR16_LO_DS keeps the low two bits

    // Relocations.
    u32 count = 0;
    auto rel = [&](u8 index_addr, u64 offset, u8 index_value, u32 type, u64 ptr) {
        const usize at = kRelOff + 24 * count++;
        b.put64(at + 0, offset);
        b.put16(at + 8, 0);
        b.put8(at + 10, index_value);
        b.put8(at + 11, index_addr);
        b.put32(at + 12, type);
        b.put64(at + 16, ptr);
    };
    x.toc = kSeg1Vaddr + 0x100 + 0x8000;
    rel(0, 0x620, 1, 1, 0x8100); // module info: toc
    rel(0, 0x624, 0, 1, 0x400);  // exports start/end
    rel(0, 0x628, 0, 1, 0x458);
    rel(0, 0x62C, 0, 1, 0x480);  // imports start/end
    rel(0, 0x630, 0, 1, options.variables ? 0x4D8 : 0x4AC);
    rel(0, 0x414, 0, 1, 0x300);  // libent[0]: NIDs, addresses
    rel(0, 0x418, 1, 1, 0x040);
    rel(0, 0x43C, 0, 1, 0x200);  // libent[1]: name, NIDs, addresses
    rel(0, 0x440, 0, 1, 0x308);
    rel(0, 0x444, 1, 1, 0x048);
    rel(0, 0x490, 0, 1, 0x210);  // libstub[0]: name, NIDs, stub table
    rel(0, 0x494, 0, 1, 0x310);
    rel(0, 0x498, 1, 1, 0x080);
    for (u32 i = 0; i < 3; i++) {
        rel(1, 8 * i, 0, 1, 0x40 * i);  // descriptor: code
        rel(1, 8 * i + 4, 1, 1, 0x8100); // descriptor: TOC
    }
    rel(1, 0x40, 1, 1, 0x00); // export address tables -> descriptors
    rel(1, 0x44, 1, 1, 0x08);
    rel(1, 0x48, 1, 1, 0x10);
    rel(1, 0x80, 0, 1, 0x100); // stub table -> stub code
    rel(1, 0x84, 0, 1, 0x120);
    if (options.variables) {
        rel(1, 0x4C, 1, 1, 0x60);   // synthPrxLib's variable address
        rel(0, 0x4BC, 0, 1, 0x220); // libstub[1]: name, variable NIDs, variable reference tables
        rel(0, 0x4C8, 0, 1, 0x318);
        rel(0, 0x4CC, 0, 1, 0x31C);
        rel(0, 0x31C, 1, 1, 0xA0);  // its reference table: {ADDR32 at segment 1 + 0xC0, addend 0}, then the end
        b.put32(kSeg1Off + 0xA0, 1);
        rel(1, 0xA4, 1, 1, 0xC0);
    }
    rel(0, 0x102, 1, 6, 0x80); // lis r12, slot@ha
    rel(0, 0x106, 1, 4, 0x80); // lwz r12, slot@l(r12)
    rel(0, 0x6F0, 0xFF, 1, 0x12345678); // absolute ADDR32
    rel(0, 0x680, 0xFF, 4, 0x12348000); // ADDR16_LO
    rel(0, 0x684, 0xFF, 5, 0x12348000); // ADDR16_HI
    rel(0, 0x688, 0xFF, 6, 0x12348000); // ADDR16_HA
    rel(0, 0x690, 1, 38, 0x10);         // ADDR64
    rel(0, 0x698, 0xFF, 57, 0x1234567C); // ADDR16_LO_DS
    rel(0, 0x6A0, 0, 10, 0x80);         // REL24: b to 0x80
    rel(0, 0x10, 0, 99, 0);             // unsupported type
    rel(7, 0x0, 0, 1, 0);               // segment 7 does not exist
    x.relocations = count;
    x.unsupported_relocations = 1;
    x.out_of_range_relocations = 1;

    phdr64(b, 0, 1, 0x5, kSeg0Off, 0, kSeg0Off + 0x600, kSeg0Size, kSeg0Size, 0x100);
    phdr64(b, 1, 1, 0x6, kSeg1Off, kSeg1Vaddr, 0, 0x200, 0x300, 0x100);
    phdr64(b, 2, 0x700000A4, 0, kRelOff, 0, 0, 24ull * count, 0, 8);
    elf64_header(b, 0xFFA4, 0x66, 0, 3, 0, 0, 0);

    x.module_info_vaddr = 0x600;
    x.exports_start = 0x400;
    x.exports_end = 0x458;
    x.imports_start = 0x480;
    x.imports_end = options.variables ? 0x4D8 : 0x4AC;
    x.module_start_descriptor = kSeg1Vaddr;
    x.module_start_code = 0x0;
    x.prx_func_code = 0x80;
    x.stub_slot0 = kSeg1Vaddr + 0x80;
    x.stub_code0 = 0x100;
    x.lis_insn_addr = 0x100;
    x.lwz_insn_addr = 0x104;
    x.bytes = std::move(b.bytes);
    return x;
}

// ---- SELF ----

std::vector<u8> wrap_in_self(std::span<const u8> elf, const SelfOptions& o)
{
    const bool is64 = elf.size() > 4 && elf[4] == 2;
    const u64 ehsize = is64 ? 64 : 52;
    const u64 phentsize = is64 ? 56 : 32;
    const u16 phnum = ps_ld16(elf.data() + (is64 ? 56 : 44));
    const u64 phoff = is64 ? ps_ld64(elf.data() + 32) : ps_ld32(elf.data() + 28);

    constexpr u64 kAppInfo = 0x70;
    constexpr u64 kElfHeader = 0x90;
    const u64 phdr_off = align_up(kElfHeader + ehsize, 0x10);
    const u64 seginfo_off = align_up(phdr_off + phnum * phentsize, 0x10);
    const u64 version_off = seginfo_off + phnum * 0x20ull;
    const u64 control_off = version_off + 0x10;
    const u64 control_size = 0x30 + 0x40 + (o.npdrm ? 0x90 : 0);
    const u64 meta_off = align_up(control_off + control_size, 0x10);
    const u64 meta_size = 0x80 + 0x20 + phnum * 0x30ull;
    const u64 header_size = align_up(meta_off + meta_size, 0x80);

    Buffer b;
    // Segment info and the data area.
    u64 data_size = 0;
    for (u16 i = 0; i < phnum; i++) {
        const u8* p = elf.data() + phoff + i * phentsize;
        const u32 type = ps_ld32(p);
        const u64 filesz = is64 ? ps_ld64(p + 32) : ps_ld32(p + 16);
        const usize at = seginfo_off + i * 0x20ull;
        b.put64(at + 0, filesz ? header_size + data_size : 0);
        b.put64(at + 8, filesz);
        b.put32(at + 16, 1); // plain (not compressed)
        b.put32(at + 20, 0);
        b.put64(at + 24, type == 1 && filesz ? 1 : 0);
        data_size += align_up(filesz, 0x10);
    }
    if (o.debug) data_size = elf.size();

    // SCE header + extended header.
    b.put32(0x00, 0x53434500);
    b.put32(0x04, 2);
    b.put16(0x08, o.debug ? 0x8000 : 0x001C);
    b.put16(0x0A, 1);
    b.put32(0x0C, static_cast<u32>(meta_off - 0x20));
    b.put64(0x10, header_size);
    b.put64(0x18, data_size);
    b.put64(0x20, 3);
    b.put64(0x28, kAppInfo);
    b.put64(0x30, kElfHeader);
    b.put64(0x38, phdr_off);
    b.put64(0x40, 0);
    b.put64(0x48, seginfo_off);
    b.put64(0x50, version_off);
    b.put64(0x58, control_off);
    b.put64(0x60, control_size);
    b.put64(0x68, 0);

    // App info.
    b.put64(kAppInfo + 0, 0x1010000001000003ull);
    b.put32(kAppInfo + 8, 0x01000002);
    b.put32(kAppInfo + 12, o.self_type);
    b.put64(kAppInfo + 16, 0x0001000000000000ull);

    // Plain copies of the ELF header and program headers.
    b.put(kElfHeader, elf.first(ehsize));
    b.put(phdr_off, elf.subspan(phoff, phnum * phentsize));

    // Version info.
    b.put32(version_off + 0, 1);
    b.put32(version_off + 4, 1);
    b.put32(version_off + 8, 0x10);

    // Control info: capability flags (type 1), ELF digest (type 2, zeros here), NPDRM (type 3).
    u64 c = control_off;
    b.put32(c + 0, 1);
    b.put32(c + 4, 0x30);
    b.put64(c + 8, 1);
    b.put32(c + 16, 0x40000000);
    c += 0x30;
    b.put32(c + 0, 2);
    b.put32(c + 4, 0x40);
    b.put64(c + 8, o.npdrm ? 1 : 0);
    b.put64(c + 0x38, 0x0003004000000000ull); // required system version
    c += 0x40;
    if (o.npdrm) {
        b.put32(c + 0, 3);
        b.put32(c + 4, 0x90);
        b.put64(c + 8, 0);
        b.put32(c + 0x10, 0x4E504400); // "NPD\0"
        b.put32(c + 0x14, 2);
        b.put32(c + 0x18, static_cast<u32>(o.npdrm_license));
        b.put32(c + 0x1C, 1);
        b.put_str(c + 0x20, kSelfContentId);
        b.reserve_to(c + 0x90);
    }

    // "Encrypted" metadata and data: filler.
    fill_noise(b, meta_off, meta_size, 0x5E1F);
    b.reserve_to(header_size);
    if (o.debug) {
        b.put(header_size, elf);
    } else {
        fill_noise(b, header_size, data_size, 0xC0FFEE);
    }
    return std::move(b.bytes);
}

} // namespace pspocket::test
