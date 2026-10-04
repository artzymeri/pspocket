// ppu-dis: disassembles PPU code from a raw image (the loaded segments laid out from a base address, as ppu-lift reads
// them), for debugging a game's translated code.
//
//   ppu-dis --image FILE --base ADDR ADDR [COUNT]
//
// Prints COUNT (default 32) instructions from ADDR: address, word, disassembly.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/ppu/disasm.hpp"

int main(int argc, char** argv)
{
    std::string image;
    unsigned long base = 0, addr = 0, count = 32;
    int positional = 0;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--image" && i + 1 < argc) image = argv[++i];
        else if (a == "--base" && i + 1 < argc) base = std::strtoul(argv[++i], nullptr, 0);
        else if (positional == 0) addr = std::strtoul(argv[i], nullptr, 0), positional++;
        else if (positional == 1) count = std::strtoul(argv[i], nullptr, 0), positional++;
    }
    if (image.empty() || positional == 0) {
        std::fprintf(stderr, "usage: ppu-dis --image FILE --base ADDR ADDR [COUNT]\n");
        return 2;
    }
    std::ifstream in(image, std::ios::binary);
    const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    for (unsigned long n = 0; n < count; n++) {
        const unsigned long pc = (addr & ~3ul) + n * 4;
        if (pc < base || pc - base + 4 > bytes.size()) break;
        const unsigned w = ps_ld32(bytes.data() + (pc - base));
        std::printf("%08lx: %08x  %s\n", pc, w, pspocket::ppu::disassemble(w, static_cast<pspocket::u32>(pc)).c_str());
    }
    return 0;
}
