#pragma once

// PS3 packages (.pkg, D-583): how a PSN game comes, and its updates, add-ons and licences. Installed under the folder
// that is /dev_hdd0 as RPCS3's package_reader installs them (rpcs3/Crypto/unpkg.cpp: read_header, read_metadata,
// set_decryption_key, read_entries, read_param_sfo, check_target_app_version, set_install_path, fill_data,
// extract_worker, extract_data and decrypt; the structures and constants in unpkg.h):
// - A package is a header ("\x7FPKG": the release type, the platform, where the metadata and the data are, the file
//   count, the content ID, the QA digest and the data's first counter block), metadata packets (the content type, the
//   package's flags, an add-on's install folder), then the data: a table of 0x20-byte entries (where each name and
//   file is, its size and type), the names and the files. A package too large for one file is split into NAME_00.pkg,
//   NAME_01.pkg, ... (the first is opened, the others are found beside it).
// - A retail package's data is AES-128 in counter mode: data byte i is XORed with byte i % 16 of the AES encryption,
//   under PKG_AES_KEY, of the header's counter block plus i / 16 (big-endian, over 128 bits). An in-store demo unit's
//   package (IDU) uses PKG_AES_KEY_IDU, which is tried when PKG_AES_KEY does not give a sound table. A debug package
//   XORs the first 16 bytes of SHA-1 over its QA digest and the block's number instead, with no key.
// - Where the files go (set_install_path): game/<install folder>/ for a game, its updates and add-ons (the install
//   folder is the title ID in the content ID, or an add-on's own folder from its metadata), home/00000001/exdata/
//   for a licence package, theme/, and widget/, vsh/modules/, home/00000001/psn_avatar/ and tmp/vmc/ with the install
//   folder for the other content types.
// - An update (its PARAM.SFO's CATEGORY "GD", with an APP_VER) must suit the game installed
//   (check_target_app_version): its TARGET_APP_VER must be the installed APP_VER; without one, a patch's APP_VER must
//   not be older than the installed one. A package for a game that is not installed passes unless it names a target.
// - Folders are made, and files written as they are, but for SDATA files, which are written decrypted (extract_worker
//   through DecryptEDAT). A PSN game's EDATA files and NPDRM executables stay encrypted: the runtime opens them with
//   the player's licence (D-581, D-582). A file already there is kept unless its entry is marked to overwrite, and of
//   two entries for one file the later one wins only when it is so marked (fill_data). A name with a "." or ".."
//   part or a leading "/", or one that leaves the install folder through a link already on disk, refuses the package.
// - When an install fails, what it wrote is removed if the install folder was new (extract_data); an update installed
//   into an existing folder leaves what it had written.
// The keys (key_vault.h PKG_AES_KEY, PKG_AES_KEY_IDU) come from the build like the SELF keys (scripts/self-keys.py;
// never committed); a debug package needs none. PSP and PS Vita packages, and PSP entries, are refused. As RPCS3, the
// package's digests and signatures are not checked.

#include <functional>
#include <string>
#include <string_view>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/npd.hpp"
#include "pspocket/recomp/npdrm.hpp"
#include "pspocket/recomp/result.hpp"

namespace pspocket::recomp {

struct PkgKeys {
    Key128 ps3{}; // PKG_AES_KEY: retail PS3 packages
    Key128 idu{}; // PKG_AES_KEY_IDU: in-store demo units' packages
    bool present = false;
};

// The build's package keys; `present` is false in a build without them (no RPCS3 source when it was configured).
const PkgKeys& builtin_pkg_keys();

// RPCS3 unpkg.h's content types (the metadata's), package flags and entry types used here.
inline constexpr u32 kPkgContentGameData = 0x04, kPkgContentGameExec = 0x05, kPkgContentTheme = 0x09,
                     kPkgContentWidget = 0x0A, kPkgContentLicense = 0x0B, kPkgContentVshModule = 0x0C,
                     kPkgContentAvatar = 0x0D, kPkgContentVmc = 0x11;
inline constexpr u32 kPkgFlagPatch = 0x10;
inline constexpr u32 kPkgEntryNpdrm = 1, kPkgEntryNpdrmEdat = 2, kPkgEntryRegular = 3, kPkgEntryFolder = 4,
                     kPkgEntrySdat = 9, kPkgEntryOverwrite = 0x80000000u, kPkgEntryPsp = 0x10000000u;

struct PkgInfo {
    std::string content_id;  // the header's
    bool retail = true;      // release type 0x8000; false: a debug package (0)
    u32 content_type = 0;    // metadata packet 2 (0 when the package has none)
    u32 drm_type = 0;        // metadata packet 1
    u32 package_flags = 0;   // metadata packet 3 (RPCS3's package_type)
    std::string install_dir; // the title ID in the content ID, or an add-on's folder (metadata packet 0xA)
    u32 file_count = 0;      // entries in the table, folders included
    u64 data_size = 0;
    u32 parts = 1;           // the files of a split package
};

// A package's header and metadata (RPCS3 read_header, read_metadata); no keys needed. Errors name what is wrong: not
// a package, a PSP or PS Vita one, a part of a split one missing, places past its end.
Result<PkgInfo> read_pkg_info(const std::string& pkg_path);

struct PkgInstalled {
    PkgInfo info;
    std::string install_path;     // the folder the files went to
    u64 files = 0, folders = 0;   // written, made (or found there)
    u64 bytes = 0;                // written
    u64 kept = 0;                 // files already there that the package did not mark to overwrite
    std::string eboot;            // the folder's USRDIR/EBOOT.BIN when the package wrote it (a game)
};

// Installs the package at `pkg_path` under `hdd0_dir`, the folder that is /dev_hdd0 (RPCS3 install_pkg's
// extract_data for one package). `progress` hears a line for the package and for each folder and file. Errors say
// why: not a package, a PSP or PS Vita one, no package keys in this build, a table neither key opens, an entry name
// that leaves the install folder or an entry of a type RPCS3 does not install, an update that does not suit the game
// installed (with both versions), a file that cannot be written, an SDATA file that does not decrypt.
Result<PkgInstalled> install_pkg(const std::string& pkg_path, const std::string& hdd0_dir,
    const std::function<void(std::string_view)>& progress = {}, const PkgKeys& keys = builtin_pkg_keys(),
    const NpdKeys& npd_keys = builtin_npd_keys());

} // namespace pspocket::recomp
