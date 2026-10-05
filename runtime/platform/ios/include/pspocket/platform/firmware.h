/*
 * The PS3 system software on the phone (route B, stage 5, D-156): /dev_flash installed from the user's PS3UPDAT.PUP
 * (pspocket/recomp/firmware.hpp), so no Mac is needed for it. C API for the Swift shell.
 */
#ifndef PSPOCKET_PLATFORM_FIRMWARE_H
#define PSPOCKET_PLATFORM_FIRMWARE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Installs /dev_flash from the update at pup_path into dev_flash_dir (replacing what was there). Blocking: call from
 * a background queue. Returns 0 with the update's version ("4.92"), or non-zero with why. */
int ps_firmware_install(const char* pup_path, const char* dev_flash_dir, char* version, size_t version_size,
    char* error, size_t error_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PSPOCKET_PLATFORM_FIRMWARE_H */
