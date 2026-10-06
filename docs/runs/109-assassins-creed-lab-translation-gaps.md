# Runs 109–118 · Assassin's Creed in the Abstergo lab: what still ran in the interpreters

2026-10-06 night to 2026-10-07 · pspocket-boot with the library prepared on the Mac (`pspocket-prepare --spu-images
inventory/BLES00158/spu-images`) · M4 Mac · Assassin's Creed (BLES00158), firmware 4.92 · the scripted walk
`games/ac1/walk-garden.txt` (START through the screens, Cross through the menus, then the sticks), 150 to 380 s.

A new game starts in the Abstergo lab; the white frame near frame 8100 is the white fade out of the Animus. The lab
renders right now. No unimplemented calls in any of these runs. Logs in inventory/BLES00158 (kept out of the repository).

## The sessions

| run | what changed before it | GPU | fps, 5 s windows (median) | PPU jumps through the interpreter | SPU instructions interpreted at LS 0x4c00 | notes |
|---|---|---|---|---|---|---|
| 109 | (the D-202 build) | Metal | 29.2 | 108 | 64.6 billion | 380 s |
| 110 | | Metal | 18.0 | 101 | 64.0 billion | 380 s; a slower stretch |
| 111 | | Metal | 29.6 | 85 | 39.4 billion | 300 s |
| 112 | | none | 59.8 | 38 | 0.12 billion | 150 s (menus mostly) |
| 113 | SPU module truncation (D-204, first half) | none | 31.6 | 86 | (no summary) | ended at 280 s: GPU command reader desync, below |
| 114 | `--rsx-recover` | none | 32.6 | 114 | 64.8 billion | 380 s; no desync this time |
| 115 | jump tables (D-203); `--log debug` | none | 59.5 | **0** | 11.2 billion | 200 s; the DMAs that drop SPU programs logged |
| 116 | D-204, D-205; prepared again | none | **60.0** | 0 | **0** | 380 s; 1.2 CPU cores busy instead of 3.6 |
| 117 | D-206; prepared again with run 116's misses as seeds | Metal | **51.0** | 0 | 0 | 380 s, frames saved: the screens, the menu (CONTINUE), the Animus loading, the lab |

"none": pspocket-boot without `--frames` or `--gpu metal` runs the GPU commands without drawing (the null backend), so
those fps say how fast the CPU side is, not the game on screen. Runs 114 and 116 are the fair pair for the SPU fix.

With the GPU, before the fixes, the lab ran at 28 to 40 fps and the menus at 60.

## PPU: one switch through the interpreter (D-203)

"indirect jump to 0x00a88fb4 has no lifted function" 85 to 114 times a session: a GCC switch at 0xa88f18 whose table
address sits two loads away in the game's data. The analyzer did read it, on its second pass, but threw the pass away:
the switch's cases restore the table register and then tail-call (not counted as leaving the function), and the
bounds check branches *to* the table code (`ble`) instead of falling into it. With both handled, run 115 has none, and
the whole executable's translation found 21 more tables and bounded 80 more (warnings 144 to 64). GTA V: the same 133
tables, 2 more bounded.

## SPU: one job chain interpreted (D-204, D-205)

Workload 1 of the 5-SPU SPURS instance (a job chain) takes 14 s of SPU time every 5 s, and its jobs ran in the
interpreter: about 64 billion instructions at LS 0x4c00 per six-minute session. "SPU program loads: 743,296 lifted, 0
interpreted" said the job binaries were found; what dropped them came later.

- Run 115's debug log: each job of one binary (spu_4944…, 42 KB) writes 4 bytes at 0xf330, its own data just past its
  code, 2,097 times. The translated program counted those words as instructions, so the write dropped the program
  (D-085) and the cached binary ran interpreted from then on; the module tracker then found the binary's tail
  changed and refused it too ("changed before it ran", 98,663 times in run 114).
- Why the words counted: the program (prepared on 2026-10-06) had 10,656 of its 10,764 words as code, including the
  job's shuffle patterns and the zeros after them. Prepare carries seeds from one lift to the next, and took them from
  the previous program's entries, which are all its labels, also those of code only discovery found. After the
  second prepare that guessed code was "code". Translated again with only the real seeds: 4,389 code words.
- Fixes: prepare keeps the seeds a lift used (NAME.seeds) and carries over those; code only a constant points at is
  speculative, like discovered code; a module cut by a later copy keeps its identity while its code is intact.

## The GPU command reader desync (run 113, open)

`[fatal] rsx: unhandled RSX method unknown (0x8f40) arg 0x00000010 (FIFO 0x001b4204, header 0x02b48f40)` at 280 s: a
header for 173 arguments on subchannel 4, which no game writes; the reader lost its place. Not seen again in runs 114
to 116, which ran with `--rsx-recover` (the phone's setting) so that a repeat logs the desync report (the words read and
what memory holds now) instead of stopping.

## With the GPU: run 117 against run 109

Same walk, Metal drawing, frames saved every 900. Run 109 (before): after the menus 27.6, 58.4, 15.0, **1.0**, 38.2,
then 18 to 24 fps for a minute, then 28 to 30 in the lab. Run 117: 31 to 47 through the same stretch, then **49 to 60
in the lab** (GPU 13.5 to 15.9 ms a frame, 670 to 1,007 draws, 35 render passes). Median of all 5 s windows 29.2 to 51.0.
The walk continues a save in run 117 (the menu offers CONTINUE since run 109's new game), so the lab scenes are close,
not identical.

## Still interpreted

- LS 0x0e408 and 0x0e740 in the job program spu_4944… (16 million instructions from half a million entries in run
  116, "in no program" before the job stayed lifted): code the translation had not reached. Seeded from run 116's log
  (`pspocket-prepare --spu-log`), lifted in run 117.
- LS 0x37c58, 81 million instructions a session: a task on the secondary SPURS instance copies a 32 KB module from
  guest 0x50195b00 to LS 0x33980 and calls 0x37c58. The guest buffer is reused before the module runs (D-206 now
  knows it by its local store copy), but each load differs: 398 different images in run 117, so no whole-image hash
  can name it. Run 118 saves them (`--spu-dump`) to see which bytes differ.
