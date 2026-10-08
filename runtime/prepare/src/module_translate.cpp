// A PRX module placed and added to a translation (D-178, D-238). See module_translate.hpp.

#include "module_translate.hpp"

#include <format>

#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/recomp/hash.hpp"
#include "pspocket/recomp/ppu_module.hpp"

namespace pspocket::prepare::detail {
namespace {

constexpr const char* kChan = "prepare";

} // namespace

std::optional<recomp::PlacedPrx> add_placed_module(std::string_view key, const recomp::ElfFile& elf,
    std::span<const u8> bytes, std::span<const u32> addrs, recomp::ppu::CodeImage& image,
    std::vector<recomp::ppu::Seed>& seeds, std::string& checks)
{
    auto info = recomp::analyze_ppu_module(elf);
    auto placed = recomp::place_prx(elf, bytes, addrs);
    if (!placed) {
        PS_LOG_WARN(kChan, "{}: {}: not translated", key, placed.error().message);
        return std::nullopt;
    }
    // An address of the analysis (each PT_LOAD at its p_vaddr, recomp::GuestImage) at its fixed place. A module's
    // first PT_LOAD often reserves more than its file part, over the second's vaddr (libpngdec: 0x50000 from 0, the
    // data at 0x40a00): the later segment is the one there, as in the analysis image.
    std::vector<std::pair<u64, u64>> vranges; // per PT_LOAD: vaddr, memsz
    for (const recomp::ProgramHeader& ph : elf.segments())
        if (ph.type == 1) vranges.emplace_back(ph.vaddr, ph.memsz);
    const auto place_of = [&](u32 a) -> std::optional<u32> {
        for (usize i = std::min(vranges.size(), addrs.size()); i-- > 0;)
            if (addrs[i] && a >= vranges[i].first && a - vranges[i].first < vranges[i].second)
                return static_cast<u32>(addrs[i] + (a - vranges[i].first));
        return std::nullopt;
    };
    for (const recomp::PlacedSegment& seg : placed->segments) {
        if (!seg.addr) continue;
        image.add_segment({seg.addr, std::vector<u8>(seg.bytes.begin(), seg.bytes.begin() + seg.memsz), seg.executable});
        if (seg.executable)
            checks += std::format("M {} {:x} {:x} {}\n", key, seg.addr, seg.memsz,
                recomp::to_hex(recomp::Sha256::hash(std::span<const u8>(seg.bytes.data(), seg.memsz))));
    }
    // Seeds: every exported function's descriptor (module_start and the rest too), read where the loader puts it,
    // relocated; and the module's descriptor table.
    const auto descriptor = [&](u32 at) -> std::optional<std::pair<u32, u32>> {
        const auto p = place_of(at);
        if (!p) return std::nullopt;
        for (const recomp::PlacedSegment& seg : placed->segments)
            if (seg.addr && *p >= seg.addr && *p + 8 <= seg.addr + seg.memsz)
                return std::pair{ps_ld32(seg.bytes.data() + (*p - seg.addr)), ps_ld32(seg.bytes.data() + (*p - seg.addr) + 4)};
        return std::nullopt;
    };
    if (info) {
        for (const recomp::ExportedLibrary& lib : info->exports) {
            if (lib.skipped) continue;
            for (const recomp::ExportedFunction& f : lib.functions)
                if (const auto d = descriptor(f.descriptor))
                    seeds.push_back({d->first, d->second, std::format("{}:{}::{:08x}", key, lib.name, f.nid)});
        }
        if (info->opd.size >= 8)
            if (const auto start = place_of(info->opd.start))
                for (const recomp::ppu::Seed& s : recomp::ppu::seeds_from_opd(image, *start, info->opd.size))
                    seeds.push_back(s);
    }
    return std::move(*placed);
}

} // namespace pspocket::prepare::detail
