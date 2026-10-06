#pragma once

// PS3 system software updates (PS3UPDAT.PUP) and /dev_flash from them (route B, stage 5, D-156), on the Mac and on the
// phone: the files a PUP holds (RPCS3 Loader/PUP.h, PUP.cpp), the ustar archives inside (Loader/TAR.cpp), and the
// installation RPCS3's firmware installer makes: the update's dev_flash_* packages, each an SCE package
// (decrypt_sce_package) whose third section is a TAR of /dev_flash's files.

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/result.hpp"

namespace pspocket::recomp {

struct PupFile {
    u64 id = 0;     // 0x100 version.txt, 0x300 update_files.tar, ...
    u64 offset = 0;
    u64 size = 0;
};
struct Pup {
    u64 package_version = 0, image_version = 0;
    std::vector<PupFile> files;
    const PupFile* find(u64 id) const;
};
// The header and file table ("SCEUF"); every file must lie inside the data. The entries' HMACs are not checked.
Result<Pup> read_pup(std::span<const u8> pup);

struct TarEntry {
    std::string name; // the ustar prefix and name joined
    char type = 0;    // '0' or 0: a file; '5': a directory
    u64 offset = 0;   // of its data in the archive
    u64 size = 0;
};
Result<std::vector<TarEntry>> read_tar(std::span<const u8> tar);

struct FirmwareInstall {
    std::string version; // the PUP's version.txt, first line ("4.92")
    size_t packages = 0, files = 0;
    u64 bytes = 0;
    size_t files_skipped = 0; // of no use to games (firmware_path_used)
    u64 bytes_skipped = 0;
};
// Installs /dev_flash from the update at pup_path into dev_flash_dir (the folder mounted as /dev_flash): every entry
// "dev_flash/X" of every dev_flash package's archive that a game can use (firmware_path_used) goes to dev_flash_dir/X.
// progress(fraction, what) between packages. `key` is the build's package key unless given (tests use made-up ones).
Result<FirmwareInstall> install_firmware(const std::string& pup_path, const std::string& dev_flash_dir,
    const std::function<void(double, const std::string&)>& progress = {}, const struct PackageKey* key = nullptr);

// Whether a /dev_flash path (relative: "sys/external/libfs.sprx") is of use to a game (D-177): the system modules and
// the SPU programs games load (sys/), the system fonts (data/font) and the root certificates (data/cert). The XMB,
// the Blu-ray player, the PS1/PS2/PSP emulators and the keyboard dictionaries are not: 150 of 4.92's 188 MB.
bool firmware_path_used(std::string_view path);
// Removes from an installed /dev_flash what firmware_path_used leaves out (an install from before D-177). The bytes
// freed.
u64 prune_firmware(const std::string& dev_flash_dir);

} // namespace pspocket::recomp
