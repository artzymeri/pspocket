// ppu-lift: lifts PPU code from a raw image into C translation units.
//
//   ppu-lift --image f.bin --base 0x10000 --seed 0x10200 [--seed ADDR[:TOC]]... [--seeds-file F] [--toc ADDR]
//            [--opd ADDR:SIZE] --out DIR [--tu N] [--name NAME] [--prefix PREFIX] [--no-comments] [--strict] [--quiet]
//
// --opd adds every function descriptor of an .opd area inside the image (address, TOC) as a seed.
//
// Prints a summary and lists every unsupported instruction (lifted as an interpreter fallback) and every illegal
// word reached by control flow. --strict turns those into a failing exit code.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "pspocket/ppu/disasm.hpp"
#include "pspocket/recomp/ppu/analyzer.hpp"
#include "pspocket/recomp/ppu/code_image.hpp"
#include "pspocket/recomp/ppu/lifter.hpp"

using namespace pspocket;
using namespace pspocket::recomp::ppu;

namespace {

void usage()
{
    std::fprintf(stderr,
        "usage: ppu-lift --image FILE --base ADDR --seed ADDR[:TOC] [--seed ...] [--seeds-file FILE] [--toc ADDR]\n"
        "                [--opd ADDR:SIZE] --out DIR [--tu N] [--name NAME] [--prefix PREFIX] [--no-comments]\n"
        "                [--strict] [--quiet]\n");
}

bool read_file(const std::string& path, std::vector<u8>& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    std::string image_path, out_dir;
    std::optional<u32> base;
    std::vector<Seed> seeds;
    std::vector<std::pair<u32, u32>> opd_areas;
    AnalyzerOptions analyzer_options;
    LiftOptions lift_options;
    bool strict = false, quiet = false;

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "ppu-lift: %s needs a value\n", arg.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        auto address = [&](const std::string& text) -> u32 {
            const auto seed = parse_seed(text);
            if (!seed || seed->toc) {
                std::fprintf(stderr, "ppu-lift: bad address '%s'\n", text.c_str());
                std::exit(2);
            }
            return seed->addr;
        };
        if (arg == "--image") image_path = value();
        else if (arg == "--base") base = address(value());
        else if (arg == "--seed") {
            const std::string text = value();
            const auto seed = parse_seed(text);
            if (!seed) {
                std::fprintf(stderr, "ppu-lift: bad seed '%s'\n", text.c_str());
                return 2;
            }
            seeds.push_back(*seed);
        } else if (arg == "--seeds-file") {
            std::string error;
            const auto more = read_seeds_file(value(), error);
            if (!more) {
                std::fprintf(stderr, "ppu-lift: %s\n", error.c_str());
                return 2;
            }
            seeds.insert(seeds.end(), more->begin(), more->end());
        } else if (arg == "--opd") {
            const std::string text = value();
            const auto area = parse_seed(text); // ADDR:SIZE has the same shape as ADDR:TOC
            if (!area || !area->toc) {
                std::fprintf(stderr, "ppu-lift: bad --opd '%s' (expected ADDR:SIZE)\n", text.c_str());
                return 2;
            }
            opd_areas.emplace_back(area->addr, *area->toc);
        } else if (arg == "--toc") analyzer_options.toc = address(value());
        else if (arg == "--out") out_dir = value();
        else if (arg == "--tu") lift_options.tu_count = static_cast<u32>(std::stoul(value()));
        else if (arg == "--name") lift_options.name = value();
        else if (arg == "--prefix") lift_options.prefix = value();
        else if (arg == "--no-comments") lift_options.comments = false;
        else if (arg == "--strict") strict = true;
        else if (arg == "--quiet") quiet = true;
        else if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "ppu-lift: unknown option %s\n", arg.c_str());
            usage();
            return 2;
        }
    }
    if (image_path.empty() || !base || out_dir.empty() || (seeds.empty() && opd_areas.empty())) {
        usage();
        return 2;
    }

    std::vector<u8> bytes;
    if (!read_file(image_path, bytes)) {
        std::fprintf(stderr, "ppu-lift: cannot read %s\n", image_path.c_str());
        return 1;
    }
    const CodeImage image(*base, std::move(bytes));
    for (const auto& [addr, size] : opd_areas) {
        const std::vector<Seed> more = seeds_from_opd(image, addr, size);
        seeds.insert(seeds.end(), more.begin(), more.end());
    }
    if (seeds.empty()) {
        std::fprintf(stderr, "ppu-lift: no seeds\n");
        return 2;
    }
    const Program program = analyze(image, seeds, analyzer_options);
    const LiftResult lifted = lift(image, program, lift_options);

    std::string error;
    if (!write_files(out_dir, lifted.files, error)) {
        std::fprintf(stderr, "ppu-lift: %s\n", error.c_str());
        return 1;
    }

    const LiftStats& s = lifted.stats;
    if (!quiet) {
        std::printf("ppu-lift: %zu functions, %zu instructions, %zu jump tables, %zu indirect jumps, %zu fallbacks, "
                    "%zu illegal -> %s (%zu files)\n",
            s.functions, s.instructions, s.jump_tables, s.indirect_jumps, s.fallbacks, s.illegal, out_dir.c_str(),
            lifted.files.size());
        for (const Diagnostic& d : program.warnings)
            std::printf("warning: %08x: %s\n", d.addr, d.message.c_str());
    }
    for (const Diagnostic& d : program.unsupported)
        std::printf("unsupported: %08x: %08x  %-32s %s\n", d.addr, d.word, ppu::disassemble(d.word, d.addr).c_str(),
            d.message.c_str());
    for (const Diagnostic& d : program.illegal) std::printf("illegal: %08x: %08x\n", d.addr, d.word);

    if (strict && (!program.unsupported.empty() || !program.illegal.empty())) return 1;
    return 0;
}
