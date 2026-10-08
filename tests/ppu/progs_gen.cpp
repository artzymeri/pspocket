// ppu-progs-gen: writes the synthetic test programs (recomp/ppu synthetic::build_test_programs) as a raw image and
// a seeds file for ppu-lift. test_ppu_programs rebuilds the same program in memory (the builder is deterministic).
//
//   ppu-progs-gen --bin FILE --seeds FILE

#include <cstdio>
#include <format>
#include <string>

#include "pspocket/recomp/ppu/synthetic.hpp"

using namespace pspocket;
using namespace pspocket::recomp::ppu;

namespace {

constexpr u32 kBase = 0x00100000; // must match --base in tests/ppu/CMakeLists.txt and kProgramsBase in the test

bool write(const std::string& path, const std::string& content)
{
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(content.data(), 1, content.size(), f) == content.size();
    return std::fclose(f) == 0 && ok;
}

} // namespace

int main(int argc, char** argv)
{
    std::string bin, seeds, continuations;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string arg = argv[i];
        if (arg == "--bin") bin = argv[i + 1];
        else if (arg == "--seeds") seeds = argv[i + 1];
        else if (arg == "--continuations") continuations = argv[i + 1];
    }
    if (bin.empty() || seeds.empty() || continuations.empty()) {
        std::fprintf(stderr, "usage: ppu-progs-gen --bin FILE --seeds FILE --continuations FILE\n");
        return 2;
    }
    const synthetic::Program prog = synthetic::build_test_programs(kBase);
    std::string seed_text, continuation_text;
    for (const Seed& s : prog.seeds) seed_text += std::format("0x{:08x}:0x{:08x} {}\n", s.addr, s.toc.value_or(0), s.name);
    for (u32 c : prog.continuations) continuation_text += std::format("0x{:08x}\n", c);
    if (!write(bin, std::string(prog.bytes.begin(), prog.bytes.end())) || !write(seeds, seed_text) ||
        !write(continuations, continuation_text)) {
        std::fprintf(stderr, "ppu-progs-gen: cannot write output\n");
        return 1;
    }
    return 0;
}
