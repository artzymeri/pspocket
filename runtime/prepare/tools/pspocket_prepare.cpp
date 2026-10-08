// pspocket-prepare: prepares a game on the Mac as the phone does (runtime/prepare, D-154): its executable translated,
// compiled with the device's compiler into a game library, linked and signed (ad hoc here), with a manifest. The gate
// then runs what the phone will run (scripts/gate.sh game --library DIR/game.dylib).
//
//   pspocket-prepare --title ID --exe EBOOT.elf --game-dir DIR [--ppu DIR] [--spu DIR | --spu-images DIR
//                    [--spu-log FILE]...] [-j N] [--memory MB]
//                    [--ppu-arg ARG]... [--spu-arg ARG]... [--mllvm OPT]... [--keep-sources] [--events FILE.csv]
//                    [--quiet]
//   pspocket-prepare --system DEV_FLASH --game-dir DIR [--for EBOOT.elf]... [--spu-modules all|KEY,...] [-j N]
//                    [--memory MB] [--keep-sources] [--quiet]
//   pspocket-prepare ... --modules DIR [--flash DEV_FLASH] [--disc IMAGE.iso]
//
// --modules DIR (with a game) also makes the game's modules library (D-238) in DIR's game folder's "modules": the
// game's own PRX modules pspocket-boot --prx-dump (or the app) saved in DIR, translated at slots 128 MB into the PRX
// region and up (above the firmware's modules of --flash, if they ever reach that far). --disc also takes the disc's
// own PRX modules and SPU ELFs (D-239), saved into the game folder's disc-prx and disc-spu, as the app does.
// --system makes the system library (D-178): the firmware's modules in DEV_FLASH translated once, into DIR (by
// convention the folder "system" beside DEV_FLASH, where pspocket-boot and the app look for it), with the SPU programs
// of the modules the games given with --for link (D-191), as the phone adds them before a game, or of the modules
// --spu-modules names ("libvdec.sprx,internal/libtrhddec.sprx", or all), besides those DIR's library holds already.
// --ppu uses translated PPU code (ppu-lift's output) instead of translating; --spu adds lifted SPU programs
// (scripts/lift-spu.sh's output); --spu-images lifts the SPU images runs saved (pspocket-boot --spu-dump) into
// DIR/spu, as the phone does, with --spu-log runs' logs for more entry points. Objects stay in DIR/objects, so a second run compiles only what changed.
// --events records every file's start and end (time, kind, predicted cost, seconds), to try other estimates of the
// time left on a real build; at the end the tool says how right its time left was.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "pspocket/disc/modules.hpp"
#include "pspocket/loader/lle.hpp"
#include "pspocket/loader/loader.hpp"
#include "pspocket/prepare/modules.hpp"
#include "pspocket/prepare/prepare.hpp"
#include "pspocket/prepare/spu.hpp"
#include "pspocket/recomp/hash.hpp"
#include "pspocket/prepare/system.hpp"

namespace fs = std::filesystem;
using namespace pspocket;

namespace {

int usage()
{
    std::fputs("usage: pspocket-prepare --title ID --exe EBOOT.elf --game-dir DIR [--ppu DIR] [--spu DIR |\n"
               "                        --spu-images DIR [--spu-log FILE]...] [-j N]\n"
               "                        [--memory MB] [--ppu-arg ARG]... [--spu-arg ARG]... [--mllvm OPT]...\n"
               "                        [--keep-sources] [--events FILE.csv] [--quiet]\n"
               "       pspocket-prepare --system DEV_FLASH --game-dir DIR [--for EBOOT.elf]... [--spu-modules all|KEY,...]\n"
               "                        [-j N] [--memory MB] [--keep-sources]\n",
        stderr);
    return 64;
}

std::string clock_text(double s)
{
    const long t = static_cast<long>(std::max(0.0, s) + 0.5);
    return t >= 3600 ? std::format("{}:{:02}:{:02}", t / 3600, t / 60 % 60, t % 60)
                     : std::format("{}:{:02}", t / 60, t % 60);
}

} // namespace

int main(int argc, char** argv)
{
    prepare::Options o;
    std::string events_path, system_flash, modules_flash;
    std::vector<std::string> modules_inputs;
    std::string disc_image;
    std::vector<std::string> system_for;
    std::set<std::string> spu_modules;
    bool quiet = false, lift_spu_only = false, discover_spu = false;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--quiet") quiet = true;
        else if (a == "--keep-sources") o.keep_sources = true;
        else if (a == "--lift-spu-only") lift_spu_only = true;
        else if (a == "--discover-spu") discover_spu = true;
        else if (a == "--no-discover-spu") o.discover_spu = false;
        else if (!(v = value())) return usage();
        else if (a == "--system") system_flash = v;
        else if (a == "--title") o.title_id = v;
        else if (a == "--exe") o.executable = v;
        else if (a == "--game-dir") o.game_dir = v;
        else if (a == "--ppu") o.ppu_dir = v;
        else if (a == "--spu") o.spu_dir = v;
        else if (a == "--spu-images") o.spu_images_dir = v;
        else if (a == "--spu-log") o.spu_logs.emplace_back(v);
        else if (a == "-j") o.workers = static_cast<unsigned>(std::max(1, std::atoi(v)));
        else if (a == "--memory") o.memory_mb = std::atof(v);
        else if (a == "--ppu-arg") o.extra_ppu_args.emplace_back(v);
        else if (a == "--spu-arg") o.extra_spu_args.emplace_back(v);
        else if (a == "--mllvm") o.llvm_options.emplace_back(v);
        else if (a == "--events") events_path = v;
        else if (a == "--modules") modules_inputs.emplace_back(v);
        else if (a == "--flash") modules_flash = v;
        else if (a == "--disc") disc_image = v;
        else if (a == "--for") system_for.emplace_back(v);
        else if (a == "--spu-modules") {
            std::string list = v;
            for (size_t at = 0; at <= list.size();) {
                const size_t end = std::min(list.find(',', at), list.size());
                const std::string key = list.substr(at, end - at);
                if (!key.empty()) spu_modules.insert(key == "all" ? prepare::kAllModules : key);
                at = end + 1;
            }
        } else return usage();
    }
    if (lift_spu_only) { // only the SPU step: --spu-images into --game-dir/spu (comparing with lift-spu.sh)
        if (o.spu_images_dir.empty() || o.game_dir.empty()) return usage();
        prepare::SpuLiftReport rep;
        std::string error;
        if (!prepare::lift_spu_images(o.spu_images_dir, (fs::path(o.game_dir) / "spu").string(), o.spu_logs, &rep,
                &error, discover_spu)) {
            std::fprintf(stderr, "pspocket-prepare: %s\n", error.c_str());
            return 1;
        }
        std::printf("lifted %zu SPU programs (%zu failed, %zu copies merged); seeds: %zu\n", rep.lifted, rep.failed,
            rep.merged, rep.seeds);
        return 0;
    }
    if (o.game_dir.empty() || (system_flash.empty() && (o.title_id.empty() || o.executable.empty()))) return usage();

    // This machine: the LLVM build's headers and the source tree's, compiling for the Mac.
    o.toolchain.resource_dir = PSPOCKET_CODEGEN_RESOURCE_DIR;
    o.toolchain.sysroot = PSPOCKET_CODEGEN_SYSROOT;
    o.toolchain.include_dirs = {PSPOCKET_SOURCE_DIR "/runtime/ppu/include", PSPOCKET_SOURCE_DIR "/runtime/spu/include",
        PSPOCKET_SOURCE_DIR "/runtime/core/include", PSPOCKET_SOURCE_DIR "/runtime/game/include"};
    o.toolchain.entry_source = PSPOCKET_GAME_LIBRARY_ENTRY_SOURCE;
    o.toolchain.target = "arm64-apple-macos13.0";
    o.toolchain.cpu = "native"; // this Mac's, as the phone compiles for itself
    o.toolchain.platform = "macos";
    o.toolchain.min_os = "13.0";
    // The last preparation here says how fast this Mac compiles each kind of file (its manifest's rates).
    if (prepare::Manifest m; prepare::read_manifest(o.game_dir, &m)) { // this game's last preparation (D-164)
        const auto r = prepare::prior_rates(&m, nullptr, m.thermal);
        for (int k = 0; k < 3; k++) o.prior_rates[k] = r[static_cast<size_t>(k)];
    }

    std::ofstream events;
    if (!events_path.empty()) {
        events.open(events_path);
        events << "event,seconds,file,group,predicted,file_seconds\n";
        o.on_file = [&](const std::string& file, int kind, double predicted, double seconds, double at) {
            events << std::format("{},{:.3f},{},{},{:.3f},{:.3f}\n", seconds < 0 ? "start" : "end", at,
                fs::path(file).filename().string(), kind, predicted, seconds);
        };
    }
    struct Sample {
        double seconds, left;
    };
    std::vector<Sample> samples;
    double last_print = -10;
    o.progress = [&](const prepare::Progress& p) {
        if (p.seconds_left >= 0) samples.push_back({p.seconds, p.seconds_left});
        const bool phase_line = p.phase != prepare::Progress::Phase::Compiling;
        if (quiet && !phase_line) return;
        if (!phase_line && p.seconds - last_print < 3) return;
        last_print = p.seconds;
        std::printf("[%3.0f%%] %s, %s%s\n", 100 * p.fraction, p.detail.c_str(), clock_text(p.seconds).c_str(),
            p.seconds_left >= 0 ? std::format(", about {} left", clock_text(p.seconds_left)).c_str() : "");
        std::fflush(stdout);
    };
    prepare::Result r;
    if (!system_flash.empty()) {
        prepare::SystemOptions s;
        s.dev_flash = system_flash;
        s.dir = o.game_dir;
        s.toolchain = o.toolchain;
        s.workers = o.workers;
        s.memory_mb = o.memory_mb;
        for (int k = 0; k < 3; k++) s.prior_rates[k] = o.prior_rates[k];
        s.keep_sources = o.keep_sources;
        s.progress = o.progress;
        s.spu_modules = spu_modules;
        loader::LleModules lle(system_flash);
        if (!system_for.empty() && !lle.index()) {
            std::fprintf(stderr, "pspocket-prepare: no firmware modules in %s\n", system_flash.c_str());
            return 1;
        }
        for (const std::string& path : system_for) { // the modules each game links (LleModules::choice_for)
            std::ifstream in(path, std::ios::binary);
            auto exe = loader::Executable::parse(std::vector<u8>(std::istreambuf_iterator<char>(in), {}));
            if (!exe) {
                std::fprintf(stderr, "pspocket-prepare: %s: %s\n", path.c_str(), exe.error().message.c_str());
                return 1;
            }
            const auto chosen = lle.choice_for(loader::LleModules::needs_of((*exe)->module()));
            std::string list;
            for (const std::string& f : chosen) list += (list.empty() ? "" : ", ") + f;
            if (!quiet) std::printf("%s links %zu system modules%s%s\n", path.c_str(), chosen.size(), list.empty() ? "" : ": ",
                list.c_str());
            s.spu_modules.insert(chosen.begin(), chosen.end());
        }
        r = prepare::prepare_system(s);
    } else {
        if (!disc_image.empty()) { // the disc's own executable files (D-239)
            const fs::path prx = fs::path(o.game_dir) / "disc-prx", spu = fs::path(o.game_dir) / "disc-spu";
            disc::DiscModules found;
            std::string why;
            if (!disc::save_disc_modules(disc_image, prx.string(), spu.string(), &found, &why)) {
                std::fprintf(stderr, "pspocket-prepare: %s: %s\n", disc_image.c_str(), why.c_str());
                return 1;
            }
            std::printf("the disc: %zu PRX modules, %zu SPU programs (%zu need a licence)\n", found.prx, found.spu,
                found.npdrm);
            if (found.prx) modules_inputs.push_back(prx.string());
            if (found.spu) o.spu_elf_dirs.push_back(spu.string());
        }
        r = prepare::prepare_game(o);
        if (r.ok && !modules_inputs.empty()) { // the game's own modules (D-238)
            prepare::ModulesOptions m;
            m.inputs = modules_inputs;
            m.title_id = o.title_id;
            m.executable_sha256 = prepare::file_sha256(o.executable);
            if (!modules_flash.empty()) { // above the firmware's modules, however far they reach
                loader::LleModules lle(modules_flash);
                if (lle.index()) m.floor = std::max(m.floor, lle.layout_end());
            }
            m.dir = (fs::path(o.game_dir) / "modules").string();
            m.toolchain = o.toolchain;
            m.workers = o.workers;
            m.memory_mb = o.memory_mb;
            m.keep_sources = o.keep_sources;
            m.progress = o.progress;
            const prepare::ModulesResult mr = prepare::prepare_game_modules(m);
            if (!mr.library.ok) {
                std::fprintf(stderr, "pspocket-prepare: its modules: %s\n", mr.library.error.c_str());
                return 1;
            }
            std::printf("%s\n", mr.none ? "the game's modules: none saved" : std::format("the game's modules: {} "
                "translated into {} ({} without a slot, {} unreadable)", mr.modules, m.dir, mr.without_slot,
                mr.unreadable).c_str());
        }
    }
    if (!r.ok) {
        std::fprintf(stderr, "pspocket-prepare: %s\n", r.error.c_str());
        return 1;
    }
    std::printf("%s: %zu files compiled, %zu kept; %s in all (translate %.1f s, compile %s, link %.1f s, sign %.1f s), "
                "rates PPU %.2f SPU %.2f\n",
        r.library.c_str(), r.files_compiled, r.files_kept, clock_text(r.seconds).c_str(), r.translate_seconds,
        clock_text(r.compile_seconds).c_str(), r.link_seconds, r.sign_seconds, r.rates[0], r.rates[1]);
    // How right the time left was, at 10, 25, 50 and 75% of the whole preparation, against its real end.
    if (r.files_compiled > 0 && !samples.empty()) {
        std::printf("time left, as said then against what it was:");
        for (const double at : {0.0, 0.1, 0.25, 0.5, 0.75}) {
            const auto s = std::find_if(samples.begin(), samples.end(),
                [&](const Sample& x) { return x.seconds >= at * r.seconds; });
            if (s == samples.end()) continue;
            const double real = r.seconds - s->seconds;
            std::printf("  %.0f%%: %s vs %s (%+.0f%%)", 100 * at, clock_text(s->left).c_str(), clock_text(real).c_str(),
                real > 0 ? 100 * (s->left - real) / real : 0.0);
        }
        std::printf("\n");
    }
    return 0;
}
