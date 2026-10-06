# Run 106 · pspocket's cellPngDec against Sony's libpngdec

2026-10-06 night · tests/hle/test_pngdec.cpp after D-199 · M4 Mac · the user's firmware 4.92 (PSPOCKET_TEST_DEV_FLASH).
No game: neither game on the Mac imports cellPngDec. The images are built in the test (png_writer.hpp): 21 PNGs of
13x7 and 19x11 pixels, every colour type at every bit depth, two Adam7-interlaced, three with tRNS, one with every
ancillary chunk kind (gAMA, sRGB, pHYs, bKGD, sBIT, oFFs, cHRM, sPLT, pCAL, sCAL, text before and after the image data,
zTXt, iTXt, tIME, a private chunk), one palette image with hIST, one RGB image with an iCCP profile. Each is decoded with
nine output settings (RGBA, ARGB, bottom to top, RGB, gray, gray with alpha, palette, 16 bits, packed), into rows wider
than the image so the padding shows. Sony's libpngdec is read in the scratch folder only.

## What Sony's libpngdec does (the probe)

- It is libpng 1.2.8 with zlib 1.2.3, and an SPU program for SPU-assisted decoding.
- A handle and a stream carry magic words (0xEF58AD41, 0xED37BC51) and states (opened 1, header 3, parameters 4,
  decoded 5, error 6).
- A chunk query that finds nothing answers SEQ and puts the stream in the error state. That made the first probe look
  as if only the text query worked: each test then asked on a stream of its own.
- An Open refused for its source leaves the handle busy for good: later Opens and Destroy answer SEQ.
- Colour to gray uses libpng's gamma tables when the image has a gamma, and otherwise coefficients that are not
  libpng's defaults; found by fitting the pixels.

## pspocket's cellPngDec against it

| step | differences |
|---|---|
| first comparison | 92 lines and some images: the five queries not yet written (iCCP, hIST, sPLT, pCAL, sCAL), the alpha libpng 1.6 adds for a palette's tRNS, cHRM and gAMA under sRGB, tRNS cleared by libpng 1.6 |
| those fixed | 16 images: gray from 1/2/4-bit gray (Sony packs, it does not scale), fixed alpha over tRNS, colour to gray |
| libpng 1.2.8's gray arithmetic, then its gamma tables | 2 images, then none, with the coefficients that fit every pixel |
| with the file source (a file, and a second PNG at an offset in it) | 2,283 lines and 121 images (66,048 bytes): 0 differences |

The answers compared include every result code, header, output parameter set (useMemorySpace included), data-out
block, every chunk query's answer, the sequence and argument errors, and the handle and stream lifetimes.

## The gate

Run with D-200 (run 107): the gate with D-199 and D-200 (one run, c73a45e+, inventory/gate/20261006-233129-c73a45e+): **PASS**. The 20 test
suites, then GTA V's prologue walk: 29.8 fps median and in the slowest 5 s window, 833 frames compared, 2 frame jumps,
24.7 GPU ms per frame at worst, 84.1 render passes, 973 MB process memory at peak, no new warning shapes. Neither game
in the walk uses an image decoder: this checks that nothing else moved.

