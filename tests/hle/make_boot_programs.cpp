// Writes synthetic executables for the pspocket-boot end-to-end tests (tests/hle/CMakeLists.txt):
//   fault.elf  _start loads from unmapped guest address 0x80000000 (the boot host must name it and the PPU address)
//
//   make_boot_programs <dir>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "pspocket/core/be.h"
#include "pspocket/recomp/ppu/asm.hpp"
#include "synthetic.hpp"

using namespace pspocket;
namespace as = pspocket::recomp::ppu::as;

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: make_boot_programs <dir>\n");
        return 64;
    }
    const auto x = test::build_ppu_exec();
    std::vector<u8> bytes = x.bytes;
    const u32 code[] = {
        as::lis(3, -0x8000), // r3 = 0x80000000 (sign-extended; the EA is its low word)
        as::lwz(4, 0, 3),    // fault
        as::li(3, 0),
        as::li(11, 22), // _sys_process_exit(0): not reached
        as::sc(),
    };
    for (usize i = 0; i < std::size(code); i++) ps_st32(bytes.data() + 0x200 + 4 * i, code[i]); // _start (0x10200)

    std::filesystem::create_directories(argv[1]);
    std::ofstream out(std::filesystem::path(argv[1]) / "fault.elf", std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out ? 0 : 1;
}
