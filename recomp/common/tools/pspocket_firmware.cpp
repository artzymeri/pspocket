// pspocket-firmware: installs /dev_flash from a PS3 system software update (PS3UPDAT.PUP), as the app does on the
// phone (D-156). The update is the user's own download; what it installs stays out of the repository (rule 1).
//
//   pspocket-firmware PS3UPDAT.PUP DEV_FLASH_DIR
//
// Prints the update's version and, per dev_flash package, its name; then how many files and bytes went in.

#include <cstdio>

#include "pspocket/recomp/firmware.hpp"

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::fputs("usage: pspocket-firmware PS3UPDAT.PUP DEV_FLASH_DIR\n", stderr);
        return 64;
    }
    const auto r = pspocket::recomp::install_firmware(argv[1], argv[2], [](double fraction, const std::string& what) {
        std::printf("[%3.0f%%] %s\n", 100 * fraction, what.c_str());
        std::fflush(stdout);
    });
    if (!r) {
        std::fprintf(stderr, "pspocket-firmware: %s\n", r.error().message.c_str());
        return 1;
    }
    std::printf("firmware %s: %zu packages, %zu files, %.1f MB into %s\n", r->version.c_str(), r->packages, r->files,
        static_cast<double>(r->bytes) / 1048576.0, argv[2]);
    return 0;
}
