#include "pspocket/recomp/inventory.hpp"

#include <algorithm>
#include <format>

#include "pspocket/core/be.h"
#include "pspocket/recomp/bytes.hpp"

namespace pspocket::recomp {

namespace {

bool has_elf_magic(std::span<const u8> d)
{
    return d.size() >= 4 && d[0] == 0x7F && d[1] == 'E' && d[2] == 'L' && d[3] == 'F';
}

std::string unknown_detail(std::span<const u8> d)
{
    auto starts = [&](std::string_view magic) {
        return d.size() >= magic.size() && std::equal(magic.begin(), magic.end(), d.begin(), [](char a, u8 b) { return static_cast<u8>(a) == b; });
    };
    using namespace std::string_view_literals;
    if (starts("\0PSF"sv)) return "PARAM.SFO (PSF)";
    if (starts("NPD\0"sv)) return "NPDRM data file (EDAT/SDAT)";
    if (starts("\x7FPKG"sv)) return "PS3 package (PKG)";
    if (starts("SCEUF"sv)) return "PS3 firmware update (PUP)";
    if (d.empty()) return "empty file";
    return "";
}

void classify(SpuProgram& program, std::span<const u8> image_bytes)
{
    program.middleware = detect_middleware(image_bytes, program.image.offset);
    if (!program.image.name.empty()) {
        const std::span<const u8> name(reinterpret_cast<const u8*>(program.image.name.data()), program.image.name.size());
        for (auto& hit : detect_middleware(name, 0)) {
            const bool known = std::any_of(program.middleware.begin(), program.middleware.end(),
                [&](const MiddlewareHit& h) { return h.product == hit.product; });
            if (!known) {
                hit.first_offset = program.image.offset; // found in the name, not the bytes
                program.middleware.push_back(std::move(hit));
            }
        }
    }
    program.classification = classify_spu_program(program.middleware);
}

void analyze_elf(FileReport& rep, std::span<const u8> bytes, u64 base, const InspectOptions& options, const NidDb& db)
{
    auto parsed = ElfFile::parse(bytes);
    if (!parsed) {
        rep.errors.push_back("ELF: " + parsed.error().message);
        return;
    }
    const ElfFile& elf = *parsed;
    const ElfHeader& h = elf.header();
    rep.elf = h;
    rep.elf_offset = base;
    rep.segments = elf.segments();
    rep.sections = elf.sections();
    rep.symbol_count = elf.symbols().size();
    for (const auto& w : elf.warnings()) rep.warnings.push_back("elf: " + w);

    if (rep.kind != FileKind::Self) {
        if (elf.is_ppu()) {
            rep.kind = h.type == elf::kTypeExec ? FileKind::PpuExecutable
                     : h.type == elf::kTypeScePpuRelExec ? FileKind::PpuPrx
                                                         : FileKind::PpuOther;
        } else if (elf.is_spu()) {
            rep.kind = FileKind::SpuElf;
        } else {
            rep.kind = FileKind::OtherElf;
        }
    }

    if (elf.is_ppu()) {
        if (h.type == elf::kTypeExec) rep.rpcs3_ppu_hash = rpcs3_ppu_executable_hash(elf);
        auto ppu = analyze_ppu_module(elf);
        if (ppu) {
            resolve_names(*ppu, db);
            for (const auto& w : ppu->warnings) rep.warnings.push_back("ppu: " + w);
            rep.ppu = std::move(*ppu);
        } else {
            rep.errors.push_back("PPU module: " + ppu.error().message);
        }
        for (auto& image : scan_spu_images(elf)) {
            SpuProgram program;
            program.image = std::move(image);
            const u64 rel = program.image.offset; // file offsets relative to the ELF start
            program.image.offset += base;
            classify(program, bytes.subspan(static_cast<usize>(rel), static_cast<usize>(program.image.size)));
            rep.spu_programs.push_back(std::move(program));
        }
    } else if (elf.is_spu()) {
        if (auto image = measure_spu_image(bytes, base)) {
            SpuProgram program;
            program.image = std::move(*image);
            classify(program, bytes.first(static_cast<usize>(program.image.size)));
            rep.spu_programs.push_back(std::move(program));
        }
    }

    rep.middleware = detect_middleware(bytes, base);
    if (options.collect_strings) rep.strings = extract_strings(bytes, options.min_string_length, base, options.max_strings);
}

void summarize(FileReport& rep)
{
    Phase0Summary& s = rep.phase0;
    if (rep.ppu) {
        const OpdInfo& opd = rep.ppu->opd;
        if (opd.source != OpdInfo::Source::None && opd.distinct_functions != 0) {
            s.function_estimate = opd.distinct_functions;
            s.function_estimate_source = std::format("function descriptors ({})", opd_source_name(opd.source));
        }
        for (const auto& lib : rep.ppu->imports) {
            ModuleImportSummary& m = s.imports[lib.name];
            m.functions += static_cast<u32>(lib.functions.size());
            m.variables += static_cast<u32>(lib.variables.size());
            for (const auto& f : lib.functions) {
                if (f.resolved.match == NidMatch::Unknown) {
                    m.unresolved++;
                    s.unresolved.push_back({lib.name, f.nid, false});
                } else if (f.resolved.match == NidMatch::OtherModule) {
                    m.other_module++;
                }
            }
            for (const auto& v : lib.variables) {
                if (v.resolved.match == NidMatch::Unknown) {
                    m.unresolved++;
                    s.unresolved.push_back({lib.name, v.nid, true});
                } else if (v.resolved.match == NidMatch::OtherModule) {
                    m.other_module++;
                }
            }
        }
    }
    for (const auto& p : rep.spu_programs) {
        s.spu_programs++;
        switch (p.classification) {
        case SpuProgramClass::SonyMiddleware:
            s.spu_sony++;
            s.spu_bytes_sony += p.image.size;
            break;
        case SpuProgramClass::ThirdPartyMiddleware:
            s.spu_third_party++;
            s.spu_bytes_third_party += p.image.size;
            break;
        case SpuProgramClass::Custom:
            s.spu_custom++;
            s.spu_bytes_custom += p.image.size;
            break;
        }
    }
}

} // namespace

std::string_view file_kind_name(FileKind kind)
{
    switch (kind) {
    case FileKind::PpuExecutable: return "ppu_executable";
    case FileKind::PpuPrx: return "ppu_prx";
    case FileKind::PpuOther: return "ppu_other";
    case FileKind::SpuElf: return "spu_elf";
    case FileKind::Self: return "self";
    case FileKind::OtherElf: return "other_elf";
    case FileKind::Unknown: return "unknown";
    }
    return "unknown";
}

std::string_view file_kind_description(FileKind kind)
{
    switch (kind) {
    case FileKind::PpuExecutable: return "PPU executable";
    case FileKind::PpuPrx: return "PPU PRX module";
    case FileKind::PpuOther: return "PPU ELF (not an executable or PRX)";
    case FileKind::SpuElf: return "SPU ELF";
    case FileKind::Self: return "SELF / SCE container";
    case FileKind::OtherElf: return "ELF for another machine";
    case FileKind::Unknown: return "unknown";
    }
    return "unknown";
}

std::optional<Sha1Digest> rpcs3_ppu_executable_hash(const ElfFile& elf)
{
    Sha1 sha;
    for (const auto& p : elf.segments()) {
        u8 head[8];
        ps_st32(head, p.type);
        ps_st32(head + 4, p.flags);
        sha.update(std::span<const u8>(head));
        if (p.type == elf::kPtLoad && p.memsz != 0) {
            if (p.filesz != 0 && !p.data_in_file) return std::nullopt;
            u8 range[16];
            ps_st64(range, p.vaddr);
            ps_st64(range + 8, p.memsz);
            sha.update(std::span<const u8>(range));
            sha.update(elf.segment_data(p));
        }
    }
    return sha.finish();
}

std::string rpcs3_ppu_hash_string(const Sha1Digest& digest) { return "PPU-" + to_hex(digest); }

FileReport inspect_bytes(std::string path, std::span<const u8> data, const InspectOptions& options)
{
    FileReport rep;
    rep.path = std::move(path);
    rep.size = data.size();
    rep.sha1 = Sha1::hash(data);
    rep.sha256 = Sha256::hash(data);
    const NidDb& db = options.nids ? *options.nids : NidDb::builtin();

    if (has_sce_magic(data)) {
        rep.kind = FileKind::Self;
        auto self = parse_self(data);
        if (!self) {
            rep.errors.push_back("SELF: " + self.error().message);
        } else {
            for (const auto& w : self->warnings) rep.warnings.push_back("self: " + w);
            rep.self = std::move(*self);
            const auto offset = rep.self->plaintext_elf_offset;
            if (rep.self->debug && offset && *offset < data.size() && has_elf_magic(data.subspan(static_cast<usize>(*offset)))) {
                analyze_elf(rep, data.subspan(static_cast<usize>(*offset)), *offset, options, db);
            } else if (rep.self->debug) {
                rep.errors.push_back("debug SELF: no plain ELF at the recorded offset");
            }
        }
    } else if (has_elf_magic(data)) {
        analyze_elf(rep, data, 0, options, db);
        if (!rep.elf) rep.kind = FileKind::Unknown;
    } else {
        rep.kind = FileKind::Unknown;
        rep.detail = unknown_detail(data);
        rep.middleware = detect_middleware(data, 0);
        if (options.collect_strings) rep.strings = extract_strings(data, options.min_string_length, 0, options.max_strings);
    }

    summarize(rep);
    return rep;
}

} // namespace pspocket::recomp
