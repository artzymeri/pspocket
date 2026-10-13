/*
 * PSN games for the app shell (Swift) and command-line tools (D-580 to D-583): installing a game's package, the
 * player's licences, and a PSN game's executable. A PSN game lives in /dev_hdd0/game/<TITLE_ID>/ (installed from its
 * .pkg); its executable and modules are NPDRM SELFs and some of its data files EDATA files, opened with the player's
 * licence for the game (a .rap file) or, for free content, the klicensee the game holds.
 */
#ifndef PSPOCKET_DISC_PSN_H
#define PSPOCKET_DISC_PSN_H

#include <stddef.h>

#include "pspocket/disc/disc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Installs a PS3 package (.pkg: a PSN game, its update or add-on, or a licence package) under hdd0_dir, the folder
 * that is /dev_hdd0 (the app's Documents/hdd0, pspocket-boot's --hdd0), as RPCS3 installs it (D-583): a game, update
 * or add-on into game/<TITLE_ID>/, a licence package into home/00000001/exdata/. A package split into parts is given
 * by its first, NAME_00.pkg. The game's executable, modules and EDATA files are written as they are, for the runtime
 * to open with the player's licence (ps_psn_install_rap); SDATA files are written decrypted. An update that does not
 * suit the version of the game installed is refused, as RPCS3 refuses it, so a game's packages go in order: the game,
 * then its updates. callback (may be NULL) hears a line for the package, for each folder and file, a summary, and, for
 * a game, which licence its executable needs and whether it is in place. Returns 0, or 1 with the reason in error.
 */
int ps_psn_install_pkg(const char* pkg_path, const char* hdd0_dir, ps_disc_callback callback, void* user, char* error,
    size_t error_size);

/*
 * Where pspocket looks for the player's licence of a piece of PSN content (D-580), as RPCS3 does:
 * <hdd0_dir>/home/00000001/exdata/<content_id>.rap, hdd0_dir being the folder that is /dev_hdd0 (the app's
 * Documents/hdd0, pspocket-boot's --hdd0). Writes the path into out and returns 0; 1 when the content ID is not
 * valid (1 to 48 characters of A-Z, a-z, 0-9, '-' and '_') or the path does not fit in out_size bytes.
 */
int ps_psn_rap_path(const char* hdd0_dir, const char* content_id, char* out, size_t out_size);

/*
 * Installs a licence file the player has (16 bytes, named <CONTENT_ID>.rap, the extension in any case) where
 * pspocket looks for it, making the folders; a licence already there is replaced. Returns 0, or 1 with the reason in
 * error: not a .rap name, an invalid content ID, a file that is not 16 bytes, a write that failed.
 */
int ps_psn_install_rap(const char* hdd0_dir, const char* rap_path, char* error, size_t error_size);

/*
 * Decrypts a PSN game's executable into the ELF it carries at elf_path (its folder is made), as the disc's is
 * (ps_disc_decrypt_executable) but with its licence (D-582): free content opens with NP_KLIC_FREE, other content with
 * the player's .rap for its content ID under hdd0_dir (ps_psn_rap_path; hdd0_dir may be NULL when there are none).
 * self_path is the executable (EBOOT.BIN, or an SPRX module) or a game's folder (/dev_hdd0/game/<TITLE_ID>, whose
 * USRDIR/EBOOT.BIN is taken, or a disc's folder with PS3_GAME/USRDIR/EBOOT.BIN). A disc SELF and a debug SELF decrypt
 * too; a plain ELF is copied. Returns 0 on success; otherwise 1 and the reason in error (a missing licence names the
 * file it was looked for in).
 */
int ps_psn_decrypt_executable(const char* self_path, const char* hdd0_dir, const char* elf_path, char* error,
    size_t error_size);

/*
 * Saves the own PRX modules and SPU ELFs of a game installed in /dev_hdd0/game (a PSN game, a disc game's update) for
 * its preparation (D-239, D-939), as ps_disc_save_modules does a disc's: game_dir is its folder, hdd0_dir the folder
 * that is /dev_hdd0 (the player's licences are under it). Returns how many files it saved (or found saved), or -1 with
 * the reason in error.
 */
int ps_psn_save_modules(const char* game_dir, const char* hdd0_dir, const char* prx_dir, const char* spu_dir,
    char* error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif /* PSPOCKET_DISC_PSN_H */
