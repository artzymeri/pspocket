# Runs 269–287 · Where GTA V's GPU time goes, and the vertex outputs (D-268 to D-270)

2026-10-08 · pspocket-boot with the Metal backend · M4 Mac, quiet (no simulator) · GTA V (BLES01807),
`games/gta5/walk-prologue.txt` (128 s). Logs in inventory/gta5 (kept out of the repository).

## The SPU side first (run 269, `sample` at 70 s for 20 s)

The SPURS threads' busy time (a third of their samples; the rest waits for work on the Mac) is two thirds translated
SPU code. Of that, the EDGE geometry program (spu_dad457863220b1cc) is 44%: shufb 16%, quadword loads 16%, fma 8%,
loads and stores from software-pipelined loops whose shuffle patterns are computed, so no pattern is known when
translating. Its hot functions make almost no calls: the call overhead (entry loads 7%, calls 7%, exit stores 4.5% of
the translated samples) is spread over many small functions. Nothing there promises more than a few percent.

## The GPU side

The phone's GPU is several times slower than the M4's; GTA V needs 15-17 ms of the M4's GPU a frame, so on the phone
the GPU may be what holds heavy scenes at 15-20 fps.

- Instruments' Metal System Trace attached to pspocket-boot (run 270) has no GPU timeline rows.
- Run 271 timed each render pass in a command buffer of its own: the times added to 100-127 ms a frame, impossible at
  30 fps: command buffers overlap on the GPU.
- Run 272, each pass waited for: 17-20 ms a frame in about 89 passes, the numbers add up. The G-buffer pass (four
  1280x720 colour targets and depth, 250 draws) 4.2 ms, 23%; the lighting pass 2.0 ms; a 64x32 pass of one draw
  0.8 ms.
- Run 278, the vertex and fragment stages apart (Metal's stage boundary timestamps, D-270): the G-buffer pass 2.7 ms,
  1.7 of it the vertex stage; a 1280x720 pass of 56 draws 1.1 ms, 0.8 vertex; the geometry passes' vertex stages
  about 3.5 ms a frame together. The 64x32 pass is all fragment (0.8 ms for 2,048 pixels).

## Two tries at the vertex stage

| Runs | Change | GPU ms a frame in play | |
|---|---|---|---|
| 274-277 | D-268: vertices and indices in whole words, not byte by byte | 14.45, 14.56 with; 14.23, 14.77 without | no difference: not kept |
| 280-287 | D-269: a vertex program puts out only the outputs its fragment program reads | 16.20 against 16.52 (three pairs, the same five windows) | -2%, lower in each pair: kept |

In the timing runs the G-buffer pass's vertex stage is about 15% less a draw with D-269 (run 279 against 278). The
Mac warmed up over the session: runs 282-287 are about 2 ms slower than 280-281 with either binary, hence pairs.

## The gate

After D-269 (translation version 5, the cache the A/B runs made): tests and spu-check pass; the walk median 29.8 fps,
slowest window 29.6, 2 compiles in play, GPU 17.7 ms a frame at worst in play (a warm Mac), 0 RSX failures.

Next: the 64x32 pass's fragment program (0.7-0.8 ms for 2,048 pixels), then the post-processing chain.
