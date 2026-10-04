// spu-lift: lifts an SPU program (SPU ELF or raw LS image) into C translation units.
//
//   spu-lift --elf prog.elf --out DIR [options]
//   spu-lift --image code.bin --base 0x0 --entry 0x0 [--entry ...] --out DIR [options]
//   options: [--seed ADDR]... [--name NAME] [--tu N] [--xfloat approximate|accurate] [--no-comments]
//            [--no-constant-entries] [--relocatable] [--strict] [--quiet]
//
// --relocatable lifts position independent code (job binaries, the modules programs load: D-081) to run at any LS
// address: the base is only where the image is analysed.
// Prints a summary (functions, instructions, labels, the program key) and every unsupported instruction reached by
// control flow (left to the interpreter at runtime). --strict turns those into a failing exit code.

#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "pspocket/recomp/spu/analyzer.hpp"
#include "pspocket/recomp/spu/image.hpp"
#include "pspocket/recomp/spu/lifter.hpp"
#include "pspocket/spu/context.h"

using namespace pspocket;
using namespace pspocket::recomp::spu;

namespace {

void usage()
{
    std::fprintf(stderr,
        "usage: spu-lift (--elf FILE | --image FILE --base ADDR --entry ADDR [--entry ADDR]...) --out DIR\n"
        "                [--seed ADDR]... [--name NAME] [--tu N] [--xfloat approximate|accurate] [--no-comments]\n"
        "                [--no-constant-entries] [--relocatable] [--strict] [--quiet]\n");
}

bool read_file(const std::string& path, std::vector<u8>& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::optional<u32> parse_u32(const std::string& text)
{
    try {
        size_t used = 0;
        const unsigned long v = std::stoul(text, &used, 0);
        if (used != text.size() || v > 0xFFFFFFFFul) return std::nullopt;
        return static_cast<u32>(v);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::string elf_path, image_path, out_dir;
    std::optional<u32> base;
    std::vector<u32> entries, seeds;
    LiftOptions options;
    AnalyzerOptions analyzer;
    bool strict = false, quiet = false;

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "spu-lift: %s needs a value\n", arg.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        auto address = [&]() -> u32 {
            const std::string text = value();
            const auto v = parse_u32(text);
            if (!v || *v >= PS_SPU_LS_SIZE) {
                std::fprintf(stderr, "spu-lift: bad LS address '%s'\n", text.c_str());
                std::exit(2);
            }
            return *v;
        };
        if (arg == "--elf") elf_path = value();
        else if (arg == "--image") image_path = value();
        else if (arg == "--base") base = address();
        else if (arg == "--entry") entries.push_back(address());
        else if (arg == "--seed") seeds.push_back(address());
        else if (arg == "--out") out_dir = value();
        else if (arg == "--name") options.name = value();
        else if (arg == "--tu") {
            const auto v = parse_u32(value());
            if (!v || *v == 0 || *v > 1024) {
                std::fprintf(stderr, "spu-lift: bad --tu\n");
                return 2;
            }
            options.tu_count = *v;
        } else if (arg == "--xfloat") {
            const std::string mode = value();
            if (mode == "accurate") options.xfloat = PS_SPU_XFLOAT_ACCURATE;
            else if (mode == "approximate") options.xfloat = PS_SPU_XFLOAT_APPROXIMATE;
            else {
                std::fprintf(stderr, "spu-lift: --xfloat is approximate or accurate\n");
                return 2;
            }
        } else if (arg == "--no-comments") options.comments = false;
        else if (arg == "--no-constant-entries") analyzer.constant_entries = false;
        else if (arg == "--relocatable") {
            // Position independent code (D-081): ila/il constants are not its code addresses either.
            options.relocatable = analyzer.relocatable = true;
            analyzer.constant_entries = false;
        }
        else if (arg == "--strict") strict = true;
        else if (arg == "--quiet") quiet = true;
        else if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "spu-lift: unknown option %s\n", arg.c_str());
            usage();
            return 2;
        }
    }
    if (out_dir.empty() || elf_path.empty() == image_path.empty() || (!image_path.empty() && (!base || entries.empty()))) {
        usage();
        return 2;
    }

    std::vector<u8> bytes;
    const std::string& path = elf_path.empty() ? image_path : elf_path;
    if (!read_file(path, bytes)) {
        std::fprintf(stderr, "spu-lift: cannot read %s\n", path.c_str());
        return 1;
    }
    auto image = elf_path.empty() ? load_raw_image(bytes, *base, entries, options.name) : load_spu_elf(bytes, options.name);
    if (!image) {
        std::fprintf(stderr, "spu-lift: %s: %s\n", path.c_str(), image.error().message.c_str());
        return 2;
    }
    for (u32 s : seeds) {
        if (!image->in_code(s)) {
            std::fprintf(stderr, "spu-lift: seed 0x%05x is outside the code range\n", s);
            return 2;
        }
        image->seeds.push_back(s);
    }

    const Program program = analyze(*image, analyzer);
    const LiftResult lifted = lift(*image, program, options);
    std::string error;
    if (!write_files(out_dir, lifted.files, error)) {
        std::fprintf(stderr, "spu-lift: %s\n", error.c_str());
        return 1;
    }
    if (!quiet) {
        std::printf("spu-lift: %s: key %016llx, code 0x%05x-0x%05x, %zu functions, %zu instructions, %zu labels, "
                    "%zu unsupported -> %s (%zu files)\n",
            path.c_str(), static_cast<unsigned long long>(lifted.hash), image->code_start, image->code_end,
            lifted.stats.functions, lifted.stats.instructions, lifted.stats.labels, lifted.stats.unsupported,
            out_dir.c_str(), lifted.files.size());
    }
    for (const Diagnostic& d : program.unsupported)
        std::printf("unsupported: %05x: %08x  %s\n", d.addr, d.word, d.message.c_str());
    return strict && !program.unsupported.empty() ? 1 : 0;
}
