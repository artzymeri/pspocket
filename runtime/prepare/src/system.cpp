// The system library (D-178): the firmware's modules translated once per firmware. See pspocket/prepare/system.hpp.

#include "pspocket/prepare/system.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>

#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/firmware.hpp"
#include "pspocket/recomp/hash.hpp"
#include "pspocket/recomp/ppu/analyzer.hpp"
#include "pspocket/recomp/ppu/code_image.hpp"
#include "pspocket/recomp/ppu/lifter.hpp"
#include "pspocket/recomp/ppu_module.hpp"
#include "pspocket/recomp/prx_place.hpp"
#include "pspocket/recomp/self_decrypt.hpp"

namespace pspocket::prepare {
namespace {

namespace fs = std::filesystem;
constexpr const char* kChan = "prepare";

bool read_all(const fs::path& p, std::vector<u8>& out)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool write_text(const fs::path& p, const std::string& text)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

// A module as LleModules indexes it (D-175, D-177): one it can decrypt, parse and analyse, keyed by file name or
// "internal/" and the name. The same set and order make the same layout.
struct Module {
    std::vector<u8> elf_bytes;
    std::string sha256;
    std::vector<u32> sizes;
};

} // namespace

size_t translate_system(const std::string& dev_flash, const std::string& dir, std::string* error, std::string* summary)
{
    auto fail = [&](std::string why) -> size_t {
        if (error) *error = std::move(why);
        return 0;
    };
    std::error_code ec;
    std::map<std::string, Module> modules;
    for (const auto& [sub, prefix] : {std::pair{"external", ""}, std::pair{"internal", "internal/"}}) {
        for (const auto& e : fs::directory_iterator(fs::path(dev_flash) / "sys" / sub, ec)) {
            if (e.path().extension() != ".sprx") continue;
            std::vector<u8> raw;
            if (!read_all(e.path(), raw)) continue;
            auto elf_bytes = recomp::decrypt_self(raw, recomp::builtin_self_keys());
            if (!elf_bytes) continue;
            auto elf = recomp::ElfFile::parse(*elf_bytes);
            if (!elf || !recomp::analyze_ppu_module(*elf)) continue;
            Module m;
            m.sizes = recomp::prx_load_sizes(*elf);
            m.sha256 = recomp::to_hex(recomp::Sha256::hash(std::span<const u8>(raw)));
            m.elf_bytes = std::move(*elf_bytes);
            modules.emplace(prefix + e.path().filename().string(), std::move(m));
        }
    }
    if (modules.empty()) return fail("no firmware modules could be read in " + dev_flash);
    std::vector<std::vector<u32>> sizes;
    for (const auto& [key, m] : modules) sizes.push_back(m.sizes);
    const auto layout = recomp::prx_fixed_layout(sizes, PS_MEM_PRX_BASE, PS_MEM_PRX_SIZE);
    if (layout.empty()) return fail("the firmware's modules do not fit in the PRX region");

    std::string identity, checks;
    recomp::ppu::CodeImage image;
    std::vector<recomp::ppu::Seed> seeds;
    size_t translated = 0, index = 0;
    for (const auto& [key, m] : modules) {
        const std::vector<u32>& addrs = layout[index++];
        identity += recomp::firmware_identity_line(key, m.sha256, addrs);
        if (!recomp::firmware_module_translated(key)) continue;
        const auto elf = recomp::ElfFile::parse(m.elf_bytes);
        auto info = recomp::analyze_ppu_module(*elf);
        auto placed = recomp::place_prx(*elf, m.elf_bytes, addrs);
        if (!placed) {
            PS_LOG_WARN(kChan, "{}: {}: not translated", key, placed.error().message);
            continue;
        }
        // An address of the analysis (each PT_LOAD at its p_vaddr, recomp::GuestImage) at its fixed place. A module's
        // first PT_LOAD often reserves more than its file part, over the second's vaddr (libpngdec: 0x50000 from 0,
        // the data at 0x40a00): the later segment is the one there, as in the analysis image.
        std::vector<std::pair<u64, u64>> vranges; // per PT_LOAD: vaddr, memsz
        for (const recomp::ProgramHeader& ph : elf->segments())
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
        translated++;
    }
    // Only seeds in code: a descriptor table may hold data.
    std::erase_if(seeds, [&](const recomp::ppu::Seed& s) { return !image.is_code(s.addr); });

    const recomp::ppu::Program program = recomp::ppu::analyze(image, seeds, {});
    size_t instructions = 0;
    for (const auto& f : program.functions) instructions += f.insns.size();
    recomp::ppu::LiftOptions lift;
    lift.name = "game_ppu"; // the library entry's names (runtime/game/library/library_entry.c)
    lift.prefix = "system_ppu_";
    lift.comments = false;
    lift.tu_count = static_cast<u32>(std::max<size_t>(1, instructions / 12000));
    const fs::path ppu = fs::path(dir) / "ppu";
    fs::remove_all(ppu, ec);
    fs::create_directories(dir, ec);
    std::string why;
    {
        recomp::ppu::LiftResult lifted = recomp::ppu::lift(image, program, lift);
        if (!recomp::ppu::write_files(ppu.string(), lifted.files, why)) return fail("cannot write the translation: " + why);
    }
    if (!write_text(fs::path(dir) / "identity.txt", identity) || !write_text(fs::path(dir) / "modules.txt", checks))
        return fail("cannot write " + dir);
    const std::string s = std::format("{} of {} modules translated: {} functions, {} instructions in {} units ({} "
                                      "unsupported, {} illegal)",
        translated, modules.size(), program.functions.size(), instructions, lift.tu_count, program.unsupported.size(),
        program.illegal.size());
    PS_LOG_INFO(kChan, "system library: {}", s);
    for (const auto& d : program.illegal)
        PS_LOG_WARN(kChan, "system library: illegal word {:08x} at {:#010x} reached: {}", d.word, d.addr, d.message);
    if (summary) *summary = s;
    return translated;
}

Result prepare_system(const SystemOptions& o)
{
    const auto t0 = std::chrono::steady_clock::now();
    if (o.progress) {
        Progress p;
        p.detail = "Translating the PS3 system software";
        o.progress(p);
    }
    std::string error;
    if (!translate_system(o.dev_flash, o.dir, &error)) {
        Result r;
        r.error = error;
        return r;
    }
    const double translated = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    Options g;
    g.executable = (fs::path(o.dir) / "identity.txt").string();
    g.title_id = kSystemTitle;
    g.game_dir = o.dir;
    g.ppu_dir = (fs::path(o.dir) / "ppu").string();
    g.discover_spu = false;
    g.toolchain = o.toolchain;
    g.workers = o.workers;
    g.memory_mb = o.memory_mb;
    g.key = o.key;
    g.identifier = o.identifier;
    for (int k = 0; k < 3; k++) g.prior_rates[k] = o.prior_rates[k];
    g.thermal = o.thermal;
    g.keep_sources = true; // ours to remove, below
    g.progress = o.progress;
    g.cancel = o.cancel;
    Result r = prepare_game(g);
    r.translate_seconds = translated;
    std::error_code ec;
    if (r.ok && !o.keep_sources) fs::remove_all(g.ppu_dir, ec);
    return r;
}

bool system_library_is_current(const std::string& dir, const std::string& identity, std::string* why)
{
    return library_is_current(dir, recomp::to_hex(recomp::Sha256::hash(std::string_view(identity))), why);
}

} // namespace pspocket::prepare
