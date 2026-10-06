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

Run with D-198 on a quiet machine (below).
