# Run 099 · Assassin's Creed: its first screens on the Mac, then a task-thread crash

2026-10-06 night · pspocket-boot after D-160 · M4 Mac · Assassin's Creed (BLES00158), the library prepared on the Mac
(inventory/BLES00158/prepared, 70,830 PPU functions, no SPU programs), firmware 4.92 at
inventory/firmware/installed/dev_flash.

## Where it gets

| Step | Before tonight | Now |
|---|---|---|
| imports | 100 of 140 | 140 of 140 (D-160); one syscall missing: 327, page-fault notification (the game ignores its result) |
| boot | spins on cellSyncMutexTryLock, no RSX | GCM up, RSX context, display buffers 1280x720, audio, SPURS tasksets and job chains |
| first minute | - | installs its HDD data (copies PIC1/ICON0/SND0, cache mount), plays nothing yet |
| screens | - | **the autosave notice ("Press the START button to continue")** from about 50 s, 18 fps |
| START | - | **the disclaimer ("Inspired by historical events and characters…")**, 60 fps |
| next | - | crash: a task thread (Bink's "binkliba" lock held) returns through a zeroed frame |

Frames (Metal, --frame-every 150, kept out of the repository): the notice, then the disclaimer.

## The crash

Always the same place and time, with or without the GPU, with or without background shader compiles, and with the
intro video (demointro.bik) hidden from the game (PSPOCKET_HIDE_FILES): a TaskThread (0x1000006 or 0x1000007) in
the game's job loop (0xD24630 → 0xBF1AE8 → 0xC258C0) reads address 0xffff95f0, a global through r30 = 0. What is
known:
- Not translation: with 0xC258C0 interpreted the interpreter itself reaches address 0 with LR = 0 and r27-r31 = 0
  (a callee returned through a frame whose saved registers are zero); with 0x880000-0xD20000 interpreted it reads
  address 8 in 0x85F220 instead, with the game allocator's freed-memory pattern 0xdddddddd in a register: a job
  object used after it was freed, by the look of it.
- Not the TOC: --watch on the TOC slot shows no store to it.
- Not an SPU: PSPOCKET_SPU_WATCH over all PPU stacks logs no SPU write there.
- Not the new cellPadGetInfo (its 648 bytes fit the game's 656-byte buffer), not cellFsStat (52 bytes, as RPCS3),
  not the spinlocks or lwmutex trylock (both as RPCS3).
- The task threads lock almost only with sys_lwmutex_trylock loops (9,390 of the last 40,000 calls).
Open: which host-side behaviour lets a job be freed under another thread. Next: compare the job object's lifetime
with --watch on its fields; check syscall 327's absence (the game's memory manager expects page-fault events) and
the AIO completion order.

## Tools added

pspocket-boot --watch ADDR[@SECONDS], fault reports that name each frame's image and base (atos on a game library
gives the guest function: `atos -o game.dylib -l BASE ADDR`), r11-r31, CTR and the interpreter's last branch to 0;
PSPOCKET_SPU_WATCH=LO-HI; PSPOCKET_HIDE_FILES=PATH,...
