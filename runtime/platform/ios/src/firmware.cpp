// /dev_flash from PS3UPDAT.PUP on the phone (pspocket/platform/firmware.h, D-156).

#include "pspocket/platform/firmware.h"

#include <cstdio>
#include <filesystem>
#include <string>

#include "pspocket/core/log.hpp"
#include "pspocket/recomp/firmware.hpp"

extern "C" int ps_firmware_install(const char* pup_path, const char* dev_flash_dir, char* version, size_t version_size,
    char* error, size_t error_size)
{
    auto say = [](char* out, size_t size, const std::string& text) {
        if (out && size) std::snprintf(out, size, "%s", text.c_str());
    };
    if (!pup_path || !dev_flash_dir) {
        say(error, error_size, "missing paths");
        return 1;
    }
    // Into a new folder, then put in place: a failed install leaves the one before.
    namespace fs = std::filesystem;
    const fs::path dest = dev_flash_dir, fresh = dest.string() + ".new";
    std::error_code ec;
    fs::remove_all(fresh, ec);
    const auto r = pspocket::recomp::install_firmware(pup_path, fresh.string());
    if (!r) {
        fs::remove_all(fresh, ec);
        PS_LOG_ERROR("firmware", "installing {}: {}", pup_path, r.error().message);
        say(error, error_size, r.error().message);
        return 1;
    }
    fs::remove_all(dest, ec);
    fs::rename(fresh, dest, ec);
    if (ec) {
        say(error, error_size, "cannot put /dev_flash in place: " + ec.message());
        return 1;
    }
    PS_LOG_INFO("firmware", "installed {}: {} packages, {} files, {:.1f} MB", r->version, r->packages, r->files,
        static_cast<double>(r->bytes) / 1048576.0);
    say(version, version_size, r->version);
    return 0;
}
