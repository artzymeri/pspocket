# runtime/spu/replace — native replacements for known SPU programs

Plan section 6, "Middleware replacement"; D-965 to D-968. Only what profiling justifies: the functions of a known
build of middleware where a game spends its SPU time, done natively and checked bit for bit against the lifted code on
states captured from the game.

- `replace.cpp` — the table of known builds (program key, code range, the listed functions: entry, native
  implementation, the registers their callers still read), the programs derived from them (registered ahead of the
  lifted program, their listed entries going to a dispatcher), the switches (`PSPOCKET_SPU_NATIVE`, `_ONLY`, `_SCRAMBLE`),
  the capture (`PSPOCKET_SPU_CAPTURE`) and the entry trace (`PSPOCKET_SPU_TRACE_ENTRIES`). The API and the contract a
  native implementation keeps are in `pspocket/spu/replace.hpp`.
- `neon.h` — building blocks: SPU float operations four lanes at a time (frest, frsqest, fm, fma, fcgt as keys),
  big-endian quadwords de-interleaved; each gives what the lifted code's helper gives (tests/spu/test_spu_replace.cpp).
- `edge_dad4.c` — Sony's EDGE geometry as GTA V's geometry job (spu_dad457863220b1cc) links it.
- `flashmp3.c` (`mp3_huffman.h`) — Sony's MP3 decoder module (dev_flash's flashMP3.pic, program 884ae88f68a9e778), the
  build every MultiStream game loads: decodeFrame, its state in the caller's buffers in the module's layout (D-974).
  The PCM and the float state are equivalent (a step at most), not the same bits: the table's regions say where.
- `zinflate.c` — zlib 1.2.3's inflate on a stream and state in local store (D-975), for GTA V's inflateServer task
  (spu_3ffe1d50f8a58e15): any build of 1.2.3 is a table of its fixed tables' and messages' addresses.

The lifter (recomp/spu) asks the table whether a call target is listed: a listed function is called through the
program's table (D-966), so the derived program can take it over even where the job calls it directly.

Checking a replacement (the game's data stays in inventory/, CLAUDE.md rule 1):

```sh
PSPOCKET_SPU_CAPTURE=dad457863220b1cc:ecf8 PSPOCKET_SPU_CAPTURE_EVERY=1000 PSPOCKET_SPU_CAPTURE_DIR=inventory/cap \
    build/host/runtime/loader/pspocket-boot ...          # the walk: states at entry and exit of the lifted function
build/host/runtime/loader/spu-native-check inventory/cap --time 9   # native against lifted, bit for bit, and timed
build/host/recomp/spu/spu-live --image ... --at RETURN_SITE --why   # what callers read after it (D-967)
PSPOCKET_SPU_NATIVE=ab PSPOCKET_SPU_AB=10 pspocket-boot ...          # A/B on the walk's SPURS time lines
PSPOCKET_SPU_NATIVE_SCRAMBLE=1 pspocket-boot ...                     # the contract's unspecified registers garbage
PSPOCKET_SPU_NATIVE=ab PSPOCKET_SPU_AB=10 PSPOCKET_SPU_NATIVE_TIME=1 pspocket-boot ...  # each function's time, both ways
build/host/runtime/loader/spu-native-check inventory/cap --ranges --dump DIR   # what differs, and the states to look at
```

A function whose output is decoded audio is held to equivalence, not bits (D-974): its regions (`Region` in
pspocket/spu/replace.hpp: float and 16-bit regions with a tolerance, dead regions with the reason in the table) are
compared as such by spu-native-check; everything outside them stays exact. A program listed for a direct call (zlib's
inflate, called from the task's main loop) needs the program lifted again (scripts/lift-spu.sh, or the phone's own
translation) before the call goes through the table; an entry reached through a register (the MP3 module's) does not.
