#pragma once

// PS3 system software updates (PS3UPDAT.PUP) and /dev_flash from them (route B, stage 5, D-156), on the Mac and on the
// phone: the files a PUP holds (RPCS3 Loader/PUP.h, PUP.cpp), the ustar archives inside (Loader/TAR.cpp), and the
// installation RPCS3's firmware installer makes: the update's dev_flash_* packages, each an SCE package
// (decrypt_sce_package) whose third section is a TAR of /dev_flash's files.

#include <functional>
#include <span>
#include <string>
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
};
// Installs /dev_flash from the update at pup_path into dev_flash_dir (the folder mounted as /dev_flash): every entry
// "dev_flash/X" of every dev_flash package's archive goes to dev_flash_dir/X. progress(fraction, what) between packages.
// `key` is the build's package key unless given (tests use made-up ones).
Result<FirmwareInstall> install_firmware(const std::string& pup_path, const std::string& dev_flash_dir,
    const std::function<void(double, const std::string&)>& progress = {}, const struct PackageKey* key = nullptr);

} // namespace pspocket::recomp
