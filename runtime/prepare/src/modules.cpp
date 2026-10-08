// A game's own PRX modules translated (D-238). See pspocket/prepare/modules.hpp.

#include "pspocket/prepare/modules.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>

#include "jobs.hpp"
#include "module_translate.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/game/host.hpp"
#include "pspocket/prepare/spu_static.hpp"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/hash.hpp"
#include "pspocket/recomp/ppu/analyzer.hpp"
#include "pspocket/recomp/ppu/code_image.hpp"
#include "pspocket/recomp/ppu/lifter.hpp"
#include "pspocket/recomp/prx_place.hpp"

namespace pspocket::prepare {
namespace {

namespace fs = std::filesystem;
constexpr const char* kChan = "prepare";

// A module the loader saved: the decrypted ELF, its identity (recomp::prx_identity), its PT_LOADs' sizes and where it
// came from.
struct Saved {
    std::string identity, source;
    std::vector<u8> bytes;
    std::vector<u32> sizes;
};

std::string source_of(const fs::path& txt)
{
    std::ifstream in(txt);
    for (std::string line; std::getline(in, line);)
        if (line.starts_with("path ")) return line.substr(5);
    return "?";
}

// The modules the inputs hold, by identity (the first folder's copy of one in several).
std::map<std::string, Saved> read_saved(const std::vector<std::string>& inputs, size_t* unreadable)
{
    std::map<std::string, Saved> out;
    std::error_code ec;
    for (const std::string& dir : inputs) {
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (e.path().extension() != ".elf") continue;
            Saved s;
            std::string text;
            if (!detail::read_bytes(e.path(), text)) continue;
            s.bytes.assign(text.begin(), text.end());
            const auto elf = recomp::ElfFile::parse(s.bytes, {.symbols = false});
            if (!elf || !elf->is_ppu() || elf->header().type != 0xFFA4) { // ET_SCE_PPURELEXEC: a PRX
                if (unreadable) ++*unreadable;
                PS_LOG_WARN(kChan, "{}: not a PRX module this build can read", e.path().string());
                continue;
            }
            s.identity = recomp::prx_identity(*elf);
            s.sizes = recomp::prx_load_sizes(*elf);
            s.source = source_of(fs::path(e.path()).replace_extension(".txt"));
            out.emplace(s.identity, std::move(s));
        }
    }
    return out;
}

// The identities of the modules the inputs hold (their files' names: the loader names them so).
std::vector<std::string> saved_identities(const std::vector<std::string>& inputs)
{
    std::vector<std::string> out;
    std::error_code ec;
    for (const std::string& dir : inputs)
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.path().extension() == ".elf" && e.path().stem().string().size() == 64)
                out.push_back(e.path().stem().string());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::optional<recomp::GameModuleLayout> read_layout(const fs::path& dir, std::string* text = nullptr)
{
    std::string t;
    if (!detail::read_bytes(dir / "identity.txt", t)) return std::nullopt;
    auto layout = recomp::parse_game_module_layout(t);
    if (!layout) return std::nullopt;
    if (text) *text = std::move(t);
    return std::move(*layout);
}

// What a failed preparation of a game's modules was for (dir/failed): the executable, this build and the saved
// modules. While it holds, the modules stay interpreted and the game is not prepared again for them on every start.
std::string failed_text(const std::string& executable_sha256, const std::vector<std::string>& identities)
{
    std::string text = std::format("E {}\nB {:016x} {:016x}\n", executable_sha256, ps_game_abi_hash,
        ps_game_translator_hash);
    for (const std::string& id : identities) text += "M " + id + "\n";
    return text;
}

} // namespace

ModulesResult prepare_game_modules(const ModulesOptions& o)
{
    ModulesResult r;
    const fs::path dir = o.dir;
    auto fail = [&](std::string why) {
        r.library.error = std::move(why);
        PS_LOG_ERROR(kChan, "{}: its modules: {}", o.title_id, r.library.error);
        const std::string failed = failed_text(o.executable_sha256, saved_identities(o.inputs));
        detail::write_bytes(dir / "failed", failed.data(), failed.size());
        return r;
    };
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return fail(std::format("cannot make {}: {}", o.dir, ec.message()));
    fs::remove(dir / "failed", ec);
    const std::map<std::string, Saved> saved = read_saved(o.inputs, &r.unreadable);
    if (saved.empty()) {
        // Nothing to translate: a marker the check reads, for this executable.
        const std::string none = std::format("E {}\n", o.executable_sha256);
        if (!detail::write_bytes(dir / "none", none.data(), none.size())) return fail("cannot write " + o.dir);
        r.none = true;
        r.library.ok = true;
        return r;
    }

    // The slots: an earlier preparation's for this executable, where they still sit above the floor, then the new
    // modules' below them.
    recomp::GameModuleLayout layout;
    layout.executable_sha256 = o.executable_sha256;
    layout.floor = o.floor;
    if (const auto earlier = read_layout(dir);
        earlier && earlier->executable_sha256 == o.executable_sha256 && earlier->lowest() >= o.floor) {
        for (const auto& m : earlier->modules)
            if (saved.contains(m.identity)) layout.modules.push_back(m);
    }
    std::vector<const Saved*> fresh;
    std::vector<std::vector<u32>> sizes;
    for (const auto& [identity, s] : saved)
        if (!layout.find(identity)) {
            fresh.push_back(&s);
            sizes.push_back(s.sizes);
        }
    const auto slots = recomp::game_module_layout(sizes, layout.lowest(), o.floor);
    for (size_t i = 0; i < fresh.size(); i++) {
        if (slots[i].empty())
            PS_LOG_WARN(kChan, "{}: {} ({}) does not fit above the firmware's modules: it runs from main memory, "
                               "interpreted", o.title_id, fresh[i]->source, fresh[i]->identity.substr(0, 16));
        layout.modules.push_back({fresh[i]->identity, slots[i], fresh[i]->source});
    }

    // Placed at their slots and translated together, as the system library's modules are (D-178).
    recomp::ppu::CodeImage image;
    std::vector<recomp::ppu::Seed> seeds;
    std::string checks;
    const fs::path spu_static = dir / "spu-modules"; // not prepare_game's spu-static (it empties that one)
    fs::remove_all(spu_static, ec);
    fs::create_directories(spu_static, ec);
    SpuStaticCount spu;
    for (const auto& m : layout.modules) {
        if (m.addrs.empty()) {
            r.without_slot++;
            continue;
        }
        const Saved& s = saved.at(m.identity);
        const auto elf = recomp::ElfFile::parse(s.bytes);
        const auto placed = detail::add_placed_module(m.identity, *elf, s.bytes, m.addrs, image, seeds, checks);
        if (!placed) continue;
        // The SPU programs it carries (D-191, D-226), as the runtime names them.
        std::vector<SpuSegment> segments;
        for (const recomp::PlacedSegment& seg : placed->segments)
            if (seg.addr) segments.push_back({seg.addr, std::span<const u8>(seg.bytes.data(), seg.memsz)});
        SpuStaticOptions so;
        so.save = true;
        so.low_entry_relocatable = true;
        so.who = o.title_id;
        save_spu_images(segments, s.source, spu_static, so, spu);
        r.modules++;
    }
    if (!r.modules) return fail("none of its modules could be placed");
    std::erase_if(seeds, [&](const recomp::ppu::Seed& s) { return !image.is_code(s.addr); });
    const recomp::ppu::Program program = recomp::ppu::analyze(image, seeds, {});
    size_t instructions = 0;
    for (const auto& f : program.functions) instructions += f.insns.size();
    recomp::ppu::LiftOptions lift;
    lift.name = "game_ppu"; // the library entry's names (runtime/game/library/library_entry.c)
    lift.prefix = "game_module_ppu_";
    lift.comments = false;
    lift.tu_count = static_cast<u32>(std::max<size_t>(1, instructions / 12000));
    const fs::path ppu = dir / "ppu";
    fs::remove_all(ppu, ec);
    std::string why;
    {
        recomp::ppu::LiftResult lifted = recomp::ppu::lift(image, program, lift);
        if (!recomp::ppu::write_files(ppu.string(), lifted.files, why)) return fail("cannot write the translation: " + why);
    }
    // The library of before goes first: an interrupted preparation must not leave it beside the new checks
    // (modules.txt), which it would pass with code translated from other bytes.
    fs::remove(dir / "game.dylib", ec);
    fs::remove(dir / "manifest.json", ec);
    const std::string identity = recomp::game_module_layout_text(layout);
    if (!detail::write_bytes(dir / "identity.txt", identity.data(), identity.size()) ||
        !detail::write_bytes(dir / "modules.txt", checks.data(), checks.size()))
        return fail("cannot write " + o.dir);
    PS_LOG_INFO(kChan, "{}: {} of its modules translated ({} without a slot): {} functions, {} instructions in {} "
                       "units ({} unsupported); {} SPU programs in them",
        o.title_id, r.modules, r.without_slot, program.functions.size(), instructions, lift.tu_count,
        program.unsupported.size(), spu.programs);

    // Compiled, linked and signed as a game's (the library holds identity.txt's SHA-256).
    Options g;
    g.executable = (dir / "identity.txt").string();
    g.title_id = o.title_id;
    g.game_dir = o.dir;
    g.ppu_dir = ppu.string();
    if (spu.programs) g.spu_images_dir = spu_static.string();
    g.discover_spu = true;
    g.toolchain = o.toolchain;
    g.workers = o.workers;
    g.memory_mb = o.memory_mb;
    g.qos_class = o.qos_class;
    g.key = o.key;
    g.identifier = o.identifier;
    for (int k = 0; k < 3; k++) g.prior_rates[k] = o.prior_rates[k];
    g.thermal = o.thermal;
    g.keep_sources = true; // ours to remove, below
    g.progress = o.progress;
    g.cancel = o.cancel;
    r.library = prepare_game(g);
    if (r.library.ok) {
        fs::remove(dir / "none", ec);
        if (!o.keep_sources) fs::remove_all(ppu, ec);
    }
    return r;
}

bool game_modules_are_current(const std::string& dir, const std::string& executable_sha256,
    const std::vector<std::string>& inputs, u32 floor, std::string* why)
{
    auto say = [&](std::string s) {
        if (why) *why = std::move(s);
        return false;
    };
    const std::vector<std::string> identities = saved_identities(inputs);
    std::string none, failed;
    std::error_code ec;
    // Failed before for these very modules with this build: they stay interpreted (logged when it failed).
    if (detail::read_bytes(fs::path(dir) / "failed", failed) && failed == failed_text(executable_sha256, identities)) {
        if (why) *why = "its modules stay interpreted (their preparation failed)";
        return true;
    }
    if (fs::exists(fs::path(dir) / "none", ec) && !fs::exists(fs::path(dir) / "game.dylib", ec)) {
        if (!detail::read_bytes(fs::path(dir) / "none", none) || none != std::format("E {}\n", executable_sha256))
            return say("made for another executable");
        if (!identities.empty()) return say(std::format("{} of its modules to translate", identities.size()));
        if (why) *why = "none";
        return true;
    }
    std::string text;
    const auto layout = read_layout(dir, &text);
    if (!layout) {
        if (!identities.empty()) return say("not prepared yet");
        if (why) *why = "none"; // a game that loaded no module of its own needs no library
        return true;
    }
    if (layout->executable_sha256 != executable_sha256) return say("made for another executable");
    if (layout->lowest() < floor) return say("the firmware's modules now reach its slots: laying them out again");
    size_t missing = 0;
    for (const std::string& id : identities)
        if (!layout->find(id)) missing++;
    if (missing) return say(std::format("{} of its modules to translate", missing));
    return library_is_current(dir, recomp::to_hex(recomp::Sha256::hash(std::string_view(text))), why);
}

} // namespace pspocket::prepare
