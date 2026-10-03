# recomp/spu — SPU extractor, analyzer, lifter (Phase 3)

Not started; Phase 3 begins when the Phase 2 gate passes (docs/plan.md section 10).

- Extract SPU programs statically (embedded SPU ELFs found by `ps3inspect`) and from runtime traces (hashes logged
  by the SPU runtime).
- One C function per SPU program entry, keyed by a hash of the code region; 128 × 128-bit registers as NEON types,
  lane order D-004 (preferred slot = guest element 0 = host lane 3 for words).
- `bi`/`bisl` through a per-program local-store dispatch table; unknown overlays fall back to the SPU interpreter.
- Oracle first: the SPU interpreter in `runtime/spu/` is built before this lifter (CLAUDE.md rule 3).
