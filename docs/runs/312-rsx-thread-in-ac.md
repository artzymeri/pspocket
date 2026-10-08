# Runs 312–323 · The RSX thread in Assassin's Creed's heaviest minute (D-277, D-278)

2026-10-08 · pspocket-boot with the Metal backend · M4 Mac. Logs in inventory/BLES00158 and inventory/gta5 (kept out
of the repository).

## Where the RSX thread's time goes (run 312)

Assassin's Creed's 600 s training walk, `sample` for 25 s from 563 s (the training's last minute, about 2,000 draws a
frame; sampling slows the game, the shares hold). Of the RSX thread's samples: 21% waiting for commands; the FIFO
reader 19% (step_commands 11.6%, dispatch 7.2%) and the vertex program and constant uploads' methods 8.7%; the draws
47% (texture binds 5.1%, the fragment program's lookup 4.6%, Metal's indexed draw call 4.7%, copies 5.1%: the transform
constants 2.7% of the thread, the vertex data 1.8%, the indices 0.5%), clears 2%. No single cost above a few percent.

## The FIFO reader (runs 313-322)

tests/rsx/bench_fifo.cpp (new, `test_rsx "[.bench]"`): command buffers shaped like the training's, 214 words a draw.
Reading headers and arguments in stretches (put, a reset and the address checked once a stretch, get published at its
end): 5.3 to 4.4 ns a word; with another thread polling get and writing put, 17-18 to 4.3-7.6 ns. The first stretch
reader broke GTA V in both its runs (314, 316: "malformed FIFO command 0x00041680"): GTA V parks the reader on a jump to
itself and overwrites that word with its next command, and the stretch read its last word twice. Fixed (one read a
word), then GTA V's walk with three binaries in rotation, two rounds (runs 317-322): RSX thread 1.296 and 1.379 s per
5 s (before), 1.353 and 1.328 (stretches), 1.321 and 1.313 (stretches and D-278); fps 29.8-29.9 all. No gain from the
stretches: not kept (D-277, as D-229). Kept: the reader goes on from a FIFO error's recovery.

## The transform constants (run 323)

D-278 under PSPOCKET_GPU_AB=10 in Assassin's Creed's 600 s walk, with the RSX thread's CPU time now summed per half:
in the nine windows of the last minute, 16.07/15.80, 15.98/15.76, 14.19/13.99, 13.61/13.49, 13.51/13.49, 13.49/13.44,
13.85/13.65, 14.10/13.94, 14.43/14.08 ms a frame (without/with): less in every one, 1.2% on average. GPU time the
same. The walk itself: 0 PPU instructions interpreted, 0 RSX failures, 32,107 flips; 38-48 flips a second in the last
minute.
