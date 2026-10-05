# Run 097 · Firmware modules (LLE): the PS3's own modules loaded where pspocket has nothing

2026-10-06 · pspocket-boot after D-157 · M4 Mac · firmware 4.92 installed from PS3UPDAT.PUP with pspocket-firmware
(D-156) into inventory/firmware/installed (never committed).

## The index

`pspocket-boot … --flash inventory/firmware/installed/dev_flash --imports`, first run: "142 of 142 firmware modules
read: 163 libraries" in 2.5 s (every module of sys/external decrypted with pspocket's own SELF code and its exports
read); later runs read lle-index.txt.

## Assassin's Creed (BLES00158), interpreter, 60 s

| Run | Libraries from firmware | Where it stops |
|---|---|---|
| run-mac-1 (before) | none | the main thread spins on the missing cellSyncMutexTryLock (24 million calls in 90 s); the RSX is never made |
| `--lle cellSync` | cellSync from libsre.sprx (it exports cellSpurs too: forced only) | past the spin: cellDiscGameRegisterDiscChangeCallback, then sys_mmapper_allocate_memory (missing), "tty: out of memory", a NULL read at 0x0c |

The firmware module: "cellSre_Library (libsre.sprx): 2 segments at 0x1200000, 1430 relocations (0 bad), 267 functions
exported, module_start 0x0"; 3 of the game's 140 imports linked to it. By default the game loads no module: each of its
10 import libraries has host functions (D-157's per-module rule), so its gaps are the host's to fill.

## Tests

tests/hle/test_lle.cpp: a synthetic PRX placed and relocated (all nine relocation types, two bad entries skipped), its
exports and imports; a /dev_flash of debug SELFs indexed, cached and chosen per module; a synthetic executable whose
import library only the module has, booted end to end, the module's function answering and its module_start run
before the entry. ctest: 19 of 19.
