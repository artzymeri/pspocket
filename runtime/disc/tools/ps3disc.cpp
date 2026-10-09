// ps3disc: extract the executables from a PS3 disc image (host side of ps_disc_extract_executables), or decrypt a
// game's EBOOT.BIN into an ELF (ps_disc_decrypt_executable, D-121; a PSN game's with its licence, D-582).
//
//   ps3disc <image.iso> <out-dir>
//   ps3disc --decrypt <image.iso | EBOOT.BIN | game folder> <out.elf> [--hdd0 DIR]
//   ps3disc --modules <image.iso> <prx-dir> <spu-dir>
//
// --decrypt takes a disc image, an executable (a disc's or a PSN game's EBOOT.BIN, an SPRX) or a game's folder
// (dev_hdd0/game/<TITLE_ID>). A PSN executable opens with its licence: NP_KLIC_FREE for free content, else the
// player's <CONTENT_ID>.rap in <hdd0>/home/00000001/exdata, <hdd0> being --hdd0 or, when the input lies inside a
// dev_hdd0 folder, that folder.
//
// --modules saves the game's own PRX modules (decrypted, as <identity>.elf) and standalone SPU ELFs, as a preparation
// reads them (D-239).
//
// Copies PARAM.SFO, EBOOT.BIN and the executable modules only; game assets stay in the image. The output is derived
// from game data: keep it in inventory/ (gitignored), never in git.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "pspocket/disc/disc.h"
#include "pspocket/disc/psn.h"

namespace {

// Whether `path` is an executable or a game's folder rather than a disc image.
bool is_executable_or_folder(const std::filesystem::path& path)
{
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) return true;
    char magic[4] = {};
    std::ifstream in(path, std::ios::binary);
    if (!in.read(magic, sizeof magic)) return false;
    return std::memcmp(magic, "SCE\0", 4) == 0 || std::memcmp(magic, "\x7F" "ELF", 4) == 0;
}

// The dev_hdd0 folder `path` lies in, or "".
std::string enclosing_hdd0(const std::filesystem::path& path)
{
    std::error_code ec;
    const std::filesystem::path full = std::filesystem::absolute(path, ec);
    for (std::filesystem::path p = full; !p.empty() && p != p.parent_path(); p = p.parent_path()) {
        if (p.filename() == "dev_hdd0") return p.string();
    }
    return {};
}

int decrypt(int argc, char** argv)
{
    std::string hdd0;
    if (argc == 6 && std::string_view(argv[4]) == "--hdd0") hdd0 = argv[5];
    else if (argc != 4) return -1;
    char error[512] = {};
    const std::filesystem::path input(argv[2]);
    int status = 0;
    if (is_executable_or_folder(input)) {
        if (hdd0.empty()) hdd0 = enclosing_hdd0(input);
        status = ps_psn_decrypt_executable(argv[2], hdd0.empty() ? nullptr : hdd0.c_str(), argv[3], error, sizeof error);
    } else {
        status = ps_disc_decrypt_executable(argv[2], argv[3], error, sizeof error);
    }
    if (status != 0) {
        std::fprintf(stderr, "ps3disc: %s\n", error);
        return 1;
    }
    std::printf("%s\n", argv[3]);
    return 0;
}

int usage()
{
    std::fprintf(stderr, "usage: ps3disc <image.iso> <out-dir>\n"
                         "       ps3disc --decrypt <image.iso|EBOOT.BIN|game folder> <out.elf> [--hdd0 DIR]\n"
                         "       ps3disc --modules <image.iso> <prx-dir> <spu-dir>\n");
    return 64;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc >= 2 && std::string_view(argv[1]) == "--decrypt") {
        const int r = decrypt(argc, argv);
        return r >= 0 ? r : usage();
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
    if (argc != 3) return usage();
    return ps_disc_extract_executables(argv[1], argv[2],
        [](void*, const char* line) {
            std::printf("%s\n", line);
            std::fflush(stdout);
        },
        nullptr);
}
