/*
 * Preparing a game on the phone (route B, stage 4, D-154; pspocket/prepare/prepare.hpp): the game's executable
 * translated and compiled into its game library, linked and signed with the user's development identity, once, the
 * first time it is played ("Preparing game…"). C API for the Swift shell; one preparation at a time, on its own
 * threads.
 */
#ifndef PSPOCKET_PLATFORM_PREPARE_H
#define PSPOCKET_PLATFORM_PREPARE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PS_PREPARE_IDLE = 0,
    PS_PREPARE_TRANSLATING = 1,
    PS_PREPARE_COMPILING = 2,
    PS_PREPARE_LINKING = 3,
    PS_PREPARE_SIGNING = 4,
    PS_PREPARE_DONE = 5,
    PS_PREPARE_FAILED = 6,
};

typedef struct ps_prepare_status {
    int phase;           /* PS_PREPARE_* */
    double fraction;     /* of the whole preparation, by time */
    double seconds;      /* since it started */
    double seconds_left; /* -1: not known yet */
    uint32_t files_done, files_total;
    char detail[160];    /* what it is doing, in a few words */
    char error[512];     /* why it failed */
    char library[1024];  /* the game library, once done */
} ps_prepare_status;

/* Whether game_dir holds a game library this app can run for this executable (1), or not (0, with why). */
int ps_prepare_check(const char* game_dir, const char* executable, char* why, size_t why_size);

/* Starts preparing: the executable (decrypted ELF), the game's title ID and its folder (the library, its manifest
 * and the work go there). Returns 0, or non-zero with a message (one already running, no compiler in this build,
 * no signing identity on this phone). */
int ps_prepare_start(const char* executable, const char* title_id, const char* game_dir, char* error, size_t error_size);

void ps_prepare_get_status(ps_prepare_status* out);

/* Stops at the next file; what is compiled is kept, so the next start goes on from there. */
void ps_prepare_cancel(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PSPOCKET_PLATFORM_PREPARE_H */
