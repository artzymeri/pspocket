# Run 108 · pspocket's cellGifDec against Sony's libgifdec

2026-10-06 night · tests/hle/test_gifdec.cpp after D-202 · M4 Mac · the user's firmware 4.92 (PSPOCKET_TEST_DEV_FLASH).
No game: neither game on the Mac imports cellGifDec. The GIFs are built in the test (gif_writer.hpp): 18 files from
7x5 to 64x64: GIF87a and 89a; global and local colour tables of 2 to 256 colours; frames filling the screen, smaller
than it at an offset, and past its edge; interlacing (frames of 1, 2, 3 and 5 rows too); LZW at code sizes 2 to 8,
with early clear codes and 7-byte sub-blocks, a full table with a clear and without one; graphic control (transparency
on one frame, on the background index), comment (up to 620 bytes in four sub-blocks), application and plain text
extensions; a two-frame animation; no colour table; no trailer. Each with four parameter sets (RGBA and ARGB, three
alpha pairs, a refused colour space), then six damaged files, eight call orders, and stop and continue through
commandPtr. Sony's libgifdec is read in the scratch folder only (D-046).

## What Sony's libgifdec is

giflib behind Sony's API (its allocations: 12-byte colour maps, a 0x6148-byte private state with the LZW tables, a
0x100-byte buffer). DecodeData returns one record a call: an extension (label and a pointer to its sub-blocks in that
buffer, which grows a sub-block at a time in whole 0x100s), an image (the whole logical screen), or the trailer.

## pspocket's cellGifDec against it

| step | differences |
|---|---|
| a model of Sony's images (background in the image's own table, alpha2, transparent pixels alpha1) | 30 of 30, then 63 of 69 |
| the background index can be the transparent one; transparency stays until the next graphic control | 0 images |
| pspocket's cellGifDec, first comparison | 28 lines: the bytes past each extension in Sony's buffer |
| those are libgifdec's own state sharing the block: compared as the extension's sub-blocks | 1131 lines and 85 images: 0 differences, SPU threads off and on |

With SPU threads on, Sony's images are the same; Create takes 0x33880 more bytes, which moves every later address
(pspocket takes the same).

The answers compared include every result code, screen descriptor, output parameter set (useMemorySpace included),
record (type, label, buffer address, status), extension bytes, image (with what a failed call left in the buffer),
and the handle and stream lifetimes. A digest of pspocket's answers is a test of its own.
