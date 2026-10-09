/*
 * PSN games for the app shell (Swift) and command-line tools (D-580 to D-583): the player's licences, and a PSN
 * game's executable. A PSN game lives in /dev_hdd0/game/<TITLE_ID>/ (installed from its .pkg); its executable and
 * modules are NPDRM SELFs and some of its data files EDATA files, opened with the player's licence for the game (a
 * .rap file) or, for free content, the klicensee the game holds.
 */
#ifndef PSPOCKET_DISC_PSN_H
#define PSPOCKET_DISC_PSN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

#endif /* PSPOCKET_DISC_PSN_H */
