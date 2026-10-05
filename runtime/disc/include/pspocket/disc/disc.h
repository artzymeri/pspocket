/*
 * C API over the disc reader for the app shell (Swift) and command-line tools.
 */
#ifndef PSPOCKET_DISC_H
#define PSPOCKET_DISC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One report line (UTF-8). Called on the thread that runs the operation. */
typedef void (*ps_disc_callback)(void* user, const char* line);

/*
 * Inspects a PS3 disc image and copies out what the Phase 0 inventory needs, and nothing else: PS3_DISC.SFB,
 * PS3_GAME/PARAM.SFO, PS3_GAME/USRDIR/EBOOT.BIN and every executable module under USRDIR (.sprx, .prx, .self, .elf,
 * .bin up to 256 MB). Game assets stay in the image. Files land in out_dir mirroring the disc layout.
 *
 * Reports the volume, the PS3 region table, PARAM.SFO (title ID, version, ...), each extracted file with its size,
 * SHA-256 and whether it reads as a SELF/ELF, and whether the image is still encrypted at disc level (a Redump-style
 * image needs its disc key before anything can be read).
 *
 * Returns 0 on success, 1 if the image can't be read as a PS3 disc, 2 if it is disc-encrypted, 3 on I/O errors.
 */
int ps_disc_extract_executables(const char* image_path, const char* out_dir, ps_disc_callback callback, void* user);

/*
 * Decrypts the disc's executable (PS3_GAME/USRDIR/EBOOT.BIN, a SELF) into the ELF it carries at elf_path (its folder is
 * made), with the keys the build carries (D-121): on the phone, so any disc picked there can boot. Returns 0 on
 * success; otherwise 1 and the reason in error.
 */
int ps_disc_decrypt_executable(const char* image_path, const char* elf_path, char* error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif /* PSPOCKET_DISC_H */
