// ps3disc: extract the executables from a PS3 disc image (host side of ps_disc_extract_executables).
//
//   ps3disc <image.iso> <out-dir>
//
// Copies PARAM.SFO, EBOOT.BIN and the executable modules only; game assets stay in the image. The output is derived
// from game data: keep it in inventory/ (gitignored), never in git.

#include <cstdio>

#include "pspocket/disc/disc.h"

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: ps3disc <image.iso> <out-dir>\n");
        return 64;
    }
    return ps_disc_extract_executables(argv[1], argv[2],
        [](void*, const char* line) {
            std::printf("%s\n", line);
            std::fflush(stdout);
        },
        nullptr);
}
