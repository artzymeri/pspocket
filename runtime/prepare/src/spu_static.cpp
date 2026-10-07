// SPU programs found by their ELF headers (D-191, D-226). See spu_static.hpp.

#include "pspocket/prepare/spu_static.hpp"

#include <format>
#include <fstream>
#include <string>

#include "pspocket/core/log.hpp"
#include "pspocket/recomp/spu/image.hpp"
#include "pspocket/recomp/spu_scan.hpp"

namespace pspocket::prepare {
namespace {

namespace fs = std::filesystem;
constexpr const char* kChan = "prepare";
constexpr u32 kFirstTaskEntry = 0x3000; // SPURS refuses a task entered below it (spurs_taskset.cpp)

void save_spu_program(const fs::path& out, std::span<const u8> code, u32 guest, u32 base, u32 entry, bool overlay,
    bool relocatable, std::string_view source, const SpuStaticOptions& o, SpuStaticCount& count)
{
    const auto image = recomp::spu::load_raw_image(code, base, {entry});
    if (!image) {
        PS_LOG_WARN(kChan, "{}: an SPU program of {}: {}", o.who, source, image.error().message);
        count.skipped++;
        return;
    }
    const std::string name = std::format("spu_{:016x}_{:05x}", image->hash(), base);
    std::error_code ec;
    if (fs::exists(out / (name + ".bin"), ec)) return; // the same program in another module
    std::ofstream(out / (name + ".bin"), std::ios::binary)
        .write(reinterpret_cast<const char*>(code.data()), static_cast<std::streamsize>(code.size()));
    // "task" lifts it where it sits; "module" lifts it relocatable (D-081). lift_spu_images reads kind, base, size and
    // entry.
    std::ofstream(out / (name + ".txt"), std::ios::binary | std::ios::trunc)
        << std::format("kind {}\nguest {:#x}\nbase {:#x}\nsize {:#x}\nentry {:#x}\nsource {}{}\n",
               relocatable ? "module" : "task", guest, base, code.size(), entry, source, overlay ? " (overlay)" : "");
    count.programs++;
    if (overlay) count.overlays++;
}

} // namespace

bool save_spu_images(std::span<const SpuSegment> segments, std::string_view source, const fs::path& out,
    const SpuStaticOptions& o, SpuStaticCount& count)
{
    bool carries = false;
    for (const SpuSegment& seg : segments) {
        if (!seg.addr) continue;
        const std::span<const u8> bytes = seg.bytes;
        for (const recomp::SpuImage& img : recomp::scan_spu_images(bytes)) {
            carries = true;
            if (!o.save) continue;
            count.images++;
            const recomp::ProgramHeader* main = nullptr;
            std::vector<const recomp::ProgramHeader*> code;
            for (const recomp::ProgramHeader& ph : img.segments) {
                if (ph.type != 1 || !(ph.flags & 1) || (ph.filesz & ~u64{3}) == 0 || !ph.data_in_file) continue;
                if (img.offset + ph.offset + ph.filesz > bytes.size() || ph.vaddr + ph.filesz > 0x40000) continue;
                code.push_back(&ph);
                if (!main && img.entry >= ph.vaddr && img.entry < ph.vaddr + ph.filesz) main = &ph;
            }
            const std::string from = std::format("{} {}", source, img.name.empty() ? "(unnamed)" : img.name);
            if (!main) {
                PS_LOG_WARN(kChan, "{}: SPU image at {:#x} of {}: no executable segment holds its entry {:#x}; not "
                                   "translated", o.who, seg.addr + img.offset, from, img.entry);
                count.skipped++;
                continue;
            }
            const bool relocatable = o.low_entry_relocatable && img.entry < kFirstTaskEntry;
            for (const recomp::ProgramHeader* ph : code) {
                const u64 at = img.offset + ph->offset;
                const auto vaddr = static_cast<u32>(ph->vaddr);
                save_spu_program(out, bytes.subspan(at, ph->filesz & ~u64{3}), static_cast<u32>(seg.addr + at), vaddr,
                    ph == main ? img.entry : vaddr, ph != main, relocatable, from, o, count);
            }
        }
    }
    return carries;
}

std::vector<SpuSegment> executable_segments(const recomp::ElfFile& elf)
{
    std::vector<SpuSegment> out;
    for (const recomp::ProgramHeader& ph : elf.segments()) {
        if (ph.type != 1 || !ph.data_in_file || ph.filesz == 0 || ph.vaddr > 0xFFFFFFFFull) continue;
        out.push_back({static_cast<u32>(ph.vaddr), elf.data().subspan(ph.offset, ph.filesz)});
    }
    return out;
}

} // namespace pspocket::prepare
