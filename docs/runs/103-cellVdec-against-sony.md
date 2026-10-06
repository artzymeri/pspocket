# Run 103 · Sony's libvdec runs in a test, then pspocket's cellVdec against it

2026-10-06 evening · tests/hle/test_vdec.cpp after D-195 and D-196 · M4 Mac · the user's firmware 4.92
(inventory/firmware/installed/dev_flash, PSPOCKET_TEST_DEV_FLASH). No game: neither game on the Mac plays a movie
through libvdec. The stream is synthetic: 64x64 H.264 pictures whose macroblocks are all I_PCM (h264_writer.hpp), so
the decoded samples must equal the written ones in any decoder. Sony's work is read in the scratch folder only: the
decrypted libvdec, libavcdec, libsre, never in the repository.

## Getting Sony's libvdec to run

| try | stopped at | what pspocket lacked |
|---|---|---|
| 1 | cellVdecOpenEx: FATAL | cellOvisGetOverlayTableSize, cellOvisInitializeOverlayTable (libsre's overlay tables) |
| 2 | the SPURS taskset policy: "a task with an exit code container is not implemented" | a task's exit callback (ELF bit 0): libvdec's first task chains the next ones that way |
| 3 | the policy: "libsre's own exit handler is not implemented" | libsre's exit code handler: the code into the container, the waiting task signalled |
| 4 | the decoder waits forever | (my misreading of the container: the waiting task's taskset is a doubleword at +0x30, its ID at +0x38) |
| 5 | runs through | |

Then Sony's libvdec opened, decoded the three pictures exactly, and closed.

## What Sony's libvdec does

- Sizes: H.264 from libavcdec's avcDecQueryMemory, reconstructed and checked against 46 answers; the other codecs per
  level (81 answers); version 0x4920000.
- The handle is the memory address.
- Callbacks for six access units: AUDONE 0, AUDONE 1, PICOUT 0, AUDONE 2, PICOUT 1, ... AUDONE 5, PICOUT 4, then at
  the end of the sequence PICOUT 5 and SEQDONE: each picture one access unit late.
- Picture items at +0xDC, +0x16C, ... (0x90 apart); H.264 information blocks at +0x1B9C, ... (0x244 apart: larger than
  RPCS3's structure; past it the picture order count type, the VUI timing, the picture's number and size).
- An item's start address is the picture in a frame buffer: luma in 16-byte strips (two strips of repeated edge on
  each side), 1 KB of zeros, chroma rows of 8 Cb then 8 Cr in strips, 1 KB of zeros (0x3800 bytes for 64x64).
- RGBA/ARGB: BT.601 or BT.709 limited range as asked; Sony's integer rounding is its own (the exact coefficients,
  truncated, agree on 91%; the rest one step off). UYVY repeats each chroma row; YUV 4:2:0 is the samples.

## pspocket's cellVdec against it

| | result |
|---|---|
| sizes, 10 codec types x 44 levels | 440 of 440 the same |
| answers, items, information blocks, callbacks (66 lines) | the same after one fix: closing a closed decoder is SEQ (Sony's context stays in its memory), not ARG |
| frame buffers (6) | the same |
| pictures: YUV 4:2:0, UYVY | the same |
| pictures: RGBA, ARGB (4 of them, 49,152 colour values) | 4,473 one step off (9.1%), none more; alpha the same |

Time: pspocket's whole run 1.2 s and the comparison 2.5 s, each with a fixed 1 s wait for pictures to settle: on six
64x64 pictures both decoders are quick (Sony's SPU decoder interpreted). A real movie's speed is still to measure.

## The gate

scripts/gate.sh all at 0da033a (inventory/gate/20261006-200851-0da033a): 18 of 18, GTA V at 29.6 fps median but
its slowest 5 s window at 24.4 (the limit is 24; 29.4 in run 102). A rerun of the game step
(20261006-203128-0da033a) failed two checks (slowest window 14.6, 661 frames compared) while the Mac's load average was
42 (an iOS simulator running PSPocket, a system asset service at 98% CPU, a browser). GTA V creates no task with an
exit callback, so the SPURS change does not run for it; the check is repeated on a quiet machine before this is
settled.
