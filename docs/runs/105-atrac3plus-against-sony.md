# Run 105 · pspocket's ATRAC3plus decoder against Sony's

2026-10-06 night · tests/hle/test_adec.cpp after D-198 · M4 Mac · the user's firmware 4.92 (PSPOCKET_TEST_DEV_FLASH).
ATRAC3plus has no encoder anywhere, so the measurement used a game's stream: Assassin's Creed's SND0.AT3 (the menu
music on its disc, RIFF ATRAC3plus, 48 kHz stereo, 688-byte frames, 302 of them), copied from the disc image into the
session's scratch folder and given to a hidden probe (PSPOCKET_PROBE_AT3). Nothing from it is in the repository. The
committed tests use frames built in code: valid silent stereo frames and one with its start bit set.

## Through the same calls

Both sides get the same cellAdec calls: pspocket's cellAdec with its own ATRAC3plus core on FFmpeg, and Sony's libadec
with Sony's libatxdec (its SPU program interpreted in the test). Every answer, PCM item, information block and callback
is compared as a line; the samples as floats.

| | result |
|---|---|
| sizes (cellAdecQueryAttr, ATRAC3plus) | 0x59F8B, version 0x492000 / 0x1020000: the same |
| frames built here: five valid silent frames, one with its start bit set | the same after one change: the bad frame's error is 0x806122B2 (FFmpeg's path gave 0x80612281) |
| the game's stream: 302 frames, 2,423 lines | 0 differences |
| the game's stream: 1,232,896 samples | largest difference 0.000001; the difference 101.1 dB below the signal |
| time for 12.9 s of sound | pspocket 0.79 s; Sony's 3.5 s with its SPU code interpreted |

The first frame of the sequence and the frame after an error come out empty from both (libatxdec's silence).

## The gate

scripts/gate.sh at d431227, three runs of GTA V's walk (load average 4.4 to 5.6; an iOS simulator running PSPocket
throughout). GTA V does not import cellAdec, so none of the changed code runs in it.

| run | result |
|---|---|
| 20261006-211613 (with the 20 test suites, all passed) | 17 of 18: one 5 s window late in the walk compiled 3 new shader programs (564 ms), so 7 programs and pipelines in play against a limit of 4; median 29.8 fps |
| 20261006-211917 | 17 of 18: one window at 22.0 fps (window 12; the limit is 24); median 29.8; 2 compiled |
| 20261006-212158 | 18 of 18: median 29.8 fps, slowest window 28.6 |

Each failure is one window at a different moment, with no code GTA V runs changed: interference from the machine,
not the change. Run 104's gate at c861df4 passed 18 of 18 the first time.
