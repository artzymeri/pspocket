#pragma once

// The game's disc image as its file systems (plan section 9: "read straight from the ISO"): the ISO 9660 reader of
// runtime/disc behind lv2's VfsImage, so nothing is extracted.

#include <memory>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/lv2/kernel.hpp"

namespace pspocket::loader {

struct DiscInfo {
    std::string volume_id;
    std::string title_id; // PARAM.SFO TITLE_ID, e.g. "BLES01807"
    std::string title;
    std::vector<u8> param_sfo; // PS3_GAME/PARAM.SFO as on the disc
};

// Opens a PS3 disc image (ISO 9660, decrypted at disc level) as a VfsImage. Null, with *error set, for an image
// that can't be read, isn't a PS3 game disc or is still encrypted at disc level.
std::shared_ptr<const lv2::VfsImage> open_disc_image(const std::string& path, DiscInfo* info, std::string* error);

// open_disc_image, then mounts the image read-only: /dev_bdvd from its root and
// /app_home from PS3_GAME/USRDIR, where a disc game started from the drive has its executable (RPCS3 mounts
// /app_home at the executable's directory). False, with *error set, for an image that can't be read, isn't a PS3
// game disc or is still encrypted at disc level.
bool mount_disc_image(lv2::Kernel& kernel, const std::string& path, DiscInfo* info, std::string* error);

} // namespace pspocket::loader
