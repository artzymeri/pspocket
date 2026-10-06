# Run 107 · pspocket's cellJpgDec against Sony's libjpgdec

2026-10-06 night · tests/hle/test_jpgdec.cpp after D-200 · M4 Mac · the user's firmware 4.92 (PSPOCKET_TEST_DEV_FLASH).
No game: neither game on the Mac imports cellJpgDec. The images are built in the test (jpeg_writer.hpp): 9 JPEGs of
16x16 to 37x21 pixels: 4:2:0, 4:2:2, 4:4:4 and gray, one with restart markers (quality 90), one with an Adobe APP14,
one with no APP0 (quality 30), and two progressive ones (gray, 4:2:0: a DC scan with successive approximation, AC scans
in spectral bands, the DC refinement). Each is decoded with 20 settings (both methods, scales 1, 2, 3, 4, 8, 16 and 0,
every colour space, both row orders, alphas), then 14 damaged files and calls out of order. Sony's libjpgdec is read in
the scratch folder only (D-046).

## What Sony's libjpgdec is

Its strings say IJG libjpeg 6b; what it runs on the PPU (disassembly, and a trace of which of its functions each decode
calls) is 6b with Sony's changes:

- Method 0 sets libjpeg's dct_method 2 (the float IDCT) built with FAST_FLOAT double; method 5 sets 3, Sony's own, a
  16-bit AltiVec IDCT in the tangent form (the Motorola/Apple AltiVec IDCT's flow) with no extra precision bits, and
  turns fancy upsampling off.
- Its reduced-size IDCTs are not 6b's jidctred.c: a four-lane float AltiVec 4-point IDCT (1/2), a float 2-point one
  (1/4).
- YCbCr to RGB: AltiVec over each row's first multiple of 16 pixels (32 in the merged upsampler), 6b's tables for the
  rest. Seen as off-by-one pixels only at x >= 32 at first.
- Its bit buffer is 64 bits (refilled to 57 bits, not 6b's 25), which shows only when a file ends early.
- Its Huffman decoder sums each MCU place's DC value; the "mean" in DecodeData's answer comes from those sums.
- useMemorySpace is Sony's own estimate over libjpeg's state.
- It also carries an SPU program (jpeg_dec_spu) with float IDCTs; with SPU threads on, every image here was still the
  same (the PPU decoded), only Create's allocations differ.

## pspocket's cellJpgDec against it

| step | differences |
|---|---|
| 6b's integer IDCTs (jidctint, jidctfst, jidctred) | 68 of 106 images |
| the float IDCT for the quality method, chroma enlarged through a larger IDCT (jdmaster.c) | full size exact in gray and YCbCr; RGB 34 bytes, the fast method ~1,800, scaled outputs |
| Sony's reduced-size IDCTs | 1/2 and 1/4 exact |
| Sony's AltiVec colour conversion | quality method exact everywhere |
| Sony's AltiVec IDCT for the fast method, 32-pixel groups in the merged upsampler | 106 of 106 images exact; 166 lines (mean, useMemorySpace) |
| Sony's mean and memory estimate | 1335 lines, 106 images: 0 differences |
| with SPU threads on (Create's three blocks, Open's figure) | 0 |
| progressive files | 0 images; useMemorySpace's other branch, gray to alpha counting 4 components |
| damaged files: 6b's entropy decoding step for step, 64-bit bit buffer, Sony's errors | 0, after one fix below |
| final | 1,848 lines and 149 images, identical with SPU threads off and on |

The bug the damaged files found: the range-limit table clamped IDCT outputs only up to 383 instead of 511, so a very
bright overflowing sample wrapped to black. No correct file reaches that range.

The answers compared include every result code, header, output parameter set (useMemorySpace included), data-out block
(mean, lines, status), the images written (with partial ones on errors), and the handle and stream lifetimes. A digest
of pspocket's answers is a test of its own, so they stay so without the firmware.

## The gate

To run with D-200 (below).
