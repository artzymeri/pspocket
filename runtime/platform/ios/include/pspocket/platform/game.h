/*
 * Running a game in the iOS app: what pspocket-boot does on the Mac (runtime/loader/tools/boot_main.cpp), with the
 * frames shown in a CAMetalLayer and the virtual controller driven by the app. C API for the Swift shell.
 *
 * One game per process: it runs until the app exits (nothing tears a running game down yet).
 */
#ifndef PSPOCKET_PLATFORM_GAME_H
#define PSPOCKET_PLATFORM_GAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps_game_options {
    const char* elf_path;  /* the game's executable, decrypted to an ELF (the disc's EBOOT.BIN is an encrypted SELF) */
    const char* iso_path;  /* the game's disc image, read in place: /dev_bdvd and /app_home */
    const char* hdd0_dir;  /* the game's /dev_hdd0 (created when missing): game data, saves, trophies */
    const char* flash_dir; /* /dev_flash, from the user's own firmware (D-067), or NULL */
    const char* log_path;  /* the run's log (written from scratch), or NULL */
    void* metal_layer;     /* the CAMetalLayer the frames go to (bridged), or NULL */
} ps_game_options;

/*
 * Boots the game on its own threads. Returns 0, or non-zero with a message in `error`. The layer is set up for the
 * backend's output (1152x640 BGRA8, its device, not framebuffer-only); call from the main thread.
 */
int ps_game_boot(const ps_game_options* options, char* error, size_t error_size);

/* The disc's title ID and title ("BLES01807", "Grand Theft Auto V"). Returns 0, or non-zero with `error` set. */
int ps_game_probe_disc(const char* iso_path, char* title_id, size_t title_id_size, char* title, size_t title_size,
    char* error, size_t error_size);

/* The virtual controller's state (port 0), as pspocket/hle/input.hpp's PadInput. */
typedef struct ps_game_pad {
    /* CellPadData button[2]: 0x80 left, 0x40 down, 0x20 right, 0x10 up, 0x08 start, 0x04 R3, 0x02 L3, 0x01 select. */
    uint16_t digital1;
    /* button[3]: 0x80 square, 0x40 cross, 0x20 circle, 0x10 triangle, 0x08 R1, 0x04 L1, 0x02 R2, 0x01 L2. */
    uint16_t digital2;
    /* Sticks, 0..255 with 128 at rest; y grows downwards. */
    uint8_t right_x, right_y, left_x, left_y;
    /* Pressure 0..255: right, left, up, down, triangle, circle, cross, square, L1, R1, L2, R2. */
    uint8_t pressure[12];
} ps_game_pad;

void ps_game_set_pad(const ps_game_pad* pad);

typedef struct ps_game_stats {
    int running;      /* booted and not exited */
    uint64_t flips;   /* frames the game presented */
    double seconds;   /* since the boot */
} ps_game_stats;

void ps_game_get_stats(ps_game_stats* out);

/* The last log lines (oldest first, one per line), at most `size - 1` bytes. */
void ps_game_log_tail(char* out, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* PSPOCKET_PLATFORM_GAME_H */
