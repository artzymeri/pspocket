// The system library (D-178): the firmware's modules translated once per firmware. See pspocket/prepare/system.hpp.

#include "pspocket/prepare/system.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/prepare/spu.hpp"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/firmware.hpp"
#include "pspocket/recomp/hash.hpp"
#include "pspocket/recomp/ppu/analyzer.hpp"
#include "pspocket/recomp/ppu/code_image.hpp"
#include "pspocket/recomp/ppu/lifter.hpp"
#include "pspocket/recomp/ppu_module.hpp"
#include "pspocket/recomp/prx_place.hpp"
#include "pspocket/recomp/self_decrypt.hpp"
#include "pspocket/recomp/spu/image.hpp"
#include "pspocket/recomp/spu_scan.hpp"
#include "pspocket/prepare/spu_static.hpp"
#include "module_translate.hpp"

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

// ---- The modules' SPU programs (D-191)
//
// The modules translated carry their SPU programs (decoders and helpers) as ELF images inside their segments, and hand
// them to SPURS as tasks or to lv2 as SPU thread images: saved and lifted as a game's are (spu_static.hpp).
// - The code a task or thread runs: the executable PT_LOAD holding the entry, at its vaddr.
// - Overlays: the other executable PT_LOADs. Several share one local store range, and the program's overlay manager
//   copies one in by DMA when it calls into it; the runtime identifies the copy by the hash of what it copied (the
//   module tracker, D-079), at the address it sits. Each is a program of its own at its vaddr, entered at its start.
struct SpuCount : SpuStaticCount {
    size_t modules = 0;
};

// Every SPU ELF in a placed module's segments; with `save`, saved for lifting. Whether the module carries any.
bool save_spu_images(const recomp::PlacedPrx& placed, const std::string& key, const fs::path& out, bool save,
    SpuCount& count)
{
    std::vector<SpuSegment> segments;
    for (const recomp::PlacedSegment& seg : placed.segments)
        segments.push_back({static_cast<u32>(seg.addr), std::span<const u8>(seg.bytes.data(), seg.memsz)});
    SpuStaticOptions options;
    options.save = save;
    return prepare::save_spu_images(segments, key, out, options, count);
}

std::set<std::string> read_lines(const fs::path& p)
{
    std::set<std::string> out;
    std::ifstream in(p);
    for (std::string line; std::getline(in, line);)
        if (!line.empty()) out.insert(line);
    return out;
}

std::string lines(const std::set<std::string>& set)
{
    std::string out;
    for (const std::string& line : set) out += line + '\n';
    return out;
}

} // namespace

std::set<std::string> system_spu_modules(const std::string& dir) { return read_lines(fs::path(dir) / "spu-modules.txt"); }

size_t translate_system(const std::string& dev_flash, const std::string& dir, std::string* error, std::string* summary,
    const std::set<std::string>& spu_modules)
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
    const fs::path spu_images = fs::path(dir) / "spu-images";
    fs::remove_all(spu_images, ec);
    fs::create_directories(spu_images, ec);
    SpuCount spu;
    std::set<std::string> carriers; // the modules translated that carry SPU programs
    for (const auto& [key, m] : modules) {
        const std::vector<u32>& addrs = layout[index++];
        identity += recomp::firmware_identity_line(key, m.sha256, addrs);
        if (!recomp::firmware_module_translated(key)) continue;
        const auto elf = recomp::ElfFile::parse(m.elf_bytes);
        const auto placed = detail::add_placed_module(key, *elf, m.elf_bytes, addrs, image, seeds, checks);
        if (!placed) continue;
        const bool save = spu_modules.contains(kAllModules) || spu_modules.contains(key);
        if (save_spu_images(*placed, key, spu_images, save, spu)) {
            carriers.insert(key);
            if (save) spu.modules++;
        }
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
    if (!write_text(fs::path(dir) / "identity.txt", identity) || !write_text(fs::path(dir) / "modules.txt", checks) ||
        !write_text(fs::path(dir) / "spu-carriers.txt", lines(carriers)))
        return fail("cannot write " + dir);
    // Their SPU programs, with code only registers reach found too (D-155): no run has met them to give seeds.
    const fs::path spu_dir = fs::path(dir) / "spu";
    fs::remove_all(spu_dir, ec);
    SpuLiftReport lifted_spu;
    if (spu.programs && !lift_spu_images(spu_images.string(), spu_dir.string(), {}, &lifted_spu, &why, true))
        return fail("the SPU programs: " + why);
    const std::string s = std::format("{} of {} modules translated: {} functions, {} instructions in {} units ({} "
                                      "unsupported, {} illegal); {} SPU programs ({} of them overlays) from {} images "
                                      "in {} of the {} modules carrying SPU programs ({} not translated)",
        translated, modules.size(), program.functions.size(), instructions, lift.tu_count, program.unsupported.size(),
        program.illegal.size(), lifted_spu.lifted, spu.overlays, spu.images, spu.modules, carriers.size(),
        spu.skipped + lifted_spu.failed);
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
    // The SPU programs of the modules asked for, and of those an earlier preparation put in: the library only grows.
    std::set<std::string> spu_modules = system_spu_modules(o.dir);
    spu_modules.insert(o.spu_modules.begin(), o.spu_modules.end());
    std::string error;
    if (!translate_system(o.dev_flash, o.dir, &error, nullptr, spu_modules)) {
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
    std::error_code ec;
    if (fs::exists(fs::path(o.dir) / "spu" / "game_spu.c", ec)) g.spu_dir = (fs::path(o.dir) / "spu").string();
    g.discover_spu = false;
    g.system_generation = kSystemGeneration;
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
    // What it holds, once it does: a preparation stopped before this leaves the library and its list as they were.
    if (r.ok && !write_text(fs::path(o.dir) / "spu-modules.txt", lines(spu_modules))) {
        r.ok = false;
        r.error = "cannot write " + o.dir;
    }
    if (r.ok && !o.keep_sources) {
        fs::remove_all(g.ppu_dir, ec);
        fs::remove_all(fs::path(o.dir) / "spu", ec);
        fs::remove_all(fs::path(o.dir) / "spu-images", ec);
    }
    return r;
}

bool system_library_is_current(const std::string& dir, const std::string& identity, std::string* why,
    const std::set<std::string>& spu_modules)
{
    if (!library_is_current(dir, recomp::to_hex(recomp::Sha256::hash(std::string_view(identity))), why)) return false;
    Manifest m;
    if (!read_manifest(dir, &m) || m.system_generation < kSystemGeneration) {
        if (why) *why = "made before a change to how pspocket translates the system software: preparing it again";
        return false;
    }
    // Only modules that carry SPU programs count: for the others (fonts, text, sync, network...) nothing is added.
    const std::set<std::string> held = system_spu_modules(dir), carriers = read_lines(fs::path(dir) / "spu-carriers.txt");
    if (held.contains(kAllModules)) return true;
    size_t missing = 0;
    for (const std::string& module : spu_modules)
        if (carriers.contains(module) && !held.contains(module)) missing++;
    if (missing) {
        if (why)
            *why = std::format("the game uses {} system module{} whose SPU programs are not translated yet: adding them",
                missing, missing == 1 ? "" : "s");
        return false;
    }
    return true;
}

} // namespace pspocket::prepare
