# Runs 217–219 · SPU programs translated during Assassin's Creed's first session (D-230 to D-236)

2026-10-07 · pspocket-boot with the Metal backend · M4 Mac · Assassin's Creed (BLES00158), `games/ac1/walk-training.txt`,
150 s, on the library prepared from the executable alone (D-226: what the phone has the first time it meets the game).
Logs in inventory/BLES00158 (kept out of the repository).

## Without the translator (run 217)

22.06 billion SPU instructions interpreted in 150 s (run 207: 21.6), about 680 million every 5 s, in the modules and
jobs the game's tasks copy in from data: 30 images without a program, 180,891 loads interpreted. D-232's cache answered
154,913 of those loads without hashing the image again. 18 fps at the title screen, 25-33 in the Animus training.

## The first try (run 218, stopped)

Every program "did not compile": clang could not rename its temporary object file into place. The game faults on
purpose (write tracking, D-134); the first compile had installed LLVM's signal handlers over the game's, and they delete
the files of the compiles and links running at each fault before passing it on. Fixed in D-236 (LLVM's handlers
installed first, under the game's; clang's outputs not registered with them at all), with a test that compiles and
links while another thread takes page faults (it fails without the fix with the same message).

## With the translator (run 219)

| | run 217 (no translator) | run 219 (translator) |
|---|---|---|
| SPU instructions interpreted in 150 s | 22.06 billion | **0.94 billion (-96%)** |
| interpreted per 5 s, from 10 s on | 615-1,321 million | 0 to 79 million, then 0.3 million |
| fps, 100-150 s (the training) | 25.2-32.8 | **59.7-60.2** |
| SPU program loads lifted / interpreted | 19,971 / 180,891 | 186,665 / 11,477 |
| modules found lifted | 0 | 13,572 |

The translator met 17 images in four batches and loaded all 17 programs, none left to the next launch, none failed:
lifted in 0.02-0.13 s a batch, compiled in 0.2 to 5.4 s each on one core at UTILITY QoS (33 s in all), linked and
signed in under 0.05 s, loaded in about 0.25 s; from first sighting to loaded at most 17.1 s. What is left: 639 and
112 million instructions in the first two windows (before the first programs landed) and three windows of 46-79
million when the training met new jobs; then 0.3 million per 5 s (single entries the lifts had no seed for, D-208).

The switch went through the next loads: the modules and job binaries are copied in again so often that each next load
found its new program (D-232's cache moves on with the registry). D-233's switch of a running context and D-234's
second lookups did not have to act here (both are covered by tests).

The cost model (fitted on GTA V's SPU code) predicted 0.2 to 576.7 s for compiles that took 0.2 to 5.4 s: the limit
above which a program waits for the next launch now scales the prediction by the session's own compile speed.
