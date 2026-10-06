# Run 101 · The system software's SPU programs translated on the Mac

2026-10-06 afternoon · pspocket-prepare --system after D-190 and D-191 · M4 Mac (10 cores) · the user's firmware 4.92
(inventory/firmware/installed/dev_flash). No game runs here: neither game on the Mac (GTA V, Assassin's Creed) links a
firmware module, and the discs that do (L.A. Noire's cellVdec) are on the phone only.

## What the firmware holds

| | |
|---|---|
| SPU ELF images in the firmware | 188, of which 171 in the 100 modules the system library translates (the rest in modules pspocket serves itself: libsre, sys_audio, the voice and video player services) |
| programs made from them | 216: 154 main programs (the code a task or SPU thread enters) and 62 overlays, from the 6 images that keep code in overlays (the AAC and ATRAC encoders, the DivX parser, the video decoders' vdec_vpxp_core) |
| left out | 1: libchnnlsv's image has no code segment holding its entry |
| SPU code | 15.2 MB in the programs lifted; 16.4 MB in all the translated modules' images, mostly video codecs (3D Blu-ray MVC 1.6 MB, H.264 decode 1.5 MB, H.264 encode 2.8 MB, a second H.264 decoder 0.9 MB) |

## The size of the translation

| | C | compile on the M4 | library |
|---|---|---|---|
| PPU code only (D-178) | 131 MB | 33 s | 18.8 MB |
| + every SPU program, before D-190 | + 1.3 GB (one 57 KB decoder: 288 MB) | about 3 h (the tool's estimate, stopped) | - |
| + every SPU program, after D-190 | + 421 MB (with comments; 28 bytes of C per byte of code, GTA V's 36) | 19:27, 7.1 GB peak | 232 MB |
| again, objects kept | | 12 s (translate 5 s, link, sign) | |

D-190 (constants into a function's code are labels, not copies) took the C from 1.3 GB to 421 MB. Even so, all of it
is most of an hour on the phone and 213 MB that most games never use (encoders, Blu-ray codecs): D-191 adds a module's SPU
programs the first time a game that links it is played. The synthetic test shows the second preparation compiling
only the new programs (8 files, 2 kept) and the third nothing (0 files, 10 kept).

## Checked against the interpreter

spu-check --library (new) over all 216 programs, 2 random-state trials at up to 40 entries each, as the gate checks
GTA V's (D-132):

- First pass: about 13,900 trials compared, 2 differed (libavcdec's thread_a1 and libsjvtd's thread manager, both H.264
  decoder tasks). Neither reproduced with the program checked alone.
- The cause was spu-check, not the translation: a random state made one decoder DMA across 178,448 pages (2.8 GB) of
  its sandbox, more than the 65,536 its reset list holds, and the pages past the list kept their bytes into later
  trials. With the whole sandbox mapped again after such a trial: 0 of 930 differ in the pass that had both.
- A second seed then hung spu-check on libvpost2's video scaler: the interpreter had filled local store from 0x3000
  with 0xa7 (a random state), run into its own overwritten code and stopped on 0xa7a7a7a7. Such a trial is never
  compared, but spu-check ran the lifted code first, which runs what was translated, took another path and looped
  through an indirect branch where no stop request reaches it (docs/improve.md). spu-check now leaves such trials
  out before the lifted run, and reports a lifted run that ignores the stop request instead of hanging.
- With both fixes:

| seed | programs | trials compared | differ | not finished (200,000 steps) | wrote over their code |
|---|---|---|---|---|---|
| 1 | 216 | 12,890 | 0 | 1,595 | 2,209 |
| 2 | 216 | 12,899 | 0 | 1,568 | 2,227 |

  Seed 2's last two groups (17 programs) ran again once the disk had room (below).

## The disk

The checks stopped with "No space left on device": game::load_library copied every library it loaded into the
temporary folder and never removed the copies, 303 of them, 24 GB, the 232 MB system library once per spu-check
process. D-192 removes the copies of processes that have ended at each load.

## GTA V's gate

The first run after this session's changes failed 4 of 18 checks: 98 shader programs and pipelines compiled in play
(the slowest window 6.6 fps, 470 frames compared, 3 frame jumps). The cache held 3 of its programs and none of its
pipelines: D-174 (the last session's fragment program changes) changed the shaders, and the gate had not run on the
Mac since. That run made the cache again; the second passes 18 of 18: 29.8 fps, the slowest window 29.2, 833 frames,
2 compiles in play, 965 MB. On the phone the first session after installing a build with D-174 compiles them in the
background (D-158).

## Next

- The phone: a game that links a firmware module with SPU programs (L.A. Noire: cellVdec) prepares the system library
  for it before its first start, and its video decoding runs translated from the first frame. Not tried on the phone
  yet (the user asked for phone tests only once everything is implemented).
- Modules a game loads while it runs that its executable does not import still go through run-time discovery (D-117).
