# Run 104 · pspocket's cellAdec against Sony's libadec

2026-10-06 night · tests/hle/test_adec.cpp after D-197 · M4 Mac · the user's firmware 4.92
(inventory/firmware/installed/dev_flash, PSPOCKET_TEST_DEV_FLASH). No game: neither game on the Mac imports cellAdec
(GTA V and Assassin's Creed play their sound through their own SPU programs). The audio is built in the test: tones
as 16- and 24-bit LPCM, byte patterns for DVD's 20- and 24-bit packing, MPEG-1 layer I frames whose allocations,
scale factors and samples are a pattern. Sony's modules are read from the user's install and never enter the
repository.

## Sony's libadec, as found by the probe

- Sizes: LPCM 0x7F524 for all three packings (the core's 0x7EBD5, eight frames of 0x68 bytes with 12-byte information
  blocks, the 0x530 context, 0x7F for alignment); version 0x492000, LPCM core 0x20070323.
- Layout: the handle is the memory; PCM items at +0x558, information blocks at +0x870, the LPCM output at +0xC80.
- AUDONE carries the frame's access unit information (+0x540 for the first); PCMOUT carries the error code.
- Querying a codec whose core is in no retail firmware (DTS, DTS-HD core, WMA Pro, WMA Lossless) crashes Sony's
  libadec: it calls through an empty table.

## pspocket's cellAdec against it

| | result |
|---|---|
| LPCM, PAMF packing: stereo (4 sequences of access units), mono, 3/0, 3/2 at 24 bits, 3/4+LFE; five decodes queued behind an output not taken (the fifth BUSY) | the same |
| LPCM, Blu-ray packing (parameters in each access unit): stereo, 3/2+LFE at 24 bits, 3/1; a reserved layout (FATAL) | the same |
| LPCM, DVD packing: 16-bit stereo and 5.1 (front pair only), 20- and 24-bit stereo, 24-bit 5.0, 20-bit 4.0; 24-bit 5.1 refused (FATAL) | the same |
| all of it: 221 answers, items, information blocks and callbacks; 22 PCM outputs (104,608 bytes) | 0 differences |
| MPEG-1 layer I through Sony's core module (cellMPL1dec), linked for pspocket's front end as the loader links it for a game, against Sony's libadec with the same core: 4 frames, one without a PTS | 43 lines and 4 PCM outputs the same (6,144 samples of sound) |

The first comparison found nothing to fix: RPCS3's reproduction of libadec's front end and LPCM core is exact, Sony's
version number aside (0x492000 here, 0x491000 in RPCS3).

Time: the three tests take 5.4 s together, most of it fixed waits for callbacks to settle and the core modules'
starts under the interpreter.

## The gate

scripts/gate.sh all at c861df4 (inventory/gate/20261006-210554-c861df4, load average 6.6 with an iOS simulator
running): 20 of 20 test suites; GTA V's walk 18 of 18, 29.8 fps median, 29.0 in the slowest 5 s window, 823 frames
compared, 921 MB peak. GTA V and Assassin's Creed do not import cellAdec; the loader change runs for every game.
