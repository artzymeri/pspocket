# Runs 288–303 · The 64x32 pass, and indexed draws run by Metal (D-271, D-272)

2026-10-08 · pspocket-boot with the Metal backend · M4 Mac, quiet · GTA V (BLES01807), `games/gta5/walk-prologue.txt`.
Logs in inventory/gta5 (kept out of the repository).

## The 64x32 pass (runs 288-295)

D-270's timing showed a pass of one draw into a 64x32 target taking 0.7-0.8 ms of fragment time. It is the last of
a chain that filters a 512x256 target down (256x128, 128x64, 64x32), one quad each (inline arrays of 24 words), all
with the same fragment program (440 instructions, 25 explicit-LOD samples). Taken out into a benchmark (the shader
cache's record compiled and drawn alone, 64x32 from a 512x256 texture) it costs 2 microseconds a draw, whatever the
sampler, mipmaps or constants; the 128x64 pass with the same program costs 0.14 ms in the game. Skipping that program's
draws (PSPOCKET_SKIP_FP, runs 292-295) gave no clear answer: the walk's GPU time a frame swings by 2 ms and more
between runs. Left open: likely an artefact of the timing mode (the GPU waits between passes).

## Indexed draws (runs 296-303)

Indexed draws were plain draws with the vertex shader reading its index: the vertex shader ran once per index
(D-271). Run Metal over the game's own big-endian indices instead, the shader reversing vertex_id:

- Runs 296-298 measured it with GTA V's shader cache still holding vertex programs of the same translation version
  without the new index modes: the walk drew broken geometry (frames dumped in runs 299-300), only with 16-bit
  indices in sight. The tests all passed (they compile afresh). Translation version 6, and a fingerprint of the
  translation in every cache record from now on (D-272).
- Run 302, the change on every other 30 frames (D-272's A/B): GPU time a frame in play 13.23 to 12.03, 16.49 to
  14.10, 9.11 to 8.38 ms.
- Run 303, the same with each pass timed: the G-buffer pass at 254 draws, vertex stage 2.98 to 1.26 ms; the other
  geometry passes about halved; fragment stages unchanged.

## The gate

Tests (with the new ones: both index widths, a 400-vertex mesh, the fingerprint) and spu-check pass; GTA V's walk
median 29.8 fps, slowest 29.4, 2 compiles in play, GPU 14.1 ms a frame at worst in play (15.9-17.7 in the gates
before), frame jumps 2 as before, 0 RSX failures.
