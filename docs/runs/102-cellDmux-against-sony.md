# Run 102 · pspocket's cellDmux against Sony's libdmux and libdmuxpamf

2026-10-06 evening · tests/hle/test_dmux.cpp after D-194 · M4 Mac · the user's firmware 4.92
(inventory/firmware/installed/dev_flash, PSPOCKET_TEST_DEV_FLASH). No game: neither game on the Mac (GTA V, Assassin's
Creed) imports cellDmux. The stream is synthetic, built in the test.

## The run

The same sequence of calls goes to pspocket's cellDmux and to Sony's libdmux, which loads libdmuxpamf and starts its
own SPURS instance ("_libdmux_pamf", one SPU) with its SPU task in the interpreter. Each observation is a line: the
answers, the sizes, the handles, each access unit (its information block, address, size, random access flag, user
data, timestamps, specific information, first bytes, and whether its bytes are the stream's), and every callback.

The stream: six 2048-byte packs. AVC video on channel 0 (four access units of 900, 2500, 300 and 700 bytes; the third
unit's delimiter split between two packs; a private stream 2 marking the first as a random access point; a PTS with
bit 31 set) and ATRAC3plus audio on channel 0 (five 264-byte frames, the fourth frame's sync word split between two
packs), then a program end code.

## Results, in order

| step | pspocket | Sony's | lines different |
|---|---|---|---|
| first run | everything | cellDmuxEnableEs: CELL_DMUX_ERROR_FATAL, then nothing | 69 of 83 |
| + cellSpursQueuePushBody (Sony's libdmuxpamf sends its SPU task commands with it) | | demuxes; its PPU thread repeats callbacks (no cellSpursQueueSize) | many |
| + cellSpursQueueSize | | the whole run | 2 of 83: cellDmuxQueryAttr's size |
| cellDmuxQueryAttr's size as Sony's libdmux computes it | 0x3e8e6 | 0x3e8e6 | **0 of 83** |

The size: Sony's libdmux asks for align16(0xC0 + 4 × 64 streams) + the core's 0x3e717 + 0xF = 0x3e8e6; RPCS3's
formula adds an elementary stream context, 0x50 more (0x3e936). The core's own size (0x1C + 0x3D880 + 0xE7B) is the
same in both. The table that turns the core's answers into the API's (0 OK, 1 FATAL, 2 to 5 ARG, 6 FATAL) is RPCS3's.

What the run shows beyond the size:

- Every access unit is at the same address in both, in queues laid out the same way: RPCS3's layouts of libdmux's
  and libdmuxpamf's structures are right.
- Three video units and four audio frames during the stream, one more of each from the flushes; then FLUSH_DONE.
- Timestamps as Sony's: the first unit of a PES packet takes its PTS and DTS; a unit that starts after another in the
  same packet has none; the unit whose delimiter was split between packs has none either, and the one after it takes
  that packet's; a PTS with bit 31 set comes back sign-extended (0xFFFFFFFF80003000).
- The errors on the way are the same: a second stream while one runs (BUSY), a flush while running and a release with
  nothing taken (SEQ), a stream enabled twice and handles after close or disable (ARG), an empty queue (EMPTY).

## Time

On the six packs: pspocket's run 0.007 s, Sony's about 0.010 s (its PPU code and SPU task interpreted). Too short a
stream to say anything about a movie's throughput.

## What it means for games

Until this run, a game that played PAMF movies through the firmware's libdmux could not: cellDmuxEnableEs failed
for want of the two SPURS queue functions. Now such a game gets pspocket's cellDmux (D-175: every function it imports
is on the host), and Sony's would work too.

## The gate

scripts/gate.sh all after the change (inventory/gate/20261006-185024-04f5e11): 18 of 18 checks pass, GTA V's prologue
walk at 29.8 fps (slowest window 29.4), no new warning or error shapes. GTA V doesn't import cellDmux: the check is
that nothing else moved.

