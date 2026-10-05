#include "pspocket/recomp/ppu/executable.hpp"

#include <format>

#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/guest_image.hpp"
#include "pspocket/recomp/ppu_module.hpp"

namespace pspocket::recomp::ppu {

Result<LoadedExecutable> load_executable(std::span<const u8> bytes)
{
    const auto elf = ElfFile::parse(bytes);
    if (!elf) return make_error("not an ELF: {}", elf.error().message);
    if (!elf->is_ppu()) return make_error("not a PPU executable");
    const auto guest = GuestImage::from_elf(*elf);
    if (!guest) return guest.error();
    const auto module = analyze_ppu_module(*elf, *guest);
    if (!module) return module.error();

    LoadedExecutable out;
    for (const auto& seg : guest->segments()) {
        if (seg.memsz == 0) continue;
        std::vector<u8> data = seg.data;
        data.resize(static_cast<usize>(seg.memsz), 0); // bss reads as zero
        out.image.add_segment(Segment{seg.vaddr, std::move(data), (seg.flags & 1u) != 0});
    }
    const OpdInfo& opd = module->opd;
    if (opd.entry_func) out.seeds.push_back(Seed{*opd.entry_func, opd.toc, "entry"});
    for (const OpdEntry& e : opd.entries)
        if (out.image.is_code(e.func)) out.seeds.push_back(Seed{e.func, e.toc, {}});
    out.toc = opd.toc;
    out.summary = std::format("{} segments, entry {:#x}, {} function descriptors ({}), TOC {:#x}",
        out.image.segments().size(), opd.entry_func.value_or(0), opd.entries.size(), opd_source_name(opd.source),
        opd.toc.value_or(0));
    return out;
}

Result<ExecutableTranslation> translate_executable(std::span<const u8> bytes, const AnalyzerOptions& analyzer,
    const LiftOptions& lift)
{
    auto loaded = load_executable(bytes);
    if (!loaded) return loaded.error();
    if (loaded->seeds.empty()) return make_error("no entry and no function descriptors to start from");
    AnalyzerOptions options = analyzer;
    if (!options.toc) options.toc = loaded->toc;
    ExecutableTranslation out;
    out.summary = std::move(loaded->summary);
    out.program = analyze(loaded->image, loaded->seeds, options);
    out.lifted = recomp::ppu::lift(loaded->image, out.program, lift);
    return out;
}

} // namespace pspocket::recomp::ppu
