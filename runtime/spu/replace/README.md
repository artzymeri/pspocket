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
```
