#include <catch_amalgamated.hpp>

#include <algorithm>
#include <string>

#include "pspocket/core/be.h"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/guest_image.hpp"
#include "pspocket/recomp/inventory.hpp"
#include "pspocket/recomp/json_writer.hpp"
#include "pspocket/recomp/strings.hpp"
#include "synthetic.hpp"

using namespace pspocket;
using namespace pspocket::recomp;
using pspocket::test::build_ppu_exec;
using pspocket::test::build_ppu_prx;
using pspocket::test::build_spu_elf;
using pspocket::test::wrap_in_self;

namespace {

const ImportedLibrary* find_import(const PpuModule& m, std::string_view name)
{
    for (const auto& lib : m.imports) {
        if (lib.name == name) return &lib;
    }
    return nullptr;
}

bool has_product(const std::vector<MiddlewareHit>& hits, std::string_view product)
{
    return std::any_of(hits.begin(), hits.end(), [&](const MiddlewareHit& h) { return h.product == product; });
}

} // namespace

TEST_CASE("ELF header and program headers of a synthetic PPU executable")
{
    const auto x = build_ppu_exec();
    const auto elf = ElfFile::parse(x.bytes);
    REQUIRE(elf);
    const ElfHeader& h = elf->header();
    CHECK(h.is64());
    CHECK(h.type == elf::kTypeExec);
    CHECK(h.machine == elf::kMachinePpc64);
    CHECK(h.os_abi == elf::kOsAbiCellLv2);
    CHECK(h.entry == x.entry);
    CHECK(elf->is_ppu());
    CHECK(elf->warnings().empty());

    const auto& seg = elf->segments();
    REQUIRE(seg.size() == 5);
    CHECK(seg[0].type == elf::kPtLoad);
    CHECK(seg[0].vaddr == 0x10000);
    CHECK(seg[1].type == elf::kPtLoad);
    CHECK(seg[1].memsz == 0x2000);
    CHECK(seg[2].type == elf::kPtTls);
    CHECK(seg[3].type == elf::kPtProcParam);
    CHECK(seg[4].type == elf::kPtProcPrx);
    CHECK(elf::program_type_name(seg[3].type) == "PROC_PARAM");
    CHECK(elf::segment_flags_string(seg[0].flags) == "R-X");
    CHECK(elf::segment_flags_string(0x00700000 | 0x6) == "RW- spu:RWX");

    // vaddr -> file offset.
    CHECK(elf->vaddr_to_offset(0x10200) == 0x200u);
    CHECK(elf->vaddr_to_offset(0x20000) == 0x4000u);
    CHECK(elf->vaddr_to_offset(0x20FFC, 4) == 0x4FFCu);
    CHECK_FALSE(elf->vaddr_to_offset(0x20FFE, 4)); // crosses the end of the file data (bss follows)
    CHECK_FALSE(elf->vaddr_to_offset(0x21000));    // bss: not in the file
    CHECK_FALSE(elf->vaddr_to_offset(0x8000));
    const auto entry_desc = elf->read_vaddr(x.entry, 8);
    REQUIRE(entry_desc);
    CHECK(ps_ld32(entry_desc->data()) == x.entry_func);
}

TEST_CASE("sections and symbols")
{
    const auto x = build_ppu_exec();
    const auto elf = ElfFile::parse(x.bytes);
    REQUIRE(elf);
    const auto* opd = elf->find_section(".opd");
    REQUIRE(opd);
    CHECK(opd->addr == x.opd_start);
    CHECK(opd->size == 0x40);
    const auto* bss = elf->find_section(".bss");
    REQUIRE(bss);
    CHECK(bss->type == elf::kShtNobits);
    CHECK_FALSE(bss->data_in_file);
    CHECK(elf->find_section(".lib.stub"));
    CHECK(elf->find_section(".shstrtab"));
    REQUIRE(elf->symbols().size() == 4);
    CHECK(elf->symbols()[1].name == "_start");
    CHECK(elf->symbols()[1].value == x.entry_func);
    CHECK(elf->symbols()[3].name == "_binary_edge_zlib_job_spu_elf_start");

    // Without the name table, sections keep their headers but lose their names.
    const auto stripped = build_ppu_exec({true, false, false});
    const auto elf2 = ElfFile::parse(stripped.bytes);
    REQUIRE(elf2);
    CHECK_FALSE(elf2->sections().empty());
    CHECK_FALSE(elf2->find_section(".opd"));
    CHECK(elf2->symbols().empty());
}

TEST_CASE("PPU executable: process parameters, TLS, import and export tables")
{
    const auto x = build_ppu_exec();
    const auto elf = ElfFile::parse(x.bytes);
    REQUIRE(elf);
    auto analyzed = analyze_ppu_module(*elf);
    REQUIRE(analyzed);
    PpuModule& m = *analyzed;
    resolve_names(m, NidDb::builtin());
    CHECK_FALSE(m.is_prx);
    CHECK(m.warnings.empty());

    REQUIRE(m.process_param);
    CHECK(m.process_param->magic_ok());
    CHECK(m.process_param->size == 0x24);
    CHECK(m.process_param->sdk_version == x.sdk_version);
    CHECK(m.process_param->primary_prio == x.primary_prio);
    CHECK(m.process_param->primary_stacksize == x.primary_stacksize);
    CHECK(m.process_param->malloc_pagesize == x.malloc_pagesize);
    CHECK(m.process_param->ppc_seg == 0);
    CHECK(m.process_param->crash_dump_param_addr == 0u);

    REQUIRE(m.prx_param);
    CHECK(m.prx_param->magic_ok());
    CHECK(m.prx_param->libent_start == x.libent_start);
    CHECK(m.prx_param->libent_end == x.libent_end);
    CHECK(m.prx_param->libstub_start == x.libstub_start);
    CHECK(m.prx_param->libstub_end == x.libstub_end);

    REQUIRE(m.tls);
    CHECK(m.tls->vaddr == x.tls_vaddr);
    CHECK(m.tls->image_size == x.tls_filesz);
    CHECK(m.tls->size == x.tls_memsz);

    // Imports: every library, NID, name and stub.
    REQUIRE(m.imports.size() == x.imports.size());
    for (usize i = 0; i < x.imports.size(); i++) {
        const auto& want = x.imports[i];
        const auto& got = m.imports[i];
        INFO(want.module);
        CHECK(got.name == want.module);
        CHECK(got.attributes == want.attributes);
        CHECK(got.problems.empty());
        REQUIRE(got.functions.size() == want.functions.size() + want.raw_function_nids.size());
        for (usize f = 0; f < want.functions.size(); f++) {
            CHECK(got.functions[f].nid == nid_of(want.functions[f]));
            CHECK(got.functions[f].resolved.name == want.functions[f]);
            CHECK(got.functions[f].resolved.match == NidMatch::Exact);
        }
        REQUIRE(got.variables.size() == want.variable_nids.size());
        for (usize v = 0; v < want.variable_nids.size(); v++) {
            CHECK(got.variables[v].nid == want.variable_nids[v]);
            CHECK(got.variables[v].resolved.name == want.variables[v]);
        }
    }
    CHECK(m.imported_function_count() == 14);
    CHECK(m.imported_variable_count() == 1);
    CHECK(m.imports[0].functions[0].slot == x.first_stub_slot);
    CHECK(m.imports[0].functions[0].stub == x.first_stub_code);

    const auto* synth = find_import(m, "synthLib");
    REQUIRE(synth);
    CHECK(synth->functions[0].nid == 0xDEADBEEFu);
    CHECK(synth->functions[0].resolved.match == NidMatch::Unknown);
    CHECK(synth->functions[1].resolved.match == NidMatch::OtherModule);
    CHECK(synth->functions[1].resolved.name == "cellFsOpen");

    const auto* spurs = find_import(m, "cellSpurs");
    REQUIRE(spurs);
    CHECK(spurs->functions[0].refs == x.spurs_refs);

    const auto* libc = find_import(m, "sys_libc");
    REQUIRE(libc);
    REQUIRE(libc->variables.size() == 1);
    CHECK(libc->variables[0].ref_table == x.var_ref_table);
    REQUIRE(libc->variables[0].refs.size() == 1);
    CHECK(libc->variables[0].refs[0].type == 1);
    CHECK(libc->variables[0].refs[0].addr == x.var_ref_target);
    CHECK_FALSE(libc->variables[0].refs_unreadable);

    // Exports: the nameless management library and a named one.
    REQUIRE(m.exports.size() == 2);
    const auto& mgmt = m.exports[0];
    CHECK(mgmt.management);
    CHECK(mgmt.name.empty());
    REQUIRE(mgmt.functions.size() == 1);
    CHECK(mgmt.functions[0].nid == 0xBC9A0086u);
    CHECK(mgmt.functions[0].resolved.name == "module_start");
    CHECK(mgmt.functions[0].descriptor == x.module_start_descriptor);
    REQUIRE(mgmt.variables.size() == 1);
    CHECK(mgmt.variables[0].resolved.name == "module_info");
    const auto& named = m.exports[1];
    CHECK_FALSE(named.management);
    CHECK(named.name == "synthLibExport");
    REQUIRE(named.functions.size() == 1);
    CHECK(named.functions[0].nid == nid_of("synthetic_export_func"));
    CHECK(named.functions[0].descriptor == x.export_func_descriptor);
    CHECK(named.functions[0].entry == x.export_func_code);
    CHECK(named.functions[0].toc == x.toc);
    CHECK(named.functions[0].resolved.match == NidMatch::Unknown);
    REQUIRE(named.variables.size() == 1);
    CHECK(named.variables[0].addr == x.export_var_addr);

    // Names supplied at runtime resolve the game's own exports.
    NidDb extra = NidDb::builtin();
    REQUIRE(extra.load_text("[synthLibExport]\nsynthetic_export_func\nsynthetic_export_var\n"));
    resolve_names(m, extra);
    CHECK(m.exports[1].functions[0].resolved.name == "synthetic_export_func");
    CHECK(m.exports[1].variables[0].resolved.name == "synthetic_export_var");
}

TEST_CASE("function descriptors: .opd section, section probe and entry-segment scan")
{
    struct Variant {
        test::PpuExecOptions options;
        OpdInfo::Source source;
    };
    const Variant variants[] = {
        {{true, true, true}, OpdInfo::Source::Section},
        {{true, false, false}, OpdInfo::Source::SectionProbe},
        {{false, false, false}, OpdInfo::Source::EntrySegment},
    };
    for (const auto& v : variants) {
        const auto x = build_ppu_exec(v.options);
        const auto elf = ElfFile::parse(x.bytes);
        REQUIRE(elf);
        const auto m = analyze_ppu_module(*elf);
        REQUIRE(m);
        INFO(opd_source_name(v.source));
        CHECK(m->opd.source == v.source);
        CHECK(m->opd.start == x.opd_start);
        CHECK(m->opd.entry_size == 8);
        CHECK(m->opd.entries.size() == x.opd_entries);
        CHECK(m->opd.distinct_functions == x.opd_distinct);
        CHECK(m->opd.invalid_entries == 0);
        CHECK(m->opd.toc == x.toc);
        CHECK(m->opd.entry_func == x.entry_func);
        CHECK(m->opd.anchor == x.entry);
        // The import tables don't depend on sections.
        CHECK(m->imports.size() == x.imports.size());
    }
}

TEST_CASE("embedded SPU images: extent, hashes, names, classification")
{
    struct Variant {
        test::PpuExecOptions options;
        std::string spu2_name;
        std::string spu2_source;
    };
    const Variant variants[] = {
        {{true, true, true}, "edge_zlib_job_spu_elf", "symbol"},
        {{true, true, false}, ".spu_image.edge", "section"},
        {{true, false, false}, "edge_zlib_job", "nearby string"},
        {{false, false, false}, "edge_zlib_job", "nearby string"},
    };
    for (const auto& v : variants) {
        const auto x = build_ppu_exec(v.options);
        const FileReport r = inspect_bytes("synthetic.elf", x.bytes);
        INFO(v.spu2_source);
        REQUIRE(r.spu_programs.size() == 2);
        for (usize i = 0; i < 2; i++) {
            const SpuImage& img = r.spu_programs[i].image;
            CHECK(img.offset == x.spu_offsets[i]);
            CHECK(img.size == x.spu_images[i].size());
            CHECK_FALSE(img.truncated);
            CHECK(img.sha1 == Sha1::hash(x.spu_images[i]));
            CHECK(img.guest_addr == x.spu_guest_addrs[i]);
            CHECK(img.container_segment == 0u);
            CHECK(img.rpcs3_hash.has_value());
            CHECK(img.segments.size() == img.phnum);
            CHECK(img.load_bytes == 0x100);
        }
        CHECK(r.spu_programs[0].image.name == "synthetic_job");
        CHECK(r.spu_programs[0].image.name_source == "SPUNAME note");
        CHECK(r.spu_programs[0].image.entry == 0x0u);
        CHECK(r.spu_programs[0].classification == SpuProgramClass::Custom);
        CHECK(r.spu_programs[1].image.name == v.spu2_name);
        CHECK(r.spu_programs[1].image.name_source == v.spu2_source);
        CHECK(r.spu_programs[1].image.entry == 0x40u);
        CHECK(r.spu_programs[1].classification == SpuProgramClass::SonyMiddleware);
        CHECK(has_product(r.spu_programs[1].middleware, "Edge zlib"));
    }
}

TEST_CASE("SPU image extent covers the section header table; truncated images are flagged")
{
    const auto spu = build_spu_elf({"name", "data", 0, true});
    const auto img = measure_spu_image(spu.bytes);
    REQUIRE(img);
    CHECK(img->size == spu.extent);
    CHECK(img->shnum == 5);
    CHECK_FALSE(img->truncated);

    // Without sections the image ends with the NOTE segment.
    const auto bare = build_spu_elf({"name", "data", 0, false});
    const auto bare_img = measure_spu_image(bare.bytes);
    REQUIRE(bare_img);
    CHECK(bare_img->size == bare.extent);
    CHECK(bare_img->shnum == 0);

    // Truncated: the measured size is clamped and the image is marked.
    const std::span<const u8> cut = std::span<const u8>(spu.bytes).first(spu.bytes.size() - 16);
    const auto cut_img = measure_spu_image(cut);
    REQUIRE(cut_img);
    CHECK(cut_img->truncated);
    CHECK(cut_img->size == cut.size());

    // A PPU ELF is not an SPU image; neither is a little-endian ELF32.
    CHECK_FALSE(measure_spu_image(build_ppu_exec().bytes));
    auto le = spu.bytes;
    le[5] = 1;
    CHECK_FALSE(measure_spu_image(le));
}

TEST_CASE("RPCS3-compatible hashes react to code and data, not to section metadata")
{
    const auto x = build_ppu_exec();
    const auto base = ElfFile::parse(x.bytes);
    REQUIRE(base);
    const auto h0 = rpcs3_ppu_executable_hash(*base);
    REQUIRE(h0);
    CHECK(rpcs3_ppu_hash_string(*h0).starts_with("PPU-"));
    CHECK(rpcs3_ppu_hash_string(*h0).size() == 44);

    auto code_changed = x.bytes;
    code_changed[0x204] ^= 0xFF; // inside .text (LOAD 0)
    const auto h1 = rpcs3_ppu_executable_hash(*ElfFile::parse(code_changed));
    CHECK(h1 != h0);

    auto names_changed = x.bytes;
    const auto* shstrtab = base->find_section(".shstrtab");
    REQUIRE(shstrtab);
    names_changed[shstrtab->offset + 1] ^= 0x20; // outside every segment
    const auto h2 = rpcs3_ppu_executable_hash(*ElfFile::parse(names_changed));
    CHECK(h2 == h0);

    // SPU: LOAD data is hashed, the section header table is not.
    const auto spu = build_spu_elf({"name", "data", 0, true});
    const auto s0 = measure_spu_image(spu.bytes)->rpcs3_hash;
    auto spu_code = spu.bytes;
    spu_code[0x100] ^= 1;
    CHECK(measure_spu_image(spu_code)->rpcs3_hash != s0);
    auto spu_meta = spu.bytes;
    spu_meta[spu_meta.size() - 4] ^= 1; // last section header's entsize
    CHECK(measure_spu_image(spu_meta)->rpcs3_hash == s0);
    CHECK(measure_spu_image(spu_meta)->sha1 != measure_spu_image(spu.bytes)->sha1);
}

TEST_CASE("inventory of a synthetic PPU executable: middleware and Phase 0 summary")
{
    const auto x = build_ppu_exec();
    InspectOptions options;
    options.collect_strings = true;
    options.min_string_length = 8;
    const FileReport r = inspect_bytes("synthetic.elf", x.bytes, options);
    CHECK(r.kind == FileKind::PpuExecutable);
    CHECK(r.errors.empty());
    CHECK(r.warnings.empty());
    CHECK_FALSE(r.encrypted());
    CHECK(r.sha256 == Sha256::hash(x.bytes));
    CHECK(r.rpcs3_ppu_hash.has_value());
    REQUIRE(r.ppu);

    for (const auto* product : {"SPURS", "RAGE", "Euphoria", "Havok", "Scaleform GFx", "Edge zlib"}) {
        INFO(product);
        CHECK(has_product(r.middleware, product));
    }
    CHECK_FALSE(has_product(r.middleware, "FMOD"));
    CHECK_FALSE(has_product(r.middleware, "Bink"));
    const auto rage = std::find_if(r.middleware.begin(), r.middleware.end(), [](const MiddlewareHit& h) { return h.product == "RAGE"; });
    REQUIRE(rage != r.middleware.end());
    CHECK(rage->origin == MiddlewareOrigin::ThirdParty);
    CHECK(rage->count == 1);

    const bool found_string = std::any_of(r.strings.begin(), r.strings.end(),
        [](const FoundString& s) { return s.text == "rage::grcDevice::BeginFrame"; });
    CHECK(found_string);

    const Phase0Summary& s = r.phase0;
    CHECK(s.function_estimate == 7u);
    CHECK(s.function_estimate_source.find("section") != std::string::npos);
    CHECK(s.imports.size() == 8);
    CHECK(s.imports.at("sysPrxForUser").functions == 4);
    CHECK(s.imports.at("sys_libc").variables == 1);
    CHECK(s.imports.at("synthLib").unresolved == 1);
    CHECK(s.imports.at("synthLib").other_module == 1);
    REQUIRE(s.unresolved.size() == 1);
    CHECK(s.unresolved[0].module == "synthLib");
    CHECK(s.unresolved[0].nid == 0xDEADBEEFu);
    CHECK(s.spu_programs == 2);
    CHECK(s.spu_sony == 1);
    CHECK(s.spu_custom == 1);
    CHECK(s.spu_third_party == 0);
    CHECK(s.spu_bytes_sony == x.spu_images[1].size());
}

TEST_CASE("PRX: module info via p_paddr, relocations, exports and imports")
{
    const auto x = build_ppu_prx();
    const auto elf = ElfFile::parse(x.bytes);
    REQUIRE(elf);
    CHECK(elf->header().type == elf::kTypeScePpuRelExec);

    const auto image = GuestImage::from_elf(*elf);
    REQUIRE(image);
    const auto& rel = image->relocations();
    CHECK(rel.total == x.relocations);
    CHECK(rel.unsupported == x.unsupported_relocations);
    CHECK(rel.out_of_range == x.out_of_range_relocations);
    CHECK(rel.applied == x.relocations - x.unsupported_relocations - x.out_of_range_relocations);
    CHECK(rel.by_type.at(1) > 20);

    // Relocation semantics (RPCS3 ppu_load_prx), checked on the relocated image.
    CHECK(image->read32(0x6F0) == 0x12345678u);           // ADDR32, absolute
    CHECK(image->read16(0x680) == 0x8000u);               // ADDR16_LO
    CHECK(image->read16(0x684) == 0x1234u);               // ADDR16_HI
    CHECK(image->read16(0x688) == 0x1235u);               // ADDR16_HA (low half has bit 15 set)
    CHECK(image->read64(0x690) == 0x1010u);               // ADDR64, segment 1 + 0x10
    CHECK(image->read16(0x698) == 0x567Fu);               // ADDR16_LO_DS keeps the low two bits
    CHECK(image->read32(0x6A0) == 0x4BFFF9E0u);           // REL24: b -0x620
    CHECK(image->read32(x.lis_insn_addr) == (0x3D800000u | ((x.stub_slot0 + 0x8000) >> 16)));
    CHECK(image->read32(x.lwz_insn_addr) == (0x818C0000u | (x.stub_slot0 & 0xFFFF)));

    auto analyzed = analyze_ppu_module(*elf, *image);
    REQUIRE(analyzed);
    PpuModule& m = *analyzed;
    resolve_names(m, NidDb::builtin());
    CHECK(m.is_prx);
    CHECK_FALSE(m.process_param);
    REQUIRE(m.module_info);
    CHECK(m.module_info->vaddr == x.module_info_vaddr);
    CHECK(m.module_info->located_by == "first phdr p_paddr");
    CHECK(m.module_info->name == "synthetic_prx");
    CHECK(m.module_info->version_major == 1);
    CHECK(m.module_info->version_minor == 1);
    CHECK(m.module_info->toc == x.toc);
    CHECK(m.module_info->exports_start == x.exports_start);
    CHECK(m.module_info->exports_end == x.exports_end);
    CHECK(m.module_info->imports_start == x.imports_start);
    CHECK(m.module_info->imports_end == x.imports_end);

    REQUIRE(m.exports.size() == 2);
    CHECK(m.exports[0].management);
    REQUIRE(m.exports[0].functions.size() == 2);
    CHECK(m.exports[0].functions[0].resolved.name == "module_start");
    CHECK(m.exports[0].functions[1].resolved.name == "module_stop");
    CHECK(m.exports[0].functions[0].descriptor == x.module_start_descriptor);
    CHECK(m.exports[0].functions[0].entry == x.module_start_code);
    CHECK(m.exports[0].functions[0].toc == x.toc);
    CHECK(m.exports[1].name == "synthPrxLib");
    REQUIRE(m.exports[1].functions.size() == 1);
    CHECK(m.exports[1].functions[0].entry == x.prx_func_code);

    REQUIRE(m.imports.size() == 1);
    CHECK(m.imports[0].name == "sysPrxForUser");
    REQUIRE(m.imports[0].functions.size() == 2);
    CHECK(m.imports[0].functions[0].resolved.name == "sys_lwmutex_lock");
    CHECK(m.imports[0].functions[1].resolved.name == "_sys_printf");
    CHECK(m.imports[0].functions[0].slot == x.stub_slot0);
    CHECK(m.imports[0].functions[0].stub == x.stub_code0);

    // Descriptors: found from module_start (no sections in this PRX).
    CHECK(m.opd.source == OpdInfo::Source::EntrySegment);
    CHECK(m.opd.anchor == x.module_start_descriptor);
    CHECK(m.opd.entries.size() == 3);
    CHECK(m.opd.distinct_functions == 3);
    CHECK(m.opd.toc == x.toc);

    const FileReport r = inspect_bytes("synthetic.prx", x.bytes);
    CHECK(r.kind == FileKind::PpuPrx);
    CHECK(r.errors.empty());
    CHECK(r.phase0.function_estimate == 3u);
    CHECK(r.phase0.unresolved.empty());

    // Without p_paddr (and without sections) the module info can't be located: a warning, not a failure.
    auto no_paddr = x.bytes;
    ps_st64(no_paddr.data() + 0x40 + 24, 0);
    const FileReport r2 = inspect_bytes("no_paddr.prx", no_paddr);
    CHECK(r2.kind == FileKind::PpuPrx);
    CHECK(r2.errors.empty());
    REQUIRE(r2.ppu);
    CHECK_FALSE(r2.ppu->module_info);
    CHECK(r2.ppu->imports.empty());
    const bool warned = std::any_of(r2.warnings.begin(), r2.warnings.end(),
        [](const std::string& w) { return w.find("module info not found") != std::string::npos; });
    CHECK(warned);
}

TEST_CASE("standalone SPU ELF")
{
    const auto spu = build_spu_elf({"standalone_job", "MLAA filter", 0x80, true});
    const FileReport r = inspect_bytes("job.elf", spu.bytes);
    CHECK(r.kind == FileKind::SpuElf);
    CHECK(r.errors.empty());
    REQUIRE(r.spu_programs.size() == 1);
    CHECK(r.spu_programs[0].image.name == "standalone_job");
    CHECK(r.spu_programs[0].image.size == spu.bytes.size());
    CHECK(r.spu_programs[0].image.entry == 0x80u);
    CHECK(r.spu_programs[0].classification == SpuProgramClass::SonyMiddleware); // "MLAA"
    CHECK_FALSE(r.ppu);
}

TEST_CASE("SELF: detected, plaintext headers parsed, reported as encrypted")
{
    const auto x = build_ppu_exec();
    const auto self = wrap_in_self(x.bytes);
    CHECK(has_sce_magic(self));
    const FileReport r = inspect_bytes("EBOOT.BIN", self);
    CHECK(r.kind == FileKind::Self);
    CHECK(r.encrypted());
    CHECK(r.errors.empty());
    CHECK_FALSE(r.ppu);          // nothing to analyze without decryption
    CHECK(r.spu_programs.empty());
    REQUIRE(r.self);
    const SelfInfo& s = *r.self;
    CHECK(s.sce.magic == kSceMagic);
    CHECK(s.sce.type == 1);
    CHECK_FALSE(s.debug);
    REQUIRE(s.ext);
    REQUIRE(s.app);
    CHECK(s.app->self_type == 8);
    CHECK(self_type_name(s.app->self_type) == "NPDRM");
    CHECK(s.app->auth_id == 0x1010000001000003ull);
    REQUIRE(s.elf);
    CHECK(s.elf->machine == elf::kMachinePpc64);
    CHECK(s.elf->type == elf::kTypeExec);
    CHECK(s.elf->entry == x.entry);
    REQUIRE(s.phdrs.size() == 5);
    CHECK(s.phdrs[3].type == elf::kPtProcParam);
    REQUIRE(s.segments.size() == 5);
    CHECK(s.segments[0].encryption == 1);
    CHECK(s.segments[0].compression == 1);
    CHECK(s.segments[0].size == 0x4000);
    REQUIRE(s.version);
    CHECK(s.control.size() == 3);
    const NpdrmInfo* npd = s.npdrm();
    REQUIRE(npd);
    CHECK(npd->license == 3);
    CHECK(npd->content_id == test::kSelfContentId);
    CHECK(npdrm_license_name(npd->license) == "free (no RAP)");
    CHECK(s.control[1].required_system_version == 0x0003004000000000ull);

    // A non-NPDRM SELF (disc application) has no type-3 control info.
    test::SelfOptions disc;
    disc.npdrm = false;
    disc.self_type = 4;
    const FileReport rd = inspect_bytes("EBOOT.BIN", wrap_in_self(x.bytes, disc));
    REQUIRE(rd.self);
    CHECK(rd.self->npdrm() == nullptr);
    CHECK(rd.self->app->self_type == 4);

    // An SPRX wrapping a PRX.
    const FileReport rp = inspect_bytes("lib.sprx", wrap_in_self(build_ppu_prx().bytes));
    CHECK(rp.kind == FileKind::Self);
    CHECK(rp.encrypted());
    REQUIRE(rp.self->elf);
    CHECK(rp.self->elf->type == elf::kTypeScePpuRelExec);
}

TEST_CASE("debug SELF: the plain ELF inside is analyzed")
{
    const auto x = build_ppu_exec();
    test::SelfOptions options;
    options.debug = true;
    const auto self = wrap_in_self(x.bytes, options);
    const FileReport r = inspect_bytes("EBOOT.BIN", self);
    CHECK(r.kind == FileKind::Self);
    CHECK_FALSE(r.encrypted());
    REQUIRE(r.self);
    CHECK(r.self->debug);
    REQUIRE(r.self->plaintext_elf_offset);
    CHECK(r.elf_offset == *r.self->plaintext_elf_offset);
    REQUIRE(r.ppu);
    CHECK(r.ppu->imports.size() == x.imports.size());
    CHECK(r.phase0.function_estimate == 7u);
    REQUIRE(r.spu_programs.size() == 2);
    // Offsets are in the SELF file's coordinates.
    CHECK(r.spu_programs[0].image.offset == r.elf_offset + x.spu_offsets[0]);
}

TEST_CASE("unknown inputs")
{
    const std::vector<u8> sfo = {0x00, 'P', 'S', 'F', 1, 1, 0, 0};
    const FileReport r = inspect_bytes("PARAM.SFO", sfo);
    CHECK(r.kind == FileKind::Unknown);
    CHECK(r.detail.find("PARAM.SFO") != std::string::npos);

    const FileReport empty = inspect_bytes("empty", {});
    CHECK(empty.kind == FileKind::Unknown);
    CHECK(empty.detail == "empty file");

    // A little-endian ELF is refused (PS3 binaries are big-endian).
    auto le = build_ppu_exec().bytes;
    le[5] = 1;
    const FileReport rle = inspect_bytes("le.elf", le);
    CHECK(rle.kind == FileKind::Unknown);
    REQUIRE_FALSE(rle.errors.empty());
    CHECK(rle.errors[0].find("big-endian") != std::string::npos);
}

TEST_CASE("middleware signatures and SPU program classification")
{
    auto detect = [](std::string_view text) {
        return detect_middleware(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
    };
    const auto zlib = detect(std::string_view("\0 inflate 1.2.3 Copyright 1995-2005 Mark Adler \0", 47));
    REQUIRE(zlib.size() == 1);
    CHECK(zlib[0].product == "zlib");
    CHECK(zlib[0].count == 2);
    CHECK(classify_spu_program(zlib) == SpuProgramClass::ThirdPartyMiddleware);

    // Runtime libraries a custom program links (SPURS) and the game engine (RAGE) don't make it middleware.
    CHECK(classify_spu_program(detect("cellSpursJobMain rage::")) == SpuProgramClass::Custom);
    CHECK(classify_spu_program(detect("EDGEGEOM job")) == SpuProgramClass::SonyMiddleware); // case-insensitive
    CHECK(classify_spu_program(detect("Havok physics + edgezlib")) == SpuProgramClass::SonyMiddleware);
    CHECK(detect("").empty());
    CHECK_FALSE(middleware_signatures().empty());

    const std::string text = std::string("ab\x01longer string\x7f\x80tail");
    const auto strings = extract_strings(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()), 4, 0x100);
    REQUIRE(strings.size() == 2);
    CHECK(strings[0].text == "longer string");
    CHECK(strings[0].offset == 0x103);
    CHECK(strings[1].text == "tail");
}

TEST_CASE("JSON writer: escaping and structure")
{
    CHECK(JsonWriter::escape("plain") == "\"plain\"");
    CHECK(JsonWriter::escape("q\"b\\n\n") == "\"q\\\"b\\\\n\\n\"");
    CHECK(JsonWriter::escape(std::string("\x01\x7f\xff", 3)) == "\"\\u0001\\u007f\\u00ff\"");

    JsonWriter j(false);
    j.begin_object();
    j.field("a", 1).field("b", "x").field("c", true).field("n", static_cast<u64>(18446744073709551615ull));
    j.key("list").begin_array().value(-1).null().begin_object().end_object().begin_array().end_array().end_array();
    j.end_object();
    CHECK(j.complete());
    CHECK(j.str() == R"({"a":1,"b":"x","c":true,"n":18446744073709551615,"list":[-1,null,{},[]]})");
}
