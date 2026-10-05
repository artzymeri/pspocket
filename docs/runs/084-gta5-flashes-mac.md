# Run 084 · Characters flash red or black: reproduced, two causes fixed

2026-10-05 · 25d64ec+ (D-119, D-120) · M4 Mac · `pspocket-boot ... --frames DIR --frame-every 1 --frame-from 2850
--shader-cache inventory/gta5/shaders`, the bank walk of run 080; `--trace-frame N` on the frames found.

The iPhone report: characters, or some of their clothes, flash fully red or black. The Mac had it too, unseen in its
sparse frame dumps: every frame from 100 s on (2,032 frames, 30 fps kept by writing them on their own threads),
scanned for red and black bumps of up to five frames, showed a robber passing the camera in solid black, then
solid red, for seconds.

| Frames 3330 to 3420 | The robber | The guard's close-up (cutscene) |
|---|---|---|
| Before | black, then red (jacket, bag, hat) | black |
| A8B8G8R8 clear (D-119) | black, then his own colours | black |
| + target corners (D-120) | his own colours: green jacket, red and teal hat | black (next) |

- The red: the damage target is cleared to 0x00ff0000 (transparent red) and was stored as transparent blue (D-119).
- The black: the target was redefined narrower and wider within a frame and lost its contents (D-120).
- Still black: the security guard at the cut to his close-up: a 640x256 target inside the 2560x512 one evicts it.
