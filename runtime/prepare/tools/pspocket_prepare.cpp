// pspocket-prepare: prepares a game on the Mac as the phone does (runtime/prepare, D-154): its executable translated,
// compiled with the device's compiler into a game library, linked and signed (ad hoc here), with a manifest. The gate
// then runs what the phone will run (scripts/gate.sh game --library DIR/game.dylib).
//
//   pspocket-prepare --title ID --exe EBOOT.elf --game-dir DIR [--ppu DIR] [--spu DIR | --spu-images DIR
//                    [--spu-log FILE]...] [-j N] [--memory MB]
//                    [--ppu-arg ARG]... [--spu-arg ARG]... [--mllvm OPT]... [--keep-sources] [--events FILE.csv]
//                    [--quiet]
//
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
#include <string>
#include <vector>

#include "pspocket/prepare/prepare.hpp"
#include "pspocket/prepare/spu.hpp"

namespace fs = std::filesystem;
using namespace pspocket;

namespace {

int usage()
{
    std::fputs("usage: pspocket-prepare --title ID --exe EBOOT.elf --game-dir DIR [--ppu DIR] [--spu DIR |\n"
               "                        --spu-images DIR [--spu-log FILE]...] [-j N]\n"
               "                        [--memory MB] [--ppu-arg ARG]... [--spu-arg ARG]... [--mllvm OPT]...\n"
               "                        [--keep-sources] [--events FILE.csv] [--quiet]\n",
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
    std::string events_path;
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
        else return usage();
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
    if (o.title_id.empty() || o.executable.empty() || o.game_dir.empty()) return usage();

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
    if (prepare::Manifest m; prepare::read_manifest(o.game_dir, &m))
        for (int k = 0; k < 3; k++) o.prior_rates[k] = m.rates[k];

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
    const prepare::Result r = prepare::prepare_game(o);
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
