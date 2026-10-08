// ps3disc: extract the executables from a PS3 disc image (host side of ps_disc_extract_executables), or decrypt its
// EBOOT.BIN into an ELF (ps_disc_decrypt_executable, D-121).
//
//   ps3disc <image.iso> <out-dir>
//   ps3disc --decrypt <image.iso> <out.elf>
//   ps3disc --modules <image.iso> <prx-dir> <spu-dir>
//
// --modules saves the game's own PRX modules (decrypted, as <identity>.elf) and standalone SPU ELFs, as a preparation
// reads them (D-239).
//
// Copies PARAM.SFO, EBOOT.BIN and the executable modules only; game assets stay in the image. The output is derived
// from game data: keep it in inventory/ (gitignored), never in git.

#include <cstdio>
#include <string_view>

#include "pspocket/disc/disc.h"

int main(int argc, char** argv)
{
    if (argc == 4 && std::string_view(argv[1]) == "--decrypt") {
        char error[512] = {};
        if (ps_disc_decrypt_executable(argv[2], argv[3], error, sizeof error) != 0) {
            std::fprintf(stderr, "ps3disc: %s\n", error);
            return 1;
        }
        std::printf("%s\n", argv[3]);
        return 0;
    }
    if (argc == 5 && std::string_view(argv[1]) == "--modules") {
        char error[512] = {};
        const int saved = ps_disc_save_modules(argv[2], argv[3], argv[4], error, sizeof error);
        if (saved < 0) {
            std::fprintf(stderr, "ps3disc: %s\n", error);
            return 1;
        }
        std::printf("%d files saved\n", saved);
        return 0;
    }
    if (argc != 3) {
        std::fprintf(stderr, "usage: ps3disc <image.iso> <out-dir>\n       ps3disc --decrypt <image.iso> <out.elf>\n");
        return 64;
    }
    return ps_disc_extract_executables(argv[1], argv[2],
        [](void*, const char* line) {
            std::printf("%s\n", line);
            std::fflush(stdout);
        },
        nullptr);
}
